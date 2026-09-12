/**
 * Paddle motion, shared verbatim by the server and every client.
 *
 * This function is the contract that makes local prediction exact. The server
 * runs it authoritatively each tick; the browser and the 3DS run it locally to
 * draw your own paddle immediately. Because the input is an ABSOLUTE target and
 * the clamp is deterministic, both arrive at the same position from the same
 * starting point -- so your own paddle never rubber-bands, on any transport, at
 * any latency.
 *
 * If this ever diverges between server and client, the symptom is subtle and
 * maddening: a paddle that drifts a few pixels and snaps back under load. Hence
 * one copy, imported by both, and mirrored in C by pong_step_paddle().
 */

import { C } from '../gen/protocol.js';

export const FIELD_H_Q4 = C.FIELD_H << C.Q4_SHIFT;
export const PADDLE_HALF_Q4 = (C.PADDLE_H << C.Q4_SHIFT) >> 1;

export function clampQ4(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

/** Moves `currentQ4` toward `targetQ4`, capped at the per-tick speed limit. */
export function stepPaddle(currentQ4: number, targetQ4: number): number {
  const lo = PADDLE_HALF_Q4;
  const hi = FIELD_H_Q4 - PADDLE_HALF_Q4;
  const target = clampQ4(targetQ4 | 0, lo, hi);
  const delta = clampQ4(target - currentQ4, -C.MAX_PADDLE_SPEED_Q4, C.MAX_PADDLE_SPEED_Q4);
  return clampQ4(currentQ4 + delta, lo, hi);
}

/**
 * Advances a paddle by a fractional number of ticks.
 *
 * Clients render at 60fps but the tick boundary rarely lines up with a frame,
 * so prediction needs sub-tick resolution. Whole ticks are stepped exactly;
 * the remainder is applied as a partial move, which keeps the result within a
 * fraction of a Q4 unit of what the server will compute.
 */
export function predictPaddle(currentQ4: number, targetQ4: number, ticks: number): number {
  let y = currentQ4;
  const whole = Math.floor(ticks);
  for (let i = 0; i < Math.min(whole, 64); i++) y = stepPaddle(y, targetQ4);
  const frac = ticks - whole;
  if (frac > 0) {
    const lo = PADDLE_HALF_Q4;
    const hi = FIELD_H_Q4 - PADDLE_HALF_Q4;
    const target = clampQ4(targetQ4 | 0, lo, hi);
    const delta = clampQ4(target - y, -C.MAX_PADDLE_SPEED_Q4 * frac, C.MAX_PADDLE_SPEED_Q4 * frac);
    y = clampQ4(y + delta, lo, hi);
  }
  return y;
}
