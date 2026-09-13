#ifndef PONG_HTTPS_PC_H
#define PONG_HTTPS_PC_H

/*
 * A small HTTPS client for the desktop, over mbedTLS.
 *
 * WHY THIS EXISTS. The desktop client speaks raw framed TCP, and the public
 * server is reachable only on 443 through a Cloudflare tunnel -- verified, not
 * assumed: /healthz answers over TLS while TCP 8787 is refused from outside.
 * So a desktop away from the LAN could not play at all.
 *
 * WHY IT IS NOT 3ds/source/net/https.c. That file does the same job and does it
 * well, having survived a header buffer sized by measurement, a 415 from
 * content negotiation, and a redirect chain to release assets. It is also
 * sixteen references deep into libctru, and it cannot be built or run on the
 * machine this was written on -- so making it portable would have meant
 * refactoring working, hard-won code blind. The lessons are carried over; the
 * risk is not.
 *
 * Specifically carried over:
 *   - an 8KB response header buffer, because GitHub sends 5KB of CSP and the
 *     original 1KB guess broke every request before it saw a status line
 *   - an Accept header of anything-goes, because a JSON endpoint asked for
 *     octet-stream answers 415
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct PongHttps PongHttps;

typedef enum {
    PONG_HTTPS_OK = 0,
    PONG_HTTPS_ERR_CONNECT = -1,
    PONG_HTTPS_ERR_TLS = -2,
    PONG_HTTPS_ERR_WRITE = -3,
    PONG_HTTPS_ERR_READ = -4,
    PONG_HTTPS_ERR_PROTOCOL = -5,
    PONG_HTTPS_ERR_TOOBIG = -6,
} PongHttpsResult;

/**
 * Opens a TLS connection.
 *
 * `verify` off is for a LAN box with a self-signed certificate and nothing
 * else; it is never the default and the caller has to ask for it.
 */
/**
 * `tls` false speaks plain HTTP on the same code path.
 *
 * Not a hedge: a server you host yourself on a spare port very often has no
 * certificate, and the address bar already says which you meant -- http:// or
 * a bare host:port is plaintext, https:// or a bare hostname is TLS. Refusing
 * plaintext would mean the client could reach the one deployment behind a
 * tunnel and nothing anyone stood up themselves.
 */
PongHttps *pong_https_open(const char *host, uint16_t port, bool tls, bool verify,
                           char *err, size_t errcap);

void pong_https_close(PongHttps *h);

/** True while the connection is still usable for another request. */
bool pong_https_alive(const PongHttps *h);

/**
 * The session id the server last handed back, or "" before one exists.
 *
 * Captured from the X-Pong-Session response header rather than parsed out of a
 * body, because that is where the server puts it and because every request
 * after the first has to send it back.
 */
const char *pong_https_session(const PongHttps *h);

/**
 * One POST, keep-alive.
 *
 * `body` may be NULL for an empty one. The response body lands in `out` and the
 * length in `out_len`; the HTTP status is returned through `status`.
 */
PongHttpsResult pong_https_post(PongHttps *h, const char *path,
                                const char *extra_header,
                                const uint8_t *body, size_t body_len,
                                uint8_t *out, size_t out_cap, size_t *out_len,
                                int *status);

#endif /* PONG_HTTPS_PC_H */
