/**
 * The game client: everything that changes at frame rate.
 *
 * Deliberately a plain class, not React state. A 60fps loop pushed through
 * React's reconciler would spend the frame budget diffing and re-running MUI's
 * style pipeline, 60 times a second, to move three rectangles. React owns the
 * shell; this owns the game. The only bridge is `subscribe`/`getHud`, which
 * React reads through `useSyncExternalStore` at human rates.
 */

import {
  C,
  MsgType,
  MatchState as Phase,
  EventKind,
  JoinMode,

  TransportKindName,
  encodeINPUT,
  encodeJOIN,
  encodePING,
  encodeLEAVE,
  decodeSNAPSHOT,
  decodeMATCH_START,
  decodeEVENT,
  decodePONG,
  decodeBYE,
  parseFrames,
  padBytes,
  unpadBytes,
} from '../../../shared/gen/protocol';
import { predictPaddle, stepPaddle, clampQ4, FIELD_H_Q4, PADDLE_HALF_Q4 } from '../../../shared/sim/paddle';
import { SnapshotRing, Clock, TICK_MS, type View } from './interp';
import { connect, type LadderEvent, type RungName } from '../net/ladder';
import type { ClientTransport } from '../net/types';

export interface Hud {
  status: 'idle' | 'connecting' | 'lobby' | 'queued' | 'playing' | 'over';
  transport: string;
  rttMs: number;
  arrivalGapMs: number;
  renderDelayMs: number;
  phase: number;
  side: number;
  scoreL: number;
  scoreR: number;
  oppName: string;
  oppPlatform: number;
  winScore: number;
  slowMode: boolean;
  countdown: number;
  ladder: LadderEvent[];
  error: string | null;
  snapshotsSeen: number;
}

const INPUT_HZ = 30;
const PING_EVERY_MS = 2000;

export class GameClient {
  private transport: ClientTransport | null = null;
  private ring = new SnapshotRing();
  private clock = new Clock();

  private raf = 0;
  private running = false;

  /** Our own paddle, predicted locally at full frame rate. */
  private myY = FIELD_H_Q4 >> 1;
  /** Last authoritative position for our paddle, and the tick it came from. */
  private myServerY = FIELD_H_Q4 >> 1;
  private myServerTick = 0;

  private targetY = FIELD_H_Q4 >> 1;
  private inputSeq = 0;
  private lastInputAt = 0;
  private lastPingAt = 0;


  private hud: Hud = {
    status: 'idle', transport: '-', rttMs: 0, arrivalGapMs: 0, renderDelayMs: 100,
    phase: Phase.LOBBY, side: 0, scoreL: 0, scoreR: 0, oppName: '', oppPlatform: 0,
    winScore: C.WIN_SCORE, slowMode: false, countdown: 0, ladder: [], error: null,
    snapshotsSeen: 0,
  };

  private listeners = new Set<() => void>();
  private view: View | null = null;

  /** Fired on goals etc. so the UI can flash without polling. */
  onEvent: ((kind: number, a: number, b: number, c: number) => void) | null = null;

  /* ------------------------------------------------------------ React glue */

  subscribe = (fn: () => void): (() => void) => {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  };

  getHud = (): Hud => this.hud;

  /** Replaces the HUD object so `useSyncExternalStore` sees a new reference. */
  private setHud(patch: Partial<Hud>): void {
    this.hud = { ...this.hud, ...patch };
    for (const l of this.listeners) l();
  }

  /** Current interpolated view, read by the renderer. Never via React. */
  getView(): View | null {
    return this.view;
  }

  getMyY(): number {
    return this.myY;
  }

  /* -------------------------------------------------------------- lifecycle */

  async start(opts: { base: string; name: string; force?: RungName | null }): Promise<void> {
    this.setHud({ status: 'connecting', error: null, ladder: [] });
    try {
      const res = await connect({
        base: opts.base,
        name: opts.name,
        force: opts.force ?? null,
        onFrame: (b) => this.ingest(b),
        onEvent: (e) => this.setHud({ ladder: [...this.hud.ladder, e] }),
      });
      this.transport = res.transport;
      this.setHud({
        status: 'lobby',
        transport: TransportKindName[res.transport.kind] ?? res.transport.name,
      });
      this.running = true;
      this.loop();
    } catch (e) {
      this.setHud({ status: 'idle', error: e instanceof Error ? e.message : String(e) });
      throw e;
    }
  }

