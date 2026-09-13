/**
 * A match: two seats, one simulation, one snapshot history.
 *
 * The ring buffer is the piece that makes slow transports work. Every tick's
 * snapshot is encoded and kept for one second. When a client that polls at 10Hz
 * asks for an update, it does not get "the current state" — it gets the newest
 * state PLUS the intermediate 30Hz sample points it missed. Its interpolator
 * can then trace the ball's actual curved path instead of drawing a straight
 * chord across it, which is the difference between "laggy but readable" and
 * "the ball teleports".
 */

import {
  C,
  MatchState as Phase,
  EventKind,
  MatchFlag,
  SnapFlag,
  TransportKind,
  encodeSNAPSHOT,
  encodeEVENT,
  encodeMATCH_START,
  padBytes,
} from '../../../shared/gen/protocol.js';
import {
  createMatch,
  stepMatch,
  FIELD_H_Q,
  PADDLE_H_Q,
  PADDLE_W_Q,
  BALL_R_Q,
  type MatchState,
  type PlayerInput,
} from '../../../shared/sim/pong.js';
import type { Session, SnapshotSource } from '../net/session.js';
import { Bot } from '../../../shared/sim/bot.js';

interface RingEntry {
  tick: number;
  frame: Uint8Array;
}

let nextMatchId = 1;

export class Room implements SnapshotSource {
  readonly code: string;
  readonly createdMs: number;

  /** Index 0 = left seat, 1 = right seat. */
  seats: (Session | null)[] = [null, null];
  bot: Bot | null = null;

  state: MatchState;
  matchId = 0;

  private ring: RingEntry[] = [];
  private ringHead = 0;
  private seq = 0;
  private started = false;

  constructor(code: string, nowMs: number, seed: number) {
    this.code = code;
    this.createdMs = nowMs;
    this.state = createMatch(seed);
    this.ring = new Array(C.SNAP_RING);
  }

  get newestTick(): number {
    return this.state.tick;
  }

  get occupancy(): number {
    return (this.seats[0] ? 1 : 0) + (this.seats[1] ? 1 : 0);
  }

  get isFull(): boolean {
    return this.occupancy >= 2 || (this.occupancy === 1 && this.bot !== null);
  }

  get isEmpty(): boolean {
    return this.occupancy === 0;
  }

  sideOf(s: Session): number {
    return this.seats[0] === s ? 0 : 1;
  }

  /* ------------------------------------------------------------------ seats */

  seat(s: Session): number | null {
    const idx = this.seats[0] === null ? 0 : this.seats[1] === null ? 1 : null;
    if (idx === null) return null;
    this.seats[idx] = s;
    s.snapshots = this;
    s.ackTick = Math.max(0, this.state.tick - 1);
    s.sentTick = s.ackTick;
    s.targetYQ4 = FIELD_H_Q >> 1;
    return idx;
  }

  addBot(difficulty = 0.82): void {
    if (this.seats[1] === null) this.bot = new Bot(1, difficulty);
    else if (this.seats[0] === null) this.bot = new Bot(0, difficulty);
  }

  remove(s: Session): void {
    const i = this.sideOf(s);
    if (this.seats[i] === s) {
      this.seats[i] = null;
      s.snapshots = null;
      const other = this.seats[1 - i];
      if (other) {
        other.push(encodeEVENT(
          { tick: this.state.tick, kind: EventKind.OPP_LEFT, a: i, b: 0, c: 0 },
          other.nextSeq(),
        ));
      }
    }
  }

  /**
   * Slow mode: when either player is on a ~10Hz transport, lower the ball's
   * speed ceiling for BOTH of them.
   *
   * A 220ms render delay is a much larger fraction of a field traverse at full
   * speed than at reduced speed. This is a fairness lever, not a netcode one —
   * the alternative is a browser player on WebSocket simply out-reacting a 3DS
   * on HTTPS every rally.
   */
  private computeSpeedCeiling(): number {
    const slow = this.seats.some(
      (s) => s !== null && (s.sink?.kind === TransportKind.LONGPOLL || s.sink?.kind === TransportKind.RR),
    );
    return slow ? C.BALL_SPEED_MAX_SLOW_Q4 : C.BALL_SPEED_MAX_Q4;
  }

  /** Announces the match to both seats once they are filled. */
  start(): void {
    if (this.started || !this.isFull) return;
    this.started = true;
    this.matchId = nextMatchId++;
    this.state.speedMaxQ4 = this.computeSpeedCeiling();
    const slow = this.state.speedMaxQ4 === C.BALL_SPEED_MAX_SLOW_Q4;

    for (let i = 0; i < 2; i++) {
      const s = this.seats[i];
      if (!s) continue;
      const opp = this.seats[1 - i];
      s.push(encodeMATCH_START({
        matchId: this.matchId,
        yourSide: i,
        oppPlatform: opp ? opp.platform : 0,
        paddleHQ4: PADDLE_H_Q,
        paddleWQ4: PADDLE_W_Q,
        ballRQ4: BALL_R_Q,
        maxPaddleSpdQ4: C.MAX_PADDLE_SPEED_Q4,
        winScore: this.state.winScore,
        matchFlags: slow ? MatchFlag.SLOW_MODE : 0,
        oppName: padBytes(opp ? opp.name : this.bot ? 'CPU' : '', C.NAME_BYTES),
      }, s.nextSeq()));

      s.push(encodeEVENT(
        { tick: this.state.tick, kind: EventKind.COUNTDOWN_START, a: 0, b: 0, c: 0 },
        s.nextSeq(),
      ));
    }
  }

