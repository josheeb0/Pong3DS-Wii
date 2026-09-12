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
 *   tcp://192.168.4.29:8787    -> raw TCP fast path (60Hz, LAN only)
 *
 * The `tcp://` form is how the LAN fast path is reached without editing a
 * config file. It is a separate scheme rather than a magic port number because
 * it is genuinely a different protocol, not the same one on another port.
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
        long p = strtol(colon + 1, NULL, 10);
        if (p < 1 || p > 65535) {
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

    if (raw_tcp) {
        snprintf(net->lan_host, sizeof net->lan_host, "%s", host);
        net->lan_port = port ? port : 8787;
        net->lan_subnet[0] = '\0';   /* explicit address: always try it */
        net->mode = PONG_MODE_LAN;
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
    /* A bare IP cannot be verified against a public CA, so do not pretend. */
    net->web_verify = tls && !isdigit((unsigned char)host[0]);
    return true;
}

void pong_addr_format(const PongNetConfig *net, char *out, size_t cap)
{
    if (net->mode == PONG_MODE_LAN && net->lan_host[0]) {
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

    swkbdInit(&swkbd, SWKBD_TYPE_QWERTY, 2, (int)cap - 1);
    swkbdSetInitialText(&swkbd, initial ? initial : "");
    swkbdSetHintText(&swkbd, "pong.wardcrew.com  or  192.168.4.29:8788");
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "Connect", true);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);

    SwkbdButton b = swkbdInputText(&swkbd, buf, cap);
    return b == SWKBD_BUTTON_RIGHT;
}
#endif /* __3DS__ */
