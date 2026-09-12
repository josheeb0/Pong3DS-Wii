/**
 * Entry point: wires the pieces together and owns the clock.
 *
 * The 60Hz loop is accumulator-based rather than a naive setInterval. Node's
 * timers drift under load, and a drifting tick rate would make the client's
 * clock estimate wrong, which shows up as a stuttering interpolator rather than
 * as anything that looks like a timer bug. Catch-up is capped at 5 ticks so a
 * long GC pause cannot trigger a spiral of doom.
 */

import { WebSocketServer } from 'ws';
import { createServer as createTcpServer, type Socket } from 'node:net';
import { config, TICK_MS } from './config.js';
import { C, TransportKindName, ByeCode, PROTOCOL_VERSION, encodeWELCOME } from '../../shared/gen/protocol.js';
import { SessionManager } from './net/session.js';
import { Matchmaker } from './game/matchmaker.js';
import { HttpLayer } from './http/server.js';
import { dispatchBuffer, type DispatchCtx } from './net/dispatch.js';
import { WsSink } from './net/transports/ws.js';
import { TcpSink } from './net/transports/tcp.js';

const startMs = Date.now();
const sessions = new SessionManager();
const matchmaker = new Matchmaker();

/** Milliseconds since process start — the timebase every client syncs to. */
const serverTime = (): number => Date.now() - startMs;

const makeCtx = (): DispatchCtx => ({
  now: Date.now(),
  serverTimeMs: serverTime(),
  sessions,
  matchmaker,
});

function concatBytes(a: Uint8Array, b: Uint8Array): Uint8Array {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

/* ------------------------------------------------------------------ metrics */

const drift: number[] = [];
let ticks = 0;

function percentile(arr: number[], p: number): number {
  if (arr.length === 0) return 0;
  const sorted = [...arr].sort((a, b) => a - b);
  const i = Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length));
  return Math.round((sorted[i] as number) * 100) / 100;
}

function stats(): Record<string, unknown> {
  const byTransport: Record<string, number> = {};
  for (const s of sessions.all()) {
    const k = s.sink ? (TransportKindName[s.sink.kind] ?? String(s.sink.kind)) : 'detached';
    byTransport[k] = (byTransport[k] ?? 0) + 1;
  }
  return {
    uptimeMs: Date.now() - startMs,
    ticks,
    tickHz: config.tickHz,
    sessions: sessions.size,
    rooms: matchmaker.roomCount,
    queued: matchmaker.queueLength,
    longPollWaiters: http.waiters.length,
    byTransport,
    tickDriftMs: { p50: percentile(drift, 50), p95: percentile(drift, 95) },
  };
}

/* --------------------------------------------------------------------- HTTP */

const http = new HttpLayer({
  sessions,
  matchmaker,
  serverStartMs: startMs,
  makeCtx,
  stats,
});

/* ---------------------------------------------------------------- WebSocket */

const wss = new WebSocketServer({ server: http.server, path: '/api/ws' });

wss.on('connection', (ws, req) => {
  const url = new URL(req.url ?? '/', 'http://localhost');
  const s = sessions.get(url.searchParams.get('s') ?? undefined);
  if (!s) {
    ws.close(4404, 'unknown session');
    return;
  }
  const sink = new WsSink(ws);
  s.attach(sink, Date.now());
  s.everyNTicks = 2; // 30Hz

  ws.on('message', (data: Buffer, isBinary: boolean) => {
    if (!isBinary) return;
    const r = dispatchBuffer(s, new Uint8Array(data), makeCtx());
    if (r.fatal || !r.ok) {
      s.close(r.fatal ? ByeCode.PROTO_ERR : ByeCode.NORMAL);
      matchmaker.leave(s);
    }
  });

  ws.on('close', () => s.detach(sink));
  ws.on('error', () => s.detach(sink));

  s.flush();
});

/* ---------------------------------------------------------------------- TCP */

/**
 * The 3DS LAN path. A socket read can split anywhere, so unconsumed bytes are
 * carried forward — the frame header's length field is what makes that safe.
 */
