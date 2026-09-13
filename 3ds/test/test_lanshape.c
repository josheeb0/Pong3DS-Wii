/*
 * Tests for the address-shape check that decides whether a bare address may
 * assume the raw TCP port.
 *
 * Reported from the desktop client: typing pong.wardcrew.com silently became
 * pong.wardcrew.com:8787 and then failed to connect with nothing on screen to
 * explain why. 8787 is the LAN port, and the public deployment is reachable
 * only over HTTPS on 443 through a tunnel -- this client speaks raw framed TCP
 * and has no TLS, so NO port makes that hostname work here. Guessing one only
 * bought a more confusing failure.
 *
 * A LAN address still gets the convenience, because there 8787 genuinely is the
 * right answer and typing it every time is noise.
 *
 * The boundaries are what this file is really for. 172.16-172.31 is private and
 * 172.32 is not, which is the kind of range that gets written as `a == 172`
 * by someone in a hurry and then quietly sends a LAN default to a public host.
 */

#include <stdio.h>
#include <string.h>

#include "../../pc/lanshape.h"

static int fails = 0;

static void expect(const char *host, bool want, const char *why)
{
    bool got = pong_lan_shaped(host);
    if (got != want) {
        printf("  FAIL %-22s -> %-7s, expected %-7s  (%s)\n", host,
               got ? "LAN" : "public", want ? "LAN" : "public", why);
        fails++;
    } else {
        printf("  ok   %-22s -> %-7s  %s\n", host, got ? "LAN" : "public", why);
    }
}

int main(void)
{
    printf("=== a public hostname must not get an assumed port ===\n");
    expect("pong.wardcrew.com", false, "the report that prompted this");
    expect("example.com",       false, "any domain name");
    expect("8.8.8.8",           false, "a public address");

    printf("\n=== a LAN address keeps the convenience ===\n");
    expect("192.168.4.29", true, "the usual server");
    expect("10.0.0.5",     true, "RFC1918 10/8");
    expect("127.0.0.1",    true, "loopback");
    expect("localhost",    true, "by name");
    expect("rdesktop.local", true, "mDNS is link-local by definition");
    expect("rdesktop",     true, "a bare name is a hostname, not a domain");
    expect("169.254.3.4",  true, "link-local");

    printf("\n=== the 172.16/12 boundary, which is easy to get wrong ===\n");
    expect("172.15.0.1", false, "just below the private range");
    expect("172.16.0.1", true,  "first private");
    expect("172.31.255.254", true, "last private");
    expect("172.32.0.1", false, "just above -- `a == 172` alone would fail here");

    printf("\n=== malformed input must not be mistaken for a LAN name ===\n");
    /* "" has no dots, so a naive no-dot rule would call it local and hand it
     * the LAN port. There is nothing to connect to either way, but the caller
     * then reports the wrong reason. */
    expect("", false, "empty");
    expect("999.1.1.1", false, "four octets, out of range -- not an address");

    printf("\n");
    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASSED: LAN address shape\n");
    return 0;
}
