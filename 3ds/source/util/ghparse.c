#include "ghparse.h"
#include <string.h>
#include <stdlib.h>

bool pong_gh_first_tag(const char *json, char *out, size_t cap)
{
    if (!json || !out || cap == 0) return false;

    const char *k = strstr(json, "\"tag_name\"");
    if (!k) return false;

    /* Skip the key, then find the opening quote of the VALUE. Scanning for the
     * next quote after the key rather than assuming a fixed offset keeps this
     * working whatever whitespace the API puts around the colon. */
    const char *p = k + strlen("\"tag_name\"");
    while (*p && *p != ':') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;

    const char *end = strchr(p, '"');
    if (!end) return false;

    size_t len = (size_t)(end - p);
    if (len == 0 || len >= cap) return false;

    memcpy(out, p, len);
    out[len] = '\0';
    return true;
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
