#include "https.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>

/* ------------------------------------------------------------------ state */

/*
 * Why the failure reason lives outside the connection.
 *
 * https_open() frees the HttpsConn on failure, which destroyed the one thing
 * worth having: the mbedTLS error explaining WHY. The caller was left printing
 * a generic "HTTPS connect failed", which is exactly the message that tells you
 * nothing when a console in another room will not connect.
 */
static char g_last_open_error[320] = "";

static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt         g_cacert;
static bool g_inited = false;
static bool g_have_ca = false;

struct HttpsConn {
    int  fd;
    bool tls;
    bool open;

    mbedtls_ssl_context     ssl;
    mbedtls_ssl_config      conf;
    uint32_t                verify_flags;

    char host[128];
    char errbuf[256];   /* mbedtls_x509_crt_verify_info output can be long */
    int  last_err;

    /* Bytes read past the end of the previous response. With keep-alive the
     * server can pipeline the next response's head into the same TCP segment,
     * so dropping this would corrupt the following request. */
    uint8_t spill[512];
    size_t  spill_len;
};

/* ------------------------------------------------------------- global init */

HttpsResult https_global_init(void)
{
    if (g_inited) return HTTPS_OK;

    /* PS is the console's CSPRNG service; mbedtls_hardware_poll() in
     * tls_glue.c reads from it, and without it we cannot seed at all. */
    if (R_FAILED(psInit())) return HTTPS_ERR_ENTROPY;

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);

    static const char pers[] = "pong3ds";
    int rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                   (const unsigned char *)pers, sizeof pers - 1);
    if (rc != 0) {
        mbedtls_ctr_drbg_free(&g_drbg);
        mbedtls_entropy_free(&g_entropy);
        psExit();
        return HTTPS_ERR_ENTROPY;
    }

    /*
     * Trust roots, shipped in romfs because the console has no CA store.
     *
     * A missing bundle is NOT fatal here -- it is reported through
     * https_have_ca() so the caller can decide. What must never happen is
     * silently continuing without verification when the caller asked for it,
     * so https_open() refuses that case explicitly.
     */
    mbedtls_x509_crt_init(&g_cacert);
    FILE *f = fopen("romfs:/cacert.pem", "rb");
    if (f) {
        static unsigned char pem[16384];
        size_t n = fread(pem, 1, sizeof pem - 1, f);
        fclose(f);
        pem[n] = '\0';
        /* mbedtls_x509_crt_parse wants the NUL included in the length for PEM. */
        if (mbedtls_x509_crt_parse(&g_cacert, pem, n + 1) == 0) {
            g_have_ca = true;
        }
    }

    g_inited = true;
    return HTTPS_OK;
}

bool https_have_ca(void) { return g_have_ca; }

void https_global_exit(void)
{
    if (!g_inited) return;
    if (g_have_ca) { mbedtls_x509_crt_free(&g_cacert); g_have_ca = false; }
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_entropy);
    psExit();
    g_inited = false;
}

/* --------------------------------------------------------------- plumbing */

/** Read timeout, enforced with poll() since the 3DS has no SO_RCVTIMEO. */
#define READ_TIMEOUT_MS 8000

/** Blocks until the socket is readable, or the deadline passes. */
static bool wait_readable(int fd, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
    int r = poll(&p, 1, timeout_ms);
    return r > 0 && (p.revents & (POLLIN | POLLHUP)) != 0;
}

/* mbedTLS BIO callbacks. We supply our own rather than using net_sockets.c so
 * the library never needs libctru's socket headers at build time. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int n = (int)send(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    if (!wait_readable(fd, READ_TIMEOUT_MS)) return MBEDTLS_ERR_SSL_TIMEOUT;
    int n = (int)recv(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    return n;
}

/** Blocking connect with a deadline, so a dead host does not hang the game. */
static int tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;      /* the 3DS has no usable IPv6 path */
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    /* Non-blocking connect + poll so we control the timeout. A blocking
     * connect to an unreachable LAN address can stall for a minute. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    if (rc < 0) {
        struct pollfd p = { .fd = fd, .events = POLLOUT, .revents = 0 };
        if (poll(&p, 1, (int)timeout_ms) <= 0 || !(p.revents & POLLOUT)) {
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
        int soerr = 0;
        socklen_t slen = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
    }

    freeaddrinfo(res);

    /* Back to blocking: the request/response loop is simpler, and the 3DS
     * network code runs on its own worker thread where blocking is fine. */
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    /* Latency beats throughput for 20-byte input frames. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    /* The 3DS SOCU service implements neither SO_RCVTIMEO nor SO_SNDTIMEO, so
     * read deadlines are enforced with poll() in wait_readable() below. */

    return fd;
}

