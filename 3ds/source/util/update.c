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
 *   2. The running .3dsx is picked up again only on the next launch; we do not
 *      try to relaunch ourselves. Replacing it is not as simple as it looks --
 *      the Homebrew Launcher holds the running file open, so the directory
 *      entry may refuse to be renamed or removed and the contents have to be
 *      rewritten in place. pong_update_download() handles both, and is written
 *      so that no failure can leave you without a working copy.
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
/* A page of releases, not one. GitHub does not return them newest-first -- a
 * freshly published build-110 came back at position seven -- so the page has to
 * be wide enough to contain the newest build wherever it lands. Twenty covers
 * the worst ordering seen by a wide margin; the real response for twenty is
 * ~230KB, and the buffer leaves room for the assets to grow. */
#define GH_RELEASES_PER_PAGE 20
#define GH_JSON_MAX  (320 * 1024)

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
             "https://api.github.com/repos/%s/%s/releases?per_page=%d",
             owner, repo, GH_RELEASES_PER_PAGE);

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

    /* The whole page is read and the largest build number wins, because the
     * order GitHub returns is not the order we need and reading only the first
     * entry is how a console sat on build 93 announcing it was current. */
    char tag[64];
    uint32_t best = pong_gh_best_release((const char *)body, tag, sizeof tag);

    if (best == 0) {
        /* Both of these are normal states rather than faults, and they need
         * different answers, so say which one it is. */
        if (!pong_gh_first_tag((const char *)body, tag, sizeof tag)) {
            snprintf(out->message, sizeof out->message,
                     "%s/%s has no releases yet - press SOURCE to pick another",
                     owner, repo);
        } else {
            snprintf(out->message, sizeof out->message,
                     "%s/%s: no release names a build number", owner, repo);
        }
        return PONG_UPDATE_ERROR;
    }

    out->remote_build = best;
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

    if (written != resp.body_len) {
        free(buf);
        remove(tmp);
        snprintf(message, message_cap, "SD write incomplete (card full?)");
        return PONG_UPDATE_ERROR;
    }

    /*
     * Move the staged file into place.
     *
     * The previous version did remove(dest) and then rename(), and deleted the
     * staging file if the rename failed. Reported from a console: "could not
     * replace sdmc:/3ds/pong3ds.3dsx" -- and by then it had deleted the
     * destination, tried and failed to rename, and deleted the download too.
     * The failure path destroyed BOTH copies of the program. A routine that
     * exists to make updating safe must never be able to leave you with less
     * than you started with.
     *
     * The cause is that the Homebrew Launcher keeps the running .3dsx open, so
     * on this platform remove() and rename() against it can both fail -- the
     * comment at the top of this file claiming the file was merely "loaded into
     * memory" and therefore free to replace was an assumption, not a fact.
     *
     * So: try the atomic move, and if the directory entry cannot be touched,
     * rewrite the contents in place instead, from the buffer still in hand.
     * The staging file is kept until one of them has actually worked.
     */
    bool placed = (rename(tmp, dest_path) == 0);
    int rename_err = placed ? 0 : errno;

    if (!placed) {
        FILE *d = fopen(dest_path, "wb");
        if (d) {
            size_t w2 = fwrite(buf, 1, resp.body_len, d);
            if (fclose(d) == 0 && w2 == resp.body_len) {
                placed = true;
                remove(tmp);
            }
        }
        pong_log("update: rename failed (errno %d); in-place rewrite %s",
                 rename_err, placed ? "succeeded" : "FAILED");
    }

    free(buf);

    if (!placed) {
        /* Both copies still exist. Say where the new one is, so this is a
         * rename away from being fixed rather than a dead end. */
        snprintf(message, message_cap,
                 "downloaded ok but could not replace it - new build is at %s",
                 tmp);
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
