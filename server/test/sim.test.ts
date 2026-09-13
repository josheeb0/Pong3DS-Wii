import { describe, it, expect } from 'vitest';
import { C } from '../../shared/gen/protocol.js';
import {
  createMatch,
  stepMatch,
  stepPaddle,
  serve,
  digest,
  ballSpeedQ4,
  isqrt,
  FIELD_H_Q,
  FIELD_W_Q,
  PADDLE_HALF_Q,
  PADDLE_W_Q,
  BALL_R_Q,
  PADDLE_X_L_Q,
  type MatchState,
  type PlayerInput,
} from '../../shared/sim/pong.js';

const idle: PlayerInput = { targetYQ4: FIELD_H_Q >> 1, buttons: 0 };

/** Runs the match forward, optionally driving each side with a strategy. */
function run(
  st: MatchState,
  ticks: number,
  L: (s: MatchState) => PlayerInput = () => idle,
  R: (s: MatchState) => PlayerInput = () => idle,
) {
  const events = [];
  for (let i = 0; i < ticks; i++) events.push(...stepMatch(st, L(st), R(st)));
  return events;
}

/** A paddle that tracks the ball perfectly — used to sustain long rallies. */
const perfect = (side: 'L' | 'R') => (s: MatchState): PlayerInput => ({
  targetYQ4: s.ballYQ4,
  buttons: 0,
});

describe('integer helpers', () => {
  it('isqrt is exact on perfect squares and floors otherwise', () => {
    expect(isqrt(0)).toBe(0);
    expect(isqrt(1)).toBe(1);
    expect(isqrt(144)).toBe(12);
    expect(isqrt(1 << 20)).toBe(1 << 10);
    expect(isqrt(143)).toBe(11);
    expect(isqrt(145)).toBe(12);
  });
});

describe('determinism', () => {
  it('same seed and inputs produce an identical 1000-tick trace', () => {
    const a = createMatch(12345);
    const b = createMatch(12345);
    const tracesA: string[] = [];
    const tracesB: string[] = [];
    for (let i = 0; i < 1000; i++) {
      stepMatch(a, perfect('L')(a), perfect('R')(a));
      stepMatch(b, perfect('L')(b), perfect('R')(b));
      tracesA.push(digest(a));
      tracesB.push(digest(b));
    }
    expect(tracesA).toEqual(tracesB);
  });

  it('different seeds diverge (the seed actually does something)', () => {
    const a = createMatch(1);
    const b = createMatch(2);
    run(a, 400);
    run(b, 400);
    expect(digest(a)).not.toEqual(digest(b));
  });

  it('every value stays an integer for 2000 ticks', () => {
    const st = createMatch(999);
    for (let i = 0; i < 2000; i++) {
      stepMatch(st, perfect('L')(st), perfect('R')(st));
      for (const [k, v] of Object.entries(st)) {
        expect(Number.isInteger(v), `${k} became non-integer: ${v}`).toBe(true);
      }
    }
  });
});

describe('paddle clamping', () => {
  it('moves at most MAX_PADDLE_SPEED_Q4 per tick', () => {
    const start = FIELD_H_Q >> 1;
    const next = stepPaddle(start, FIELD_H_Q); // ask for the far edge
    expect(next - start).toBe(C.MAX_PADDLE_SPEED_Q4);
  });

  it('takes a predictable number of ticks to cross a known distance', () => {
    // 400px of travel at 12px/tick is exactly 34 ticks (ceil).
    const distancePx = 400;
    let y = PADDLE_HALF_Q;
    const target = y + (distancePx << C.Q4_SHIFT);
    let ticks = 0;
    while (y < target && ticks < 1000) {
      y = stepPaddle(y, target);
      ticks++;
    }
    expect(ticks).toBe(Math.ceil((distancePx << C.Q4_SHIFT) / C.MAX_PADDLE_SPEED_Q4));
  });

  it('never lets a paddle leave the field, even asked to teleport', () => {
    let y = FIELD_H_Q >> 1;
    for (let i = 0; i < 200; i++) y = stepPaddle(y, -999999);
    expect(y).toBe(PADDLE_HALF_Q);
    for (let i = 0; i < 400; i++) y = stepPaddle(y, 999999);
    expect(y).toBe(FIELD_H_Q - PADDLE_HALF_Q);
  });

  it('keeps stepping toward a stale target when no new input arrives', () => {
    // This is what makes a 10Hz client feel continuous rather than steppy.
    const st = createMatch(7);
    const target = (FIELD_H_Q >> 1) + (100 << C.Q4_SHIFT);
    const held: PlayerInput = { targetYQ4: target, buttons: 0 };
    const seen: number[] = [];
    for (let i = 0; i < 6; i++) {
      stepMatch(st, held, idle);
      seen.push(st.leftYQ4);
    }
    // Six distinct positions from one unchanged target.
    expect(new Set(seen).size).toBe(6);
    for (let i = 1; i < seen.length; i++) {
      expect(seen[i]! - seen[i - 1]!).toBe(C.MAX_PADDLE_SPEED_Q4);
    }
  });
});

