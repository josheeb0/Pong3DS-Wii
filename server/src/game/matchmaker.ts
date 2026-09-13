/**
 * Pairs players into rooms.
 *
 * Prefers pairing ACROSS platforms -- a 3DS with a browser -- because that is
 * the entire point of the project, and a queue of two browsers should not
 * consume a 3DS's opponent while the 3DS waits. After BOT_AFTER_MS a waiting
 * player is offered a bot rather than left staring at a spinner.
 */

import { C, JoinMode, Platform } from '../../../shared/gen/protocol.js';
import { Room } from './room.js';
import type { Session } from '../net/session.js';

/** Give cross-play a head start before settling for a same-platform match. */
const CROSS_PLATFORM_WINDOW_MS = 8_000;
/** Offer a bot rather than leave someone waiting indefinitely. */
const BOT_AFTER_MS = 15_000;

const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // no I/O/0/1

interface Waiting {
  session: Session;
  since: number;
}

export class Matchmaker {
  private rooms = new Map<string, Room>();
  private queue: Waiting[] = [];
  private seedCounter = 0x9e3779b9;

  get roomCount(): number {
    return this.rooms.size;
  }

  get queueLength(): number {
    return this.queue.length;
  }

  allRooms(): IterableIterator<Room> {
    return this.rooms.values();
  }

  roomOf(s: Session): Room | null {
    for (const r of this.rooms.values()) {
      if (r.seats[0] === s || r.seats[1] === s) return r;
    }
    return null;
  }

  private nextSeed(): number {
    this.seedCounter = (this.seedCounter * 1664525 + 1013904223) >>> 0;
    return this.seedCounter;
  }

  private makeCode(): string {
    for (let attempt = 0; attempt < 64; attempt++) {
      let code = '';
      for (let i = 0; i < C.ROOM_CODE_BYTES; i++) {
        code += CODE_ALPHABET[Math.floor(Math.random() * CODE_ALPHABET.length)];
      }
      if (!this.rooms.has(code)) return code;
    }
    return 'RM' + String(this.rooms.size).padStart(4, '0');
  }

  /** Handles a JOIN. Returns the room the session ended up in, if any. */
  join(s: Session, mode: number, roomCode: string, nowMs: number): Room | null {
    this.leave(s); // idempotent: re-joining should not double-seat

    if (mode === JoinMode.VS_BOT) {
      const room = new Room(this.makeCode(), nowMs, this.nextSeed());
      this.rooms.set(room.code, room);
      room.seat(s);
      room.addBot();
      room.start();
      return room;
    }

    if (mode === JoinMode.ROOM_CODE && roomCode) {
      const code = roomCode.toUpperCase();
      let room = this.rooms.get(code);
      if (!room) {
        room = new Room(code, nowMs, this.nextSeed());
        this.rooms.set(code, room);
      }
      if (room.isFull) return null; // caller sends ROOM_FULL
      room.seat(s);
      if (room.isFull) room.start();
      return room;
    }

    // Quickmatch: look for the best waiting opponent.
    const idx = this.pickOpponent(s, nowMs);
    if (idx >= 0) {
      const other = this.queue.splice(idx, 1)[0] as Waiting;
      const room = new Room(this.makeCode(), nowMs, this.nextSeed());
      this.rooms.set(room.code, room);
      room.seat(other.session);
      room.seat(s);
      room.start();
      return room;
    }

    this.queue.push({ session: s, since: nowMs });
    return null;
  }

  /**
   * Chooses who to pair with. Cross-platform wins outright; a same-platform
   * opponent is only taken once they have waited past the window, so a browser
   * pair does not steal a 3DS's opponent in the first seconds.
   */
  private pickOpponent(s: Session, nowMs: number): number {
    let sameIdx = -1;
    for (let i = 0; i < this.queue.length; i++) {
      const w = this.queue[i] as Waiting;
      if (w.session === s || w.session.isClosed) continue;
      if (w.session.platform !== s.platform) return i; // cross-play: take it
      if (sameIdx < 0) sameIdx = i;
    }
    if (sameIdx >= 0) {
      const w = this.queue[sameIdx] as Waiting;
      if (nowMs - w.since >= CROSS_PLATFORM_WINDOW_MS) return sameIdx;
    }
    return -1;
  }

  /**
   * Pairs everyone in the queue who is now eligible.
   *
   * Walks from the front so the longest-waiting player is served first, which
   * is also the one most likely to have cleared the window.
   */
  private pairWaiting(nowMs: number): void {
    for (let i = 0; i < this.queue.length; i++) {
      const w = this.queue[i] as Waiting;
      if (!w || w.session.isClosed) continue;

      const j = this.pickOpponent(w.session, nowMs);
      if (j < 0) continue;

      // Remove the higher index first, or removing the lower one shifts the
      // other and the wrong entry comes out.
      const hi = Math.max(i, j);
      const lo = Math.min(i, j);
      const second = this.queue.splice(hi, 1)[0] as Waiting;
      const first = this.queue.splice(lo, 1)[0] as Waiting;

      const room = new Room(this.makeCode(), nowMs, this.nextSeed());
      this.rooms.set(room.code, room);
      room.seat(first.session);
      room.seat(second.session);
      room.start();

      i--;   // the queue shrank under us
    }
  }

  /**
   * Housekeeping: pairs who can now be paired, offers bots, prunes the dead.
   *
   * The pairing pass is here because pickOpponent used to be consulted only
   * when someone JOINED. A same-platform pair is deliberately held back for
   * CROSS_PLATFORM_WINDOW_MS -- but nothing ever revisited the decision, so two
   * clients of the same platform that queued within that window were each
   * refused once and then left in the queue indefinitely, until the bot timer
   * rescued them separately.
   *
   * Reported as two desktops that would each pair with a 3DS instantly and
   * never with each other, which is exactly the shape of "only reconsidered on
   * arrival".
   */
  tick(nowMs: number): void {
    this.pairWaiting(nowMs);

    for (let i = this.queue.length - 1; i >= 0; i--) {
      const w = this.queue[i] as Waiting;
      if (w.session.isClosed) {
        this.queue.splice(i, 1);
        continue;
      }
      if (nowMs - w.since >= BOT_AFTER_MS) {
        this.queue.splice(i, 1);
        const room = new Room(this.makeCode(), nowMs, this.nextSeed());
        this.rooms.set(room.code, room);
        room.seat(w.session);
        room.addBot();
        room.start();
      }
    }

    for (const [code, room] of this.rooms) {
      if (room.isEmpty && nowMs - room.createdMs > 5_000) this.rooms.delete(code);
    }
  }

  leave(s: Session): void {
    const qi = this.queue.findIndex((w) => w.session === s);
    if (qi >= 0) this.queue.splice(qi, 1);
    const room = this.roomOf(s);
    if (room) {
      room.remove(s);
      if (room.isEmpty) this.rooms.delete(room.code);
    }
  }

  stepAll(): void {
    for (const room of this.rooms.values()) room.step();
  }
}
