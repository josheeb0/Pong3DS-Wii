/**
 * A match played entirely in this tab.
 *
 * The browser is the one client that did NOT need a port to get here: the
 * authoritative simulation is already TypeScript, so this reuses the server's
 * own `stepMatch` and `Bot` rather than reimplementing them. The C clients had
 * to port both, and 3ds/test/test_sim.c is what proves those ports agree with
 * the code this file calls directly.
 *
 * The same two shapes as everywhere else: against the built-in opponent, or two
 * people sharing a keyboard.
 */

import { createMatch, stepMatch, type MatchState } from '../../../server/src/sim/pong';
import { Bot } from '../../../server/src/game/bot';
import { C } from '../../../shared/gen/protocol';
import { FIELD_H_Q4 } from '../../../shared/sim/paddle';
import type { View } from './interp';

export type LocalMode = 'ai' | 'two-player';

/**
 * Difficulty, matching src/sim/pong_bot.c so the browser's EASY is the 3DS's
 * EASY. The values are measured there, against a perfect tracker, and the
 * spacing is deliberately uneven: skill saturates past about 0.8, where the
 * opponent already reaches every returnable ball.
 */
export const AI_LEVELS = ['EASY', 'NORMAL', 'HARD'] as const;
export type AiLevel = typeof AI_LEVELS[number];

const SKILL: Record<AiLevel, number> = {
  EASY: 80 / 256,
  NORMAL: 150 / 256,
  HARD: 250 / 256,
};

/** One tick, in milliseconds. */
const TICK_MS = 1000 / C.TICK_HZ;

/**
 * The most ticks one frame may simulate.
 *
 * A backgrounded tab does not get animation frames, so returning to it can
 * deliver a delta of minutes. Simulating all of it would lock the tab up; the
 * time is dropped instead, and the match simply continues from where it was.
 */
const MAX_STEPS_PER_FRAME = 8;

export class LocalMatch {
  readonly mode: LocalMode;
  private st: MatchState;
  private bot: Bot | null;
  private accumMs = 0;

  /** Absolute paddle targets, the same shape the wire carries. */
  targetL = FIELD_H_Q4 >> 1;
  targetR = FIELD_H_Q4 >> 1;

  constructor(mode: LocalMode, level: AiLevel, seed = (Date.now() & 0x7fffffff) | 1) {
    this.mode = mode;
    this.st = createMatch(seed, C.WIN_SCORE, C.BALL_SPEED_MAX_Q4);
    /* The opponent always plays the right-hand paddle, so "your side" is left
     * in every local match and the renderer needs no special case. */
    this.bot = mode === 'ai' ? new Bot(1, SKILL[level]) : null;
  }

  get state(): MatchState { return this.st; }
  get over(): boolean { return this.st.state === 4 /* GAME_OVER */; }

  /** Advances by real elapsed time, in fixed ticks. */
  advance(dtMs: number): void {
    this.accumMs += dtMs;

    let steps = 0;
    while (this.accumMs >= TICK_MS && steps < MAX_STEPS_PER_FRAME) {
      this.accumMs -= TICK_MS;
      steps++;

      const inR = this.bot
        ? this.bot.think(this.st)
        : { targetYQ4: this.targetR, buttons: 0 };

      stepMatch(this.st, { targetYQ4: this.targetL, buttons: 0 }, inR);
    }

    if (steps >= MAX_STEPS_PER_FRAME) this.accumMs = 0;
  }

  /** The view the renderer draws, straight out of the simulation. */
  view(): View {
    return {
      ballX: this.st.ballXQ4,
      ballY: this.st.ballYQ4,
      leftY: this.st.leftYQ4,
      rightY: this.st.rightYQ4,
      scoreL: this.st.scoreL,
      scoreR: this.st.scoreR,
      state: this.st.state,
      flags: 0,
      /* Never starved: this IS the authority, so there is nothing to run out
       * of and nothing to extrapolate past. */
      starved: false,
    };
  }
}
