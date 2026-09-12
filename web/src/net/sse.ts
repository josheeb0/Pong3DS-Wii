import { TransportKind, MsgType, parseFrames } from '../../../shared/gen/protocol';
import { ProofOfLife, TransportFailed, type ClientTransport, type FrameHandler, type SessionHandle } from './types';
import { HttpUpstream } from './http';

/** Decodes a base64 SSE payload back into frame bytes. */
function b64(s: string): Uint8Array {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

/**
 * SSE rung -- a long-lived HTTP response downstream, POSTs upstream.
 *
 * Includes a buffering detector. An intermediary that accumulates the stream
 * and releases it in bursts produces a game that looks connected but plays in
 * lurches, which is worse than an honest failure. The signature is: a long
 * silence, then several events within a few milliseconds. When that is seen,
 * this rung reports failure so the ladder drops to long-poll.
 */
export class SseTransport implements ClientTransport {
  readonly kind = TransportKind.SSE;
  readonly name = 'SSE';

  private es: EventSource | null = null;
  private up: HttpUpstream | null = null;
  private proof = new ProofOfLife();
  private lastEventAt = 0;
  private burst = 0;
  private sawGap = false;
  private bufferingDetected = false;
  private onBuffered: (() => void) | null = null;

  open(session: SessionHandle, onFrame: FrameHandler, probe: Uint8Array): Promise<void> {
    const url = `${session.base}/api/stream?s=${encodeURIComponent(session.id)}`;
    const es = new EventSource(url);
    this.es = es;
    this.up = new HttpUpstream(session, onFrame);

    // The probe must not be sent until the stream is actually established.
    // Sent early, the upstream POST would arrive first and the server would
    // answer the PING on that response instead -- claiming the downstream and
    // leaving the SSE stream to time out. Waiting for `onopen` removes the race.
    es.onopen = () => {
      this.up?.send(probe);
    };

    es.addEventListener('d', (ev) => {
      const now = performance.now();
      this.detectBuffering(now);
      this.lastEventAt = now;

      const buf = b64((ev as MessageEvent<string>).data);
      if (!this.proof.settled) {
        const { frames } = parseFrames(buf);
        if (frames.some((f) => f.type === MsgType.SNAPSHOT || f.type === MsgType.PONG)) {
          this.proof.prove();
        }
      }
      onFrame(buf);
    });

    es.onerror = () => {
      // EventSource retries on its own; only a failure BEFORE proof of life
      // should sink the rung.
      if (!this.proof.settled) {
        this.proof.fail(new TransportFailed(this.name, 'stream error before first snapshot'));
      }
    };

    return this.proof.promise;
  }

  /**
   * A gap over 500ms followed by 5+ events under 5ms apart means something
   * between us and the server is batching the stream.
   */
  private detectBuffering(now: number): void {
    if (this.lastEventAt === 0) return;
    const gap = now - this.lastEventAt;
    if (gap > 500) {
      this.sawGap = true;
      this.burst = 0;
    } else if (this.sawGap && gap < 5) {
      this.burst++;
      if (this.burst >= 5 && !this.bufferingDetected) {
        this.bufferingDetected = true;
        this.onBuffered?.();
      }
    } else if (gap > 50) {
      this.sawGap = false;
      this.burst = 0;
    }
  }

  onBuffering(cb: () => void): void {
    this.onBuffered = cb;
  }

  get buffered(): boolean {
    return this.bufferingDetected;
  }

  send(frame: Uint8Array): void {
    this.up?.send(frame);
  }

  close(): void {
    this.es?.close();
    this.es = null;
    this.up?.close();
    this.up = null;
  }

  isOpen(): boolean {
    return this.es !== null && this.es.readyState !== EventSource.CLOSED;
  }

  rttMs(): number {
    return this.up?.rtt ?? 0;
  }
}
