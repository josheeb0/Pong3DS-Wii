/*
 * Address parser tests.
 *
 * The parser infers scheme, port and TLS from whatever the user leaves out, so
 * a bare hostname "just works" like Minecraft's Direct Connect. That inference
 * is exactly the kind of thing that quietly gets a case wrong, and getting it
 * wrong means connecting somewhere the player did not ask for.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../source/util/addr.h"

static int fails = 0;

static void ok(const char *in, const char *host, int port, int tls, int mode)
{
    PongNetConfig c;
    memset(&c, 0, sizeof c);
    char err[96] = {0};

    if (!pong_addr_parse(in, &c, err, sizeof err)) {
        printf("  FAIL %-30s rejected: %s\n", in, err);
        fails++;
        return;
    }

    /* Which pair of fields was filled in is decided by whether a LAN host was
     * set, not by the mode: a bare private IP configures the LAN host while
     * staying in AUTO so the web path remains available away from home. */
    bool is_lan = (c.lan_host[0] != '\0');
    const char *got_host = is_lan ? c.lan_host : c.web_host;
    int got_port = is_lan ? c.lan_port : c.web_port;

    if (strcmp(got_host, host) != 0 || got_port != port ||
        (int)c.web_tls != tls || (int)c.mode != mode) {
        printf("  FAIL %-30s -> host=%s port=%d tls=%d mode=%d (want %s %d %d %d)\n",
               in, got_host, got_port, (int)c.web_tls, (int)c.mode, host, port, tls, mode);
        fails++;
        return;
    }
    printf("  ok   %-30s -> %s:%d tls=%d\n", in, got_host, got_port, (int)c.web_tls);
}

static void rejected(const char *in, const char *why)
{
    PongNetConfig c;
    memset(&c, 0, sizeof c);
    char err[96] = {0};
    if (pong_addr_parse(in, &c, err, sizeof err)) {
        printf("  FAIL %-30s was accepted, expected rejection (%s)\n", in, why);
        fails++;
        return;
    }
    printf("  ok   %-30s rejected: %s\n", in, err);
}

