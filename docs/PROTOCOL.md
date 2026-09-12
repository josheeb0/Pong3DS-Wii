# Wire protocol

Generated from `shared/protocol.json` by `tools/gen-protocol.mjs` into:

- `shared/gen/protocol.ts` — TypeScript codec (server + web)
- `3ds/source/net/pong_proto.{h,c}` — C codec (3DS, and reusable by a Wii client)
- `shared/gen/golden.json` + `3ds/test/golden_data.h` — canonical byte vectors

**Do not hand-edit the generated files.** Change `shared/protocol.json` and run
`make proto`. `make verify` fails if the committed output is stale.

## Why binary, and why the same bytes everywhere

The 3DS has no JSON parser in libctru. A JSON path for the browser would mean
either hand-writing one in C or maintaining two protocols — and two protocols
means a desync class that only appears in cross-platform matches, which is the
one case this project exists to support.

A snapshot is **32 bytes framed**; the JSON equivalent is roughly 180. On the
10Hz HTTP path with 8 snapshots batched per response that is 256 bytes versus
~1.5KB, per round trip, through a tunnel, on a console with a small socket
buffer.

Debuggability is preserved by `GET /api/stats` and the browser's debug drawer
rather than by making the hot path human-readable.

## Framing

Every message is **fixed size** — there are no variable-length fields anywhere.
That makes the C side trivial and removes a whole category of parsing bug.

Header, 8 bytes, little-endian (ARM11 and x86 are both LE, so this is a plain
load on every target we care about):

| Offset | Type | Field | Notes |
|--------|------|-------|-------|
| 0 | u8 | magic0 | `0x50` `'P'` |
| 1 | u8 | magic1 | `0x47` `'G'` |
| 2 | u8 | version | `1`; mismatch is rejected |
| 3 | u8 | type | see below |
| 4 | u16 | length | payload bytes following |
| 6 | u16 | seq | per-sender counter, wraps; diagnostics only |

The magic exists so a desynced stream is distinguishable from a partial frame.
Without it, garbage and "wait for more bytes" look identical and the parser
cannot fail safely.

`length` is what makes one protocol work on four transports: a TCP reader
accumulates bytes and pops frames; an HTTP body is a buffer that happens to
contain N frames; a WebSocket message is the same; an SSE `data:` line is the
same after base64 decoding. One parser, four carriers.

## Messages

| Code | Name | Dir | Payload | Purpose |
|------|------|-----|---------|---------|
| 0x01 | HELLO | C→S | 24 | platform, transport, name, build |
| 0x02 | WELCOME | S→C | 28 | session id, server time, rates, field size |
| 0x03 | JOIN | C→S | 8 | quickmatch / room code / vs bot |
| 0x04 | MATCH_START | S→C | 32 | seat, opponent, dimensions, win score, flags |
| 0x05 | INPUT | C→S | 12 | seq, **lastTickSeen**, absolute paddle Y, buttons |
| 0x06 | SNAPSHOT | S→C | 24 | tick, ball, paddles, score, state, flags |
| 0x07 | EVENT | S→C | 8 | goal, match over, opponent left, countdown |
| 0x08 | PING | C→S | 8 | clock sync |
| 0x09 | PONG | S→C | 12 | echo + server time + server tick |
| 0x0A | BYE | S→C | 4 | reason code |
| 0x0B | LEAVE | C→S | 0 | |
| 0x0D | RESUME | C→S | 20 | reattach an existing session after a transport switch |

`INPUT.lastTickSeen` is the field that makes batching work — it tells the server
exactly which snapshots this client is missing. See `docs/NETCODE.md`.

## Coordinates

Virtual field **800 × 480**, origin top-left, +Y down. Paddle and ball positions
are **centres**.

800 × 480 is exactly 2× the 3DS top screen (400 × 240), so the console's mapping
is a single shift with no rounding:

```c
#define PONG_Q4_TO_TOPSCREEN(q) ((q) >> (PONG_Q4_SHIFT + 1))
```

Fixed point is **Q4** (1/16 px). Field maxima are `12800 × 7680`, comfortably
inside `i16` with headroom for a ball that has left the field during a goal.
Signed throughout, so an off-field position cannot wrap.

Everything is integer. The clients re-simulate and interpolate, and `double` on
V8 versus `float` on ARM11 would drift apart; integers cannot. Bounce angles come
from a shared 65-entry Q12 sine table (`shared/gen/trig.ts` /
`3ds/source/game/pong_trig.h`, generated together so the values are identical)
rather than from each platform's `sin()`.

## Carrying frames over each transport

| Transport | How |
|-----------|-----|
| TCP | Bytes appended to the stream; reader keeps any partial tail |
| WebSocket | One binary message per batch, coalesced per server tick |
| SSE | `event: d` / `data: <base64 of concatenated frames>` — SSE is text-only, and 33% on 32 bytes is 11 bytes |
| HTTP RR | Request body = client frames; response body = server frames |

The HTTP case is the one worth noting: **input travels up on the same round trip
that brings state down**, which halves the request count. On a transport where a
round trip may cost 200ms, that is the difference between playable and not.
