/*
 * glibc hides clock_gettime and CLOCK_MONOTONIC behind this under -std=c99, so
 * without it the Linux build fails on a struct it cannot see -- while macOS
 * compiles the same file happily. pc/net_pc.c carries the identical guard for
 * the identical reason; this is the second time it has bitten.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
  #define _POSIX_C_SOURCE 200809L
#endif

#include "net_https_pc.h"
#include "https_pc.h"

#if defined(_WIN32)
#  include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The desktop's HTTPS transport.
 *
 * Same shape as the raw TCP one -- connect, send, recv -- so the app does not
 * care which is underneath. The difference is that nothing arrives unasked:
 * there is no socket to drain, so recv() is what drives the exchange.
 *
 * ONE POST DOES BOTH DIRECTIONS. Pending outbound frames go up as the body and
 * whatever the server has queued comes back as the response, which halves the
 * round trips against a path that already costs a tunnel hop. The 3DS client
 * does the same thing for the same reason.
 */

#define OUT_MAX  4096
#define IN_MAX   32768

/*
 * How often to poll while nothing is happening.
 *
 * The server holds an /api/rpc for up to 20 seconds when asked to wait, but
 * this client does not use that: a held request cannot carry input that the
 * player generates halfway through it. So it polls, and the interval is a
 * compromise -- paddle updates want to go out at 10Hz or better, and every poll
 * is a request through a tunnel.
 */
#define POLL_MS 60

struct PongNetHttps {
    PongHttps *h;
    char host[128];
    uint16_t port;
    bool tls;
    bool verify;

    uint8_t out[OUT_MAX];
    size_t  out_len;

    uint8_t in[IN_MAX];
    size_t  in_len, in_pos;

    uint32_t last_poll_ms;
    bool     failed;
    char     err[192];

    /*
     * The session id, kept HERE rather than only on the TLS handle.
     *
     * It has to outlive the connection: a keep-alive socket through a tunnel
     * gets closed regularly -- by the tunnel, by the server, by an idle window
     * -- and the whole point of the protocol carrying a session id is that
     * identity survives that. Reconnecting with the same id resumes the match;
     * reconnecting without it starts a new one, which looks to the player like
     * being thrown out of a game they were winning.
     */
    char session[80];
    uint32_t reconnects;
};

