/*
 * TLS probe, version 2 -- now using our OWN bundled mbedTLS rather than the
 * console's system SSL module.
 *
 * The first version of this probe asked whether the 3DS firmware could
 * negotiate with Cloudflare. That question is now moot: pong.wardcrew.com
 * serves an ECDSA P-256 certificate and offers no RSA certificate at all
 * (verified with openssl -- RSA-auth suites return handshake_failure), and
 * the console's cipher list is not ours to change.
 *
 * With mbedTLS linked in we pick the suites, so this probe answers the
 * questions that actually remain:
 *
 *   1. Does the handshake complete, and with which suite?
 *   2. How long does it take on this CPU? (ECDHE P-256 on a 67MHz ARM11 is
 *      not free, and it decides whether HTTPS polling is viable.)
 *   3. Does keep-alive let the SECOND request skip the handshake? That is the
 *      difference between a ~10Hz poll rate and a ~2Hz slideshow.
 *
 * A = HTTPS to pong.wardcrew.com   Y = plain HTTP to the LAN server
 * START = exit
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "https.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

#define PUBLIC_HOST "pong.wardcrew.com"
#define LAN_HOST    "192.168.4.29"
#define LAN_PORT    8788

static u32 *g_socBuf = NULL;
static u8   g_body[4096];

static void socShutdown(void) { socExit(); }

static void probe(const char *host, uint16_t port, bool tls)
{
    printf("\x1b[36m-> %s://%s:%u/healthz\x1b[0m\n", tls ? "https" : "http", host, port);

    u64 t0 = osGetTime();
    HttpsConn *c = https_open(host, port, tls, tls && https_have_ca());
    u64 t1 = osGetTime();

    if (!c) {
        printf("  \x1b[31mconnect/handshake FAILED\x1b[0m (%llums)\n", t1 - t0);
        printf("  \x1b[33m%s\x1b[0m\n\n", https_open_error());
        return;
    }

    printf("  \x1b[32mhandshake OK\x1b[0m in %llums\n", t1 - t0);
    if (tls) {
        printf("  %s / %s\n", https_tls_version(c), https_ciphersuite(c));
        uint32_t vf = https_verify_flags(c);
        if (!https_have_ca()) {
            printf("  \x1b[33mcert NOT verified (no CA bundle in romfs)\x1b[0m\n");
        } else if (vf == 0) {
            printf("  \x1b[32mcertificate verified\x1b[0m\n");
        } else {
            /* Reaching here means only clock complaints were tolerated. */
            printf("  \x1b[33mverified except clock (3DS RTC is off)\x1b[0m\n");
        }
    }

    HttpsResponse r;
    HttpsResult rc = https_request(c, "GET", "/healthz", NULL, NULL, 0,
                                   g_body, sizeof g_body, &r);
    if (rc != HTTPS_OK) {
        printf("  \x1b[31mrequest failed rc=%d\x1b[0m\n\n", (int)rc);
        https_close(c);
        return;
    }

    printf("  HTTP %d, %u bytes, %lums\n", r.status, (unsigned)r.body_len, r.elapsed_ms);
    if (r.body_len) {
        int n = (int)(r.body_len < 70 ? r.body_len : 70);
        printf("  \"");
        for (int i = 0; i < n; i++) {
            char ch = (char)g_body[i];
            putchar((ch >= 32 && ch < 127) ? ch : '.');
        }
        printf("\"\n");
    }

    /* The keep-alive question: a second request on the SAME connection must
     * skip the handshake entirely. */
    if (https_is_open(c)) {
        HttpsResponse r2;
        rc = https_request(c, "GET", "/healthz", NULL, NULL, 0,
                           g_body, sizeof g_body, &r2);
        if (rc == HTTPS_OK) {
            printf("  2nd request (same conn): %lums", r2.elapsed_ms);
            if (r2.elapsed_ms * 3 < (uint32_t)(t1 - t0)) {
                printf(" \x1b[32m-- keep-alive works\x1b[0m\n");
                printf("  \x1b[32mestimated poll rate: %luHz\x1b[0m\n",
                       r2.elapsed_ms ? (1000UL / r2.elapsed_ms) : 60UL);
            } else {
                printf(" \x1b[33m-- no reuse\x1b[0m\n");
            }
        } else {
            printf("  2nd request failed rc=%d (no keep-alive)\n", (int)rc);
        }
    } else {
        printf("  \x1b[33mserver closed the connection (no keep-alive)\x1b[0m\n");
    }

    https_close(c);
    printf("\n");
}

int main(void)
{
    romfsInit();
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("\x1b[32m=== Pong3DS TLS probe (mbedTLS) ===\x1b[0m\n\n");

    g_socBuf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!g_socBuf) { printf("memalign failed\n"); goto idle; }

    if (R_FAILED(socInit(g_socBuf, SOC_BUFFERSIZE))) {
        printf("\x1b[31msocInit failed -- is wifi on?\x1b[0m\n");
        goto idle;
    }
    atexit(socShutdown);

    if (https_global_init() != HTTPS_OK) {
        printf("\x1b[31mentropy init failed (PS service)\x1b[0m\n");
        goto idle;
    }
    printf("mbedTLS ready, entropy seeded\n");
    printf("CA bundle: %s\n\n", https_have_ca() ? "loaded" : "MISSING");
    printf("A = HTTPS %s\n", PUBLIC_HOST);
    printf("Y = HTTP  %s:%d\n", LAN_HOST, LAN_PORT);
    printf("START = exit\n\n");

idle:
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;
        if (kDown & KEY_A) probe(PUBLIC_HOST, 443, true);
        if (kDown & KEY_Y) probe(LAN_HOST, LAN_PORT, false);

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    https_global_exit();
    gfxExit();
    return 0;
}
