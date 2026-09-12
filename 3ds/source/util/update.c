/*
 * In-app auto-update.
 *
 * Checks GET /api/version, and if the server advertises a newer build,
 * downloads the .3dsx over the connection we already know how to make and
 * writes it to the SD card.
 *
 * Two honest limitations, stated here rather than discovered later:
 *
 *   1. This updates the .3dsx, not the .cia. A CIA cannot install another CIA
 *      without elevated am:u access, which is FBI's job. When running as an
 *      installed title the updater reports the new build and the URL, and you
 *      install it with FBI -- it does not pretend to have updated itself.
 *
 *   2. The running .3dsx is already loaded into memory, so overwriting the file
 *      on disk is safe; the new build is picked up on the next launch. We do
 *      not try to relaunch ourselves.
 *
 * For development, none of this is the fast path -- `make send` pushes a build
 * over wifi with 3dslink in about a second and never touches the SD card.
 */

#include "update.h"
#include "https.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_MAX 512
#define DOWNLOAD_MAX (2 * 1024 * 1024)   /* the .3dsx is ~370KB; 2MB is slack */

/* Reuses the same key=value shape as config.txt rather than introducing JSON,
 * which the console has no parser for. */
static void parse_manifest(const char *text, PongUpdateInfo *out)
{
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 0 && len < 200) {
            char line[200];
            memcpy(line, p, len);
            line[len] = '\0';
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                const char *k = line, *v = eq + 1;
                if      (!strcmp(k, "build"))    out->remote_build = (uint32_t)atoi(v);
                else if (!strcmp(k, "protocol")) out->remote_protocol = (uint32_t)atoi(v);
                else if (!strcmp(k, "dsx"))      snprintf(out->dsx_path, sizeof out->dsx_path, "%s", v);
                else if (!strcmp(k, "cia"))      snprintf(out->cia_path, sizeof out->cia_path, "%s", v);
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
}

PongUpdateResult pong_update_check(const PongNetConfig *net, uint32_t local_build,
                                   PongUpdateInfo *out)
{
    memset(out, 0, sizeof *out);
    out->local_build = local_build;

    if (net->web_tls && https_global_init() != HTTPS_OK) {
        snprintf(out->message, sizeof out->message, "entropy init failed");
        return PONG_UPDATE_ERROR;
    }

    HttpsConn *c = https_open(net->web_host, net->web_port, net->web_tls, net->web_verify);
    if (!c) {
        snprintf(out->message, sizeof out->message, "cannot reach %s", net->web_host);
        return PONG_UPDATE_ERROR;
    }

    static uint8_t body[MANIFEST_MAX];
    HttpsResponse resp;
    HttpsResult rc = https_request(c, "GET", "/api/version", NULL, NULL, 0,
                                   body, sizeof body - 1, &resp);
    https_close(c);

    if (rc != HTTPS_OK || resp.status != 200) {
        snprintf(out->message, sizeof out->message, "version check failed (%d)", (int)rc);
        return PONG_UPDATE_ERROR;
    }

    body[resp.body_len] = '\0';
    parse_manifest((const char *)body, out);

    if (out->remote_protocol != 0 && out->remote_protocol != PONG_PROTOCOL_VERSION) {
        /* A protocol bump is not optional -- an old client cannot talk to the
         * new server at all, so say so plainly rather than offering a choice. */
        snprintf(out->message, sizeof out->message,
                 "server speaks protocol v%lu, this build speaks v%d -- update required",
                 (unsigned long)out->remote_protocol, PONG_PROTOCOL_VERSION);
        return PONG_UPDATE_AVAILABLE;
    }

    if (out->remote_build > local_build) {
        snprintf(out->message, sizeof out->message,
                 "build %lu available (running %lu)",
                 (unsigned long)out->remote_build, (unsigned long)local_build);
        return PONG_UPDATE_AVAILABLE;
    }

    snprintf(out->message, sizeof out->message, "up to date (build %lu)",
             (unsigned long)local_build);
    return PONG_UPDATE_CURRENT;
}

PongUpdateResult pong_update_download(const PongNetConfig *net,
                                      const PongUpdateInfo *info,
                                      const char *dest_path,
                                      char *message, size_t message_cap)
{
    if (!info->dsx_path[0]) {
        snprintf(message, message_cap, "server advertised no download");
        return PONG_UPDATE_ERROR;
    }

    HttpsConn *c = https_open(net->web_host, net->web_port, net->web_tls, net->web_verify);
    if (!c) {
        snprintf(message, message_cap, "cannot reach %s", net->web_host);
        return PONG_UPDATE_ERROR;
    }

    uint8_t *buf = (uint8_t *)malloc(DOWNLOAD_MAX);
    if (!buf) {
        https_close(c);
        snprintf(message, message_cap, "out of memory");
        return PONG_UPDATE_ERROR;
    }

    HttpsResponse resp;
    HttpsResult rc = https_request(c, "GET", info->dsx_path, NULL, NULL, 0,
                                   buf, DOWNLOAD_MAX, &resp);
    https_close(c);

    if (rc != HTTPS_OK || resp.status != 200 || resp.body_len == 0) {
        free(buf);
        snprintf(message, message_cap, "download failed (%d / HTTP %d)", (int)rc, resp.status);
        return PONG_UPDATE_ERROR;
    }

    /*
     * Write to a temporary file and rename, so an interrupted download cannot
     * leave a truncated .3dsx that fails to launch -- which would be much worse
     * than simply not updating.
     */
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.part", dest_path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(buf);
        snprintf(message, message_cap, "cannot write to SD card");
        return PONG_UPDATE_ERROR;
    }
    size_t written = fwrite(buf, 1, resp.body_len, f);
    fclose(f);
    free(buf);

    if (written != resp.body_len) {
        remove(tmp);
        snprintf(message, message_cap, "SD write incomplete (card full?)");
        return PONG_UPDATE_ERROR;
    }

    remove(dest_path);
    if (rename(tmp, dest_path) != 0) {
        remove(tmp);
        snprintf(message, message_cap, "could not replace %s", dest_path);
        return PONG_UPDATE_ERROR;
    }

    snprintf(message, message_cap, "updated to build %lu -- relaunch to apply",
             (unsigned long)info->remote_build);
    return PONG_UPDATE_DONE;
}
