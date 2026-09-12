/**
 * Server-Sent Events -- the middle rung, for when WebSocket upgrade fails but
 * a long-lived HTTP response still works.
 *
 * Everything unusual in here exists to survive an intermediary that wants to
 * buffer or transform the stream:
 *
 *   - `no-transform` and `Content-Encoding: identity` stop compression proxies
 *     from accumulating output before flushing.
 *   - `X-Accel-Buffering: no` is the nginx-family opt-out, which matters
 *     because CapRover fronts this with nginx.
 *   - A 2KB comment preamble pushes past any fixed-size buffer sitting between
 *     us and the client, so the first real event is not held hostage.
 *   - A keepalive every 15s stops idle-connection reapers.
 *   - The stream is recycled every 5 minutes so no connection ages into a
 *     proxy's maximum-lifetime cutoff mid-match.
 *
 * SSE is a text protocol, so frames are base64. On a 32-byte snapshot that is
 * 12 bytes of overhead -- irrelevant next to inventing a text encoding.
 */

import type { ServerResponse } from 'node:http';
import { TransportKind, concatFrames } from '../../../../shared/gen/protocol.js';
import type { Sink } from '../sink.js';

const KEEPALIVE_MS = 15_000;
const RECYCLE_MS = 5 * 60_000;
const PREAMBLE_BYTES = 2048;

export class SseSink implements Sink {
  readonly kind = TransportKind.SSE;
  readonly pull = false;

  private open = true;
  private keepalive: NodeJS.Timeout;
  private recycle: NodeJS.Timeout;

  constructor(private res: ServerResponse) {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache, no-store, no-transform',
      'Content-Encoding': 'identity',
      'X-Accel-Buffering': 'no',
      Connection: 'keep-alive',
    });

    // Blow through any fixed-size buffer between here and the client.
    res.write(':' + ' '.repeat(PREAMBLE_BYTES) + '\n\n');
    res.write('retry: 1000\n\n');
    res.flushHeaders?.();

    this.keepalive = setInterval(() => {
      if (this.open) this.res.write(': ka\n\n');
    }, KEEPALIVE_MS);

    // Ending the response cleanly makes EventSource reconnect on its own.
    this.recycle = setTimeout(() => this.close(0), RECYCLE_MS);

    res.on('close', () => this.teardown());
    res.on('error', () => this.teardown());
  }

  write(frames: Uint8Array[]): void {
    if (!this.open) return;
    const b64 = Buffer.from(concatFrames(frames)).toString('base64');
    this.res.write('event: d\ndata: ' + b64 + '\n\n');
  }

  close(_code: number): void {
    if (!this.open) return;
    this.teardown();
    try {
      this.res.end();
    } catch {
      /* already gone */
    }
  }

  isOpen(): boolean {
    return this.open && !this.res.writableEnded;
  }

  private teardown(): void {
    this.open = false;
    clearInterval(this.keepalive);
    clearTimeout(this.recycle);
  }
}
