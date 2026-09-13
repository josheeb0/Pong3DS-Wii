/**
 * A practice opponent.
 *
 * Exists so a lone player is never stuck staring at a matchmaking spinner --
 * which, on a project whose whole point is cross-play, is the difference
 * between "broken" and "waiting". Also drives the soak test.
 *
 * Deliberately imperfect: it tracks the ball only while the ball is coming
 * toward it, reacts after a delay, and aims at a slightly wrong spot. A
 * pixel-perfect bot would be unbeatable, since the paddle speed clamp applies
 * equally to both sides.
 */

import { FIELD_H_Q, PADDLE_HALF_Q, type MatchState, type PlayerInput } from './pong.js';

export class Bot {
  readonly side: number;
  /** 0..1; higher tracks more tightly and reacts sooner. */
  private skill: number;
  private target: number = FIELD_H_Q >> 1;
  private cooldown = 0;
  private rng: number;

  constructor(side: number, skill = 0.82) {
    this.side = side;
    this.skill = Math.min(1, Math.max(0, skill));
    this.rng = 0x2545f491 ^ (side + 1);
  }

  private nextRng(): number {
    let s = this.rng;
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5;  s >>>= 0;
    this.rng = s || 0x1d872b41;
    return this.rng;
  }

  think(st: MatchState): PlayerInput {
    const approaching = this.side === 0 ? st.ballVXQ4 < 0 : st.ballVXQ4 > 0;

    if (this.cooldown > 0) {
      this.cooldown--;
    } else if (approaching) {
      // Re-aim periodically rather than every tick, so it visibly "reacts".
      const jitterRange = Math.round((1 - this.skill) * PADDLE_HALF_Q * 2);
      const jitter = jitterRange > 0 ? (this.nextRng() % (jitterRange * 2)) - jitterRange : 0;
      this.target = st.ballYQ4 + jitter;
      this.cooldown = Math.round(2 + (1 - this.skill) * 14);
    } else {
      // Drift back toward centre when the ball is away, like a real player.
      this.target = this.target + ((FIELD_H_Q >> 1) - this.target) / 32;
    }

    const lo = PADDLE_HALF_Q;
    const hi = FIELD_H_Q - PADDLE_HALF_Q;
    const clamped = Math.round(Math.min(hi, Math.max(lo, this.target)));
    return { targetYQ4: clamped, buttons: 0 };
  }
}
