/**
 * Snapshot history and the interpolator.
 *
 * The server sends state stamped with a TICK, not a wall-clock time. Because
 * the simulation runs at a fixed 60Hz and never skips, tick is an exact clock:
 * `serverMs = tick * 16.667`. That removes a whole class of bugs where a
 * timestamp and a tick count disagree, and it means a client only has to
 * estimate one number -- the offset between its own clock and the server's
 * tick counter.
 *
 * Rendering happens in the PAST, at `now - renderDelay`, so there are almost
 * always two real snapshots bracketing the moment being drawn and the ball
 * follows its true path. The alternative -- render the newest state and
 * extrapolate forward -- looks sharper for a frame and then visibly snaps every
 * time the prediction is wrong, which on a paddle hit is always.
 */

import { C } from '../../../shared/gen/protocol';

export const TICK_MS = 1000 / C.TICK_HZ;

/** One second of 30Hz history is plenty to bracket any sane render delay. */
const RING = 48;

export interface Snap {
  tick: number;
  ballX: number;
  ballY: number;
  ballVX: number;
  ballVY: number;
  leftY: number;
  rightY: number;
  scoreL: number;
  scoreR: number;
  state: number;
  flags: number;
  /** Local arrival time, for measuring the cadence we are actually getting. */
  arrivedAt: number;
}

export interface View {
  ballX: number;
  ballY: number;
  leftY: number;
  rightY: number;
  scoreL: number;
  scoreR: number;
  state: number;
  flags: number;
  /** True when the interpolator ran dry and had to extrapolate. */
  starved: boolean;
}

function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

export class SnapshotRing {
  private buf: (Snap | null)[] = new Array(RING).fill(null);
  private head = 0;
  private count = 0;

  /** Inter-arrival gaps, used to size the render delay to reality. */
  private gaps: number[] = [];
  private lastArrival = 0;

  newestTick = 0;

  push(s: Snap): void {
    // Out-of-order or duplicate snapshots are discarded: every transport can
    // deliver them, and inserting one behind the newest would make the
    // interpolator walk backwards.
    if (s.tick <= this.newestTick && this.count > 0) return;

    if (this.lastArrival > 0) {
      this.gaps.push(s.arrivedAt - this.lastArrival);
      if (this.gaps.length > 32) this.gaps.shift();
    }
    this.lastArrival = s.arrivedAt;

    this.buf[this.head] = s;
    this.head = (this.head + 1) % RING;
    if (this.count < RING) this.count++;
    this.newestTick = s.tick;
  }

  get size(): number {
    return this.count;
  }

  /** Oldest-to-newest iteration over what we hold. */
  private *entries(): Generator<Snap> {
    for (let i = 0; i < this.count; i++) {
      const idx = (this.head - this.count + i + RING * 2) % RING;
      const s = this.buf[idx];
      if (s) yield s;
    }
  }

  newest(): Snap | null {
    if (this.count === 0) return null;
    return this.buf[(this.head - 1 + RING) % RING] ?? null;
  }

  oldest(): Snap | null {
    if (this.count === 0) return null;
    return this.buf[(this.head - this.count + RING * 2) % RING] ?? null;
  }

  /**
   * How far behind live to render, in milliseconds.
   *
   * Derived from the arrival cadence we are ACTUALLY seeing rather than from
   * the transport's nominal rate. A long-poll client receives batched 30Hz
   * snapshots but receives them in bursts ten times a second, so its buffer has
   * to absorb ~100ms of burst spacing, not ~33ms of sample spacing. Measuring
   * gets that right without the interpolator needing to know which transport it
   * is sitting on.
   */
  renderDelayMs(): number {
    if (this.gaps.length < 4) return 100;
    const sorted = [...this.gaps].sort((a, b) => a - b);
    const median = sorted[Math.floor(sorted.length / 2)] ?? 33;
    return Math.max(50, Math.min(250, median * 2 + 20));
  }

  /** Median arrival gap, exposed for the HUD. */
  arrivalGapMs(): number {
    if (this.gaps.length === 0) return 0;
    const sorted = [...this.gaps].sort((a, b) => a - b);
    return Math.round(sorted[Math.floor(sorted.length / 2)] ?? 0);
  }

