/**
 * Pairs players into rooms.
 *
 * Prefers pairing ACROSS platforms -- a 3DS with a browser -- because that is
 * the entire point of the project. The preference applies only when both kinds
 * are actually waiting; nobody is ever made to wait for an opponent who might
 * not turn up. After BOT_AFTER_MS a lone player is offered a bot rather than
 * left staring at a spinner.
 */

import { C, JoinMode, Platform, PlatformName } from '../../../shared/gen/protocol.js';
import { Room } from './room.js';
import type { Session } from '../net/session.js';

/*
 * There is no waiting period any more.
 *
 * Cross-play used to get an eight second head start before a same-platform
 * pair was allowed, so that two browsers could not take each other while a 3DS
 * waited for a human. The intent was good and the cost was not: two desktops
 * queueing together stared at "waiting for an opponent" for eight seconds every
 * single time, which reads as broken, while the 3DS it was protecting might
 * never arrive at all.
 *
 * The preference is kept and the delay is gone: a cross-platform opponent is
 * still chosen ahead of a same-platform one whenever BOTH are actually waiting.
 * What is no longer done is holding a slot open for someone hypothetical.
 */
/** Offer a bot rather than leave someone waiting indefinitely. */
const BOT_AFTER_MS = 15_000;

const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // no I/O/0/1

interface Waiting {
  session: Session;
  since: number;
}

/*
 * Matchmaking logging.
 *
 * Added after an afternoon of guessing why two machines would not pair. The
 * server recorded nothing about who connected or what platform they claimed,
 * so every question had to be answered by reading the source and inferring --
 * which produced a correct diagnosis and an incorrect confidence, twice.
 *
 * One line per decision. A busy server would not want this at info level, but a
 * game of Pong is not a busy server and the failure it describes cost far more
 * than the noise will.
 */
function log(msg: string): void {
  console.log(`[match] ${msg}`);
}

/*
 * Is this session still on the other end of a connection?
 *
 * A dropped connection keeps its session alive for GRACE_MS so that a flaky
 * 3DS wifi blip cannot forfeit a match in progress. That protection is right
 * for a MATCH and wrong for a QUEUE: a player who has gone away cannot play,
 * and leaving their slot warm meant the next person to connect could be paired
 * with it.
 *
 * Reported as being able to queue with yourself: back out, press quick match
 * again, and the new connection was matched against the session you had just
 * abandoned -- a game where the other paddle never moves.
 */
function connected(s: Session): boolean {
  return s.sink?.isOpen() === true;
}

/** "TESTA (PC)" -- the two facts every pairing question has needed. */
function who(s: Session): string {
  const name = s.name && s.name.length > 0 ? s.name : '(unnamed)';
  return `${name} (${PlatformName[s.platform] ?? s.platform})`;
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
      log(`match ${room.code}: ${who(other.session)} vs ${who(s)}`);
      return room;
    }

    this.queue.push({ session: s, since: nowMs });
    log(`queued ${who(s)} (waiting: ${this.describeQueue()})`);
    return null;
  }

  /**
   * Chooses who to pair with: a different platform if one is waiting, otherwise
   * whoever has waited longest. Never nobody, if the queue holds anyone at all.
   *
   * The preference costs nothing because it only applies when there is a real
   * choice. Making it cost a delay is what made two desktops feel broken.
   */
  private pickOpponent(s: Session, nowMs: number): number {
    void nowMs;   // no longer time-dependent; kept for call-site symmetry
    let sameIdx = -1;
    for (let i = 0; i < this.queue.length; i++) {
      const w = this.queue[i] as Waiting;
      if (w.session === s || w.session.isClosed) continue;
      if (!connected(w.session)) continue;             // gone, grace or not
      if (w.session.platform !== s.platform) return i; // cross-play: take it
      if (sameIdx < 0) sameIdx = i;                    // longest-waiting match
    }
    return sameIdx;
  }

  /** A one-line summary of who is waiting, for the log. */
  private describeQueue(): string {
    if (this.queue.length === 0) return 'nobody';
    return this.queue.map((w) => who(w.session)).join(', ');
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
      if (!w || w.session.isClosed || !connected(w.session)) continue;

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
      log(`match ${room.code}: ${who(first.session)} vs ${who(second.session)} (swept)`);

      i--;   // the queue shrank under us
    }
  }

  /**
   * Housekeeping: pairs who can now be paired, offers bots, prunes the dead.
   *
   * The pairing pass exists because pickOpponent is consulted when someone
   * JOINS, and a queue can still end up holding two people who could be paired
   * -- a session closing, a room filling, anything that changes the queue
   * without a new arrival. Sweeping here means the queue never sits in a state
   * where two waiting players could have been matched and were not.
   */
  tick(nowMs: number): void {
    this.pairWaiting(nowMs);

    for (let i = this.queue.length - 1; i >= 0; i--) {
      const w = this.queue[i] as Waiting;
      if (w.session.isClosed || !connected(w.session)) {
        // Grace keeps a session alive so a match can be resumed. It does not
        // hold a place in the queue: there is nothing to resume, and the slot
        // would otherwise be handed to whoever connects next.
        this.queue.splice(i, 1);
        log(`dropped ${who(w.session)} from the queue (disconnected)`);
        continue;
      }
      if (nowMs - w.since >= BOT_AFTER_MS) {
        this.queue.splice(i, 1);
        const room = new Room(this.makeCode(), nowMs, this.nextSeed());
        this.rooms.set(room.code, room);
        room.seat(w.session);
        room.addBot();
        room.start();
        log(`match ${room.code}: ${who(w.session)} vs CPU (nobody came)`);
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