describe('ball physics', () => {
  it('bounces off the top and bottom walls without escaping', () => {
    const st = createMatch(42);
    run(st, C.COUNTDOWN_TICKS + 1); // get into PLAY
    st.ballVXQ4 = 16;
    st.ballVYQ4 = -C.BALL_SPEED_MAX_Q4;
    for (let i = 0; i < 2000; i++) {
      stepMatch(st, perfect('L')(st), perfect('R')(st));
      if (st.state !== 2) continue;
      expect(st.ballYQ4 + BALL_R_Q).toBeLessThanOrEqual(FIELD_H_Q + 1);
      expect(st.ballYQ4 - BALL_R_Q).toBeGreaterThanOrEqual(-1);
    }
  });

  it('never exceeds the speed ceiling however long the rally', () => {
    const st = createMatch(3);
    run(st, 5000, perfect('L'), perfect('R'));
    expect(ballSpeedQ4(st)).toBeLessThanOrEqual(C.BALL_SPEED_MAX_Q4 + 2);
  });

  it('speeds up on paddle hits', () => {
    const st = createMatch(11);
    run(st, C.COUNTDOWN_TICKS + 1);
    const initial = ballSpeedQ4(st);
    let hits = 0;
    for (let i = 0; i < 3000 && hits < 4; i++) {
      const ev = stepMatch(st, perfect('L')(st), perfect('R')(st));
      hits += ev.filter((e) => e.kind === 6).length;
    }
    expect(hits).toBeGreaterThanOrEqual(4);
    expect(ballSpeedQ4(st)).toBeGreaterThan(initial);
  });

  it('cannot tunnel through a paddle that is in position', () => {
    // The structural guarantee: one tick of ball travel is shorter than the
    // paddle is wide, so no step can straddle the paddle box. Asserting the
    // inequality directly means a future speed increase fails HERE rather than
    // as an occasional phantom goal in play.
    expect(C.BALL_SPEED_MAX_Q4).toBeLessThan(PADDLE_W_Q);

    // Empirically: with the paddle already on the ball's line (so this tests
    // tunnelling, not whether a speed-clamped paddle could reach in time), a
    // ball at maximum speed must always be returned — at every contact offset
    // and every sub-pixel phase of approach.
    let tunnelled = 0;
    for (let trial = 0; trial < 400; trial++) {
      const st = createMatch(trial + 1);
      st.state = 2;
      // Vary the contact offset across the full face, and the approach phase
      // so the ball does not always land on the same sub-pixel boundary.
      const offset = ((trial % 41) - 20) * (PADDLE_HALF_Q / 20);
      st.ballYQ4 = (FIELD_H_Q >> 1) + Math.trunc(offset);
      st.leftYQ4 = FIELD_H_Q >> 1;
      st.ballXQ4 = PADDLE_X_L_Q + (60 << C.Q4_SHIFT) + (trial % 17);
      st.ballVXQ4 = -C.BALL_SPEED_MAX_Q4;
      st.ballVYQ4 = 0; // straight on: the paddle never needs to move

      const hold: PlayerInput = { targetYQ4: FIELD_H_Q >> 1, buttons: 0 };
      let bounced = false;
      for (let i = 0; i < 40 && !bounced; i++) {
        const ev = stepMatch(st, hold, idle);
        if (ev.some((e) => e.kind === 6)) bounced = true;
        if (ev.some((e) => e.kind === 1)) break; // conceded == tunnelled
      }
      if (!bounced) tunnelled++;
    }
    expect(tunnelled).toBe(0);
  });
});