/* ------------------------------------------------------------------- open */

const char *https_open_error(void) { return g_last_open_error; }

HttpsConn *https_open(const char *host, uint16_t port, bool use_tls, bool verify)
{
    g_last_open_error[0] = '\0';

    if (use_tls && !g_inited) {
        snprintf(g_last_open_error, sizeof g_last_open_error, "TLS not initialised");
        return NULL;
    }

    HttpsConn *c = (HttpsConn *)calloc(1, sizeof *c);
    if (!c) return NULL;

    snprintf(c->host, sizeof c->host, "%s", host);
    c->tls = use_tls;

    c->fd = tcp_connect(host, port, 4000);
    if (c->fd < 0) {
        snprintf(g_last_open_error, sizeof g_last_open_error,
                 "tcp connect to %s:%u failed (errno %d)", host, (unsigned)port, errno);
        free(c);
        return NULL;
    }

    if (!use_tls) {
        c->open = true;
        return c;
    }

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);

    int rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) goto fail;

    /*
     * Certificate verification.
     *
     * The 3DS problem is not that verification is undesirable -- it is that the
     * console's real-time clock is frequently wrong, which fails the validity
     * window on a certificate that is otherwise perfectly good. Disabling
     * verification outright to dodge that throws away MITM protection to solve
     * a clock problem.
     *
     * So: VERIFY_OPTIONAL completes the handshake and records WHY it failed,
     * and we then accept only when the sole complaints are clock-related.
     * A bad chain, an unknown issuer or a hostname mismatch is still fatal --
     * an attacker cannot present a certificate they have no valid chain for,
     * whatever our clock says.
     */
    if (verify) {
        if (!g_have_ca) {
            rc = MBEDTLS_ERR_X509_FATAL_ERROR;
            snprintf(c->errbuf, sizeof c->errbuf,
                     "verification requested but romfs:/cacert.pem is missing");
            goto fail_msg;
        }
        mbedtls_ssl_conf_ca_chain(&c->conf, &g_cacert, NULL);
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    } else {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &g_drbg);

    rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc != 0) goto fail;

    /* Cloudflare will not serve a certificate without SNI -- omitting this
     * produces a handshake failure that looks like a cipher problem. */
    rc = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (rc != 0) goto fail;

    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);

    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) goto fail;
    }

    if (verify) {
        c->verify_flags = mbedtls_ssl_get_verify_result(&c->ssl);
        /* Clock-only failures are tolerated; everything else is fatal. */
        const uint32_t clock_only =
            MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;
        uint32_t fatal = c->verify_flags & ~clock_only;
        if (fatal != 0) {
            char why[192];
            mbedtls_x509_crt_verify_info(why, sizeof why, "", fatal);
            /* Collapse the multi-line report onto one line for the 3DS screen. */
            for (char *q = why; *q; q++) if (*q == '\n') *q = ' ';
            snprintf(c->errbuf, sizeof c->errbuf, "cert rejected: %s", why);
            rc = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
            goto fail_msg;
        }
    }

    c->open = true;
    return c;

fail:
    c->last_err = rc;
    mbedtls_strerror(rc, c->errbuf, sizeof c->errbuf);
fail_msg:
    /* Copy out before the connection is freed, or the reason dies with it. */
    snprintf(g_last_open_error, sizeof g_last_open_error, "%s (-0x%04x)",
             c->errbuf, (unsigned)-rc);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    close(c->fd);
    free(c);
    return NULL;
}

bool https_is_open(const HttpsConn *c) { return c && c->open; }

const char *https_last_error(const HttpsConn *c) { return c ? c->errbuf : "no connection"; }

uint32_t https_verify_flags(const HttpsConn *c) { return c ? c->verify_flags : 0; }

const char *https_tls_version(const HttpsConn *c)
{
    if (!c || !c->tls) return "none (plain HTTP)";
    return mbedtls_ssl_get_version(&c->ssl);
}

