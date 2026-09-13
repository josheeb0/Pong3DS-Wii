#include "https_pc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Case-insensitive compare is spelled differently on Windows, and header names
 * are case-insensitive per the HTTP spec -- servers really do vary. */
#if defined(_WIN32)
#  define pong_strncasecmp _strnicmp
#else
#  include <strings.h>
#  define pong_strncasecmp strncasecmp
#endif

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>

/*
 * Response header buffer.
 *
 * 8KB, taken straight from the 3DS client's hard-won number rather than guessed
 * again. That one started at 1KB, which fit the game server and nothing else,
 * so every GitHub request died before it saw a status line -- a 302 to a
 * release asset carries 5KB of Content-Security-Policy on its own.
 */
#define HEADER_MAX 8192

struct PongHttps {
    mbedtls_net_context      net;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt         ca;
    char host[128];
    char session[80];
    bool tls;
    bool open;
};

/*
 * Trust roots.
 *
 * The system store, by whatever name this platform calls it. A bundle of our
 * own would have to be shipped, kept current, and would go stale silently --
 * the system's is maintained by someone whose job that is. Tried in order
 * because no single path covers Debian, Fedora, macOS and the BSDs.
 */
static bool load_ca(mbedtls_x509_crt *ca)
{
    static const char *files[] = {
        "/etc/ssl/certs/ca-certificates.crt",              /* Debian, Ubuntu  */
        "/etc/pki/tls/certs/ca-bundle.crt",                /* Fedora, RHEL    */
        "/etc/ssl/cert.pem",                               /* macOS, OpenBSD  */
        "/usr/local/etc/openssl/cert.pem",                 /* brew            */
        NULL,
    };
    for (int i = 0; files[i]; i++) {
        if (mbedtls_x509_crt_parse_file(ca, files[i]) == 0) return true;
    }
    /* A directory of individual certs, which some distributions prefer. A
     * negative return means SOME failed to parse and the rest are usable, so
     * only a hard error counts as failure here. */
    if (mbedtls_x509_crt_parse_path(ca, "/etc/ssl/certs") >= 0) return true;
    return false;
}