const tcp = createTcpServer((sock: Socket) => {
  sock.setNoDelay(true);

  // Explicit generic: Buffer's backing store is ArrayBufferLike, and mixing it
  // with a plain Uint8Array<ArrayBuffer> trips TS 5.7's stricter typed arrays.
  let pending: Uint8Array<ArrayBufferLike> = new Uint8Array(0);
  let bound: ReturnType<typeof sessions.create> | null = null;
  const sink = new TcpSink(sock);

  // A TCP client has no HTTP bootstrap, so it gets a session on connect and
  // receives its WELCOME as the first frame on the stream.
  const ctx = makeCtx();
  // Refuse rather than accept-then-fail: a TCP client has no HTTP bootstrap to
  // report a 503 through, so the honest signal is a closed connection.
  if (sessions.size >= config.maxSessions) {
    sock.destroy();
    return;
  }
  bound = sessions.create(ctx.now);
  bound.attach(sink, ctx.now);
  bound.everyNTicks = 2;
  bound.platform = 2; // N3DS until its HELLO says otherwise

  bound.push(encodeWELCOME({
    sessionId: bound.token,
    serverTimeMs: serverTime(),
    tickRate: config.tickHz,
    snapshotRate: C.SNAPSHOT_HZ,
    fieldW: C.FIELD_W,
    fieldH: C.FIELD_H,
  }, bound.nextSeq()));
  bound.flush();

  sock.on('data', (chunk: Buffer) => {
    pending = pending.length === 0
      ? Uint8Array.from(chunk)
      : concatBytes(pending, Uint8Array.from(chunk));
    if (pending.length > 8192) {
      // Nothing legitimate accumulates this much; treat it as a desync.
      sock.destroy();
      return;
    }
    if (!bound) return;
    const r = dispatchBuffer(bound, pending, makeCtx());
    pending = pending.subarray(r.consumed);
    if (r.fatal) {
      bound.close(ByeCode.PROTO_ERR);
      sock.destroy();
    } else if (!r.ok) {
      matchmaker.leave(bound);
      bound.close(ByeCode.NORMAL);
      sock.end();
    }
  });

  const drop = () => {
    if (bound) {
      bound.detach(sink);
      // Leave the session alive for the grace period so a flaky 3DS wifi blip
      // does not forfeit the match.
    }
  };
  sock.on('close', drop);
  sock.on('error', drop);
});

/* ---------------------------------------------------------------- game loop */

let nextTickAt = Date.now();
let houseKeepAt = Date.now();

function loop(): void {
  const now = Date.now();
  let caught = 0;

  while (now >= nextTickAt && caught < 5) {
    drift.push(now - nextTickAt);
    if (drift.length > 600) drift.shift();

    matchmaker.stepAll();
    ticks++;
    nextTickAt += TICK_MS;
    caught++;
  }

  // A long stall (GC, host contention) must not queue up hundreds of ticks.
  if (now > nextTickAt + TICK_MS * 5) nextTickAt = now;

  // Push transports get whatever is queued; pull transports are resolved here
  // too, so a parked long-poll wakes within a tick of a frame appearing.
  for (const s of sessions.all()) {
    if (s.sink && !s.sink.pull) s.flush();
  }
  http.pumpWaiters(now);

  if (now - houseKeepAt >= 1000) {
    houseKeepAt = now;
    matchmaker.tick(now);
    sessions.sweep(now, (s) => matchmaker.leave(s));
  }

  const delay = Math.max(0, nextTickAt - Date.now());
  setTimeout(loop, delay);
}

/* ------------------------------------------------------------------- boot */

http.server.listen(config.httpPort, () => {
  console.log(`[pong] http+ws  :${config.httpPort}`);
  if (config.staticDir) console.log(`[pong] static    ${config.staticDir}`);
});

if (config.tcpPort > 0) {
  tcp.listen(config.tcpPort, () => {
    console.log(`[pong] tcp (3ds) :${config.tcpPort}  (LAN fast path)`);
  });
} else {
  // Not a degraded mode: the 3DS reaches the same server over HTTPS on the
  // HTTP port, which is all a 443-only deployment can offer anyway.
  console.log('[pong] tcp disabled (TCP_PORT=0) -- 3DS will use the HTTPS path');
}

loop();
console.log(`[pong] sim ${config.tickHz}Hz, snapshots ${C.SNAPSHOT_HZ}Hz, protocol v${PROTOCOL_VERSION}`);

/* --------------------------------------------------------------- shutdown */

let shuttingDown = false;
function shutdown(signal: string): void {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`[pong] ${signal}, draining`);
  for (const s of sessions.all()) s.close(ByeCode.NORMAL);
  wss.close();
  if (config.tcpPort > 0) tcp.close();
  http.server.close();
  // Give BYE frames a moment to leave before the socket dies under them.
  setTimeout(() => process.exit(0), 500).unref();
}

process.on('SIGTERM', () => shutdown('SIGTERM'));
process.on('SIGINT', () => shutdown('SIGINT'));
