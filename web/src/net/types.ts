import type { TransportKind } from '../../../shared/gen/protocol';

export type FrameHandler = (buf: Uint8Array) => void;

export interface SessionHandle {
  /** Hex session id from the bootstrap POST. */
  id: string;
  /** Base URL for API calls, no trailing slash. */
  base: string;
}

/**
 * One rung of the ladder.
 *
 * `open` deliberately resolves on a frame ARRIVING rather than on connect. A
 * WebSocket that upgrades and then silently delivers nothing is exactly the
 * failure mode a hostile proxy produces, and resolving on `onopen` would sail
 * straight past it into a frozen game.
 *
 * `probe` is sent as soon as the channel is usable -- a PING, whose PONG is the
 * proof. It cannot be a SNAPSHOT: those only exist once the player is in a
 * room, and joining requires a working transport, so waiting for one here would
 * deadlock. A PONG traverses the identical path, so the detection property is
 * unchanged.
 */
export interface ClientTransport {
  readonly kind: TransportKind;
  readonly name: string;
  open(session: SessionHandle, onFrame: FrameHandler, probe: Uint8Array): Promise<void>;
  /** Queue a frame for delivery. Implementations may batch. */
  send(frame: Uint8Array): void;
  close(): void;
  isOpen(): boolean;
  /** Observed round-trip in ms, or 0 if not yet measured. */
  rttMs(): number;
}

export class TransportFailed extends Error {
  constructor(public readonly rung: string, message: string) {
    super(`${rung}: ${message}`);
    this.name = 'TransportFailed';
  }
}

/** Resolves when `p` settles, or rejects at `ms`. */
export function withTimeout<T>(p: Promise<T>, ms: number, rung: string, what: string): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const t = setTimeout(() => reject(new TransportFailed(rung, `timed out after ${ms}ms ${what}`)), ms);
    p.then(
      (v) => { clearTimeout(t); resolve(v); },
      (e) => { clearTimeout(t); reject(e); },
    );
  });
}

/**
 * A promise that a transport resolves once it has proof of life.
 *
 * Used by every rung so the ladder's success condition is uniform.
 */
export class ProofOfLife {
  readonly promise: Promise<void>;
  private done = false;
  private resolveFn!: () => void;
  private rejectFn!: (e: Error) => void;

  constructor() {
    this.promise = new Promise<void>((res, rej) => {
      this.resolveFn = res;
      this.rejectFn = rej;
    });
  }

  prove(): void {
    if (this.done) return;
    this.done = true;
    this.resolveFn();
  }

  fail(e: Error): void {
    if (this.done) return;
    this.done = true;
    this.rejectFn(e);
  }

  get settled(): boolean {
    return this.done;
  }
}
