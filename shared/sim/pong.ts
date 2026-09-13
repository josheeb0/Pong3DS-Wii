/**
 * The authoritative Pong simulation.
 *
 * Three properties this file is built around, in order of importance:
 *
 * 1. **Pure.** `stepMatch` takes state and inputs and returns new state plus
 *    events. No clocks, no I/O, no `Math.random`. That is what makes it
 *    directly unit-testable and what lets a determinism test hash 1000 ticks.
 *
 * 2. **Integer-only.** Every quantity is Q4 fixed point (1/16 px) held in a
 *    JS number that never leaves the integer range. Floats would be fine for a
 *    server talking to itself, but the 3DS client re-simulates ball motion to
 *    extrapolate between snapshots, and `double` on V8 versus `float` on ARM11
 *    would drift apart. Integers cannot.
 *
 * 3. **Seeded.** Serve direction comes from an xorshift32 carried inside the
 *    state, so a match replays identically from its opening state.
 *
 * Coordinates: origin top-left, +Y down, field 800x480 px == 12800x7680 Q4.
 * Paddle and ball positions are CENTRES.
 */

import { C } from '../gen/protocol.js';
import { SIN_Q12, COS_Q12 } from '../gen/trig.js';
import { stepPaddle as sharedStepPaddle } from './paddle.js';

/* ------------------------------------------------------------------ derived */

export const Q = C.Q4_SHIFT;
export const FIELD_W_Q = C.FIELD_W << Q;
export const FIELD_H_Q = C.FIELD_H << Q;
export const PADDLE_W_Q = C.PADDLE_W << Q;
export const PADDLE_H_Q = C.PADDLE_H << Q;
export const PADDLE_HALF_Q = PADDLE_H_Q >> 1;
export const BALL_R_Q = C.BALL_R << Q;
export const PADDLE_X_L_Q = C.PADDLE_X_L << Q;
export const PADDLE_X_R_Q = C.PADDLE_X_R << Q;

/** Half-span of the contact zone: how far off-centre a hit can land. */
const CONTACT_SPAN_Q = PADDLE_HALF_Q + BALL_R_Q;

export const enum Side {
  Left = 0,
  Right = 1,
}

export interface PlayerInput {
  /** Absolute desired paddle CENTRE in Q4. Clamped by the server. */
  targetYQ4: number;
  buttons: number;
}

export interface MatchState {
  tick: number;
  /** One of C.MatchState / the MatchState enum in the protocol. */
  state: number;
  ballXQ4: number;
  ballYQ4: number;
  ballVXQ4: number;
  ballVYQ4: number;
  leftYQ4: number;
  rightYQ4: number;
  scoreL: number;
  scoreR: number;
  /** xorshift32 state; never zero. */
  rng: number;
  /** Tick at which the current COUNTDOWN / GOAL_FREEZE phase ends. */
  phaseUntil: number;
  /** Ball speed ceiling; lowered when a player is on a slow transport. */
  speedMaxQ4: number;
  /** Paddle hits this rally, for the speed ramp. */
  rallyHits: number;
  winScore: number;
}

export interface SimEvent {
  kind: number;
  a: number;
  b: number;
  c: number;
}

/* ---------------------------------------------------------------- utilities */

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

/** xorshift32 — small, fast, and identical everywhere. */
function nextRng(s: number): number {
  s ^= s << 13;
  s >>>= 0;
  s ^= s >>> 17;
  s ^= s << 5;
  s >>>= 0;
  return s === 0 ? 0x1d872b41 : s;
}

/** Ball speed magnitude, Q4. Integer sqrt keeps this deterministic. */
export function ballSpeedQ4(st: MatchState): number {
  const vx = st.ballVXQ4;
  const vy = st.ballVYQ4;
  return isqrt(vx * vx + vy * vy);
}

/** Integer square root (Newton), exact for our range. */
export function isqrt(n: number): number {
  if (n <= 0) return 0;
  let x = n;
  let y = (x + 1) >> 1;
  while (y < x) {
    x = y;
    y = ((x + Math.floor(n / x)) >> 1) | 0;
  }
  return x;
}

/**
 * Sets ball velocity from a bounce angle index and a speed, using the shared
 * integer trig table. `idx` is 0..TRIG_STEPS over 0..90 degrees; `dirX` is the
 * horizontal direction the ball leaves in.
 */
function setVelocity(st: MatchState, idx: number, speedQ4: number, dirX: number, dirY: number): void {
  const i = clamp(Math.abs(idx) | 0, 0, 64);
  const cos = COS_Q12[i] as number;
  const sin = SIN_Q12[i] as number;
  st.ballVXQ4 = (dirX * ((speedQ4 * cos) >> 12)) | 0;
  st.ballVYQ4 = (dirY * ((speedQ4 * sin) >> 12)) | 0;
  // A perfectly horizontal rally is dull and a perfectly vertical one is stuck;
  // nudge vx so the ball always makes progress across the field.
  if (st.ballVXQ4 === 0) st.ballVXQ4 = dirX * 8;
}

/* ------------------------------------------------------------------- serving */

