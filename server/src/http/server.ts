/**
 * HTTP surface: session bootstrap, the request/response channel, SSE, health,
 * and static serving of the web client.
 *
 * Route map (all game routes live under /api so nginx/Cloudflare rules can
 * target them without touching the static bundle):
 *
 *   POST /api/session        create a session, returns WELCOME, sets the id header
 *   POST /api/rpc[?wait=ms]  frames up, frames down -- long-poll AND the 3DS path
 *   GET  /api/stream?s=id    SSE downstream
 *   GET  /healthz            liveness for CapRover's healthcheck
 *   GET  /api/stats          operational detail, the tunnel-debugging instrument
 *   GET  /*                  the built web client
 */

import { createServer, type IncomingMessage, type ServerResponse, type Server } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import {
  C,
  TransportKind,
  encodeWELCOME,
  PROTOCOL_VERSION,
} from '../../../shared/gen/protocol.js';
import { config } from '../config.js';
import type { SessionManager, Session } from '../net/session.js';
import type { Matchmaker } from '../game/matchmaker.js';
import { dispatchBuffer, type DispatchCtx } from '../net/dispatch.js';
import { RrSink } from '../net/transports/httprr.js';
import { SseSink } from '../net/transports/sse.js';

const MIME: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
  '.map': 'application/json; charset=utf-8',
};

const MAX_BODY = 4096; // a client should never send more than a few frames

export interface Waiter {
  session: Session;
  sink: RrSink;
  deadline: number;
}

export interface HttpDeps {
  sessions: SessionManager;
  matchmaker: Matchmaker;
  serverStartMs: number;
  makeCtx: () => DispatchCtx;
  stats: () => Record<string, unknown>;
}

export class HttpLayer {
  readonly server: Server;
  /** Parked long-poll responses, resolved by the game loop. */
  readonly waiters: Waiter[] = [];

  constructor(private deps: HttpDeps) {
    this.server = createServer((req, res) => {
      this.handle(req, res).catch((err) => {
        // Never let a handler rejection take the process down mid-match.
        console.error('[http] unhandled', err);
        if (!res.headersSent) res.writeHead(500);
        res.end();
      });
    });
  }

  private async handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const url = new URL(req.url ?? '/', 'http://localhost');
    const path = url.pathname;

