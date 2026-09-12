/**
 * The transport seam.
 *
 * Everything above this interface — sessions, rooms, the simulation — has no
 * idea whether bytes leave over a TCP socket, a WebSocket, an SSE stream or an
 * HTTP response body. That is the whole reason a 10Hz polling 3DS and a 60Hz
 * WebSocket browser can share one server without a second code path.
 */

import type { TransportKind } from '../../../shared/gen/protocol.js';

export interface Sink {
  readonly kind: TransportKind;

  /**
   * True for request/response transports (long-poll, the 3DS RR path), where
   * the client drives the cadence and tells us what it has already seen.
   *
   * Push transports advance their own idea of what the client has, because
   * asking a 30Hz stream to acknowledge every snapshot would turn an
   * intentionally lossy channel into a reliable one.
   */
  readonly pull: boolean;

  /** Queued frames leave together. Never called with an empty array. */
  write(frames: Uint8Array[]): void;

  close(code: number): void;

  /** False once the underlying connection is gone. */
  isOpen(): boolean;
}
