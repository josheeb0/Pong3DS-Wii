import { describe, it, expect } from 'vitest';
import { GameClient } from '../src/game/client';
import { encodeSNAPSHOT, encodeMATCH_START, MatchState } from '../../shared/gen/protocol';
import { FIELD_H_Q4 } from '../../shared/sim/paddle';

/**
 * Regression test for a paddle that stops responding after the first match.
 *
 * Reported as: "you have to refresh after every game or else you can't see your
 * paddle moving when you move it."
 *
 * The client keeps the tick of the last authoritative position of its own
 * paddle, and accepts a snapshot only when it is at least that new:
 *
 *     if (s.tick >= this.myServerTick) { ... }
 *
 * That is right WITHIN a match -- it is what rejects snapshots arriving out of
 * order on a lossy transport. It is wrong ACROSS matches, because the server
 * builds a fresh simulation per match and its tick counter starts again at
 * zero. Every snapshot of the second match is therefore "older" than the tick
 * left over from the first, so none of them is accepted: the authoritative
 * position stays frozen wherever the previous game ended, local prediction has
 * nothing to advance from, and the paddle ignores input until the page is
 * reloaded and the leftover tick goes with it.
 *
 * The condition is not the bug. The missing reset is.
 *
 * These tests reach into private fields on purpose. What broke is precisely the
 * state that is not otherwise observable, and asserting on it directly is what
 * makes the test fail for the actual reason rather than for a symptom.
 */

/** The fields the fix is about, read straight off the instance. */
function own(c: GameClient): { tick: number; y: number } {
  const a = c as unknown as { myServerTick: number; myServerY: number };
  return { tick: a.myServerTick, y: a.myServerY };
}

function matchStart(yourSide: number): Uint8Array {
  return encodeMATCH_START({
    matchId: 1,
    yourSide,
    oppPlatform: 0,
    paddleHQ4: 16 * 16,
    paddleWQ4: 16 * 4,
    ballRQ4: 16 * 4,
    maxPaddleSpdQ4: 240,
    winScore: 5,
    matchFlags: 0,
    oppName: new Uint8Array(16),
  });
}

function snapshot(tick: number, leftYQ4: number, rightYQ4: number): Uint8Array {
  return encodeSNAPSHOT({
    tick,
    ackInputSeq: 0,
    ballXQ4: 100, ballYQ4: 100, ballVXQ4: 10, ballVYQ4: 10,
    leftYQ4, rightYQ4,
    scoreL: 0, scoreR: 0,
    state: MatchState.PLAY,
    flags: 0,
  });
}

/** `ingest` is private; the frame path is what needs exercising. */
function feed(c: GameClient, buf: Uint8Array): void {
  (c as unknown as { ingest(b: Uint8Array): void }).ingest(buf);
}

const CENTRE = FIELD_H_Q4 >> 1;

describe('own paddle across matches', () => {
  it('takes the authoritative position during the first match', () => {
    const c = new GameClient();
    feed(c, matchStart(0));
    feed(c, snapshot(500, 1234, 4321));
    expect(own(c)).toEqual({ tick: 500, y: 1234 });
  });

  it('still rejects out-of-order snapshots within a match', () => {
    const c = new GameClient();
    feed(c, matchStart(0));
    feed(c, snapshot(500, 1234, 4321));
    feed(c, snapshot(499, 9999, 9999));   // arrived late; must not win
    expect(own(c)).toEqual({ tick: 500, y: 1234 });
  });

  it('accepts the second match, whose ticks start again at zero', () => {
    const c = new GameClient();

    // Match one runs for a while and ends with our paddle somewhere specific.
    feed(c, matchStart(0));
    feed(c, snapshot(5000, 1234, 4321));
    expect(own(c).tick).toBe(5000);

    // Match two. The server made a new simulation, so tick counts from 0.
    feed(c, matchStart(0));
    feed(c, snapshot(3, 777, 888));

    // Without the reset this is still { tick: 5000, y: 1234 } -- the paddle is
    // pinned to where the LAST game left it and never moves again.
    expect(own(c)).toEqual({ tick: 3, y: 777 });
  });

  it('centres the paddle when a new match starts', () => {
    const c = new GameClient();
    feed(c, matchStart(0));
    feed(c, snapshot(5000, 1234, 4321));

    feed(c, matchStart(0));

    // A new match starts with paddles centred. Carrying the old position over
    // would show the paddle somewhere it is not until the first snapshot of the
    // new match arrives.
    expect(own(c)).toEqual({ tick: 0, y: CENTRE });
  });

  it('follows the side it was given, when the sides swap between matches', () => {
    const c = new GameClient();

    feed(c, matchStart(0));                 // left
    feed(c, snapshot(100, 1111, 2222));
    expect(own(c).y).toBe(1111);

    feed(c, matchStart(1));                 // right this time
    feed(c, snapshot(7, 3333, 4444));
    expect(own(c).y).toBe(4444);
  });
});