static uint32_t now_ms(void)
{
    /* Only has to be monotonic-ish and cheap; the caller supplies real timing. */
#if defined(_WIN32)
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

PongNetHttps *pong_https_net_connect(const char *host, uint16_t port,
                                     bool tls, bool verify,
                                     char *err, size_t errcap)
{
    PongNetHttps *n = (PongNetHttps *)calloc(1, sizeof *n);
    if (!n) { snprintf(err, errcap, "out of memory"); return NULL; }

    snprintf(n->host, sizeof n->host, "%s", host);
    n->port = port;
    n->tls = tls;
    n->verify = verify;

    n->h = pong_https_open(host, port, tls, verify, err, errcap);
    if (!n->h) { free(n); return NULL; }

    /* The session POST returns WELCOME in its body, which the caller is about
     * to want -- so it goes straight into the inbound buffer rather than being
     * decoded here. This layer moves bytes and knows nothing about frames. */
    size_t got = 0; int status = 0;
    PongHttpsResult rc = pong_https_post(n->h, "/api/session", NULL, NULL, 0,
                                         n->in, sizeof n->in, &got, &status);
    if (rc != PONG_HTTPS_OK || status != 200) {
        snprintf(err, errcap, "session refused (rc %d, HTTP %d)", (int)rc, status);
        pong_https_close(n->h);
        free(n);
        return NULL;
    }

    n->in_len = got;
    n->in_pos = 0;
    n->last_poll_ms = now_ms();
    snprintf(n->session, sizeof n->session, "%s", pong_https_session(n->h));
    return n;
}

void pong_https_net_close(PongNetHttps *n)
{
    if (!n) return;
    pong_https_close(n->h);
    free(n);
}

bool pong_https_net_send(PongNetHttps *n, const uint8_t *buf, size_t len)
{
    if (!n || n->failed) return false;
    /*
     * Queued, not sent. Input arrives far faster than it is worth making
     * requests, and the next poll carries everything pending in one body.
     */
    if (n->out_len + len > sizeof n->out) {
        /* Dropping the OLDEST is right for this protocol: inputs are absolute
         * paddle targets, so the newest one supersedes anything behind it.
         * Dropping the newest would send stale positions and look like lag. */
        size_t drop = n->out_len + len - sizeof n->out;
        if (drop > n->out_len) drop = n->out_len;
        memmove(n->out, n->out + drop, n->out_len - drop);
        n->out_len -= drop;
    }
    if (len > sizeof n->out) return false;
    memcpy(n->out + n->out_len, buf, len);
    n->out_len += len;
    return true;
}

int pong_https_net_recv(PongNetHttps *n, uint8_t *buf, size_t cap)
{
    if (!n || n->failed) return -1;

    /* Anything already buffered goes out first, no request needed. */
    if (n->in_pos < n->in_len) {
        size_t left = n->in_len - n->in_pos;
        size_t take = left < cap ? left : cap;
        memcpy(buf, n->in + n->in_pos, take);
        n->in_pos += take;
        return (int)take;
    }

    uint32_t t = now_ms();
    bool due = (uint32_t)(t - n->last_poll_ms) >= POLL_MS;
    /* Outbound input goes immediately rather than waiting for the timer: a
     * paddle that moves should be sent now, not up to a poll interval later. */
    if (!due && n->out_len == 0) return 0;

    n->last_poll_ms = t;

    char hdr[128] = "";
    if (n->session[0]) snprintf(hdr, sizeof hdr, "X-Pong-Session: %s\r\n", n->session);

    size_t got = 0; int status = 0;
    PongHttpsResult rc = pong_https_post(n->h, "/api/rpc", hdr,
                                         n->out, n->out_len,
                                         n->in, sizeof n->in, &got, &status);

    /*
     * A dead connection is not a dead match.
     *
     * A keep-alive socket through a tunnel gets closed regularly, and treating
     * that as the end of the game was wrong: it showed up as the client
     * "randomly disconnecting" with nothing to explain it, because from the
     * player's side nothing had happened. The session id outlives the socket,
     * so the fix is to dial again and carry on.
     *
     * Only CONNECTION failures are retried. An HTTP status the server chose --
     * a 404 for a session it has forgotten, say -- is a real answer and
     * retrying it would just spin.
     */
    const bool connection_died = (rc == PONG_HTTPS_ERR_READ ||
                                  rc == PONG_HTTPS_ERR_WRITE ||
                                  rc == PONG_HTTPS_ERR_CONNECT ||
                                  !pong_https_alive(n->h));

    if (rc != PONG_HTTPS_OK && connection_died) {
        char err[192] = "";
        pong_https_close(n->h);
        n->h = pong_https_open(n->host, n->port, n->tls, n->verify, err, sizeof err);
        if (!n->h) {
            /* Bounded: the open() message can be longer than this buffer. */
            snprintf(n->err, sizeof n->err, "reconnect failed: %.150s", err);
            n->failed = true;
            return -1;
        }
        n->reconnects++;

        /* The same request again, with the same session and the same pending
         * input -- which was never cleared, precisely so this can happen. */
        got = 0; status = 0;
        rc = pong_https_post(n->h, "/api/rpc", hdr, n->out, n->out_len,
                             n->in, sizeof n->in, &got, &status);
    }

    if (rc != PONG_HTTPS_OK || status != 200) {
        snprintf(n->err, sizeof n->err, "rpc failed (rc %d, HTTP %d)", (int)rc, status);
        n->failed = true;
        return -1;
    }

    /* Only cleared once the server has actually taken them. A failed POST
     * leaves the queue intact so the next one carries the same input rather
     * than silently losing a player's last move. */
    n->out_len = 0;
    {
        const char *sess = pong_https_session(n->h);
        if (sess && sess[0]) snprintf(n->session, sizeof n->session, "%s", sess);
    }

    n->in_len = got;
    n->in_pos = 0;
    if (got == 0) return 0;

    size_t take = got < cap ? got : cap;
    memcpy(buf, n->in, take);
    n->in_pos = take;
    return (int)take;
}

uint32_t pong_https_net_reconnects(const PongNetHttps *n) { return n ? n->reconnects : 0; }

const char *pong_https_net_error(const PongNetHttps *n)
{
    return (n && n->err[0]) ? n->err : "";
}
