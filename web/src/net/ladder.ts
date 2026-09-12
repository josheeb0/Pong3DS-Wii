import {
  C,
  Platform,
  TransportKind,
  encodeHELLO,
  decodeWELCOME,
  parseFrames,
  padBytes,
  MsgType,
  encodePING,
} from '../../../shared/gen/protocol';
import type { ClientTransport, FrameHandler, SessionHandle } from './types';
import { WsTransport } from './ws';
import { SseTransport } from './sse';
import { LongPollTransport } from './longpoll';

const STORAGE_KEY = 'pong.transport';
/** Remembering a working rung for this long avoids re-probing on every reload. */
const MEMORY_TTL_MS = 10 * 60_000;

export type RungName = 'ws' | 'sse' | 'poll';

export interface LadderEvent {
  rung: RungName;
  ok: boolean;
  detail: string;
  ms: number;
}

export interface LadderResult {
  transport: ClientTransport;
  session: SessionHandle;
  welcome: ReturnType<typeof decodeWELCOME>;
  log: LadderEvent[];
}

/** Budget for each rung to prove it can actually carry game state. */
const BUDGET_MS: Record<RungName, number> = { ws: 2500, sse: 3000, poll: 4000 };

const ORDER: RungName[] = ['ws', 'sse', 'poll'];

function make(rung: RungName): ClientTransport {
  switch (rung) {
    case 'ws': return new WsTransport();
    case 'sse': return new SseTransport();
    case 'poll': return new LongPollTransport();
  }
}

function remembered(): RungName | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return null;
    const { kind, at } = JSON.parse(raw) as { kind: RungName; at: number };
    if (Date.now() - at > MEMORY_TTL_MS) return null;
    return ORDER.includes(kind) ? kind : null;
  } catch {
    return null;
  }
}

function remember(rung: RungName): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ kind: rung, at: Date.now() }));
  } catch {
    /* private browsing: not worth failing over */
  }
}

/**
 * Creates the session, then walks the ladder until a rung proves it can carry
 * game state.
 *
 * Session creation happens FIRST, over a plain POST, and is independent of
 * every rung. That is what lets a failed WebSocket attempt cost nothing but
 * time, and what lets a mid-match demotion keep the same seat.
 */
export async function connect(opts: {
  base: string;
  name: string;
  force?: RungName | null;
  onFrame: FrameHandler;
  onEvent?: (e: LadderEvent) => void;
}): Promise<LadderResult> {
  const log: LadderEvent[] = [];

  const hello = encodeHELLO({
    platform: Platform.WEB,
    transport: TransportKind.WS,
    caps: 0,
    name: padBytes(opts.name, C.NAME_BYTES),
    buildId: 1,
  }, 1);

  const res = await fetch(opts.base + '/api/session', {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: hello,
    cache: 'no-store',
  });
  if (!res.ok) throw new Error(`session bootstrap failed: ${res.status}`);
  const id = res.headers.get('x-pong-session');
  if (!id) throw new Error('server did not return a session id');

  const wf = parseFrames(new Uint8Array(await res.arrayBuffer())).frames[0];
  if (!wf || wf.type !== MsgType.WELCOME) throw new Error('no WELCOME frame');
  const welcome = decodeWELCOME(wf.buf, wf.payloadOff);
  const session: SessionHandle = { id, base: opts.base };

  // Try a forced rung, else a remembered one, else the full ladder.
  const preferred = opts.force ?? remembered();
  const order = preferred
    ? [preferred, ...ORDER.filter((r) => r !== preferred)]
    : ORDER;
  const attempts = opts.force ? [opts.force] : order;

  let lastErr: unknown = null;

  for (const rung of attempts) {
    const t = make(rung);
    const t0 = performance.now();
    try {
      await Promise.race([
        t.open(session, opts.onFrame, encodePING({ clientTimeMs: Date.now(), echoServerTimeMs: 0 }, 0)),
        new Promise<never>((_, rej) =>
          setTimeout(() => rej(new Error(`no server frame within ${BUDGET_MS[rung]}ms`)), BUDGET_MS[rung]),
        ),
      ]);
      const ev: LadderEvent = { rung, ok: true, detail: t.name, ms: Math.round(performance.now() - t0) };
      log.push(ev);
      opts.onEvent?.(ev);
      remember(rung);
      return { transport: t, session, welcome, log };
    } catch (e) {
      lastErr = e;
      const ev: LadderEvent = {
        rung, ok: false,
        detail: e instanceof Error ? e.message : String(e),
        ms: Math.round(performance.now() - t0),
      };
      log.push(ev);
      opts.onEvent?.(ev);
      // Tear the failed rung down hard before trying the next, so a late
      // WebSocket open cannot deliver frames into a game already on SSE.
      t.close();
    }
  }

  throw new Error(`all transports failed; last: ${String(lastErr)}`);
}
