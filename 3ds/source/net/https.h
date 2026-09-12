/*
 * Minimal HTTPS/1.1 client for the 3DS, over mbedTLS on raw BSD sockets.
 *
 * Exists because libctru's httpc delegates TLS to the console's system SSL
 * module, whose cipher list is fixed in firmware and may not include the
 * ECDHE-ECDSA suites our Cloudflare edge requires. Owning the TLS stack means
 * the handshake is ours to debug.
 *
 * Scope is deliberately tiny: one connection, keep-alive reuse, POST and GET
 * with a binary body, Content-Length and chunked responses. That is all the
 * game protocol needs.
 */

#ifndef PONG_HTTPS_H
#define PONG_HTTPS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct HttpsConn HttpsConn;

typedef enum {
    HTTPS_OK = 0,
    HTTPS_ERR_RESOLVE = -1,
    HTTPS_ERR_CONNECT = -2,
    HTTPS_ERR_HANDSHAKE = -3,
    HTTPS_ERR_WRITE = -4,
    HTTPS_ERR_READ = -5,
    HTTPS_ERR_PROTOCOL = -6,
    HTTPS_ERR_TOOBIG = -7,
    HTTPS_ERR_ENTROPY = -8,
    HTTPS_ERR_CLOSED = -9,
} HttpsResult;

/** One-time global init (entropy + DRBG). Call after socInit(). */
HttpsResult https_global_init(void);
void https_global_exit(void);

/**
 * Opens a connection.
 *
 * `verify` enables certificate validation against the trust roots shipped in
 * romfs:/cacert.pem (see 3ds/vendor/fetch-cacerts.sh).
 *
 * Validation is deliberately NOT all-or-nothing. The console's real-time clock
 * is frequently wrong, which fails the validity window on certificates that are
 * otherwise perfectly good -- a clock problem, not a security one. So the chain,
 * the issuer and the hostname are all required, while an expired-or-not-yet-valid
 * complaint is tolerated. An attacker still cannot present a certificate they
 * have no valid chain for.
 *
 * With `verify` true and no CA bundle present, this FAILS rather than silently
 * continuing unverified.
 *
 * `use_tls` false gives a plain HTTP connection on the same API, which is what
 * the LAN path uses.
 */
HttpsConn *https_open(const char *host, uint16_t port, bool use_tls, bool verify);

/**
 * Why the last https_open() failed.
 *
 * Valid only immediately after https_open() returns NULL. Kept globally because
 * the connection object -- and its error buffer -- is freed on failure.
 */
const char *https_open_error(void);

/** True if the connection is still usable for another keep-alive request. */
bool https_is_open(const HttpsConn *c);

void https_close(HttpsConn *c);

/*
 * Redirect targets are large. A GitHub release asset redirects to a signed URL
 * of roughly 1100 characters -- a JWT plus an Azure SAS token -- so anything
 * sized for "a URL" rather than measured against a real one will silently
 * truncate and produce a baffling 400 from the next hop.
 */
#define HTTPS_MAX_URL 2048

typedef struct {
    int      status;        /* HTTP status code, e.g. 200 / 204 / 404 */
    size_t   body_len;
    uint32_t elapsed_ms;
    /* Value of a single response header we care about, or empty. */
    char     session[64];
    /* Set on a 3xx; empty otherwise. */
    char     location[HTTPS_MAX_URL];
} HttpsResponse;

/**
 * Sends one request and reads the whole response.
 *
 * `method` is "POST" or "GET". `session_hdr`, when non-NULL, is sent as
 * X-Pong-Session. Body may be NULL/0 for GET.
 *
 * The connection is kept alive on success so the next call skips the TLS
 * handshake -- which on a 67MHz ARM11 is by far the dominant cost and the
 * difference between a usable poll rate and a slideshow.
 */
HttpsResult https_request(HttpsConn *c,
                          const char *method,
                          const char *path,
                          const char *session_hdr,
                          const uint8_t *body, size_t body_len,
                          uint8_t *out, size_t out_cap,
                          HttpsResponse *resp);

/**
 * GET a URL, following redirects across hosts.
 *
 * Opens and closes its own connections, because a redirect usually lands on a
 * different host and the existing keep-alive connection cannot serve it. That
 * makes this the wrong tool for the game's hot path -- it is for fetching an
 * update, where a few extra handshakes do not matter.
 *
 * `url` must be absolute (https://host[:port]/path or http://...).
 */
HttpsResult https_get_url(const char *url, const char *session_hdr,
                          uint8_t *out, size_t out_cap,
                          bool verify, int max_redirects,
                          HttpsResponse *resp);

/** Human-readable form of the last mbedTLS error, for on-screen diagnostics. */
const char *https_last_error(const HttpsConn *c);

/** True once romfs:/cacert.pem has been loaded successfully. */
bool https_have_ca(void);

/**
 * mbedTLS verification flags from the handshake, 0 if clean.
 *
 * Non-zero with a successful connection means clock-only complaints were
 * tolerated; worth surfacing so a wrong console clock is diagnosable.
 */
uint32_t https_verify_flags(const HttpsConn *c);

/** Negotiated TLS version and cipher suite, for the probe's report. */
const char *https_tls_version(const HttpsConn *c);
const char *https_ciphersuite(const HttpsConn *c);

#endif /* PONG_HTTPS_H */