  /**
   * Samples the world at a given server tick (fractional).
   *
   * The cursor is clamped to the range actually held, which is what keeps this
   * stable when the clock estimate is briefly wrong: a bad estimate degrades to
   * "slightly stale" rather than to an empty view or a stutter.
   */
  sample(atTick: number): View | null {
    const newest = this.newest();
    const oldest = this.oldest();
    if (!newest || !oldest) return null;

    const cursor = Math.max(oldest.tick, Math.min(newest.tick, atTick));
    const starved = atTick > newest.tick + 0.5;

    let a: Snap | null = null;
    let b: Snap | null = null;
    for (const s of this.entries()) {
      if (s.tick <= cursor) a = s;
      if (s.tick >= cursor && b === null) { b = s; break; }
    }
    if (!a) a = oldest;
    if (!b) b = newest;

    const span = b.tick - a.tick;
    const t = span > 0 ? (cursor - a.tick) / span : 0;

    // A keyframe marks a discontinuity -- a serve, a goal, the end of a
    // countdown. Interpolating across one would drag the ball smoothly from
    // where it died to where it was re-served, which looks like a glitch.
    const crossesKeyframe = (b.flags & C.SNAP_RING * 0 + 1) !== 0 && span > 0;

    if (crossesKeyframe) {
      return {
        ballX: b.ballX, ballY: b.ballY,
        leftY: lerp(a.leftY, b.leftY, t),
        rightY: lerp(a.rightY, b.rightY, t),
        scoreL: b.scoreL, scoreR: b.scoreR,
        state: b.state, flags: b.flags, starved,
      };
    }

    return {
      ballX: lerp(a.ballX, b.ballX, t),
      ballY: lerp(a.ballY, b.ballY, t),
      leftY: lerp(a.leftY, b.leftY, t),
      rightY: lerp(a.rightY, b.rightY, t),
      scoreL: b.scoreL,
      scoreR: b.scoreR,
      state: b.state,
      flags: b.flags,
      starved,
    };
  }

  clear(): void {
    this.buf.fill(null);
    this.head = 0;
    this.count = 0;
    this.gaps.length = 0;
    this.lastArrival = 0;
    this.newestTick = 0;
  }
}

/**
 * Estimates the server's tick counter in local time.
 *
 * Uses the MINIMUM round-trip sample rather than an average: the least-queued
 * exchange is the one least distorted by buffering, and a mean is dragged
 * around by exactly the jitter we are trying to see through.
 */
export class Clock {
  private samples: { rtt: number; offset: number }[] = [];
  private offsetTicks = 0;
  private haveOffset = false;
  minRtt = 0;

  /** Feed a PONG. `serverTick` is the server's tick at the time it replied. */
  sample(sentAtMs: number, serverTimeMs: number, serverTick: number, nowMs: number): void {
    // Plain subtraction. Do NOT "harden" this with `>>> 0` or other 32-bit
    // integer coercion: these are Date.now() milliseconds (~1.7e12), far beyond
    // 2^32, so truncating them produces a garbage RTT rather than guarding
    // against one. web/test/clock.test.ts fails if this is reintroduced.
    const rtt = nowMs - sentAtMs;

    // Discard impossible samples instead of letting one poison the estimator.
    // A negative value means the clock moved backwards; a huge one means the
    // reply was queued so long it says nothing useful about latency.
    if (!Number.isFinite(rtt) || rtt < 0 || rtt > 5000) return;

    // Where the server's tick counter stood when the reply reached us.
    const serverTickNow = serverTick + rtt / 2 / TICK_MS;
    const offset = serverTickNow - nowMs / TICK_MS;

    this.samples.push({ rtt, offset });
    if (this.samples.length > 16) this.samples.shift();

    let best = this.samples[0] as { rtt: number; offset: number };
    for (const s of this.samples) if (s.rtt < best.rtt) best = s;
    this.minRtt = Math.round(best.rtt);

    if (!this.haveOffset) {
      this.offsetTicks = best.offset;
      this.haveOffset = true;
    } else {
      // Ease toward the new estimate. A jump would make the render cursor leap,
      // which is visible as a stutter even when the new estimate is better.
      const delta = best.offset - this.offsetTicks;
      this.offsetTicks += Math.max(-0.06, Math.min(0.06, delta));
    }
    void serverTimeMs;
  }

  /** Estimated server tick right now (fractional). */
  serverTick(nowMs: number): number {
    return nowMs / TICK_MS + this.offsetTicks;
  }

  get ready(): boolean {
    return this.haveOffset;
  }
}