describe('scoring and match flow', () => {
  it('awards a point when the ball leaves a side, then freezes and re-serves', () => {
    const st = createMatch(5);
    run(st, C.COUNTDOWN_TICKS + 1);
    st.ballXQ4 = 32;
    st.ballVXQ4 = -C.BALL_SPEED_MAX_Q4;
    st.ballVYQ4 = 0;

    let goal = null;
    for (let i = 0; i < 60 && !goal; i++) {
      const ev = stepMatch(st, idle, idle);
      goal = ev.find((e) => e.kind === 1) ?? null;
    }
    expect(goal).not.toBeNull();
    expect(st.scoreR).toBe(1);
    expect(st.state).toBe(3); // GOAL_FREEZE

    run(st, C.GOAL_FREEZE_TICKS + 1);
    expect(st.state).toBe(2); // back to PLAY
    expect(st.ballXQ4).not.toBe(0);
  });

  it('ends at the win score and stops simulating the ball', () => {
    const st = createMatch(8, 3);
    st.state = 2;
    st.scoreL = 2;
    st.ballXQ4 = FIELD_W_Q - 32;
    st.ballVXQ4 = C.BALL_SPEED_MAX_Q4;
    st.ballVYQ4 = 0;

    let over = null;
    for (let i = 0; i < 120 && !over; i++) {
      const ev = stepMatch(st, idle, idle);
      over = ev.find((e) => e.kind === 2) ?? null;
    }
    expect(over).not.toBeNull();
    expect(st.state).toBe(4); // GAME_OVER
    expect(st.scoreL).toBe(3);

    const before = digest(st);
    run(st, 100);
    // Tick advances, but nothing else moves once the match is over.
    expect(st.scoreL).toBe(3);
    expect(st.ballVXQ4).toBe(0);
    expect(before.split(',').slice(2, 8)).toEqual(digest(st).split(',').slice(2, 8));
  });

  it('serves toward the player who just conceded', () => {
    const st = createMatch(21);
    st.state = 2;
    st.ballXQ4 = 16; // conceded on the left
    st.ballVXQ4 = -C.BALL_SPEED_MAX_Q4;
    for (let i = 0; i < 30 && st.state === 2; i++) stepMatch(st, idle, idle);
    expect(st.state).toBe(3);
    run(st, C.GOAL_FREEZE_TICKS + 1);
    expect(st.ballVXQ4).toBeLessThan(0); // toward the left player
  });
});

describe('slow mode', () => {
  it('honours a reduced speed ceiling for a laggy pairing', () => {
    const st = createMatch(77, C.WIN_SCORE, C.BALL_SPEED_MAX_SLOW_Q4);
    run(st, 4000, perfect('L'), perfect('R'));
    expect(ballSpeedQ4(st)).toBeLessThanOrEqual(C.BALL_SPEED_MAX_SLOW_Q4 + 2);
  });
});

describe('serve', () => {
  it('always produces horizontal progress', () => {
    for (let seed = 1; seed < 200; seed++) {
      const st = createMatch(seed);
      serve(st, seed % 2 ? 1 : -1);
      expect(st.ballVXQ4).not.toBe(0);
      expect(Math.sign(st.ballVXQ4)).toBe(seed % 2 ? 1 : -1);
    }
  });

  it('opens at a returnable angle', () => {
    for (let seed = 1; seed < 200; seed++) {
      const st = createMatch(seed);
      serve(st, 1);
      // |vy| < |vx| means shallower than 45 degrees.
      expect(Math.abs(st.ballVYQ4)).toBeLessThan(Math.abs(st.ballVXQ4));
    }
  });
});
