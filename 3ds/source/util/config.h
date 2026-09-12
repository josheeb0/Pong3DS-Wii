/*
 * Configuration, loaded from romfs with an SD-card override.
 *
 * romfs:/config.txt ships the defaults; sdmc:/3ds/pong3ds.cfg overrides any of
 * them. That split means changing servers never requires a rebuild, and the
 * app can also record which transport last worked so a launch away from home
 * does not stall retrying the LAN.
 */

#ifndef PONG_CONFIG_H
#define PONG_CONFIG_H

#include <stdbool.h>
#include "net.h"
#include "update.h"

#define PONG_SD_CONFIG "sdmc:/3ds/pong3ds.cfg"

typedef struct {
    PongNetConfig net;
    char          player_name[17];
    bool          autoupdate;
    /* Where UPDATE looks: the game server, or GitHub Releases. */
    PongUpdateSource update_source;
    char          gh_owner[48];
    char          gh_repo[64];
} PongConfig;

/** Loads defaults from romfs, then applies any SD-card overrides. */
void pong_config_load(PongConfig *cfg);

/** Persists the transport that worked, so the next launch starts there. */
void pong_config_remember_mode(const PongConfig *cfg, PongNetMode mode);

/** Writes the whole config to the SD card, so an entered address persists. */
void pong_config_save(const PongConfig *cfg);

#endif /* PONG_CONFIG_H */