  join(mode: number, room = ''): void {
    if (!this.transport) return;
    this.ring.clear();
    this.transport.send(encodeJOIN({
      mode,
      flags: 0,
      roomCode: padBytes(room.toUpperCase(), C.ROOM_CODE_BYTES),
    }, ++this.inputSeq & 0xffff));
    this.setHud({ status: mode === JoinMode.VS_BOT ? 'playing' : 'queued', scoreL: 0, scoreR: 0 });
  }

  leave(): void {
    this.transport?.send(encodeLEAVE(undefined, 0));
    this.setHud({ status: 'lobby' });
  }

  stop(): void {
    this.running = false;
    cancelAnimationFrame(this.raf);
    this.transport?.close();
    this.transport = null;
    this.setHud({ status: 'idle' });
  }

  /* ------------------------------------------------------------------ input */

  /** Sets the desired paddle centre from a normalised 0..1 vertical position. */
  setTargetNormalized(t: number): void {
    const y = Math.round(clampQ4(t, 0, 1) * FIELD_H_Q4);
    this.targetY = clampQ4(y, PADDLE_HALF_Q4, FIELD_H_Q4 - PADDLE_HALF_Q4);
  }

  /** Nudges the target, for keyboard control. */
  nudgeTarget(deltaQ4: number): void {
    this.targetY = clampQ4(this.targetY + deltaQ4, PADDLE_HALF_Q4, FIELD_H_Q4 - PADDLE_HALF_Q4);
  }

  getTargetY(): number {
    return this.targetY;
  }

  /* --------------------------------------------------------------- receive */

  private ingest(buf: Uint8Array): void {
    const { frames, error } = parseFrames(buf);
    if (error !== 0) {
      this.setHud({ error: `protocol desync (${error})` });
      return;
    }
    const now = performance.now();

    for (const f of frames) {
      switch (f.type) {
        case MsgType.SNAPSHOT: {
          const s = decodeSNAPSHOT(f.buf, f.payloadOff);
          this.ring.push({
            tick: s.tick,
            ballX: s.ballXQ4, ballY: s.ballYQ4,
            ballVX: s.ballVXQ4, ballVY: s.ballVYQ4,
            leftY: s.leftYQ4, rightY: s.rightYQ4,
            scoreL: s.scoreL, scoreR: s.scoreR,
            state: s.state, flags: s.flags,
            arrivedAt: now,
          });

          // Our own paddle's authoritative position, for reconciliation.
          const mine = this.hud.side === 0 ? s.leftYQ4 : s.rightYQ4;
          if (s.tick >= this.myServerTick) {
            this.myServerTick = s.tick;
            this.myServerY = mine;
          }

          if (this.hud.scoreL !== s.scoreL || this.hud.scoreR !== s.scoreR ||
              this.hud.phase !== s.state) {
            this.setHud({
              scoreL: s.scoreL, scoreR: s.scoreR, phase: s.state,
              status: s.state === Phase.GAME_OVER ? 'over' : 'playing',
              snapshotsSeen: this.hud.snapshotsSeen + 1,
            });
          }
          break;
        }

        case MsgType.MATCH_START: {
          const m = decodeMATCH_START(f.buf, f.payloadOff);
          this.ring.clear();

          /*
           * Forget the previous match's paddle state.
           *
           * The server builds a fresh simulation per match, so its tick counter
           * starts again at zero. Our own-paddle update accepts a snapshot only
           * when it is at least as new as the last tick seen -- correct within a
           * match, where it rejects snapshots that arrive out of order, and
           * fatal across one: every tick of the new match is "older" than the
           * tick left over from the old, so none is accepted. The authoritative
           * position then stays pinned wherever the last game ended, prediction
           * has nothing to advance from, and the paddle stops responding to
           * input until the page is reloaded.
           *
           * Centred rather than kept, because that is where the server puts the
           * paddles at the start of a match. `targetY` is deliberately NOT
           * reset: the pointer has not moved, so the player's intent still
           * stands, and the paddle travels to it under the same speed clamp the
           * server applies.
           */
          this.myServerTick = 0;
          this.myServerY = FIELD_H_Q4 >> 1;
          this.myY = FIELD_H_Q4 >> 1;

          this.setHud({
            status: 'playing',
            side: m.yourSide,
            oppName: unpadBytes(m.oppName) || 'OPPONENT',
            oppPlatform: m.oppPlatform,
            winScore: m.winScore,
            slowMode: (m.matchFlags & 1) !== 0,
            scoreL: 0, scoreR: 0,
          });
          break;
        }

        case MsgType.EVENT: {
          const e = decodeEVENT(f.buf, f.payloadOff);
          if (e.kind === EventKind.MATCH_OVER) this.setHud({ status: 'over' });
          if (e.kind === EventKind.OPP_LEFT) this.setHud({ error: 'Opponent left' });
          this.onEvent?.(e.kind, e.a, e.b, e.c);
          break;
        }

        case MsgType.PONG: {
          const p = decodePONG(f.buf, f.payloadOff);
          this.clock.sample(p.clientTimeMs, p.serverTimeMs, p.serverTick, Date.now());
          this.setHud({ rttMs: this.clock.minRtt });
          break;
        }

        case MsgType.BYE: {
          const b = decodeBYE(f.buf, f.payloadOff);
          this.setHud({ error: `disconnected (code ${b.code})`, status: 'idle' });
          break;
        }

        default:
          break;
      }
    }
  }

