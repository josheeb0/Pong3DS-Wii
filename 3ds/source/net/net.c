#include "net.h"
#include "https.h"
#include "pong_proto.h"
#include "log.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

#define TX_QUEUE_BYTES 1024
#define RX_QUEUE_BYTES 4096
/*
 * Stack for the HTTPS worker.
 *
 * A TLS handshake is stack-hungry: bignum arithmetic for ECDHE, plus parsing a
 * multi-certificate chain, all on this thread. 48KB was a guess and is close to
 * the usual guidance for mbedTLS, which is not a comfortable place to be -- an
 * overflow here would present as a crash or corruption rather than an error
 * message. 128KB on a console with 64MB of userland RAM is not worth economising.
 */
#define WORKER_STACK   (128 * 1024)

struct PongNet {
    PongNetConfig cfg;
    PongNetMode   active;
    PongLinkState state;

    char desc[128];
    char err[128];
    char session[80];

    /* Kept separately so the fallback cannot erase why the primary failed. */
    char lan_err[128];
    bool lan_attempted;
    char local_ip[24];
    char diag[512];

    uint32_t rtt_ms;
    uint32_t hz;

    /* ---- LAN ---- */
    int sock;

    /* ---- WEB ---- */
    HttpsConn *https;
    Thread     worker;
    LightLock  lock;
    volatile bool stop;

    /* Shared between the worker and the main thread, under `lock`. */
    uint8_t tx[TX_QUEUE_BYTES];
    size_t  tx_len;
    uint8_t rx[RX_QUEUE_BYTES];
    size_t  rx_len;

    uint32_t req_count;
    uint32_t req_window_start;
};

/* -------------------------------------------------------------- LAN helpers */

static bool on_lan(const char *subnet, char *ip_out, size_t ip_cap)
{
    struct in_addr me;
    me.s_addr = (in_addr_t)gethostid();
    const char *s = inet_ntoa(me);
    snprintf(ip_out, ip_cap, "%s", s ? s : "0.0.0.0");
    if (!subnet || !subnet[0]) return true;
    return s && strncmp(s, subnet, strlen(subnet)) == 0;
}

/*
 * Is this a .local (mDNS/Bonjour) name?
 *
 * Worth calling out by name because the failure is otherwise baffling: the
 * address works from every laptop on the network and fails only on the console.
 * .local is resolved by multicast DNS, which the 3DS has no resolver for -- it
 * asks the configured DNS server, which has never heard of the name. Nothing
 * about the address is wrong; the console simply cannot look it up.
 */
static bool is_mdns_name(const char *h)
{
    size_t n = h ? strlen(h) : 0;
    return n > 6 && strcasecmp(h + n - 6, ".local") == 0;
}

static int lan_connect(const char *host, uint16_t port, uint32_t timeout_ms, char *err, size_t errcap)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) {
            if (is_mdns_name(host)) {
                snprintf(err, errcap,
                         "%s is a .local name - the 3DS cannot resolve those, use the IP",
                         host);
            } else {
                snprintf(err, errcap, "cannot resolve %s", host);
            }
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof addr.sin_addr);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, errcap, "socket() failed (errno %d)", errno); return -1; }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    if (rc != 0 && errno != EINPROGRESS && errno != EALREADY) {
        snprintf(err, errcap, "connect to %s:%u failed (errno %d)",
                 host, (unsigned)port, errno);
        close(fd);
        return -1;
    }

    /*
     * Probe by re-calling connect() rather than waiting on poll(POLLOUT).
     * poll never signals connect completion on this console -- see the note in
     * https.c, where relying on it made every connection fail after the full
     * timeout with a stale EINPROGRESS in errno.
     */
    if (rc != 0) {
        uint64_t deadline = osGetTime() + timeout_ms;
        for (;;) {
            rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
            if (rc == 0 || errno == EISCONN) break;
            if (errno != EINPROGRESS && errno != EALREADY) {
                snprintf(err, errcap, "no LAN server at %s:%u (errno %d)",
                         host, (unsigned)port, errno);
                close(fd);
                return -1;
            }
            if (osGetTime() >= deadline) {
                snprintf(err, errcap, "no LAN server at %s:%u (timed out)",
                         host, (unsigned)port);
                close(fd);
                return -1;
            }
            svcSleepThread(20 * 1000 * 1000LL);   /* 20ms */
        }
    }

    /* Stay non-blocking: the game loop drains it once per frame and must never
     * stall waiting on the network. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

/* --------------------------------------------------------------- HTTP worker */

