/**
 * WebSocket transport -- the fastest rung when it survives the tunnel.
 *
 * Frames queued during one server tick are coalesced into a single binary
 * message. Sending each 32-byte snapshot as its own message would multiply
 * per-message framing and syscall overhead for no benefit.
 */

import type { WebSocket } from 'ws';
import { TransportKind } from '../../../../shared/gen/protocol.js';
import { concatFrames } from '../../../../shared/gen/protocol.js';
import type { Sink } from '../sink.js';

export class WsSink implements Sink {
  readonly kind = TransportKind.WS;
  readonly pull = false;

  constructor(private ws: WebSocket) {}

  write(frames: Uint8Array[]): void {
    if (this.ws.readyState !== 1 /* OPEN */) return;
    this.ws.send(concatFrames(frames), { binary: true });
  }

  close(_code: number): void {
    try {
      this.ws.close();
    } catch {
      /* already gone */
    }
  }

  isOpen(): boolean {
    return this.ws.readyState === 1;
  }
}
