#include "ghparse.h"
#include <string.h>
#include <stdlib.h>

/*
 * The first string value for `key` in the document.
 *
 * "First" is the whole point: the API is asked for one release, newest first,
 * so the first occurrence of each key belongs to it.
 *
 * `key` includes its own quotes, and a match must be preceded by a structural
 * separator so that a quoted word sitting inside a VALUE cannot be taken for a
 * key. A release body is free-form markdown that may contain anything, and a
 * URL may carry a query; neither should be able to answer a field lookup.
 *
 * Field ordering carries the rest of the weight, and the real document relies
 * on it: the six release assets each have a "name", and only the release's own
 * name coming first in the document keeps an asset filename out of the answer.
 */
static bool first_string(const char *json, const char *key, char *out, size_t cap)
{
    if (!json || !key || !out || cap == 0) return false;

    const size_t klen = strlen(key);
    for (const char *k = strstr(json, key); k; k = strstr(k + 1, key)) {
        if (k != json) {
            char before = k[-1];
            if (before != '{' && before != ',' && before != ' ' &&
                before != '\n' && before != '\r' && before != '\t') {
                continue;   /* the tail of a longer key */
            }
        }

        /* Skip the key, then find the opening quote of the VALUE. Scanning for
         * the next quote rather than assuming a fixed offset keeps this working
         * whatever whitespace the API puts around the colon. */
        const char *p = k + klen;
        while (*p && *p != ':' && *p != '"') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != '"') continue;
        p++;

        const char *end = strchr(p, '"');
        if (!end) return false;

        size_t len = (size_t)(end - p);
        if (len == 0 || len >= cap) return false;

        memcpy(out, p, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

bool pong_gh_first_tag(const char *json, char *out, size_t cap)
{
    return first_string(json, "\"tag_name\"", out, cap);
}

bool pong_gh_first_name(const char *json, char *out, size_t cap)
{
    return first_string(json, "\"name\"", out, cap);
}

/*
 * Finds "build" followed by digits, ignoring case and whatever separates them.
 * Matches "Build 95", "build-95" and "(build 95)" alike, because the names are
 * written for people first.
 */
static uint32_t build_in_text(const char *s)
{
    if (!s) return 0;
    for (const char *p = s; *p; p++) {
        if ((p[0] != 'b' && p[0] != 'B') ||
            (p[1] != 'u' && p[1] != 'U') ||
            (p[2] != 'i' && p[2] != 'I') ||
            (p[3] != 'l' && p[3] != 'L') ||
            (p[4] != 'd' && p[4] != 'D')) continue;

        const char *q = p + 5;
        while (*q == ' ' || *q == '-' || *q == '_' || *q == ':' || *q == '#') q++;
        if (*q < '0' || *q > '9') continue;

        long v = strtol(q, NULL, 10);
        if (v > 0) return (uint32_t)v;
    }
    return 0;
}

uint32_t pong_gh_build_number(const char *name, const char *tag)
{
    uint32_t n = build_in_text(name);
    if (n) return n;
    /* No build number in the name: a hand-made release, or one from before
     * this mattered. The tag is the only thing left and is right for the
     * build-N form. */
    return pong_gh_build_from_tag(tag);
}

uint32_t pong_gh_build_from_tag(const char *tag)
{
    if (!tag) return 0;
    const char *p = tag;
    while (*p && (*p < '0' || *p > '9')) p++;
    if (!*p) return 0;
    long v = strtol(p, NULL, 10);
    return v > 0 ? (uint32_t)v : 0;
}
