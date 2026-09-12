import { describe, it, expect } from 'vitest';
import { Clock, SnapshotRing, TICK_MS } from '../src/game/interp';

/**
 * Regression tests for the clock estimator.
 *
 * The RTT calculation has twice been "fixed" by coercing to 32-bit unsigned
 * integers: `((nowMs >>> 0) - sentAtMs) >>> 0`. For normal values that is
 * harmless -- the modular arithmetic cancels and it returns the right answer,
 * which is exactly why it survives review.
 *
 * What it actually breaks is the edge case it claims to fix. A negative RTT
 * (the clock moved backwards) does not get rejected; it WRAPS to ~4.29e9 and
 * enters the sample window as a plausible-looking number. The guard is the
 * thing doing the work, not the arithmetic.
 *
 * So these tests assert on rejection, not on the value.
 */
describe('Clock RTT', () => {
  // Realistic epoch milliseconds, well beyond 2^32 (4.29e9).
  const NOW = 1_757_700_000_000;

  it('measures a plain round trip at realistic epoch timestamps', () => {
    const c = new Clock();
    c.sample(NOW - 80, 0, 1000, NOW);
    expect(c.minRtt).toBe(80);
  });

  it('survives values far beyond 2^32 (the 32-bit truncation trap)', () => {
    const c = new Clock();
    expect(NOW).toBeGreaterThan(2 ** 32);
    for (const rtt of [1, 25, 150, 999]) {
      const fresh = new Clock();
      fresh.sample(NOW - rtt, 0, 1000, NOW);
      // `((NOW >>> 0) - (NOW - rtt)) >>> 0` yields a huge bogus number here.
      expect(fresh.minRtt).toBe(rtt);
      expect(fresh.minRtt).toBeLessThan(5000);
    }
  });

  it('keeps the MINIMUM sample, not the latest or the mean', () => {
    const c = new Clock();
    // Each sample's sentAt is relative to the nowMs of THAT exchange, which is
    // what a real client does -- the first version of this test got that wrong
    // and measured 140ms for the "clean" sample.
    const exchange = (at: number, rtt: number, tick: number) =>
      c.sample(at - rtt, 0, tick, at);

    exchange(NOW, 300, 1000);        // congested
    exchange(NOW + 100, 40, 1010);   // clean
    exchange(NOW + 200, 250, 1020);  // congested again

    expect(c.minRtt).toBe(40);
  });

  it('discards impossible samples rather than letting them poison it', () => {
    const c = new Clock();
    c.sample(NOW - 50, 0, 1000, NOW);
    expect(c.minRtt).toBe(50);

    c.sample(NOW + 1000, 0, 1000, NOW);   // negative: clock went backwards
    c.sample(NOW - 60_000, 0, 1000, NOW); // absurd: 60s
    expect(c.minRtt).toBe(50);
  });

  it('does NOT lock on when every sample is impossible', () => {
    // The load-bearing assertion. Wrapping a negative RTT to ~4.29e9 with
    // `>>> 0` produces a number that looks plausible enough to be accepted,
    // and the clock then locks onto a garbage offset. Rejecting it means the
    // clock correctly reports that it still has no idea what time it is.
    const c = new Clock();
    for (let i = 0; i < 8; i++) {
      c.sample(NOW + 1000 + i, 0, 1000, NOW); // every one is negative
    }
    expect(c.ready).toBe(false);
    expect(c.minRtt).toBe(0);
  });

  it('rejects a non-finite sample', () => {
    const c = new Clock();
    c.sample(Number.NaN, 0, 1000, NOW);
    expect(c.ready).toBe(false);
  });

  it('locks on and produces a forward-moving tick estimate', () => {
    const c = new Clock();
    expect(c.ready).toBe(false);
    c.sample(NOW - 20, 0, 6000, NOW);
    expect(c.ready).toBe(true);

    const t0 = c.serverTick(NOW);
    const t1 = c.serverTick(NOW + 1000);
    // One second of wall clock must advance the estimate by ~one second of ticks.
    expect(t1 - t0).toBeCloseTo(1000 / TICK_MS, 1);
    expect(t0).toBeGreaterThan(0);
  });
});

describe('SnapshotRing render delay', () => {
  const snap = (tick: number, arrivedAt: number) => ({
    tick, ballX: 0, ballY: 0, ballVX: 0, ballVY: 0,
    leftY: 0, rightY: 0, scoreL: 0, scoreR: 0, state: 2, flags: 0, arrivedAt,
  });

  it('sizes the buffer from the cadence actually observed', () => {
    // A push transport arriving every ~33ms should buffer far less than a
    // poller arriving in ~100ms bursts. This is the whole reason the delay is
    // measured rather than hardcoded per transport.
    const fast = new SnapshotRing();
    for (let i = 1; i <= 10; i++) fast.push(snap(i * 2, i * 33));
    const slow = new SnapshotRing();
    for (let i = 1; i <= 10; i++) slow.push(snap(i * 2, i * 100));

    expect(fast.renderDelayMs()).toBeLessThan(slow.renderDelayMs());
    expect(fast.renderDelayMs()).toBeGreaterThanOrEqual(50);
    expect(slow.renderDelayMs()).toBeLessThanOrEqual(250);
  });

  it('ignores duplicate and out-of-order snapshots', () => {
    const r = new SnapshotRing();
    r.push(snap(10, 1000));
    r.push(snap(8, 1010));   // older
    r.push(snap(10, 1020));  // duplicate
    expect(r.newestTick).toBe(10);
    expect(r.size).toBe(1);
  });
});
