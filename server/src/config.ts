/** Typed environment config with defaults that work for local `npm run dev`. */

function num(name: string, dflt: number): number {
  const raw = process.env[name];
  if (raw === undefined || raw === '') return dflt;
  const n = Number(raw);
  if (!Number.isFinite(n)) throw new Error(`${name} must be a number, got ${raw}`);
  return n;
}

function str(name: string, dflt: string): string {
  return process.env[name] ?? dflt;
}

export const config = {
  /** HTTP + WebSocket. CapRover routes pong.wardcrew.com here. */
  httpPort: num('HTTP_PORT', 8788),
  /** Raw TCP for the 3DS on the LAN. Published on the host by CapRover. */
  tcpPort: num('TCP_PORT', 8787),
  tickHz: num('TICK_HZ', 60),
  /** Directory containing the built web client; empty disables static serving. */
  staticDir: str('STATIC_DIR', ''),
  publicOrigin: str('PUBLIC_ORIGIN', 'https://pong.wardcrew.com'),
  logLevel: str('LOG_LEVEL', 'info'),
  /** Cap on concurrent sessions, to bound memory on a busy shared host. */
  maxSessions: num('MAX_SESSIONS', 512),
  /**
   * Monotonic build number, compared by the 3DS auto-updater.
   *
   * Bumped by the deploy, not by the clock: a 3DS whose RTC is wrong (common)
   * must still be able to tell "newer" from "older".
   */
  buildId: num('BUILD_ID', 1),
} as const;

export const TICK_MS = 1000 / config.tickHz;
