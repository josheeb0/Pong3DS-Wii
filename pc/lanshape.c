#include "lanshape.h"

#include <stdio.h>
#include <string.h>

bool pong_lan_shaped(const char *host)
{
    if (!host || !host[0]) return false;

    unsigned a, b, c, d;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
        a < 256 && b < 256 && c < 256 && d < 256) {
        /* RFC1918, plus loopback and link-local. Anything else with four
         * octets is a public address and gets no assumed port. */
        if (a == 10) return true;
        if (a == 192 && b == 168) return true;
        if (a == 172 && b >= 16 && b <= 31) return true;
        if (a == 127) return true;
        if (a == 169 && b == 254) return true;
        return false;
    }

    if (strcmp(host, "localhost") == 0) return true;

    /* mDNS names are by definition on the local link. */
    size_t n = strlen(host);
    if (n > 6 && strcmp(host + n - 6, ".local") == 0) return true;

    /* A bare name with no dots is a local hostname, not a domain. */
    return strchr(host, '.') == NULL;
}