  /* ------------------------------------------------------------------- tick */

  step(): void {
    if (!this.started) return;

    const inputs: PlayerInput[] = [
      { targetYQ4: FIELD_H_Q >> 1, buttons: 0 },
      { targetYQ4: FIELD_H_Q >> 1, buttons: 0 },
    ];

    for (let i = 0; i < 2; i++) {
      const s = this.seats[i];
      if (s) {
        inputs[i] = { targetYQ4: s.targetYQ4, buttons: s.buttons };
      } else if (this.bot && this.bot.side === i) {
        inputs[i] = this.bot.think(this.state);
      }
    }

    const events = stepMatch(this.state, inputs[0] as PlayerInput, inputs[1] as PlayerInput);

    this.record();

    if (events.length > 0) {
      for (const ev of events) {
        // Wall and paddle bounces are cosmetic (sound, particles) and arrive at
        // up to 30Hz; sending them reliably would bloat the queue for no gain.
        // Goals and match end change state the client cannot re-derive.
        const cosmetic = ev.kind === EventKind.WALL_HIT || ev.kind === EventKind.PADDLE_HIT;
        if (cosmetic) continue;
        for (const s of this.seats) {
          if (s) {
            s.push(encodeEVENT(
              { tick: this.state.tick, kind: ev.kind, a: ev.a, b: ev.b, c: ev.c },
              s.nextSeq(),
            ));
          }
        }
      }
    }
  }

  /** Encodes this tick's snapshot once and stores it for everyone to pull. */
  private record(): void {
    const st = this.state;
    const bothHere = this.occupancy === 2 || (this.occupancy === 1 && this.bot !== null);
    let flags = 0;
    if (bothHere) flags |= SnapFlag.OPP_CONNECTED;
    if (st.speedMaxQ4 === C.BALL_SPEED_MAX_SLOW_Q4) flags |= SnapFlag.SLOW_MODE;
    // A keyframe marks a discontinuity the client must snap to rather than
    // smooth through: a serve, a goal, the end of a countdown.
    if (st.state !== Phase.PLAY || st.tick === st.phaseUntil) flags |= SnapFlag.KEYFRAME;

    const frame = encodeSNAPSHOT({
      tick: st.tick,
      // ackInputSeq is per-client, but both seats read the same ring. The
      // client matches on its own seq from the INPUT it sent, and a shared
      // value here would be wrong for one of them — so send 0 and let the
      // per-session PONG carry acknowledgement instead.
      ackInputSeq: 0,
      ballXQ4: st.ballXQ4,
      ballYQ4: st.ballYQ4,
      ballVXQ4: st.ballVXQ4,
      ballVYQ4: st.ballVYQ4,
      leftYQ4: st.leftYQ4,
      rightYQ4: st.rightYQ4,
      scoreL: st.scoreL,
      scoreR: st.scoreR,
      state: st.state,
      flags,
    }, this.seq = (this.seq + 1) & 0xffff);

    this.ring[this.ringHead] = { tick: st.tick, frame };
    this.ringHead = (this.ringHead + 1) % C.SNAP_RING;
  }

  /* --------------------------------------------------------- SnapshotSource */

  /**
   * Snapshots newer than `sinceTick`, decimated to every `everyNTicks`, at most
   * `max`.
   *
   * `includeNewest` force-appends the current tick even when decimation would
   * skip it. That is exactly right for a PULL client -- a poller must always
   * receive current truth, whatever the parity of the tick it lands on -- and
   * exactly wrong for a push client, which drains every tick and would
   * therefore receive the decimated frame AND the newest one, doubling its
   * traffic and silently turning a 30Hz stream into 60Hz.
   *
   * When the cap truncates, the NEWEST frames are kept: a client that fell far
   * behind wants current truth plus recent history, not ancient history.
   */
  snapshotsSince(sinceTick: number, everyNTicks: number, max: number, includeNewest = false): Uint8Array[] {
    if (max <= 0) return [];
    const step = Math.max(1, everyNTicks | 0);
    const picked: RingEntry[] = [];

    // Walk oldest -> newest.
    for (let i = 0; i < C.SNAP_RING; i++) {
      const e = this.ring[(this.ringHead + i) % C.SNAP_RING];
      if (!e) continue;
      if (e.tick <= sinceTick) continue;
      if (e.tick % step !== 0) continue;
      picked.push(e);
    }

    if (includeNewest) {
      const newest = this.ring[(this.ringHead + C.SNAP_RING - 1) % C.SNAP_RING];
      if (newest && newest.tick > sinceTick && !picked.some((p) => p.tick === newest.tick)) {
        picked.push(newest);
      }
    }

    const kept = picked.length > max ? picked.slice(picked.length - max) : picked;
    return kept.map((e) => e.frame);
  }
}