PongHttps *pong_https_open(const char *host, uint16_t port, bool tls, bool verify,
                           char *err, size_t errcap)
{
    PongHttps *h = (PongHttps *)calloc(1, sizeof *h);
    if (!h) { snprintf(err, errcap, "out of memory"); return NULL; }

    mbedtls_net_init(&h->net);
    mbedtls_ssl_init(&h->ssl);
    mbedtls_ssl_config_init(&h->conf);
    mbedtls_entropy_init(&h->entropy);
    mbedtls_ctr_drbg_init(&h->drbg);
    mbedtls_x509_crt_init(&h->ca);

    snprintf(h->host, sizeof h->host, "%s", host);
    h->tls = tls;

    static const char pers[] = "pong-pc";
    if (mbedtls_ctr_drbg_seed(&h->drbg, mbedtls_entropy_func, &h->entropy,
                              (const unsigned char *)pers, sizeof pers - 1) != 0) {
        snprintf(err, errcap, "could not seed the random generator");
        pong_https_close(h);
        return NULL;
    }

    bool have_ca = tls ? load_ca(&h->ca) : false;
    if (tls && verify && !have_ca) {
        /* Refuse rather than quietly stop verifying. Falling back to an
         * unverified connection because the trust store was missing is how a
         * client ends up trusting anything at all without ever saying so. */
        snprintf(err, errcap, "no system certificate store found; refusing to "
                              "connect without verification");
        pong_https_close(h);
        return NULL;
    }

    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
    if (mbedtls_net_connect(&h->net, host, portstr, MBEDTLS_NET_PROTO_TCP) != 0) {
        snprintf(err, errcap, "cannot reach %s:%u", host, (unsigned)port);
        pong_https_close(h);
        return NULL;
    }

    if (!tls) {
        /* The socket is connected and that is all a plaintext exchange needs.
         * Everything below sets up a TLS session that will not be used. */
        h->open = true;
        return h;
    }

    mbedtls_ssl_config_defaults(&h->conf, MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&h->conf, verify ? MBEDTLS_SSL_VERIFY_REQUIRED
                                               : MBEDTLS_SSL_VERIFY_NONE);
    if (have_ca) mbedtls_ssl_conf_ca_chain(&h->conf, &h->ca, NULL);
    mbedtls_ssl_conf_rng(&h->conf, mbedtls_ctr_drbg_random, &h->drbg);

    if (mbedtls_ssl_setup(&h->ssl, &h->conf) != 0) {
        snprintf(err, errcap, "TLS setup failed");
        pong_https_close(h);
        return NULL;
    }
    /* SNI. Without it a tunnel fronting many hostnames on one address serves
     * whichever certificate it guesses, and the handshake fails for a reason
     * that looks nothing like a missing header. */
    mbedtls_ssl_set_hostname(&h->ssl, host);
    mbedtls_ssl_set_bio(&h->ssl, &h->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    int rc;
    while ((rc = mbedtls_ssl_handshake(&h->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            uint32_t flags = mbedtls_ssl_get_verify_result(&h->ssl);
            if (flags != 0) {
                char why[256];
                mbedtls_x509_crt_verify_info(why, sizeof why, "", flags);
                /* The reason, not just "handshake failed". An expired or
                 * wrong-host certificate is a completely different problem
                 * from an unreachable server and needs a different fix. */
                snprintf(err, errcap, "certificate rejected: %s", why);
            } else {
                snprintf(err, errcap, "TLS handshake failed (-0x%04x)", (unsigned)-rc);
            }
            pong_https_close(h);
            return NULL;
        }
    }

    h->open = true;
    return h;
}

void pong_https_close(PongHttps *h)
{
    if (!h) return;
    if (h->open && h->tls) mbedtls_ssl_close_notify(&h->ssl);
    mbedtls_x509_crt_free(&h->ca);
    mbedtls_ssl_free(&h->ssl);
    mbedtls_ssl_config_free(&h->conf);
    mbedtls_ctr_drbg_free(&h->drbg);
    mbedtls_entropy_free(&h->entropy);
    mbedtls_net_free(&h->net);
    free(h);
}

bool pong_https_alive(const PongHttps *h) { return h && h->open; }

const char *pong_https_session(const PongHttps *h) { return h ? h->session : ""; }

static int read_some(PongHttps *h, uint8_t *buf, size_t cap)
{
    if (!h->tls) {
        int n = mbedtls_net_recv(&h->net, buf, cap);
        if (n <= 0) { h->open = false; return n == 0 ? 0 : -1; }
        return n;
    }
    for (;;) {
        int n = mbedtls_ssl_read(&h->ssl, buf, cap);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { h->open = false; return 0; }
        if (n < 0) { h->open = false; return -1; }
        return n;
    }
}

static int write_all(PongHttps *h, const uint8_t *buf, size_t len)
{
    if (!h->tls) {
        size_t sent = 0;
        while (sent < len) {
            int n = mbedtls_net_send(&h->net, buf + sent, len - sent);
            if (n <= 0) { h->open = false; return -1; }
            sent += (size_t)n;
        }
        return 0;
    }
    size_t sent = 0;
    while (sent < len) {
        int n = mbedtls_ssl_write(&h->ssl, buf + sent, len - sent);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) { h->open = false; return -1; }
        sent += (size_t)n;
    }
    return 0;
}

PongHttpsResult pong_https_post(PongHttps *h, const char *path,
                                const char *extra_header,
                                const uint8_t *body, size_t body_len,
                                uint8_t *out, size_t out_cap, size_t *out_len,
                                int *status)
{
    if (!h || !h->open) return PONG_HTTPS_ERR_CONNECT;
    *out_len = 0;
    *status = 0;

    char head[1024];
    int hn = snprintf(head, sizeof head,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: PongPC/1.0\r\n"
        /* Anything, not octet-stream. GitHub's API treats Accept as a
         * media-type selector and answers a JSON endpoint asked for
         * octet-stream with 415 -- the 3DS client learned that the hard way and
         * there is no reason to learn it twice. */
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %zu\r\n%s\r\n",
        path, h->host, body_len, extra_header ? extra_header : "");
    if (hn < 0 || hn >= (int)sizeof head) return PONG_HTTPS_ERR_TOOBIG;

    if (write_all(h, (const uint8_t *)head, (size_t)hn) != 0) return PONG_HTTPS_ERR_WRITE;
    if (body_len && write_all(h, body, body_len) != 0) return PONG_HTTPS_ERR_WRITE;

    /* ---- response head ---- */
    static uint8_t hbuf[HEADER_MAX];
    size_t hlen = 0, header_end = 0;
    for (;;) {
        if (hlen >= sizeof hbuf - 1) return PONG_HTTPS_ERR_TOOBIG;
        int n = read_some(h, hbuf + hlen, sizeof hbuf - 1 - hlen);
        if (n <= 0) return PONG_HTTPS_ERR_READ;
        hlen += (size_t)n;
        hbuf[hlen] = '\0';

        uint8_t *end = (uint8_t *)strstr((char *)hbuf, "\r\n\r\n");
        if (end) { header_end = (size_t)(end - hbuf) + 4; break; }
    }

    if (strncmp((char *)hbuf, "HTTP/1.", 7) != 0) return PONG_HTTPS_ERR_PROTOCOL;
    *status = atoi((char *)hbuf + 9);

    long content_len = -1;
    bool chunked = false;
    {
        char *line = strstr((char *)hbuf, "\r\n");
        while (line && (size_t)((uint8_t *)line - hbuf) + 2 < header_end) {
            line += 2;
            char *eol = strstr(line, "\r\n");
            if (!eol || eol == line) break;
            *eol = '\0';
            if (pong_strncasecmp(line, "content-length:", 15) == 0) content_len = atol(line + 15);
            else if (pong_strncasecmp(line, "x-pong-session:", 15) == 0) {
                const char *v = line + 15;
                while (*v == ' ' || *v == '\t') v++;
                snprintf(h->session, sizeof h->session, "%s", v);
            }
            else if (pong_strncasecmp(line, "transfer-encoding:", 18) == 0 &&
                     strstr(line + 18, "chunked")) chunked = true;
            *eol = '\r';
            line = eol;
        }
    }

    /* Whatever arrived after the headers is the start of the body. */
    size_t tail = hlen - header_end;
    if (tail > out_cap) return PONG_HTTPS_ERR_TOOBIG;
    memcpy(out, hbuf + header_end, tail);
    size_t got = tail;

    if (chunked) {
        /*
         * Not supported, and refused rather than mis-parsed.
         *
         * Our own server sends Content-Length for every /api/rpc reply, so this
         * path is unreachable in practice -- and a half-written chunked decoder
         * that silently returns the wrong bytes is far worse than a clear
         * refusal. The 3DS client has a real one if this ever changes.
         */
        return PONG_HTTPS_ERR_PROTOCOL;
    }

    if (content_len > 0) {
        if ((size_t)content_len > out_cap) return PONG_HTTPS_ERR_TOOBIG;
        while (got < (size_t)content_len) {
            int n = read_some(h, out + got, (size_t)content_len - got);
            if (n <= 0) return PONG_HTTPS_ERR_READ;
            got += (size_t)n;
        }
    }

    *out_len = got;
    return PONG_HTTPS_OK;
}
