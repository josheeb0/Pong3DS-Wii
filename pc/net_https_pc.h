#ifndef PONG_NET_HTTPS_PC_H
#define PONG_NET_HTTPS_PC_H

/*
 * The desktop's HTTPS transport, for reaching a server that is only exposed on
 * 443 -- which is every deployment behind a tunnel, including this project's.
 *
 * Same four calls as pc/net_pc.h so the app can hold either. The one behavioural
 * difference worth knowing: nothing arrives unasked, so recv() is what drives
 * the exchange and returning 0 means "nothing yet", not "idle socket".
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct PongNetHttps PongNetHttps;

PongNetHttps *pong_https_net_connect(const char *host, uint16_t port,
                                     bool tls, bool verify,
                                     char *err, size_t errcap);
void pong_https_net_close(PongNetHttps *n);
int  pong_https_net_recv(PongNetHttps *n, uint8_t *buf, size_t cap);
bool pong_https_net_send(PongNetHttps *n, const uint8_t *buf, size_t len);
const char *pong_https_net_error(const PongNetHttps *n);

/** How many times the connection had to be re-dialled. Diagnostic only. */
uint32_t pong_https_net_reconnects(const PongNetHttps *n);

#endif /* PONG_NET_HTTPS_PC_H */
