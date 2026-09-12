/*
 * The update target presets and the lookups over them.
 *
 * Split out of update.c because none of this needs libctru, which update.c does.
 * Keeping it separate means the host test links the REAL table rather than a
 * copy of it, so the presets the console cycles through and the presets the test
 * checks cannot drift apart -- which is the only failure mode a copy would have
 * been silent about.
 */

#include "update.h"
#include <stdio.h>
#include <string.h>

/*
 * Upstream first, because that is where the project is meant to live and a
 * shipped default should not point at one person's fork. The fork is an
 * explicit choice rather than the default.
 */
const PongUpdateTarget PONG_UPDATE_TARGETS[] = {
    { PONG_UPDATE_SRC_GITHUB, "josheeb0",       "Pong3DS-Wii", "GitHub: josheeb0" },
    { PONG_UPDATE_SRC_GITHUB, "johndoe6345789", "Pong3DS-Wii", "GitHub: johndoe6345789" },
    { PONG_UPDATE_SRC_SERVER, NULL,             NULL,          "game server" },
};
const int PONG_UPDATE_TARGET_COUNT =
    (int)(sizeof PONG_UPDATE_TARGETS / sizeof PONG_UPDATE_TARGETS[0]);

int pong_update_target_index(PongUpdateSource src, const char *owner, const char *repo)
{
    for (int i = 0; i < PONG_UPDATE_TARGET_COUNT; i++) {
        const PongUpdateTarget *t = &PONG_UPDATE_TARGETS[i];
        if (t->source != src) continue;
        if (src == PONG_UPDATE_SRC_SERVER) return i;
        if (owner && repo && strcmp(t->owner, owner) == 0 && strcmp(t->repo, repo) == 0) {
            return i;
        }
    }
    return -1;   /* a custom owner/repo from the config file */
}

const char *pong_update_source_name(PongUpdateSource s)
{
    return s == PONG_UPDATE_SRC_GITHUB ? "GitHub Releases" : "game server";
}

const char *pong_update_target_label(PongUpdateSource src, const char *owner,
                                     const char *repo, char *buf, size_t cap)
{
    int i = pong_update_target_index(src, owner, repo);
    if (i >= 0) {
        snprintf(buf, cap, "%s", PONG_UPDATE_TARGETS[i].label);
    } else if (src == PONG_UPDATE_SRC_GITHUB) {
        /* Name it in full, so a hand-edited config is visible rather than
         * looking like one of the presets. */
        snprintf(buf, cap, "GitHub: %s/%s", owner ? owner : "?", repo ? repo : "?");
    } else {
        snprintf(buf, cap, "game server");
    }
    return buf;
}