/*
 * Why a worker thread at all.
 *
 * A TLS request blocks: handshake, write, read. Running that on the main thread
 * would pin the entire game to the request rate -- a 10Hz poll would mean a
 * 10fps game. The worker keeps rendering at 60fps while requests are in flight,
 * and the split is narrow: two byte queues guarded by a LightLock that is never
 * held across a network call.
 */
static void http_worker(void *arg)
{
    PongNet *n = (PongNet *)arg;
    static uint8_t reqbuf[TX_QUEUE_BYTES] __attribute__((aligned(4)));
    static uint8_t respbuf[RX_QUEUE_BYTES];

    uint32_t next_at = 0;

    while (!n->stop) {
        uint32_t now = (uint32_t)osGetTime();
        if (now < next_at) {
            svcSleepThread(2 * 1000 * 1000LL); /* 2ms */
            continue;
        }

        /* Take whatever is queued. Lock held only for the copy. */
        LightLock_Lock(&n->lock);
        size_t reqlen = n->tx_len;
        if (reqlen > sizeof reqbuf) reqlen = sizeof reqbuf;
        memcpy(reqbuf, n->tx, reqlen);
        n->tx_len = 0;
        LightLock_Unlock(&n->lock);

        if (!n->https || !https_is_open(n->https)) {
            /* Reconnect, but not in a tight loop. */
            if (n->https) { https_close(n->https); n->https = NULL; }
            pong_log("worker         : opening connection");
            n->https = https_open(n->cfg.web_host, n->cfg.web_port,
                                  n->cfg.web_tls, n->cfg.web_verify);
            if (!n->https) {
                snprintf(n->err, sizeof n->err, "%s", https_open_error());
                pong_log("worker         : OPEN FAILED -- %s", n->err);
                n->state = PONG_LINK_FAILED;
                next_at = (uint32_t)osGetTime() + 2000;
                continue;
            }
            n->state = PONG_LINK_OPEN;
        }

        /* Session bootstrap.
         *
         * The HTTP path has no connect event, so identity is established by a
         * one-shot POST to /api/session whose body is the queued HELLO and
         * whose response carries WELCOME plus the session id header. Doing this
         * before any transport is chosen is what lets the session outlive a
         * dropped connection: a reconnect reuses the same id and keeps the seat. */
        const char *path = n->cfg.web_path;
        bool bootstrapping = (n->session[0] == '\0');
        if (bootstrapping) path = "/api/session";

        HttpsResponse resp;
        HttpsResult rc = https_request(n->https, "POST", path,
                                       n->session[0] ? n->session : NULL,
                                       reqlen ? reqbuf : NULL, reqlen,
                                       respbuf, sizeof respbuf, &resp);

        if (rc != HTTPS_OK) {
            https_close(n->https);
            n->https = NULL;
            snprintf(n->err, sizeof n->err, "request failed (%d)", (int)rc);
            pong_log("worker         : request failed rc=%d (%s)", (int)rc,
                     bootstrapping ? "bootstrap" : "rpc");
            next_at = (uint32_t)osGetTime() + 500;
            continue;
        }

        if (resp.status == 404) {
            /* Session expired server-side; the caller re-bootstraps. */
            n->session[0] = '\0';
            snprintf(n->err, sizeof n->err, "session expired");
        }
        if (resp.session[0]) {
            bool first = (n->session[0] == '\0');
            snprintf(n->session, sizeof n->session, "%s", resp.session);
            if (first) pong_log("worker         : session %.12s... established", n->session);
        }

        n->rtt_ms = resp.elapsed_ms;

        if (resp.body_len > 0) {
            LightLock_Lock(&n->lock);
            size_t space = sizeof n->rx - n->rx_len;
            size_t take = resp.body_len < space ? resp.body_len : space;
            memcpy(n->rx + n->rx_len, respbuf, take);
            n->rx_len += take;
            LightLock_Unlock(&n->lock);
        }

        /* Self-pacing: hold ~10Hz on a fast link, and on a slow one simply run
         * as fast as the link allows rather than queueing requests behind each
         * other. The measured RTT decides. */
        uint32_t period = bootstrapping ? 0 : 100;
        uint32_t elapsed = resp.elapsed_ms;
        next_at = (uint32_t)osGetTime() + (elapsed >= period ? 0 : period - elapsed);

        n->req_count++;
        uint32_t t = (uint32_t)osGetTime();
        if (t - n->req_window_start >= 1000) {
            n->hz = n->req_count;
            n->req_count = 0;
            n->req_window_start = t;
        }
    }
}

