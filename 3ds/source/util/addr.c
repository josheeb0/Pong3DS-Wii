/*
 * Server address entry and parsing.
 *
 * Modelled on Minecraft's Direct Connect: one text field, one address, sensible
 * defaults filled in for whatever you leave out. Typing a bare hostname should
 * just work.
 *
 * Accepted forms:
 *
 *   pong.wardcrew.com          -> https on 443
 *   pong.wardcrew.com:8443     -> https on 8443   (443 or 8443 imply TLS)
 *   192.168.4.29:8788          -> plain http      (a LAN port implies no TLS)
 *   http://192.168.4.29:8788   -> plain http, explicit
 *   https://example.com        -> https on 443, explicit
 *   tcp://192.168.4.29:8787    -> raw TCP fast path (60Hz), web as fallback
 *
 * The `tcp://` form is how the LAN fast path is reached without editing a
 * config file. It is a separate scheme rather than a magic port number because
 * it is genuinely a different protocol, not the same one on another port.
 *
 * A LAN address never disables the web path. Forcing LAN-only is a debugging
 * switch and lives in pong3ds.cfg as mode=lan, not in this box: an address
 * typed here should never be able to make the console refuse to play.
 *
 * Note for anyone pointing this at their own server: .local names do not work.
 * They are resolved by multicast DNS, which the 3DS has no resolver for, so the
 * address works from every laptop on the network and fails only on the console.
 * Use the IP.
 */

#include "addr.h"

/* Only the keyboard prompt needs libctru. Guarding it keeps the parsing logic
 * -- which has real inference in it -- compilable and testable on the host. */
#ifdef __3DS__
#include <3ds.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '/')) {
        s[--n] = '\0';
    }
}

/*
 * Is this a private IPv4 literal?
 *
 * Used to decide what a bare address means when no scheme was typed. It exists
 * because of the keyboard: the 3DS software keyboard greys out its symbol page
 * in QWERTY mode, so ':' and '/' can be genuinely impossible to enter, and
 * "tcp://192.168.4.29:8787" is then unreachable no matter how correct it is.
 * Digits and dots are always available.
 */
static bool is_private_ipv4(const char *h)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(h, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    if (a == 10) return true;                          /* 10.0.0.0/8 */
    if (a == 192 && b == 168) return true;             /* 192.168.0.0/16 */
    if (a == 172 && b >= 16 && b <= 31) return true;   /* 172.16.0.0/12 */
    if (a == 169 && b == 254) return true;             /* link-local */
    return false;
}

/*
 * "192.168.4.29" -> "192.168.4." -- the /24 the address sits in.
 *
 * Used as a cheap "are we plausibly on that network" test. Not a real netmask:
 * the console's actual mask is available, but a prefix match is enough to
 * decide whether attempting a LAN connect is worth three seconds, and being
 * wrong in either direction costs only that.
 */
static void derive_subnet(const char *ip, char *out, size_t cap)
{
    const char *last = strrchr(ip, '.');
    if (!last) { out[0] = '\0'; return; }
    size_t n = (size_t)(last - ip) + 1;      /* include the dot */
    if (n >= cap) { out[0] = '\0'; return; }
    memcpy(out, ip, n);
    out[n] = '\0';
}