    // The 3DS sends no Origin and needs no CORS, but a browser served from a
    // different host during development does.
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, X-Pong-Session');
    res.setHeader('Access-Control-Expose-Headers', 'X-Pong-Session');
    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      res.end();
      return;
    }

    if (path === '/healthz') return this.healthz(res);
    if (path === '/api/version') return this.version(res);
    if (path === '/api/stats') return this.stats(res);
    if (path === '/api/session' && req.method === 'POST') return this.createSession(req, res);
    if (path === '/api/rpc' && req.method === 'POST') return this.rpc(req, res, url);
    if (path === '/api/stream' && req.method === 'GET') return this.stream(req, res, url);

    return this.static(path, res);
  }

  /* ------------------------------------------------------------- endpoints */

  private healthz(res: ServerResponse): void {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify({
      ok: true,
      protocol: PROTOCOL_VERSION,
      build: config.buildId,
      sha: config.gitSha,
      uptimeMs: Date.now() - this.deps.serverStartMs,
    }));
  }

  /**
   * Build manifest for the 3DS auto-updater.
   *
   * Deliberately plain text, one `key=value` per line, rather than JSON: the
   * console has no JSON parser and adding one to parse six fields would be
   * absurd. The updater reads this with the same tiny line parser it already
   * uses for its config file.
   */
  private version(res: ServerResponse): void {
    const body =
      `build=${config.buildId}\n` +
      `sha=${config.gitSha}\n` +
      `protocol=${PROTOCOL_VERSION}\n` +
      `dsx=/downloads/pong3ds.3dsx\n` +
      `cia=/downloads/pong3ds.cia\n` +
      (config.releaseUrl ? `release=${config.releaseUrl}\n` : '');
    res.writeHead(200, {
      'Content-Type': 'text/plain; charset=utf-8',
      'Content-Length': String(Buffer.byteLength(body)),
      'Cache-Control': 'no-store',
    });
    res.end(body);
  }

  private stats(res: ServerResponse): void {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(this.deps.stats(), null, 2));
  }

  /**
   * Creates the session over a plain one-shot POST, before any transport is
   * chosen. Doing it this way means identity exists independently of the
   * connection, which is what lets a client fall from WebSocket to SSE to
   * long-poll mid-match without forfeiting its seat.
   */
  private async createSession(req: IncomingMessage, res: ServerResponse): Promise<void> {
    if (this.deps.sessions.size >= config.maxSessions) {
      res.writeHead(503, { 'Cache-Control': 'no-store' });
      res.end();
      return;
    }

    const body = await readBody(req);
    const ctx = this.deps.makeCtx();
    const s = this.deps.sessions.create(ctx.now);

    if (body.length > 0) {
      const r = dispatchBuffer(s, body, ctx);
      if (r.fatal) {
        s.close(3 /* PROTO_ERR */);
        res.writeHead(400, { 'Cache-Control': 'no-store' });
        res.end();
        return;
      }
    }

    const welcome = encodeWELCOME({
      sessionId: s.token,
      serverTimeMs: ctx.serverTimeMs,
      tickRate: config.tickHz,
      snapshotRate: C.SNAPSHOT_HZ,
      fieldW: C.FIELD_W,
      fieldH: C.FIELD_H,
    }, s.nextSeq());

    const buf = Buffer.from(welcome);
    res.writeHead(200, {
      'Content-Type': 'application/octet-stream',
      'Content-Length': String(buf.length),
      'Cache-Control': 'no-store, no-transform',
      'X-Pong-Session': s.id,
    });
    res.end(buf);
  }

  /**
   * The universal request/response channel.
   *
   * `?wait=N` parks the response until something is queued (browser long-poll).
   * Omitted, it answers immediately with whatever is ready (the 3DS path).
   */
  private async rpc(req: IncomingMessage, res: ServerResponse, url: URL): Promise<void> {
    const id = (req.headers['x-pong-session'] as string | undefined) ?? url.searchParams.get('s');
    const s = this.deps.sessions.get(id ?? undefined);
    if (!s) {
      // 404 rather than 401: the client's correct response is to re-bootstrap.
      res.writeHead(404, { 'Cache-Control': 'no-store' });
      res.end();
      return;
    }

    const body = await readBody(req);
    const ctx = this.deps.makeCtx();

    const wait = Math.min(
      Math.max(0, Number(url.searchParams.get('wait') ?? 0) || 0),
      C.LONGPOLL_HOLD_MS,
    );
    const kind = wait > 0 ? TransportKind.LONGPOLL : TransportKind.RR;

    // An SSE or WebSocket client also POSTs here, but only to push input --
    // its state arrives on the stream it already holds. Attaching a sink for
    // those would evict that stream and silently kill the downstream channel.
    // So an upstream-only POST dispatches and returns, claiming nothing.
    const existing = s.sink;
    if (existing && existing.isOpen() && !existing.pull) {
      if (body.length > 0) {
        const r = dispatchBuffer(s, body, ctx);
        if (r.fatal) {
          res.writeHead(400, { 'Cache-Control': 'no-store' });
          res.end();
          return;
        }
      }
      s.lastSeenMs = ctx.now;
      res.writeHead(204, { 'Cache-Control': 'no-store, no-transform', 'X-Accel-Buffering': 'no' });
      res.end();
      return;
    }

    const sink = new RrSink(res, kind);
    s.attach(sink, ctx.now);
    // Pull transports poll at ~10Hz; giving them every 2nd tick (30Hz) means up
    // to 8 real sample points per response rather than one stale position.
    s.everyNTicks = 2;

    if (body.length > 0) {
      const r = dispatchBuffer(s, body, ctx);
      if (r.fatal) {
        res.writeHead(400, { 'Cache-Control': 'no-store' });
        res.end();
        return;
      }
      if (!r.ok) {
        // A LEAVE arrived; acknowledge and let the session go.
        sink.finishEmpty();
        s.detach(sink);
        return;
      }
    }

    if (s.hasPending() || wait === 0) {
      if (!s.flush(C.SNAP_BATCH_MAX + 4)) sink.finishEmpty();
      s.detach(sink);
      return;
    }

    // Park it. The game loop resolves this when a frame appears or the hold
    // expires; 20s max keeps us far under Cloudflare's ~100s idle cutoff.
    const waiter: Waiter = { session: s, sink, deadline: ctx.now + wait };
    this.waiters.push(waiter);
    res.on('close', () => {
      const i = this.waiters.indexOf(waiter);
      if (i >= 0) this.waiters.splice(i, 1);
      s.detach(sink);
    });
  }

  private stream(req: IncomingMessage, res: ServerResponse, url: URL): void {
    const id = (req.headers['x-pong-session'] as string | undefined) ?? url.searchParams.get('s');
    const s = this.deps.sessions.get(id ?? undefined);
    if (!s) {
      res.writeHead(404, { 'Cache-Control': 'no-store' });
      res.end();
      return;
    }
    const ctx = this.deps.makeCtx();
    const sink = new SseSink(res);
    s.attach(sink, ctx.now);
    s.everyNTicks = 2; // 30Hz
    res.on('close', () => s.detach(sink));
    s.flush();
  }

  /** Resolves parked long-polls. Called from the game loop each tick. */
  pumpWaiters(now: number): void {
    for (let i = this.waiters.length - 1; i >= 0; i--) {
      const w = this.waiters[i] as Waiter;
      if (!w.sink.isOpen()) {
        this.waiters.splice(i, 1);
        w.session.detach(w.sink);
        continue;
      }
      const ready = w.session.hasPending();
      const expired = now >= w.deadline;
      if (!ready && !expired) continue;

      this.waiters.splice(i, 1);
      if (!w.session.flush(C.SNAP_BATCH_MAX + 4)) {
        // Hold expired with nothing to say. An empty 204 keeps the client's
        // loop ticking and gives it a free RTT sample.
        w.sink.finishEmpty();
      }
      w.session.detach(w.sink);
    }
  }

  /* ---------------------------------------------------------------- static */

  private async static(path: string, res: ServerResponse): Promise<void> {
    if (!config.staticDir) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('no static dir configured');
      return;
    }

    const root = resolve(config.staticDir);
    let rel = normalize(path).replace(/^(\.\.[/\\])+/, '');
    if (rel === '/' || rel === '') rel = '/index.html';
    let file = join(root, rel);

    // Path traversal guard: the resolved file must stay inside the root.
    if (!resolve(file).startsWith(root)) {
      res.writeHead(403);
      res.end();
      return;
    }

    try {
      const st = await stat(file);
      if (st.isDirectory()) file = join(file, 'index.html');
    } catch {
      // SPA fallback: unknown paths render the app, which routes client-side.
      file = join(root, 'index.html');
    }

    try {
      const data = await readFile(file);
      const ext = extname(file).toLowerCase();
      const immutable = /-[A-Za-z0-9_]{8,}\.(js|css|woff2)$/.test(file);
      res.writeHead(200, {
        'Content-Type': MIME[ext] ?? 'application/octet-stream',
        'Cache-Control': immutable ? 'public, max-age=31536000, immutable' : 'no-cache',
      });
      res.end(data);
    } catch {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('not found');
    }
  }
}

/** Reads a bounded request body. Oversized bodies are truncated, not buffered. */
function readBody(req: IncomingMessage): Promise<Uint8Array> {
  return new Promise((resolve_, reject) => {
    const chunks: Buffer[] = [];
    let total = 0;
    req.on('data', (c: Buffer) => {
      total += c.length;
      if (total > MAX_BODY) {
        req.destroy();
        resolve_(new Uint8Array(0));
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve_(new Uint8Array(Buffer.concat(chunks))));
    req.on('error', reject);
  });
}