/* ------------------------------------------------------------------- public */

PongNet *pong_net_open(const PongNetConfig *cfg)
{
    PongNet *n = (PongNet *)calloc(1, sizeof *n);
    if (!n) return NULL;
    n->cfg = *cfg;
    n->sock = -1;
    n->state = PONG_LINK_CONNECTING;
    LightLock_Init(&n->lock);

    /* LAN first, but only when we are plausibly on it. Attempting a LAN
     * connect from a phone hotspot would stall for the full timeout on every
     * launch for no possible benefit. */
    bool same_subnet = on_lan(cfg->lan_subnet, n->local_ip, sizeof n->local_ip);
    pong_log("local IP       : %s (subnet filter '%s' -> %s)",
             n->local_ip, cfg->lan_subnet[0] ? cfg->lan_subnet : "(none)",
             same_subnet ? "match" : "no match");
    /* No lan_host configured means no raw-TCP port to talk to -- which is the
     * correct default, since a deployment behind 443 has no such port. */
    bool lan_configured = cfg->lan_host[0] != '\0';
    bool try_lan = lan_configured &&
                   ((cfg->mode == PONG_MODE_LAN) ||
                    (cfg->mode == PONG_MODE_AUTO && same_subnet));

    pong_log("LAN path       : %s", try_lan ? "attempting" :
             (cfg->lan_host[0] ? "skipped (wrong subnet)" : "not configured"));

    if (try_lan) {
        n->lan_attempted = true;
        /* 3DS wifi is slow, especially on the first connect after association.
         * 800ms was a laptop's idea of generous and timed out on real hardware. */
        n->sock = lan_connect(cfg->lan_host, cfg->lan_port, 3000,
                              n->lan_err, sizeof n->lan_err);
        if (n->sock >= 0) {
            pong_log("LAN path       : connected to %s:%u",
                     cfg->lan_host, (unsigned)cfg->lan_port);
            n->lan_err[0] = '\0';
            n->active = PONG_MODE_LAN;
            n->state = PONG_LINK_OPEN;
            n->hz = PONG_SNAPSHOT_HZ;
            snprintf(n->desc, sizeof n->desc, "LAN TCP %s:%u", cfg->lan_host, cfg->lan_port);
            return n;
        }
    }

    if (!try_lan) {
        if (!lan_configured) {
            snprintf(n->lan_err, sizeof n->lan_err, "not configured (using HTTPS)");
        } else {
            snprintf(n->lan_err, sizeof n->lan_err,
                     "skipped: %s is not on %s", n->local_ip, cfg->lan_subnet);
        }
    }

    if (cfg->mode == PONG_MODE_LAN) {
        snprintf(n->err, sizeof n->err, "%s", n->lan_err);
        n->state = PONG_LINK_FAILED;
        return n;
    }

    /* Internet path. The worker owns the connection from here. */
    if (n->lan_err[0]) pong_log("LAN path       : %s", n->lan_err);

    n->active = PONG_MODE_WEB;
    pong_log("WEB path       : %s://%s:%u%s verify=%d",
             cfg->web_tls ? "https" : "http", cfg->web_host,
             (unsigned)cfg->web_port, cfg->web_path, cfg->web_verify ? 1 : 0);
    snprintf(n->desc, sizeof n->desc, "%s %s",
             cfg->web_tls ? "HTTPS" : "HTTP", cfg->web_host);

    if (cfg->web_tls && https_global_init() != HTTPS_OK) {
        snprintf(n->err, sizeof n->err, "entropy init failed (PS service)");
        n->state = PONG_LINK_FAILED;
        return n;
    }

    n->req_window_start = (uint32_t)osGetTime();

    /* main_prio + 1 is LOWER priority on the 3DS, so rendering always wins. */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    n->worker = threadCreate(http_worker, n, WORKER_STACK, prio + 1, -2, false);
    if (!n->worker) {
        snprintf(n->err, sizeof n->err, "could not start network thread");
        n->state = PONG_LINK_FAILED;
        return n;
    }

    return n;
}