const char *https_ciphersuite(const HttpsConn *c)
{
    if (!c || !c->tls) return "none";
    return mbedtls_ssl_get_ciphersuite(&c->ssl);
}

void https_close(HttpsConn *c)
{
    if (!c) return;
    if (c->tls) {
        if (c->open) mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
    }
    if (c->fd >= 0) close(c->fd);
    free(c);
}

/* ----------------------------------------------------------------- io ---- */

static int conn_write(HttpsConn *c, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n;
        if (c->tls) {
            n = mbedtls_ssl_write(&c->ssl, buf + off, len - off);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        } else {
            n = (int)send(c->fd, buf + off, len - off, 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        }
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int conn_read(HttpsConn *c, uint8_t *buf, size_t cap)
{
    /* Serve anything left over from the previous response first. */
    if (c->spill_len > 0) {
        size_t n = c->spill_len < cap ? c->spill_len : cap;
        memcpy(buf, c->spill, n);
        memmove(c->spill, c->spill + n, c->spill_len - n);
        c->spill_len -= n;
        return (int)n;
    }

    for (;;) {
        int n;
        if (c->tls) {
            n = mbedtls_ssl_read(&c->ssl, buf, cap);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        } else {
            if (!wait_readable(c->fd, READ_TIMEOUT_MS)) return -1;
            n = (int)recv(c->fd, buf, cap, 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        }
        return n;
    }
}

/* --------------------------------------------------------- http response */

/** Case-insensitive header prefix match. */
static bool hdr_is(const char *line, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) {
        char a = line[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return line[n] == ':';
}

static const char *hdr_value(const char *line)
{
    const char *v = strchr(line, ':');
    if (!v) return "";
    v++;
    while (*v == ' ' || *v == '\t') v++;
    return v;
}

HttpsResult https_request(HttpsConn *c,
                          const char *method,
                          const char *path,
                          const char *session_hdr,
                          const uint8_t *body, size_t body_len,
                          uint8_t *out, size_t out_cap,
                          HttpsResponse *resp)
{
    if (!c || !c->open) return HTTPS_ERR_CLOSED;

    uint64_t t0 = osGetTime();
    memset(resp, 0, sizeof *resp);

    /* ---- request ------------------------------------------------------- */
    char head[512];
    int hn = snprintf(head, sizeof head,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Pong3DS/1.0\r\n"
        "Accept: application/octet-stream\r\n"
        "Connection: keep-alive\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %u\r\n",
        method, path, c->host, (unsigned)body_len);
    if (hn < 0 || hn >= (int)sizeof head) return HTTPS_ERR_TOOBIG;

    if (session_hdr && session_hdr[0]) {
        int extra = snprintf(head + hn, sizeof head - (size_t)hn,
                             "X-Pong-Session: %s\r\n", session_hdr);
        if (extra < 0 || extra >= (int)(sizeof head - (size_t)hn)) return HTTPS_ERR_TOOBIG;
        hn += extra;
    }
    if (hn + 2 >= (int)sizeof head) return HTTPS_ERR_TOOBIG;
    head[hn++] = '\r';
    head[hn++] = '\n';

    if (conn_write(c, (const uint8_t *)head, (size_t)hn) != 0) {
        c->open = false;
        return HTTPS_ERR_WRITE;
    }
    if (body_len > 0 && conn_write(c, body, body_len) != 0) {
        c->open = false;
        return HTTPS_ERR_WRITE;
    }

    /* ---- response head -------------------------------------------------- */
    /* Read until the blank line. Headers are small and bounded; anything that
     * does not fit is a server we do not recognise. */
    char hbuf[1024];
    size_t hlen = 0;
    size_t header_end = 0;

    for (;;) {
        if (hlen >= sizeof hbuf - 1) return HTTPS_ERR_TOOBIG;
        int n = conn_read(c, (uint8_t *)hbuf + hlen, sizeof hbuf - 1 - hlen);
        if (n <= 0) { c->open = false; return HTTPS_ERR_READ; }
        hlen += (size_t)n;
        hbuf[hlen] = '\0';

        char *end = strstr(hbuf, "\r\n\r\n");
        if (end) { header_end = (size_t)(end - hbuf) + 4; break; }
    }

    /* Status line. */
    if (strncmp(hbuf, "HTTP/1.", 7) != 0) return HTTPS_ERR_PROTOCOL;
    resp->status = atoi(hbuf + 9);

    /* Headers we care about. */
    long content_len = -1;
    bool chunked = false;
    bool close_after = false;

    {
        char *line = strstr(hbuf, "\r\n");
        while (line && (size_t)(line - hbuf) + 2 < header_end) {
            line += 2;
            char *eol = strstr(line, "\r\n");
            if (!eol || eol == line) break;
            *eol = '\0';

            if (hdr_is(line, "content-length")) {
                content_len = atol(hdr_value(line));
            } else if (hdr_is(line, "transfer-encoding")) {
                /* Cloudflare frequently re-chunks origin responses, so this
                 * path is not hypothetical -- it is the common case in
                 * production even though our own server sends Content-Length. */
                if (strstr(hdr_value(line), "chunked")) chunked = true;
            } else if (hdr_is(line, "connection")) {
                if (strstr(hdr_value(line), "close")) close_after = true;
            } else if (hdr_is(line, "x-pong-session")) {
                snprintf(resp->session, sizeof resp->session, "%s", hdr_value(line));
            }

            *eol = '\r';
            line = eol;
        }
    }

    /* Whatever arrived after the header block is the start of the body. */
    uint8_t tail[sizeof hbuf];
    size_t tail_len = hlen - header_end;
    memcpy(tail, hbuf + header_end, tail_len);

    /* ---- response body -------------------------------------------------- */
    size_t got = 0;

    if (resp->status == 204 || content_len == 0) {
        /* No body. Anything already read belongs to the next response. */
        if (tail_len > 0 && tail_len <= sizeof c->spill) {
            memcpy(c->spill, tail, tail_len);
            c->spill_len = tail_len;
        }
    } else if (chunked) {
        /* Minimal chunked decoder: <hex size>\r\n<data>\r\n ... 0\r\n\r\n */
        uint8_t work[2048];
        size_t wlen = tail_len;
        if (wlen > sizeof work) return HTTPS_ERR_TOOBIG;
        memcpy(work, tail, wlen);
        size_t pos = 0;
        bool done = false;

        while (!done) {
            /* Ensure we have a full size line. */
            char *nl = NULL;
            while (!(nl = (char *)memchr(work + pos, '\n', wlen - pos))) {
                if (wlen >= sizeof work) return HTTPS_ERR_TOOBIG;
                int n = conn_read(c, work + wlen, sizeof work - wlen);
                if (n <= 0) { c->open = false; return HTTPS_ERR_READ; }
                wlen += (size_t)n;
            }
            size_t line_len = (size_t)((uint8_t *)nl - (work + pos)) + 1;
            long chunk = strtol((char *)(work + pos), NULL, 16);
            pos += line_len;

            if (chunk <= 0) { done = true; break; }

            /* Pull the chunk plus its trailing CRLF. */
            while (wlen - pos < (size_t)chunk + 2) {
                if (wlen >= sizeof work) return HTTPS_ERR_TOOBIG;
                int n = conn_read(c, work + wlen, sizeof work - wlen);
                if (n <= 0) { c->open = false; return HTTPS_ERR_READ; }
                wlen += (size_t)n;
            }
            if (got + (size_t)chunk > out_cap) return HTTPS_ERR_TOOBIG;
            memcpy(out + got, work + pos, (size_t)chunk);
            got += (size_t)chunk;
            pos += (size_t)chunk + 2;
        }
    } else if (content_len > 0) {
        if ((size_t)content_len > out_cap) return HTTPS_ERR_TOOBIG;
        size_t want = (size_t)content_len;
        size_t take = tail_len < want ? tail_len : want;
        memcpy(out, tail, take);
        got = take;

        /* Any excess belongs to the next keep-alive response. */
        if (tail_len > take) {
            size_t rest = tail_len - take;
            if (rest <= sizeof c->spill) {
                memcpy(c->spill, tail + take, rest);
                c->spill_len = rest;
            }
        }

        while (got < want) {
            int n = conn_read(c, out + got, want - got);
            if (n <= 0) { c->open = false; return HTTPS_ERR_READ; }
            got += (size_t)n;
        }
    }

    resp->body_len = got;
    resp->elapsed_ms = (uint32_t)(osGetTime() - t0);

    if (close_after) c->open = false;
    return HTTPS_OK;
}
