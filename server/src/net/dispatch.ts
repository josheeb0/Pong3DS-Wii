/**
 * Turns inbound frames into game actions.
 *
 * Every transport funnels through here, so a 3DS POSTing a body of frames and a
 * browser pushing a WebSocket message take the identical path. Anything that
 * needed a per-transport branch in this file would be a design smell.
 */

import {
  C,
  MsgType,
  ByeCode,
  decodeHELLO,
  decodeJOIN,
  decodeINPUT,
  decodePING,
  decodeRESUME,
  encodePONG,
  encodeBYE,
  parseFrames,
  unpadBytes,
  type Frame,
} from '../../../shared/gen/protocol.js';
import { FIELD_H_Q, PADDLE_HALF_Q } from '../../../shared/sim/pong.js';
import type { Session, SessionManager } from './session.js';
import type { Matchmaker } from '../game/matchmaker.js';

export interface DispatchCtx {
  now: number;
  serverTimeMs: number;
  sessions: SessionManager;
  matchmaker: Matchmaker;
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

/**
 * Applies one already-parsed frame.
 *
 * Returns false if the session should be torn down.
 */
function applyFrame(s: Session, f: Frame, ctx: DispatchCtx): boolean {
  s.lastSeenMs = ctx.now;

  switch (f.type) {
    case MsgType.HELLO: {
      const m = decodeHELLO(f.buf, f.payloadOff);
      s.platform = m.platform;
      s.buildId = m.buildId;
      s.name = unpadBytes(m.name).slice(0, C.NAME_BYTES) || 'PLAYER';
      return true;
    }

    case MsgType.RESUME: {
      // Identity is proven by possession of the token, which was only ever
      // delivered over the session's own creation response.
      return true;
    }

    case MsgType.JOIN: {
      const m = decodeJOIN(f.buf, f.payloadOff);
      const code = unpadBytes(m.roomCode).toUpperCase();
      const room = ctx.matchmaker.join(s, m.mode, code, ctx.now);
      if (!room && m.mode === 1 /* ROOM_CODE */) {
        s.push(encodeBYE({ code: ByeCode.ROOM_FULL, reserved: 0 }, s.nextSeq()));
      }
      return true;
    }

    case MsgType.INPUT: {
      const m = decodeINPUT(f.buf, f.payloadOff);

      // Reordered or duplicated input is discarded rather than applied: an old
      // absolute target would yank the paddle backwards. Sequence numbers wrap
      // at 2^32, so treat a large backward jump as a wrap rather than as stale.
      const delta = (m.inputSeq - s.lastInputSeq) | 0;
      if (s.lastInputSeq !== 0 && delta <= 0 && delta > -0x40000000) return true;
      s.lastInputSeq = m.inputSeq >>> 0;

      // Clamping here, not in the sim, keeps the sim honest about its inputs.
      s.targetYQ4 = clamp(m.desiredYQ4, PADDLE_HALF_Q, FIELD_H_Q - PADDLE_HALF_Q);
      s.buttons = m.buttons;

      // What the client says it holds drives snapshot batching for pull
      // transports. Clamp forward-only: a client cannot claim a future tick.
      const claimed = m.lastTickSeen >>> 0;
      const newest = s.snapshots?.newestTick ?? 0;
      if (claimed <= newest && claimed > s.ackTick) s.ackTick = claimed;
      return true;
    }

    case MsgType.PING: {
      const m = decodePING(f.buf, f.payloadOff);
      s.push(encodePONG({
        clientTimeMs: m.clientTimeMs,
        serverTimeMs: ctx.serverTimeMs,
        serverTick: s.snapshots?.newestTick ?? 0,
      }, s.nextSeq()));
      return true;
    }

    case MsgType.LEAVE: {
      ctx.matchmaker.leave(s);
      return false;
    }

    default:
      // Unknown but well-formed frames are ignored rather than fatal, so a
      // newer client talking to an older server degrades instead of dying.
      return true;
  }
}

export interface DispatchResult {
  ok: boolean;
  consumed: number;
  /** Set when the buffer was malformed; the caller should close. */
  fatal?: string;
}

/**
 * Parses and applies a buffer of frames.
 *
 * `leftover` handling matters for TCP, where a read can split mid-frame: the
 * caller keeps the unconsumed tail and prepends it to the next read.
 */
export function dispatchBuffer(s: Session, buf: Uint8Array, ctx: DispatchCtx): DispatchResult {
  const { frames, consumed, error } = parseFrames(buf);

  for (const f of frames) {
    if (!applyFrame(s, f, ctx)) {
      return { ok: false, consumed };
    }
  }

  if (error !== 0) {
    // A malformed stream cannot be resynchronised safely -- the magic exists
    // precisely so we can tell garbage from a partial frame.
    return { ok: false, consumed, fatal: `parse error ${error}` };
  }

  return { ok: true, consumed };
}