export function createMatch(
  seed: number,
  winScore: number = C.WIN_SCORE,
  speedMaxQ4: number = C.BALL_SPEED_MAX_Q4,
): MatchState {
  const st: MatchState = {
    tick: 0,
    state: 1 /* COUNTDOWN */,
    ballXQ4: FIELD_W_Q >> 1,
    ballYQ4: FIELD_H_Q >> 1,
    ballVXQ4: 0,
    ballVYQ4: 0,
    leftYQ4: FIELD_H_Q >> 1,
    rightYQ4: FIELD_H_Q >> 1,
    scoreL: 0,
    scoreR: 0,
    rng: seed >>> 0 || 0x9e3779b9,
    phaseUntil: C.COUNTDOWN_TICKS,
    speedMaxQ4,
    rallyHits: 0,
    winScore,
  };
  return st;
}

/** Places the ball at centre with a fresh serve velocity toward `dirX`. */
export function serve(st: MatchState, dirX: number): void {
  st.ballXQ4 = FIELD_W_Q >> 1;
  st.ballYQ4 = FIELD_H_Q >> 1;
  st.rallyHits = 0;

  st.rng = nextRng(st.rng);
  // Serve angle within +/- 30 degrees so the opening is always returnable.
  const idx = (st.rng % 22) | 0;
  const dirY = st.rng & 0x10000 ? 1 : -1;
  setVelocity(st, idx, C.BALL_SPEED_START_Q4, dirX, dirY);
}

/* ----------------------------------------------------------------- collision */

/**
 * Circle-vs-box overlap, treating the ball as a box. Exact enough for Pong and
 * entirely integer.
 *
 * Tunnelling is impossible by construction: the per-tick ball step is capped at
 * BALL_SPEED_MAX_Q4 (176) which is below the paddle width (192), so the ball
 * can never step across a paddle without a tick landing inside it. The host
 * test asserts that inequality so a future speed increase cannot silently break
 * it.
 */
function overlapsPaddle(ballX: number, ballY: number, padX: number, padY: number): boolean {
  const dx = Math.abs(ballX - padX);
  if (dx > (PADDLE_W_Q >> 1) + BALL_R_Q) return false;
  const dy = Math.abs(ballY - padY);
  if (dy > CONTACT_SPAN_Q) return false;
  return true;
}

/**
 * Reflects the ball off a paddle.
 *
 * ---------------------------------------------------------------------------
 * DESIGN CHOICE — this function defines how the game feels, and it is the one
 * place in the simulation where there is no single correct answer.
 *
 * The default implemented here is classic-Atari: the outgoing ANGLE is a pure
 * function of where the ball struck the paddle, and speed ramps a fixed step
 * per hit regardless of contact point. Hit centre, the ball goes flat and fast;
 * hit the edge, it leaves at up to 53 degrees. Paddle motion is ignored.
 *
 * Two alternatives worth considering, both a small edit here:
 *
 *   - Velocity english: add a fraction of the paddle's own velocity to the
 *     outgoing vy, so dragging the paddle through the ball curves the return.
 *     Rewards deliberate movement, punishes camping, and is much harder for a
 *     laggy player to control.
 *
 *   - Contact-weighted speed: ramp speed more on centre hits than edge hits,
 *     so precision is rewarded with pace rather than with angle.
 *
 * The classic default is deliberately the most forgiving of latency, which
 * matters because one player may be on a 200ms HTTP path.
 * ---------------------------------------------------------------------------
 */
export function applyPaddleBounce(
  st: MatchState,
  padY: number,
  dirX: number,
  _padVelQ4: number,
): void {
  const offset = clamp(st.ballYQ4 - padY, -CONTACT_SPAN_Q, CONTACT_SPAN_Q);

  // Map contact offset onto the angle table. Integer throughout: a rounding
  // difference here would desync a client's prediction from the server.
  const idx = Math.trunc((offset * C.BALL_MAX_ANGLE_STEPS) / CONTACT_SPAN_Q);
  const dirY = offset === 0 ? (st.ballVYQ4 >= 0 ? 1 : -1) : offset > 0 ? 1 : -1;

  st.rallyHits++;
  const speed = Math.min(
    ballSpeedQ4(st) + C.BALL_SPEED_STEP_Q4,
    st.speedMaxQ4,
  );

  setVelocity(st, idx, speed, dirX, dirY);
}

/* --------------------------------------------------------------------- step */

/**
 * Advances the match by exactly one tick.
 *
 * Paddles are stepped toward their targets on EVERY tick whether or not fresh
 * input arrived. That is what makes a 10Hz HTTP client feel continuous rather
 * than steppy: between its updates the server keeps gliding its paddle toward
 * the last stated target, and because the client applies the identical clamp
 * locally, its own prediction matches the server exactly.
 */
