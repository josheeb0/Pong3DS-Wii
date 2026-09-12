/*
 * Transport abstraction for the 3DS client.
 *
 * Two implementations behind one interface, mirroring the browser's ladder:
 *
 *   LAN  -- raw TCP to the server's 8787. Full 60Hz, sub-millisecond on wifi,
 *           and the protocol's length-prefixed frames make the byte stream
 *           self-delimiting, so no extra framing is needed.
 *
 *   WEB  -- HTTPS request/response to pong.wardcrew.com, using our own bundled
 *           mbedTLS (see net/https.h for why we do not use libctru's httpc).
 *           One POST carries pending input AND returns a batch of snapshots,
 *           which halves the round-trip count -- the single most important
 *           optimisation when each trip may cost 200ms.
 *
 * Everything above this interface is transport-agnostic; only the bottom-screen
 * status text knows which one is live.
 */

#ifndef PONG_NET_H
#define PONG_NET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PONG_LINK_IDLE = 0,
    PONG_LINK_CONNECTING,
    PONG_LINK_OPEN,
    PONG_LINK_FAILED,
} PongLinkState;

typedef enum {
    PONG_MODE_AUTO = 0,
    PONG_MODE_LAN,
    PONG_MODE_WEB,
} PongNetMode;

typedef struct {
    char        lan_host[64];
    uint16_t    lan_port;
    char        lan_subnet[24];   /* e.g. "192.168.4." -- skip LAN if not on it */
    char        web_host[96];
    uint16_t    web_port;
    bool        web_tls;
    /* Verify the server certificate against romfs:/cacert.pem. On by default;
     * see https.h for how clock errors are handled without disabling it. */
    bool        web_verify;
    char        web_path[64];
    PongNetMode mode;
} PongNetConfig;

typedef struct PongNet PongNet;

/** Allocates and starts connecting. Returns NULL only on allocation failure. */
PongNet *pong_net_open(const PongNetConfig *cfg);

void pong_net_close(PongNet *n);

PongLinkState pong_net_state(const PongNet *n);

/** "LAN TCP 192.168.4.29:8787" or "HTTPS pong.wardcrew.com" -- for the HUD. */
const char *pong_net_describe(const PongNet *n);

/** Last error, for the bottom screen when a connection fails. */
const char *pong_net_error(const PongNet *n);

/** Observed round-trip in ms, 0 if unknown. */
uint32_t pong_net_rtt(const PongNet *n);

/** Effective updates per second, for the HUD. */
uint32_t pong_net_hz(const PongNet *n);

/**
 * Queues a frame for delivery. Never blocks.
 *
 * On the HTTP path the queue is drained by a worker thread; INPUT frames
 * naturally coalesce there because an absolute target supersedes the one
 * behind it.
 */
bool pong_net_send(PongNet *n, const uint8_t *frame, size_t len);

/**
 * Copies received bytes into `buf`. Returns how many.
 *
 * Call once per frame. The caller pops complete frames with
 * pong_parse_frame(); any partial tail is retained internally.
 */
size_t pong_net_recv(PongNet *n, uint8_t *buf, size_t cap);

/** Session id for the HTTP path, hex. Empty on the LAN path. */
const char *pong_net_session(const PongNet *n);

#endif /* PONG_NET_H */
