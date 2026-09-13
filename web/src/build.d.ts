/**
 * Injected by Vite's `define` at build time from the BUILD_ID build arg.
 *
 * A constant, not a variable: it is substituted into the bundle textually, so
 * it is visible in the shipped JavaScript and cannot drift from the bundle it
 * was built with. That is the whole point -- it answers "which bundle is this
 * browser running", which /healthz cannot, because /healthz describes the
 * server and a browser can be holding a cached one.
 */
declare const __BUILD_ID__: number;