void pong_net_close(PongNet *n)
{
    if (!n) return;
    if (n->worker) {
        n->stop = true;
        threadJoin(n->worker, U64_MAX);
        threadFree(n->worker);
        n->worker = NULL;
    }
    if (n->https) { https_close(n->https); n->https = NULL; }
    if (n->sock >= 0) { close(n->sock); n->sock = -1; }
    free(n);
}

PongLinkState pong_net_state(const PongNet *n) { return n ? n->state : PONG_LINK_IDLE; }
const char *pong_net_describe(const PongNet *n) { return n ? n->desc : ""; }
const char *pong_net_error(const PongNet *n) { return n ? n->err : ""; }
const char *pong_net_local_ip(const PongNet *n) { return n ? n->local_ip : "?"; }

const char *pong_net_diag(const PongNet *n)
{
    if (!n) return "";
    PongNet *m = (PongNet *)n;   /* diag is a formatting cache, not state */
    snprintf(m->diag, sizeof m->diag,
             "3DS %s\nLAN %s:%u -> %s\nWEB %s -> %s",
             n->local_ip,
             n->cfg.lan_host, (unsigned)n->cfg.lan_port,
             n->lan_err[0] ? n->lan_err : (n->active == PONG_MODE_LAN ? "connected" : "not tried"),
             n->cfg.web_host,
             n->active == PONG_MODE_WEB
                 ? (n->err[0] ? n->err : "connecting")
                 : "not tried");
    return m->diag;
}
uint32_t pong_net_rtt(const PongNet *n) { return n ? n->rtt_ms : 0; }
uint32_t pong_net_hz(const PongNet *n) { return n ? n->hz : 0; }
const char *pong_net_session(const PongNet *n) { return n ? n->session : ""; }

bool pong_net_send(PongNet *n, const uint8_t *frame, size_t len)
{
    /*
     * The HTTP path starts in CONNECTING: its worker thread has not completed
     * the first request yet. HELLO and JOIN are queued right after open(), so
     * refusing to queue while connecting would silently drop them and the
     * client would sit in a lobby it never actually joined.
     */
    if (!n) return false;
    bool web_warming = (n->active == PONG_MODE_WEB && n->state == PONG_LINK_CONNECTING);
    if (n->state != PONG_LINK_OPEN && !web_warming) return false;

    if (n->active == PONG_MODE_LAN) {
        ssize_t w = send(n->sock, frame, len, 0);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            snprintf(n->err, sizeof n->err, "send failed (%d)", errno);
            n->state = PONG_LINK_FAILED;
            return false;
        }
        return w == (ssize_t)len;
    }

    LightLock_Lock(&n->lock);
    bool ok = false;
    if (n->tx_len + len <= sizeof n->tx) {
        memcpy(n->tx + n->tx_len, frame, len);
        n->tx_len += len;
        ok = true;
    }
    /* A full queue means the worker is stalled. Dropping is correct: the next
     * INPUT carries an absolute target that supersedes anything lost. */
    LightLock_Unlock(&n->lock);
    return ok;
}

size_t pong_net_recv(PongNet *n, uint8_t *buf, size_t cap)
{
    if (!n) return 0;

    if (n->active == PONG_MODE_LAN) {
        if (n->sock < 0) return 0;
        ssize_t r = recv(n->sock, buf, cap, 0);
        if (r < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                snprintf(n->err, sizeof n->err, "recv failed (%d)", errno);
                n->state = PONG_LINK_FAILED;
            }
            return 0;
        }
        if (r == 0) {
            snprintf(n->err, sizeof n->err, "server closed the connection");
            n->state = PONG_LINK_FAILED;
            return 0;
        }
        return (size_t)r;
    }

    LightLock_Lock(&n->lock);
    size_t take = n->rx_len < cap ? n->rx_len : cap;
    memcpy(buf, n->rx, take);
    if (take < n->rx_len) memmove(n->rx, n->rx + take, n->rx_len - take);
    n->rx_len -= take;
    LightLock_Unlock(&n->lock);
    return take;
}
