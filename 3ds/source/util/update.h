/*
 * Auto-update: ask the server whether a newer build exists, and fetch it.
 *
 * See update.c for the two limitations that matter (CIA cannot self-install;
 * the running .3dsx is replaced on disk and applied at next launch).
 */

#ifndef PONG_UPDATE_H
#define PONG_UPDATE_H

#include <stdint.h>
#include <stddef.h>
#include "net.h"
#include "pong_proto.h"

/** Where a .3dsx launched from the Homebrew Launcher normally lives. */
#define PONG_DSX_PATH "sdmc:/3ds/pong3ds.3dsx"

/** Bumped by the build; compared against the server's manifest. */
#ifndef PONG_BUILD_ID
#define PONG_BUILD_ID 1
#endif

typedef enum {
    PONG_UPDATE_CURRENT = 0,   /* already newest */
    PONG_UPDATE_AVAILABLE,     /* newer build exists */
    PONG_UPDATE_DONE,          /* downloaded and written */
    PONG_UPDATE_ERROR,
} PongUpdateResult;

typedef struct {
    uint32_t local_build;
    uint32_t remote_build;
    uint32_t remote_protocol;
    char     dsx_path[96];
    char     cia_path[96];
    char     release_url[128];
    char     message[160];
} PongUpdateInfo;

PongUpdateResult pong_update_check(const PongNetConfig *net, uint32_t local_build,
                                   PongUpdateInfo *out);

PongUpdateResult pong_update_download(const PongNetConfig *net,
                                      const PongUpdateInfo *info,
                                      const char *dest_path,
                                      char *message, size_t message_cap);

#endif /* PONG_UPDATE_H */
