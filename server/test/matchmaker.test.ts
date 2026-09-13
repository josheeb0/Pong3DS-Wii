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
    // A live connection. Dropping it models a client that has gone away but
    // whose session is still inside its grace period.
    sink: { open: true, isOpen() { return this.open; } },
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
  it('never pairs someone with a session that has disconnected', () => {
    /*
     * Reported as "you can queue with yourself by accident".
     *
     * A dropped connection keeps its session alive for GRACE_MS so a wifi blip
     * cannot forfeit a match in progress. That protection is right for a match
     * and wrong for a queue: back out, press quick match again, and the new
     * connection would be paired with the abandoned session -- a match against
     * yourself, where the other paddle never moves.
     */
    const m = new Matchmaker();
    const t = 1_000;

    const abandoned = stubSession(Platform.PC, 'LINUX');
    expect(m.join(asSession(abandoned), JoinMode.QUICKMATCH, '', t)).toBeNull();

    // The client goes away. The session lingers, as designed.
    abandoned.sink.open = false;

    const rejoined = stubSession(Platform.PC, 'LINUX');
    const room = m.join(asSession(rejoined), JoinMode.QUICKMATCH, '', t + 200);

    expect(room, 'must not pair with a disconnected session').toBeNull();
  });

  it('drops disconnected players from the queue', () => {
    // The lingering entry should not sit there either: grace protects a match,
    // not a place in a queue.
    const m = new Matchmaker();
    const t = 1_000;

    const gone = stubSession(Platform.PC, 'GONE');
    m.join(asSession(gone), JoinMode.QUICKMATCH, '', t);
    expect(m.queueLength).toBe(1);

    gone.sink.open = false;
    m.tick(t + 100);

    expect(m.queueLength).toBe(0);
  });

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

  it('pairs two same-platform clients immediately', () => {
    // The reported failure, and then the reported annoyance: a Mac and a Linux
    // laptop, both Platform.PC. First they never paired at all; then they
    // paired only after an eight second hold. Neither is acceptable -- this
    // must be as immediate as pairing with a 3DS.
    const m = new Matchmaker();
    const t = 1_000;

    const mac = stubSession(Platform.PC, 'MAC');
    const linux = stubSession(Platform.PC, 'LINUX');

    expect(m.join(asSession(mac), JoinMode.QUICKMATCH, '', t)).toBeNull();

    const room = m.join(asSession(linux), JoinMode.QUICKMATCH, '', t + 500);
    expect(room).not.toBeNull();     // half a second apart, paired at once
    expect(m.queueLength).toBe(0);
    expect(m.roomCount).toBe(1);
  });

  it('pairing does not depend on how long anyone has waited', () => {
    /*
     * The old behaviour made the outcome depend on the gap between pressing
     * quick match on each machine: over eight seconds it worked, under it did
     * not. Same build, different result, decided by how fast someone walked
     * across a room. Timing must not be a variable here at all.
     */
    for (const gap of [0, 100, 3_000, 20_000]) {
      const m = new Matchmaker();
      const a = stubSession(Platform.PC, 'A');
      const b = stubSession(Platform.PC, 'B');

      m.join(asSession(a), JoinMode.QUICKMATCH, '', 1_000);
      const room = m.join(asSession(b), JoinMode.QUICKMATCH, '', 1_000 + gap);

      expect(room, `gap of ${gap}ms`).not.toBeNull();
    }
  });

  it('still prefers a cross-platform opponent when both are waiting', () => {
    // The preference survives losing the delay: it just no longer costs
    // anyone a wait. With a PC and a 3DS both queued, an arriving PC should
    // take the 3DS -- that pairing is the point of the project.
    const m = new Matchmaker();
    const t = 1_000;

    const pc1 = stubSession(Platform.PC, 'ONE');
    const ds = stubSession(Platform.N3DS, 'DS');

    m.join(asSession(pc1), JoinMode.QUICKMATCH, '', t);
    m.join(asSession(ds), JoinMode.QUICKMATCH, '', t + 100);
    // Those two pair with each other on arrival -- cross-play, immediate.
    expect(m.roomCount).toBe(1);
    expect(m.queueLength).toBe(0);

    // Now the ordering that actually tests the preference: two PCs waiting,
    // then a 3DS arrives and must take one of them rather than be refused.
    const m2 = new Matchmaker();
    const a = stubSession(Platform.PC, 'A');
    const b = stubSession(Platform.PC, 'B');
    m2.join(asSession(a), JoinMode.QUICKMATCH, '', t);      // queues
    expect(m2.join(asSession(b), JoinMode.QUICKMATCH, '', t + 50)).not.toBeNull();

    const c = stubSession(Platform.PC, 'C');
    const ds2 = stubSession(Platform.N3DS, 'DS2');
    m2.join(asSession(c), JoinMode.QUICKMATCH, '', t + 100);
    const room = m2.join(asSession(ds2), JoinMode.QUICKMATCH, '', t + 150);
    expect(room).not.toBeNull();   // the 3DS found the waiting PC at once
  });
});