bool pong_addr_parse(const char *input, PongNetConfig *net, char *err, size_t errcap)
{
    char buf[160];
    snprintf(buf, sizeof buf, "%s", input ? input : "");
    trim(buf);

    if (buf[0] == '\0') {
        snprintf(err, errcap, "address is empty");
        return false;
    }

    bool raw_tcp = false;
    bool tls = true;
    bool scheme_given = false;
    char *host = buf;

    if (strncmp(host, "https://", 8) == 0) { host += 8; tls = true;  scheme_given = true; }
    else if (strncmp(host, "http://", 7) == 0) { host += 7; tls = false; scheme_given = true; }
    else if (strncmp(host, "tcp://", 6) == 0) { host += 6; raw_tcp = true; scheme_given = true; }

    /* Strip any path -- we own the endpoints, the user only supplies a host. */
    char *slash = strchr(host, '/');
    if (slash) *slash = '\0';

    /* Port, if present. IPv6 literals are not supported: the 3DS has no usable
     * IPv6 path, so accepting the syntax would only mislead. */
    uint16_t port = 0;
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        char *end = NULL;
        long p = strtol(colon + 1, &end, 10);
        if (end == colon + 1 || *end != '\0' || p < 1 || p > 65535) {
            snprintf(err, errcap, "port must be 1-65535");
            return false;
        }
        port = (uint16_t)p;
    }

    if (host[0] == '\0') {
        snprintf(err, errcap, "missing host");
        return false;
    }
    for (const char *c = host; *c; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '-' || *c == '_')) {
            snprintf(err, errcap, "invalid character '%c' in host", *c);
            return false;
        }
    }

    /* Reject rather than truncate. A silently shortened hostname would resolve
     * to somewhere the user did not ask for, which is worse than an error. */
    size_t hostlen = strlen(host);
    size_t cap = raw_tcp ? sizeof net->lan_host : sizeof net->web_host;
    if (hostlen >= cap) {
        snprintf(err, errcap, "host too long (max %u)", (unsigned)(cap - 1));
        return false;
    }

    /*
     * A bare private IP means the LAN server, so tcp:// need not be typed.
     *
     * On a console this is not a guess worth agonising over: nobody enters
     * 192.168.x.y hoping to reach a public HTTPS deployment, and the raw TCP
     * path is dramatically better when it applies -- 60Hz against a 10Hz poll,
     * and single-digit latency instead of a round trip through Cloudflare to a
     * machine on the same switch.
     *
     * Scoped to no port, or to 8787 which is the raw TCP port itself. A private
     * IP with some other port is left alone: 192.168.4.29:8788 is the HTTP
     * server on the LAN, and quietly turning that into a raw TCP connection to
     * a port that speaks HTTP would fail in a way nobody could read. An
     * explicit scheme always wins.
     */
    if (!raw_tcp && !scheme_given && is_private_ipv4(host) &&
        (port == 0 || port == 8787)) {
        /*
         * AUTO, not LAN: this must not strand the console away from home.
         *
         * PONG_MODE_LAN means LAN or nothing -- pong_net_open returns FAILED
         * rather than falling back. That is a reasonable thing to ask for
         * explicitly and a terrible thing to get by typing an IP address, since
         * the console would then work at home and refuse to connect anywhere
         * else, with nothing on screen to explain why.
         *
         * The subnet is derived from the address so the LAN attempt is skipped
         * outright when we are somewhere else, rather than stalling for the
         * full connect timeout on every launch to discover the obvious.
         */
        snprintf(net->lan_host, sizeof net->lan_host, "%s", host);
        net->lan_port = port ? port : 8787;
        derive_subnet(host, net->lan_subnet, sizeof net->lan_subnet);
        net->mode = PONG_MODE_AUTO;
        return true;
    }

    if (raw_tcp) {
        /*
         * Also AUTO. I had this as LAN-only on the reasoning that typing a
         * scheme is deliberate, and the first person to use it typed
         * tcp://<host>.local and got a hard failure with no fallback and
         * nothing playable. "Deliberate" described the typing, not the wish:
         * nobody means "and if that fails, refuse to play".
         *
         * LAN-only remains reachable by putting mode=lan in pong3ds.cfg, which
         * is where a debugging switch belongs -- it is the thing you want when
         * proving the raw path works without the web path hiding a failure, and
         * it is not the thing you want from an address box.
         */
        snprintf(net->lan_host, sizeof net->lan_host, "%s", host);
        net->lan_port = port ? port : 8787;
        /* Only an IP tells us which network it belongs to; for a hostname we
         * cannot know, so the attempt is made wherever we are. */
        if (is_private_ipv4(host)) derive_subnet(host, net->lan_subnet, sizeof net->lan_subnet);
        else net->lan_subnet[0] = '\0';
        net->mode = PONG_MODE_AUTO;
        return true;
    }

    /* No scheme and no port: assume the public HTTPS deployment, which is what
     * a bare hostname almost always means. */
    if (!scheme_given && port == 0) {
        port = 443;
        tls = true;
    } else if (!scheme_given) {
        /* A port was given without a scheme. 443/8443 are conventionally TLS;
         * anything else on a LAN box almost certainly is not. */
        tls = (port == 443 || port == 8443);
    } else if (port == 0) {
        port = tls ? 443 : 80;
    }

    snprintf(net->web_host, sizeof net->web_host, "%s", host);
    net->web_port = port;
    net->web_tls = tls;
    net->mode = PONG_MODE_WEB;
    net->web_verify = tls && strspn(host, "0123456789.") != strlen(host);
    return true;
}

void pong_addr_format(const PongNetConfig *net, char *out, size_t cap)
{
    /*
     * Keyed on the LAN host being set, not on the mode.
     *
     * Both LAN and AUTO can carry one now, and checking the mode meant an AUTO
     * config with a LAN host formatted from the web fields -- which produced
     * "http://:0" when they were empty, and that string is what gets saved to
     * the SD card and shown on the menu. Caught by the round-trip test rather
     * than by reading it, which is the whole reason that test exists.
     */
    if (net->lan_host[0]) {
        snprintf(out, cap, "tcp://%s:%u", net->lan_host, (unsigned)net->lan_port);
        return;
    }
    if (net->web_tls && net->web_port == 443) {
        snprintf(out, cap, "%s", net->web_host);
    } else if (!net->web_tls && net->web_port == 80) {
        snprintf(out, cap, "http://%s", net->web_host);
    } else {
        snprintf(out, cap, "%s://%s:%u",
                 net->web_tls ? "https" : "http",
                 net->web_host, (unsigned)net->web_port);
    }
}

#ifdef __3DS__
bool pong_addr_prompt(char *buf, size_t cap, const char *initial)
{
    static SwkbdState swkbd;

    /* NORMAL, not QWERTY: the QWERTY layout greys out its symbol page, which
     * makes ':' and '/' impossible to type and an address like
     * tcp://host:8787 impossible to enter. The tabbed keyboard has them. */
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, (int)cap - 1);
    swkbdSetInitialText(&swkbd, initial ? initial : "");
    swkbdSetHintText(&swkbd, "pong.wardcrew.com  or  192.168.4.29 for LAN");
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "Connect", true);
    /* NOTEMPTY_NOTBLANK only refuses an empty result; the two zeros are the
     * character filter and digit cap, and must stay zero. Any filter here also
     * greys out keyboard pages, which is the thing that made a colon
     * untypeable in the first place. */
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    /* Let the symbol page be reached by leaving every feature default; nothing
     * about entering a host name benefits from restricting input. */

    SwkbdButton b = swkbdInputText(&swkbd, buf, cap);
    return b == SWKBD_BUTTON_RIGHT;
}
#endif /* __3DS__ */
