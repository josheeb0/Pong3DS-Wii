/**
 * Request/response transport -- one endpoint serving two very different clients.
 *
 *   - The browser's long-poll rung calls it with `?wait=20000`: the response is
 *     parked until a frame is ready or the hold expires. The 20s cap sits well
 *     under Cloudflare's ~100s idle timeout, with room to spare.
 *
 *   - The 3DS calls it with no wait: it gets whatever is queued immediately and
 *     issues the next request itself. Holding a 3DS request would pin its
 *     worker thread, and libctru's httpc offers no clean way to abort one
 *     in flight.
 *
 * The request body carries the client's frames; the response body carries the
 * server's. Input rides upstream on the same round trip that brings state down,
 * which halves the request count -- the single most important optimisation on a
 * transport where each round trip may cost 200ms.
 */

import type { ServerResponse } from 'node:http';
import { TransportKind } from '../../../../shared/gen/protocol.js';
import { concatFrames } from '../../../../shared/gen/protocol.js';
import type { Sink } from '../sink.js';

export class RrSink implements Sink {
  readonly kind: TransportKind;
  readonly pull = true;

  private sent = false;

  constructor(private res: ServerResponse, kind: TransportKind) {
    this.kind = kind;
  }

  write(frames: Uint8Array[]): void {
    if (this.sent || this.res.writableEnded) return;
    this.sent = true;
    const body = Buffer.from(concatFrames(frames));
    this.res.writeHead(200, {
      'Content-Type': 'application/octet-stream',
      'Content-Length': String(body.length),
      // Without this a caching layer can serve a stale game state and the
      // match silently freezes. It is not a nicety.
      'Cache-Control': 'no-store, no-transform',
      'X-Accel-Buffering': 'no',
    });
    this.res.end(body);
  }

  /** Ends the round trip with an empty body when nothing is queued. */
  finishEmpty(): void {
    if (this.sent || this.res.writableEnded) return;
    this.sent = true;
    this.res.writeHead(204, {
      'Cache-Control': 'no-store, no-transform',
      'X-Accel-Buffering': 'no',
    });
    this.res.end();
  }

  close(_code: number): void {
    if (!this.sent && !this.res.writableEnded) {
      this.sent = true;
      try {
        this.res.writeHead(204, { 'Cache-Control': 'no-store' });
        this.res.end();
      } catch {
        /* already gone */
      }
    }
  }

  /** A response is "open" only until it has been written once. */
  isOpen(): boolean {
    return !this.sent && !this.res.writableEnded;
  }

  get hasSent(): boolean {
    return this.sent;
  }
}