  /* ------------------------------------------------------------------- loop */

  private loop = (): void => {
    if (!this.running) return;
    this.raf = requestAnimationFrame(this.loop);

    const nowMs = Date.now();

    // --- own paddle: predicted from the last authoritative position --------
    // Because the server applies the same clamp to the same absolute target,
    // this converges on exactly what the server will compute -- no rubber-band.
    const serverTickNow = this.clock.ready ? this.clock.serverTick(nowMs) : this.myServerTick;
    const ticksSince = Math.max(0, serverTickNow - this.myServerTick);
    const predicted = predictPaddle(this.myServerY, this.targetY, ticksSince);

    // Reconcile gently. A large error means the server rejected or clamped
    // something, and snapping is the honest response; a small one is sub-tick
    // noise and should be smoothed away invisibly.
    const err = predicted - this.myY;
    if (Math.abs(err) > 64 /* 4px */) this.myY = predicted;
    else this.myY += err * 0.25;

    // --- everything else: interpolated in the past ------------------------
    const delay = this.ring.renderDelayMs();
    const renderTick = serverTickNow - delay / TICK_MS;
    this.view = this.ring.sample(renderTick);

    // --- outbound ---------------------------------------------------------
    if (nowMs - this.lastInputAt >= 1000 / INPUT_HZ) {
      this.lastInputAt = nowMs;
      this.transport?.send(encodeINPUT({
        inputSeq: ++this.inputSeq >>> 0,
        lastTickSeen: this.ring.newestTick,
        desiredYQ4: this.targetY,
        buttons: 0,
        flags: 0,
      }, this.inputSeq & 0xffff));
    }

    if (nowMs - this.lastPingAt >= PING_EVERY_MS) {
      this.lastPingAt = nowMs;
      
      this.transport?.send(encodePING({ clientTimeMs: nowMs, echoServerTimeMs: 0 }, 0));
    }

    // --- HUD, at human rates ----------------------------------------------
    if (nowMs % 100 < 17) {
      const gap = this.ring.arrivalGapMs();
      if (gap !== this.hud.arrivalGapMs || Math.round(delay) !== this.hud.renderDelayMs) {
        this.setHud({ arrivalGapMs: gap, renderDelayMs: Math.round(delay) });
      }
    }
  };

  /** Exposed for the debug drawer. */
  stats(): { ringSize: number; newestTick: number; predictedY: number } {
    return { ringSize: this.ring.size, newestTick: this.ring.newestTick, predictedY: this.myY };
  }
}

export { stepPaddle };
