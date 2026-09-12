/**
 * Raw TCP -- the 3DS's LAN fast path.
 *
 * No framing of its own: the protocol's length-prefixed header already makes
 * the byte stream self-delimiting, which is exactly why the same parser works
 * here and inside an HTTP body.
 */

import type { Socket } from 'node:net';
import { TransportKind, concatFrames } from '../../../../shared/gen/protocol.js';
import type { Sink } from '../sink.js';

export class TcpSink implements Sink {
  readonly kind = TransportKind.TCP;
  readonly pull = false;
  private open = true;

  constructor(private sock: Socket) {
    sock.on('close', () => { this.open = false; });
    sock.on('error', () => { this.open = false; });
  }

  write(frames: Uint8Array[]): void {
    if (!this.open || this.sock.destroyed) return;
    this.sock.write(concatFrames(frames));
  }

  close(_code: number): void {
    this.open = false;
    try {
      this.sock.end();
    } catch {
      /* already gone */
    }
  }

  isOpen(): boolean {
    return this.open && !this.sock.destroyed;
  }
}