int main(void)
{
    printf("=== address parsing ===\n");

    /* A bare hostname is the common case and must mean the public deployment. */
    ok("pong.wardcrew.com",           "pong.wardcrew.com", 443,  1, PONG_MODE_WEB);
    ok("  pong.wardcrew.com  ",       "pong.wardcrew.com", 443,  1, PONG_MODE_WEB);
    ok("https://pong.wardcrew.com",   "pong.wardcrew.com", 443,  1, PONG_MODE_WEB);
    ok("https://pong.wardcrew.com/",  "pong.wardcrew.com", 443,  1, PONG_MODE_WEB);

    /* A LAN address with a non-TLS port must not be assumed to be HTTPS. */
    ok("192.168.4.29:8788",           "192.168.4.29",      8788, 0, PONG_MODE_WEB);
    ok("http://192.168.4.29:8788",    "192.168.4.29",      8788, 0, PONG_MODE_WEB);
    /* A bare private IP is the LAN server. Nobody types 192.168.x.y hoping to
     * reach a public HTTPS deployment, and on a 3DS it may be the only thing
     * that CAN be typed: the software keyboard greys out its symbol page, so
     * "tcp://" and ":8787" can be genuinely unreachable. */
    ok("192.168.4.29",                "192.168.4.29",      8787, 0, PONG_MODE_AUTO);
    ok("10.0.0.5",                    "10.0.0.5",          8787, 0, PONG_MODE_AUTO);
    ok("172.16.3.9",                  "172.16.3.9",        8787, 0, PONG_MODE_AUTO);
    /* Public IPs are not assumed to be a LAN server. */
    ok("8.8.8.8",                     "8.8.8.8",           443,  1, PONG_MODE_WEB);
    ok("pong.wardcrew.com:8443",      "pong.wardcrew.com", 8443, 1, PONG_MODE_WEB);
    ok("http://example.com",          "example.com",       80,   0, PONG_MODE_WEB);

    /* The raw TCP fast path, reachable without editing a config file. */
    ok("tcp://192.168.4.29:8787",     "192.168.4.29",      8787, 0, PONG_MODE_AUTO);
    ok("tcp://192.168.4.29",          "192.168.4.29",      8787, 0, PONG_MODE_AUTO);
    ok("tcp://rdesktop.local:9787",   "rdesktop.local",    9787, 0, PONG_MODE_AUTO);

    printf("\n=== a LAN address must not strand the console ===\n");
    {
        /* PONG_MODE_LAN means LAN or nothing. Reaching it by typing an IP would
         * make the console work at home and refuse to connect anywhere else,
         * with nothing on screen to explain why. */
        PongNetConfig c; char e[96];
        memset(&c, 0, sizeof c);
        snprintf(c.web_host, sizeof c.web_host, "pong.wardcrew.com");
        c.web_port = 443; c.web_tls = true;

        if (!pong_addr_parse("192.168.4.29", &c, e, sizeof e)) {
            printf("  FAIL bare LAN IP rejected: %s\n", e); fails++;
        } else if (c.mode == PONG_MODE_LAN) {
            printf("  FAIL bare LAN IP set LAN-only mode, no web fallback\n"); fails++;
        } else if (strcmp(c.web_host, "pong.wardcrew.com") != 0 || c.web_port != 443) {
            printf("  FAIL setting a LAN host destroyed the web host\n"); fails++;
        } else {
            printf("  ok   keeps the web host as a fallback (%s:%u)\n",
                   c.web_host, (unsigned)c.web_port);
        }

        /* And the subnet is derived, so being away skips the LAN attempt
         * instantly instead of stalling for the connect timeout. */
        if (strcmp(c.lan_subnet, "192.168.4.") != 0) {
            printf("  FAIL subnet not derived (got '%s', want '192.168.4.')\n", c.lan_subnet);
            fails++;
        } else {
            printf("  ok   derives subnet '%s' so a foreign network skips LAN\n", c.lan_subnet);
        }

        /* Nor does an explicit tcp://. Reported from a console: tcp:// with a
         * .local host failed hard with nothing playable, because LAN-only left
         * no fallback. Typing a scheme is deliberate; being stranded is not. */
        memset(&c, 0, sizeof c);
        snprintf(c.web_host, sizeof c.web_host, "pong.wardcrew.com");
        pong_addr_parse("tcp://rdesktop.local:9787", &c, e, sizeof e);
        if (c.mode == PONG_MODE_LAN) {
            printf("  FAIL explicit tcp:// strands the console with no fallback\n"); fails++;
        } else if (strcmp(c.web_host, "pong.wardcrew.com") != 0) {
            printf("  FAIL tcp:// destroyed the web host\n"); fails++;
        } else {
            printf("  ok   explicit tcp:// still falls back to %s\n", c.web_host);
        }
    }

    printf("\n=== rejections ===\n");
    rejected("",                      "empty");
    rejected("   ",                   "blank");
    rejected("host:0",                "port 0");
    rejected("host:99999",            "port out of range");
    rejected("bad host!",             "invalid character");
    rejected("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.com",
             "host too long -- must not silently truncate");

    printf("\n=== verification policy ===\n");
    {
        /* A bare IP has no name to verify against a public CA, so claiming to
         * verify it would be a lie. A hostname over TLS must verify. */
        PongNetConfig c; char e[96];
        memset(&c, 0, sizeof c);
        pong_addr_parse("203.0.113.9", &c, e, sizeof e);   /* public, so WEB */
        if (c.web_verify) { printf("  FAIL bare IP should not claim verification\n"); fails++; }
        else printf("  ok   bare IP does not claim certificate verification\n");

        memset(&c, 0, sizeof c);
        pong_addr_parse("pong.wardcrew.com", &c, e, sizeof e);
        if (!c.web_verify) { printf("  FAIL hostname over TLS must verify\n"); fails++; }
        else printf("  ok   hostname over TLS verifies\n");
    }

    printf("\n=== round-trip through format ===\n");
    {
        const char *cases[] = { "pong.wardcrew.com", "http://192.168.4.29:8788",
                                "tcp://192.168.4.29:8787", NULL };
        for (int i = 0; cases[i]; i++) {
            PongNetConfig c; char e[96], out[PONG_ADDR_MAX];
            memset(&c, 0, sizeof c);
            if (!pong_addr_parse(cases[i], &c, e, sizeof e)) { fails++; continue; }
            pong_addr_format(&c, out, sizeof out);
            /* Formatting must produce something that parses back the same way,
             * since it is what gets saved to the SD card and reloaded. */
            PongNetConfig c2; memset(&c2, 0, sizeof c2);
            if (!pong_addr_parse(out, &c2, e, sizeof e) ||
                strcmp(c2.web_host, c.web_host) != 0 ||
                strcmp(c2.lan_host, c.lan_host) != 0 ||
                c2.web_port != c.web_port || c2.web_tls != c.web_tls ||
                c2.lan_port != c.lan_port || c2.mode != c.mode) {
                printf("  FAIL %-26s formats to %-26s which does not round-trip\n", cases[i], out);
                fails++;
            } else {
                printf("  ok   %-26s <-> %s\n", cases[i], out);
            }
        }
    }

    printf("\n");
    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASSED: address parsing\n");
    return 0;
}
