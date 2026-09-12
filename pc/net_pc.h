/*
 * Plain TCP for the PC build.
 *
 * Deliberately not the console's net.c. That file carries a transport ladder,
 * a worker thread, mbedTLS and a pile of 3DS socket workarounds, all of which
 * exist because the console has to reach a server through Cloudflare from
 * someone's living room. A PC talking to a LAN server needs a socket.
 *
 * The HTTPS path is not ported and is not missing: play over the internet from
 * a PC by opening the web client, which already does it better.
 */

#ifndef PONG_NET_PC_H
#define PONG_NET_PC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct PongNetPC PongNetPC;

/** Connects, or returns NULL and fills `err`. Blocks for up to `timeout_ms`. */
PongNetPC *pong_pc_connect(const char *host, uint16_t port, uint32_t timeout_ms,
                           char *err, size_t errcap);

void pong_pc_close(PongNetPC *n);

/** Non-blocking. Returns bytes read, 0 for nothing waiting, -1 once closed. */
int pong_pc_recv(PongNetPC *n, uint8_t *buf, size_t cap);

bool pong_pc_send(PongNetPC *n, const uint8_t *buf, size_t len);

#endif /* PONG_NET_PC_H */
