import { concatFrames } from '../../../shared/gen/protocol';
import type { SessionHandle } from './types';

/**
 * Shared upstream for the two HTTP rungs.
 *
 * SSE and long-poll differ only in how state comes DOWN; both push input UP the
 * same way, so that lives here rather than being written twice.
 */
export class HttpUpstream {
  private outbox: Uint8Array[] = [];
  private inFlight = false;
  private timer: number | null = null;
  private lastRtt = 0;
  private closed = false;

  constructor(
    private session: SessionHandle,
    private onFrame: (b: Uint8Array) => void,
    /** Upstream rate. 20Hz is plenty: input is an absolute target, so the
     *  newest one supersedes anything queued behind it. */
    private hz = 20,
  ) {
    this.schedule();
  }

  send(frame: Uint8Array): void {
    this.outbox.push(frame);
  }

  get rtt(): number {
    return this.lastRtt;
  }

  private schedule(): void {
    if (this.closed) return;
    this.timer = window.setTimeout(() => {
      void this.pump();
      this.schedule();
    }, 1000 / this.hz);
  }

  private async pump(): Promise<void> {
    if (this.closed || this.inFlight || this.outbox.length === 0) return;
    const batch = this.outbox;
    this.outbox = [];
    this.inFlight = true;
    const t0 = performance.now();
    try {
      const res = await fetch(this.session.base + '/api/rpc', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/octet-stream',
          'X-Pong-Session': this.session.id,
        },
        body: concatFrames(batch),
        cache: 'no-store',
      });
      this.lastRtt = Math.round(performance.now() - t0);
      // The response may carry state too; never waste it.
      if (res.ok && res.status !== 204) {
        const buf = new Uint8Array(await res.arrayBuffer());
        if (buf.length) this.onFrame(buf);
      }
    } catch {
      /* a dropped upstream POST is recoverable: the next absolute target wins */
    } finally {
      this.inFlight = false;
    }
  }

  close(): void {
    this.closed = true;
    if (this.timer !== null) clearTimeout(this.timer);
    this.timer = null;
  }
}
