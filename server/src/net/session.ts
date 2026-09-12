/**
 * A player's identity and outbound queue, independent of how they are connected.
 *
 * A session is created by `POST /api/session` before any transport is chosen,
 * and it outlives transport churn: a browser can fall from WebSocket to SSE to
 * long-poll mid-match and keep its seat, its score and its room, because none
 * of those live in the connection.
 *
 * The queueing rule here is what makes slow pull transports playable:
 *
 *   - Reliable frames (MATCH_START, EVENT, BYE, WELCOME, PONG) are queued and
 *     never dropped. Missing a GOAL event would leave a client's score wrong
 *     forever.
 *
 *   - Snapshots are NOT queued at all. They are pulled from the room's ring at
 *     drain time, using what this client has actually seen. A 3DS polling at
 *     10Hz therefore receives the newest world state plus the intermediate
 *     sample points between its polls — never a backlog of stale ones.
 */

import { randomBytes } from 'node:crypto';
import type { Sink } from './sink.js';
import { C, encodeBYE, ByeCode } from '../../../shared/gen/protocol.js';

/** Reliable frames beyond this mean the client is not draining; drop them. */
const RELIABLE_CAP = 64;

export interface SnapshotSource {
  /**
   * Snapshots strictly newer than `sinceTick`, decimated to roughly
   * `everyNTicks`, newest always included, at most `max` frames.
   */
  snapshotsSince(sinceTick: number, everyNTicks: number, max: number, includeNewest?: boolean): Uint8Array[];
  readonly newestTick: number;
}

export class Session {
  readonly id: string;
  readonly token: Uint8Array;

  platform = 0;
  name = '';
  buildId = 0;

  /** Transport currently attached, if any. */
  sink: Sink | null = null;

  /** Set by the room when this session takes a seat. */
  snapshots: SnapshotSource | null = null;

  /** Newest tick this client is known to hold. */
  ackTick = 0;
  /** Newest tick we have actually sent. */
  sentTick = 0;

  /** Latest input the client asked for, applied by the room each tick. */
  targetYQ4 = (C.FIELD_H << C.Q4_SHIFT) >> 1;
  buttons = 0;
  lastInputSeq = 0;

  /** Snapshot decimation for this session's transport class. */
  everyNTicks = 2;

  lastSeenMs: number;
  createdMs: number;

  /** Round-trip samples, min-filtered by the client; kept here for /healthz. */
  rttMs = 0;

  private reliable: Uint8Array[] = [];
  private outSeq = 0;
  private closed = false;

  constructor(nowMs: number) {
    this.token = randomBytes(C.SESSION_ID_BYTES);
    this.id = Buffer.from(this.token).toString('hex');
    this.lastSeenMs = nowMs;
    this.createdMs = nowMs;
  }

  get isClosed(): boolean {
    return this.closed;
  }

  nextSeq(): number {
    this.outSeq = (this.outSeq + 1) & 0xffff;
    return this.outSeq;
  }

  /** Queue a frame that must not be lost. */
  push(frame: Uint8Array): void {
    if (this.closed) return;
    if (this.reliable.length >= RELIABLE_CAP) {
      // The client is not consuming. Dropping reliable frames would corrupt its
      // view silently, so end the session instead and let it resume cleanly.
      this.close(ByeCode.TIMEOUT);
      return;
    }
    this.reliable.push(frame);
  }

  /** True if there is anything to send right now (drives long-poll wake-up). */
  hasPending(): boolean {
    if (this.reliable.length > 0) return true;
    const src = this.snapshots;
    if (!src) return false;
    return src.newestTick > (this.sink?.pull ? this.ackTick : this.sentTick);
  }

  /**
   * Collects frames to send: reliable first, then as many snapshots as the
   * budget allows.
   *
   * Reliable frames get priority because an EVENT the client misses is
   * permanent, whereas a skipped snapshot is corrected by the next one.
   */
  drain(maxFrames: number): Uint8Array[] {
    if (this.closed) return [];
    const out: Uint8Array[] = [];

    while (out.length < maxFrames && this.reliable.length > 0) {
      out.push(this.reliable.shift() as Uint8Array);
    }

    const src = this.snapshots;
    if (src && out.length < maxFrames) {
      // Pull transports tell us what they hold; push transports do not ack, so
      // we advance on what we sent.
      const pull = this.sink?.pull === true;
      const since = pull ? Math.max(this.ackTick, 0) : this.sentTick;
      // Only a poller needs the newest tick forced in; see Room.snapshotsSince.
      const frames = src.snapshotsSince(since, this.everyNTicks, maxFrames - out.length, pull);
      for (const f of frames) out.push(f);
      if (src.newestTick > this.sentTick) this.sentTick = src.newestTick;
    }

    return out;
  }

  /** Sends whatever is pending over the attached sink, if any. */
  flush(maxFrames = C.SNAP_BATCH_MAX + 4): boolean {
    const sink = this.sink;
    if (!sink || !sink.isOpen()) return false;
    const frames = this.drain(maxFrames);
    if (frames.length === 0) return false;
    sink.write(frames);
    return true;
  }

  attach(sink: Sink, nowMs: number): void {
    if (this.sink && this.sink !== sink && this.sink.isOpen()) {
      // One connection per session. A second is either a stale tab or a
      // transport switch that did not tear down; the newest wins.
      this.sink.close(ByeCode.NORMAL);
    }
    this.sink = sink;
    this.lastSeenMs = nowMs;
  }

  detach(sink: Sink): void {
    if (this.sink === sink) this.sink = null;
  }

  close(code: number): void {
    if (this.closed) return;
    this.closed = true;
    const sink = this.sink;
    if (sink && sink.isOpen()) {
      try {
        sink.write([encodeBYE({ code, reserved: 0 }, this.nextSeq())]);
      } catch {
        /* the connection is going away regardless */
      }
      sink.close(code);
    }
    this.sink = null;
    this.reliable.length = 0;
  }
}

/**
 * Owns every live session and expires the abandoned ones.
 *
 * A disconnected session is kept for GRACE_MS so a transport switch or a brief
 * network blip does not forfeit a match in progress.
 */
export class SessionManager {
  private byId = new Map<string, Session>();

  create(nowMs: number): Session {
    const s = new Session(nowMs);
    this.byId.set(s.id, s);
    return s;
  }

  get(id: string | null | undefined): Session | null {
    if (!id) return null;
    const s = this.byId.get(id);
    if (!s || s.isClosed) return null;
    return s;
  }

  /** Resolves the hex id a client presents, in constant-ish time. */
  getByToken(token: Uint8Array): Session | null {
    return this.get(Buffer.from(token).toString('hex'));
  }

  all(): IterableIterator<Session> {
    return this.byId.values();
  }

  get size(): number {
    return this.byId.size;
  }

  /** Drops sessions that have been silent past the grace period. */
  sweep(nowMs: number, onExpire?: (s: Session) => void): number {
    let removed = 0;
    for (const [id, s] of this.byId) {
      const connected = s.sink?.isOpen() ?? false;
      if (connected) {
        s.lastSeenMs = nowMs;
        continue;
      }
      if (nowMs - s.lastSeenMs > C.GRACE_MS || s.isClosed) {
        onExpire?.(s);
        s.close(ByeCode.TIMEOUT);
        this.byId.delete(id);
        removed++;
      }
    }
    return removed;
  }
}
