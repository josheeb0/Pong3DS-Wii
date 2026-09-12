import { TransportKind, MsgType, parseFrames, concatFrames } from '../../../shared/gen/protocol';
import { ProofOfLife, TransportFailed, type ClientTransport, type FrameHandler, type SessionHandle } from './types';

/**
 * WebSocket rung -- lowest latency when the path allows an upgrade.
 *
 * Frames queued within one animation frame are coalesced into a single binary
 * message, matching what the server does in the other direction.
 */
export class WsTransport implements ClientTransport {
  readonly kind = TransportKind.WS;
  readonly name = 'WebSocket';

  private ws: WebSocket | null = null;
  private proof = new ProofOfLife();
  private outbox: Uint8Array[] = [];
  private flushHandle: number | null = null;
  private lastRtt = 0;

  open(session: SessionHandle, onFrame: FrameHandler, probe: Uint8Array): Promise<void> {
    const url = new URL(session.base + '/api/ws', location.href);
    url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    url.searchParams.set('s', session.id);

    const ws = new WebSocket(url.toString());
    ws.binaryType = 'arraybuffer';
    this.ws = ws;

    ws.onmessage = (ev) => {
      const buf = new Uint8Array(ev.data as ArrayBuffer);
      // Proof of life is a SNAPSHOT specifically: an upgrade that completes but
      // never carries game state is the failure we are probing for.
      if (!this.proof.settled) {
        const { frames } = parseFrames(buf);
        if (frames.some((f) => f.type === MsgType.SNAPSHOT || f.type === MsgType.PONG)) {
          this.proof.prove();
        }
      }
      onFrame(buf);
    };
    ws.onopen = () => {
      // Ask a question the server must answer, so silence is distinguishable
      // from a successful-but-dead upgrade.
      ws.send(probe);
    };
    ws.onerror = () => this.proof.fail(new TransportFailed(this.name, 'socket error'));
    ws.onclose = (e) => this.proof.fail(new TransportFailed(this.name, `closed (${e.code})`));

    return this.proof.promise;
  }

  send(frame: Uint8Array): void {
    this.outbox.push(frame);
    if (this.flushHandle === null) {
      this.flushHandle = requestAnimationFrame(() => {
        this.flushHandle = null;
        this.flush();
      });
    }
  }

  private flush(): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN || this.outbox.length === 0) return;
    this.ws.send(concatFrames(this.outbox));
    this.outbox.length = 0;
  }

  close(): void {
    if (this.flushHandle !== null) cancelAnimationFrame(this.flushHandle);
    this.flushHandle = null;
    this.ws?.close();
    this.ws = null;
  }

  isOpen(): boolean {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  rttMs(): number {
    return this.lastRtt;
  }

  noteRtt(ms: number): void {
    this.lastRtt = ms;
  }
}
