import { C, TransportKind, MsgType, parseFrames } from '../../../shared/gen/protocol';
import { ProofOfLife, TransportFailed, type ClientTransport, type FrameHandler, type SessionHandle } from './types';
import { HttpUpstream } from './http';

/**
 * Long-poll rung -- the backstop that works through anything that passes plain
 * HTTP POSTs.
 *
 * Two concurrent requests rather than one:
 *
 *   - A held downstream POST (`?wait=20000`) that the server parks until a
 *     frame is ready. During a match it returns almost immediately, because a
 *     snapshot is always due within ~33ms; the hold only actually engages in
 *     the lobby.
 *   - Independent upstream POSTs at 20Hz carrying input.
 *
 * Using a single in-flight request for both -- the obvious design, and the one
 * the 3DS is forced into -- would stall input for the length of the hold. A
 * browser has no reason to accept that, so it does not.
 *
 * The 20s hold is well under Cloudflare's ~100s idle cutoff.
 */
export class LongPollTransport implements ClientTransport {
  readonly kind = TransportKind.LONGPOLL;
  readonly name = 'HTTP long-poll';

  private up: HttpUpstream | null = null;
  private proof = new ProofOfLife();
  private closed = false;
  private session: SessionHandle | null = null;
  private onFrame: FrameHandler | null = null;
  private lastRtt = 0;
  private consecutiveErrors = 0;

  open(session: SessionHandle, onFrame: FrameHandler, probe: Uint8Array): Promise<void> {
    this.session = session;
    this.onFrame = onFrame;
    this.up = new HttpUpstream(session, (b) => this.ingest(b));
    this.up.send(probe);
    void this.pollLoop();
    return this.proof.promise;
  }

  private ingest(buf: Uint8Array): void {
    if (buf.length === 0) return;
    if (!this.proof.settled) {
      const { frames } = parseFrames(buf);
      if (frames.some((f) => f.type === MsgType.SNAPSHOT || f.type === MsgType.PONG)) {
        this.proof.prove();
      }
    }
    this.onFrame?.(buf);
  }

  private async pollLoop(): Promise<void> {
    while (!this.closed && this.session) {
      const t0 = performance.now();
      try {
        const res = await fetch(
          `${this.session.base}/api/rpc?wait=${C.LONGPOLL_HOLD_MS}`,
          {
            method: 'POST',
            headers: {
              'Content-Type': 'application/octet-stream',
              'X-Pong-Session': this.session.id,
            },
            body: new Uint8Array(0),
            cache: 'no-store',
          },
        );
        this.lastRtt = Math.round(performance.now() - t0);

        if (res.status === 404) {
          this.proof.fail(new TransportFailed(this.name, 'session unknown'));
          return;
        }
        if (res.ok && res.status !== 204) {
          this.ingest(new Uint8Array(await res.arrayBuffer()));
        }
        this.consecutiveErrors = 0;
      } catch (e) {
        if (this.closed) return;
        this.consecutiveErrors++;
        if (!this.proof.settled && this.consecutiveErrors >= 2) {
          this.proof.fail(new TransportFailed(this.name, String(e)));
          return;
        }
        // Back off briefly so a hard-down server is not hammered.
        await new Promise((r) => setTimeout(r, Math.min(2000, 200 * this.consecutiveErrors)));
      }
    }
  }

  send(frame: Uint8Array): void {
    this.up?.send(frame);
  }

  close(): void {
    this.closed = true;
    this.up?.close();
    this.up = null;
  }

  isOpen(): boolean {
    return !this.closed;
  }

  rttMs(): number {
    // The held poll's duration says nothing about latency; the upstream POST
    // is the honest sample.
    return this.up?.rtt || this.lastRtt;
  }
}