export function stepMatch(
  st: MatchState,
  inL: PlayerInput,
  inR: PlayerInput,
): SimEvent[] {
  const events: SimEvent[] = [];
  st.tick++;

  // --- paddles: always, regardless of phase -------------------------------
  const prevL = st.leftYQ4;
  const prevR = st.rightYQ4;
  st.leftYQ4 = stepPaddle(st.leftYQ4, inL.targetYQ4);
  st.rightYQ4 = stepPaddle(st.rightYQ4, inR.targetYQ4);
  const velL = st.leftYQ4 - prevL;
  const velR = st.rightYQ4 - prevR;

  // --- phase handling ------------------------------------------------------
  if (st.state === 1 /* COUNTDOWN */) {
    if (st.tick >= st.phaseUntil) {
      st.state = 2 /* PLAY */;
      st.rng = nextRng(st.rng);
      serve(st, st.rng & 1 ? 1 : -1);
    }
    return events;
  }

  if (st.state === 3 /* GOAL_FREEZE */) {
    if (st.tick >= st.phaseUntil) {
      st.state = 2 /* PLAY */;
      // Serve toward whoever just conceded, which is the fair convention.
      // The ball is resting past the edge it left through, so a ball on the
      // left half means the LEFT player conceded and is served to.
      serve(st, st.ballXQ4 < FIELD_W_Q >> 1 ? -1 : 1);
    }
    return events;
  }

  if (st.state !== 2 /* PLAY */) return events;

  // --- ball ----------------------------------------------------------------
  st.ballXQ4 += st.ballVXQ4;
  st.ballYQ4 += st.ballVYQ4;

  // Top / bottom walls.
  if (st.ballYQ4 - BALL_R_Q < 0 && st.ballVYQ4 < 0) {
    st.ballYQ4 = BALL_R_Q + (BALL_R_Q - st.ballYQ4);
    st.ballVYQ4 = -st.ballVYQ4;
    events.push({ kind: 7 /* WALL_HIT */, a: 0, b: 0, c: 0 });
  } else if (st.ballYQ4 + BALL_R_Q > FIELD_H_Q && st.ballVYQ4 > 0) {
    const over = st.ballYQ4 + BALL_R_Q - FIELD_H_Q;
    st.ballYQ4 = FIELD_H_Q - BALL_R_Q - over;
    st.ballVYQ4 = -st.ballVYQ4;
    events.push({ kind: 7 /* WALL_HIT */, a: 1, b: 0, c: 0 });
  }

  // Paddles. Only test the paddle the ball is travelling toward, so a ball
  // leaving a paddle cannot immediately re-collide with it.
  if (st.ballVXQ4 < 0 && overlapsPaddle(st.ballXQ4, st.ballYQ4, PADDLE_X_L_Q, st.leftYQ4)) {
    st.ballXQ4 = PADDLE_X_L_Q + (PADDLE_W_Q >> 1) + BALL_R_Q;
    applyPaddleBounce(st, st.leftYQ4, +1, velL);
    events.push({ kind: 6 /* PADDLE_HIT */, a: Side.Left, b: st.rallyHits & 0xff, c: 0 });
  } else if (st.ballVXQ4 > 0 && overlapsPaddle(st.ballXQ4, st.ballYQ4, PADDLE_X_R_Q, st.rightYQ4)) {
    st.ballXQ4 = PADDLE_X_R_Q - (PADDLE_W_Q >> 1) - BALL_R_Q;
    applyPaddleBounce(st, st.rightYQ4, -1, velR);
    events.push({ kind: 6 /* PADDLE_HIT */, a: Side.Right, b: st.rallyHits & 0xff, c: 0 });
  }

  // --- scoring -------------------------------------------------------------
  if (st.ballXQ4 + BALL_R_Q < 0) {
    st.scoreR++;
    events.push({ kind: 1 /* GOAL */, a: Side.Right, b: st.scoreL, c: st.scoreR });
    endPoint(st, events);
  } else if (st.ballXQ4 - BALL_R_Q > FIELD_W_Q) {
    st.scoreL++;
    events.push({ kind: 1 /* GOAL */, a: Side.Left, b: st.scoreL, c: st.scoreR });
    endPoint(st, events);
  }

  return events;
}

function endPoint(st: MatchState, events: SimEvent[]): void {
  st.ballVXQ4 = 0;
  st.ballVYQ4 = 0;
  if (st.scoreL >= st.winScore || st.scoreR >= st.winScore) {
    st.state = 4 /* GAME_OVER */;
    events.push({
      kind: 2 /* MATCH_OVER */,
      a: st.scoreL > st.scoreR ? Side.Left : Side.Right,
      b: st.scoreL,
      c: st.scoreR,
    });
  } else {
    st.state = 3 /* GOAL_FREEZE */;
    st.phaseUntil = st.tick + C.GOAL_FREEZE_TICKS;
  }
}

/**
 * Moves one paddle toward its target, clamped to the speed limit and the field.
 *
 * Re-exported from shared/sim/paddle.ts, which both clients also import. One
 * definition is the point: if this diverged between server and client, your own
 * paddle would rubber-band.
 */
export const stepPaddle = sharedStepPaddle;

/** Compact digest of the visible state, for the determinism test. */
export function digest(st: MatchState): string {
  return [
    st.tick, st.state, st.ballXQ4, st.ballYQ4, st.ballVXQ4, st.ballVYQ4,
    st.leftYQ4, st.rightYQ4, st.scoreL, st.scoreR, st.rng, st.rallyHits,
  ].join(',');
}
