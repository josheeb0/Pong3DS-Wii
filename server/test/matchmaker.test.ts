/*
 * Matchmaking.
 *
 * Written after two desktop clients -- a Mac and a Linux laptop -- sat in the
 * queue and never found each other, while either would pair with a 3DS
 * instantly. Both report Platform.PC, so they are a SAME-platform pair, and
 * same-platform pairs are held back briefly so a queue of browsers cannot
 * consume a 3DS's opponent.
 *
 * The hold-back was fine. The bug was that nothing ever revisited the decision:
 * pairing was only considered when someone JOINED, so two clients that queued
 * within the window were each refused once and then left there.
 */

import { describe, it, expect } from 'vitest';
import { Matchmaker } from '../src/game/matchmaker.js';
import { JoinMode, Platform } from '../../shared/gen/protocol.js';

/** The least a Session can be and still survive being seated and started. */
function stubSession(platform: number, name: string) {
  return {
    platform,
    name,
    isClosed: false,
    sent: [] as Uint8Array[],
    seq: 0,
    push(frame: Uint8Array) { this.sent.push(frame); },
    nextSeq() { return ++this.seq; },
    // written by Room.seat
    snapshots: null as unknown,
    ackTick: 0,
    sentTick: 0,
    targetYQ4: 0,
  };
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
const asSession = (s: ReturnType<typeof stubSession>) => s as any;

describe('matchmaker', () => {
  it('pairs across platforms immediately', () => {
    const m = new Matchmaker();
    const t = 1_000;

    const a = stubSession(Platform.N3DS, '3DS');
    const b = stubSession(Platform.PC, 'MAC');

    expect(m.join(asSession(a), JoinMode.QUICKMATCH, '', t)).toBeNull();
    const room = m.join(asSession(b), JoinMode.QUICKMATCH, '', t + 500);

    expect(room).not.toBeNull();
    expect(m.queueLength).toBe(0);
  });

  it('pairs two same-platform clients that queued together', () => {
    // The reported failure: a Mac and a Linux laptop, both Platform.PC,
    // queueing half a second apart. Neither ever paired.
    const m = new Matchmaker();
    const t = 1_000;

    const mac = stubSession(Platform.PC, 'MAC');
    const linux = stubSession(Platform.PC, 'LINUX');

    expect(m.join(asSession(mac), JoinMode.QUICKMATCH, '', t)).toBeNull();
    expect(m.join(asSession(linux), JoinMode.QUICKMATCH, '', t + 500)).toBeNull();
    expect(m.queueLength).toBe(2);

    // Past the cross-play window, but before the bot timer. They should find
    // each other rather than each be handed a CPU.
    m.tick(t + 9_000);

    expect(m.queueLength).toBe(0);
    expect(m.roomCount).toBe(1);
  });

  it('pairs a same-platform arrival once the other has waited out the window', () => {
    /*
     * Why Mac vs Windows worked while Mac vs Linux did not, on the same build:
     * it is entirely down to the gap between pressing QUICK MATCH on each
     * machine. Walk to the other computer and eight seconds pass on their own,
     * and the second arrival pairs immediately. Press them in quick succession
     * and -- before the tick fix -- both waited forever.
     *
     * This half always worked, and is here so the explanation is checked
     * rather than asserted.
     */
    const m = new Matchmaker();
    const t = 1_000;

    const mac = stubSession(Platform.PC, 'MAC');
    const win = stubSession(Platform.PC, 'WINDOWS');

    expect(m.join(asSession(mac), JoinMode.QUICKMATCH, '', t)).toBeNull();

    // Ten seconds later, which is about how long it takes to cross a room.
    const room = m.join(asSession(win), JoinMode.QUICKMATCH, '', t + 10_000);
    expect(room).not.toBeNull();
    expect(m.queueLength).toBe(0);
  });

  it('still holds a same-platform pair back inside the window', () => {
    // The hold-back is the point: a 3DS arriving a moment later must still be
    // able to find a human rather than two browsers having taken each other.
    const m = new Matchmaker();
    const t = 1_000;

    const pc1 = stubSession(Platform.PC, 'ONE');
    const pc2 = stubSession(Platform.PC, 'TWO');

    m.join(asSession(pc1), JoinMode.QUICKMATCH, '', t);
    m.join(asSession(pc2), JoinMode.QUICKMATCH, '', t + 200);

    m.tick(t + 1_000);            // well inside the window
    expect(m.queueLength).toBe(2);

    const ds = stubSession(Platform.N3DS, 'DS');
    const room = m.join(asSession(ds), JoinMode.QUICKMATCH, '', t + 1_100);
    expect(room).not.toBeNull();  // the 3DS got a human
    expect(m.queueLength).toBe(1);
  });
});
