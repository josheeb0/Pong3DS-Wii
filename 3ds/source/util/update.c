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
#include "ghparse.h"
#include "log.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define MANIFEST_MAX 512
#define DOWNLOAD_MAX (2 * 1024 * 1024)   /* the .3dsx is ~420KB; 2MB is slack */
#define GH_JSON_MAX  (24 * 1024)

/* Checks GitHub Releases directly, so an un-redeployed server cannot hide a
 * newer build. */
static PongUpdateResult check_github(const PongNetConfig *net, uint32_t local_build,
                                     const char *owner, const char *repo,
                                     PongUpdateInfo *out)
{
    if (https_global_init() != HTTPS_OK) {
        snprintf(out->message, sizeof out->message, "entropy init failed");
        return PONG_UPDATE_ERROR;
    }

    char url[256];
    snprintf(url, sizeof url,
             "https://api.github.com/repos/%s/%s/releases?per_page=1", owner, repo);

    static uint8_t body[GH_JSON_MAX];
    HttpsResponse resp;
    HttpsResult rc = https_get_url(url, NULL, body, sizeof body - 1,
                                   net->web_verify, 3, &resp);

    if (rc != HTTPS_OK || resp.status != 200) {
        snprintf(out->message, sizeof out->message,
                 "GitHub check failed (rc %d, HTTP %d)", (int)rc, resp.status);
        return PONG_UPDATE_ERROR;
    }

    body[resp.body_len] = '\0';

    char tag[64];
    if (!pong_gh_first_tag((const char *)body, tag, sizeof tag)) {
        /* An empty repository is a normal state, not a fault -- say so, and say
         * what to do, rather than reporting it like a network error. */
        snprintf(out->message, sizeof out->message,
                 "%s/%s has no releases yet - press SOURCE to pick another",
                 owner, repo);
        return PONG_UPDATE_ERROR;
    }

    out->remote_build = pong_gh_build_from_tag(tag);
    out->remote_protocol = 0;   /* GitHub does not know the protocol version */

    snprintf(out->release_url, sizeof out->release_url,
             "https://github.com/%s/%s/releases/tag/%s", owner, repo, tag);
    snprintf(out->dsx_url, sizeof out->dsx_url,
             "https://github.com/%s/%s/releases/download/%s/pong3ds.3dsx",
             owner, repo, tag);
    snprintf(out->cia_url, sizeof out->cia_url,
             "https://github.com/%s/%s/releases/download/%s/pong3ds.cia",
             owner, repo, tag);

    if (out->remote_build > local_build) {
        snprintf(out->message, sizeof out->message, "%s available (running %lu)",
                 tag, (unsigned long)local_build);
        return PONG_UPDATE_AVAILABLE;
    }
    snprintf(out->message, sizeof out->message, "up to date (%s)", tag);
    return PONG_UPDATE_CURRENT;
}


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
                else if (!strcmp(k, "release"))  snprintf(out->release_url, sizeof out->release_url, "%s", v);
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
}

PongUpdateResult pong_update_check(const PongNetConfig *net, uint32_t local_build,
                                   PongUpdateSource source,
                                   const char *gh_owner, const char *gh_repo,
                                   PongUpdateInfo *out)
{
    memset(out, 0, sizeof *out);
    out->local_build = local_build;

    if (source == PONG_UPDATE_SRC_GITHUB) {
        return check_github(net, local_build, gh_owner, gh_repo, out);
    }

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
                                      PongUpdateAsset which,
                                      const char *dest_path,
                                      char *message, size_t message_cap)
{
    const bool cia = (which == PONG_ASSET_CIA);
    const char *url  = cia ? info->cia_url  : info->dsx_url;
    const char *path = cia ? info->cia_path : info->dsx_path;

    if (!url[0] && !path[0]) {
        /* Name the artifact: "no download was advertised" gave no clue that
         * the .3dsx was there and only the .cia was missing. */
        snprintf(message, message_cap, "no %s was advertised for this build",
                 cia ? ".cia" : ".3dsx");
        return PONG_UPDATE_ERROR;
    }

    uint8_t *buf = (uint8_t *)malloc(DOWNLOAD_MAX);
    if (!buf) {
        snprintf(message, message_cap, "out of memory");
        return PONG_UPDATE_ERROR;
    }

    HttpsResponse resp;
    HttpsResult rc;

    if (url[0]) {
        /* GitHub: an absolute URL that redirects once to a signed asset host. */
        rc = https_get_url(url, NULL, buf, DOWNLOAD_MAX,
                           net->web_verify, 3, &resp);
    } else {
        HttpsConn *c = https_open(net->web_host, net->web_port,
                                  net->web_tls, net->web_verify);
        if (!c) {
            free(buf);
            snprintf(message, message_cap, "cannot reach %s", net->web_host);
            return PONG_UPDATE_ERROR;
        }
        rc = https_request(c, "GET", path, NULL, NULL, 0,
                           buf, DOWNLOAD_MAX, &resp);
        https_close(c);
    }

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

    /*
     * Create the containing directory first.
     *
     * fopen does not create missing directories, so a destination like
     * sdmc:/cias/pong3ds.cia fails on any SD card that has never held a CIA --
     * and the only symptom would be "cannot write to SD card", which points at
     * a full or broken card rather than a missing folder. EEXIST is the normal
     * case and is not an error.
     */
    {
        char dir[128];
        snprintf(dir, sizeof dir, "%s", dest_path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = '\0';
            if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
                pong_log("update: mkdir %s failed (errno %d)", dir, errno);
            }
        }
    }

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

    if (cia) {
        /* Deliberately does NOT say "updated": nothing about the running title
         * changed, and saying otherwise is what made a successful download look
         * like a failed one. */
        /* Names the actual path: "saved to SD" was fine when it went to the
         * root and is not once there is a folder to find. */
        snprintf(message, message_cap,
                 "build %lu saved to SD:/cias/pong3ds.cia - install it with FBI",
                 (unsigned long)info->remote_build);
    } else {
        snprintf(message, message_cap,
                 "updated to build %lu -- relaunch to apply",
                 (unsigned long)info->remote_build);
    }
    return PONG_UPDATE_DONE;
}
