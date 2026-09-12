# Netcode

Server-authoritative, 60Hz, integer-only. Clients predict their own paddle and
interpolate everything else.

## The problem

Two players on wildly different links must share one simulation:

| Client | Transport | Realistic rate |
|--------|-----------|----------------|
| Browser, WebSocket | push | 30Hz, ~5ms RTT on LAN |
| Browser, SSE | push | 30Hz |
| Browser, long-poll | pull | ~10Hz round trips |
| 3DS on LAN | raw TCP | 30Hz, ~1ms |
| 3DS over HTTPS | pull | ~10Hz, 100–250ms |

A design that only works at 30Hz would exclude half of that table.

## 1. Absolute targets, never deltas

`INPUT.desiredYQ4` is where the paddle **should be**, not how far to move it.

- **Loss tolerance.** A dropped absolute target costs nothing — the next one is
  still correct. A dropped delta is a permanent desync, and the HTTP path offers
  no delivery guarantee worth relying on.
- **Reorder tolerance.** Stale inputs are discarded by sequence number. Deltas
  would require ordering.
- **Coalescing.** The 3DS worker thread and the browser's 20Hz upstream both
  collapse many 60fps samples into one send. With absolute targets that is "keep
  the newest". With deltas it is "sum them", which breaks the moment one send is
  lost.
- **It matches the hardware.** Touch and mouse are already absolute. The circle
  pad integrates locally at 60fps, so pad feel stays full-rate on a 10Hz link.

The obvious objection — a client could request a teleport — is removed by the
clamp below, so there is no anti-cheat argument for input-axis either.

## 2. The server steps paddles every tick regardless of input

```
target = clamp(desiredY, PADDLE_HALF, FIELD_H - PADDLE_HALF)
step   = clamp(target - current, -MAX_PADDLE_SPEED, +MAX_PADDLE_SPEED)
current += step
```

Because this runs on **every** tick whether or not fresh input arrived, a 10Hz
client's paddle keeps gliding toward its last stated target for the six ticks
between updates. This is the mechanism that makes a slow client feel continuous
rather than steppy — and it only works because the input is an absolute target.

The clients run the identical function (`shared/sim/paddle.ts`, mirrored by
`pong_step_paddle` in C), so local prediction of your own paddle is *exact* under
zero loss. One definition, imported by both, because a divergence here shows up
as a paddle that drifts and snaps under load — subtle and maddening to debug.

## 3. Snapshot history, not just current state

The room keeps one second of per-tick snapshots (`C.SNAP_RING = 60`).

A client's `INPUT.lastTickSeen` says what it holds. Building a response is: walk
the ring from there, take every 2nd tick (30Hz), cap at 8 frames.

So a 10Hz poller receives the newest state **plus the intermediate sample points
between its polls**. Its interpolator then traces the ball's real path instead of
a straight chord across the curve. Measured against the live server:

```
responses       : 81 in 8s (10.1/s, asked 10)
snapshots       : 292 (36.5/s effective)
snapshots/resp  : avg 3.65
tick gap in batch: avg 1.75 ticks   (2 == true 30Hz sampling)
```

**Push transports must not force-include the newest tick.** They drain every
tick, so they would receive the decimated frame *and* the newest one — silently
doubling a 30Hz stream to 60Hz. `Room.snapshotsSince(..., includeNewest)` is true
only for pullers. (This was a real bug, caught by the C integration test
reporting 60 snapshots/sec on a transport documented as 30.)

## 4. Rendering in the past

Clients render at `now - renderDelay`, so two real snapshots almost always
bracket the moment being drawn.

`renderDelay` is derived from the **arrival cadence actually observed**, not from
the transport's nominal rate:

```
delay = clamp(median(arrival gaps) * 2 + 20, 50, 250)
```

That distinction matters: a long-poll client receives batched 30Hz snapshots but
receives them in ~100ms bursts, so its buffer must absorb the burst spacing, not
the sample spacing. Measuring gets this right without the interpolator needing to
know which transport it is sitting on.

The render cursor is **clamped to the range actually held**. That clamp is what
keeps the interpolator stable when the clock estimate is briefly wrong: a bad
estimate degrades to "slightly stale" rather than to an empty view or a stutter.

Snapshots flagged `KEYFRAME` (a serve, a goal, the end of a countdown) are
snapped to rather than interpolated across — otherwise the ball would slide
smoothly from where it died to where it was re-served.

## 5. Clock sync

PING every 2s, keep 16 samples, use the one with **minimum RTT**:

```
offset = serverTick + rtt/2 - localTick
```

Minimum rather than mean because the least-queued exchange is the least distorted
by buffering, which is exactly the noise being filtered. The offset then eases
toward new estimates rather than jumping, since a jump moves the render cursor
and reads as a stutter even when the new estimate is better.

## 6. Fairness: slow mode

When either seat is on a ~10Hz transport, the room lowers the ball's speed
ceiling for **both** players and flags it in `MATCH_START`.

A 220ms render delay is a much larger fraction of a field traverse at full speed
than at reduced speed. This is a game-design lever rather than a netcode one, and
it is the honest fix: the alternative is a browser player on WebSocket
out-reacting a 3DS on HTTPS every single rally.

## What is deliberately absent

**Lag compensation / rollback.** Pong has one moving object and it is
authoritative. Rewinding paddle positions to honour a laggy player's view would
let them score through the other player's paddle. Not worth it for this game.

**Delta-compressed snapshots.** At 32 bytes framed there is nothing to win, and
it would cost the C side a per-connection baseline state machine.

## Transport ladder (browser)

| Rung | Budget to prove itself | Notes |
|------|------------------------|-------|
| WebSocket | 2500ms | fastest when the path allows an upgrade |
| SSE | 3000ms | plus anti-buffering headers and a 2KB preamble |
| Long-poll | 4000ms | 20s hold, well under Cloudflare's ~100s idle cut |

A rung proves itself by a **frame arriving**, not by connecting. An upgrade that
succeeds and then silently delivers nothing is precisely the failure a hostile
proxy produces, and resolving on `onopen` would sail past it into a frozen game.

The probe is a PING, whose PONG is the proof. It cannot be a SNAPSHOT: snapshots
only exist once you are in a room, and joining requires a working transport — so
waiting for one would deadlock. (It did, the first time.)

The working rung is cached in `localStorage` for 10 minutes, so a blocked
WebSocket is not re-probed on every page load. `?transport=ws|sse|poll` forces
one for testing.
