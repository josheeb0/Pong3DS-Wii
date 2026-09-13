#ifndef PONG_SFX_EVENTS_H
#define PONG_SFX_EVENTS_H

/*
 * Which sound an event makes.
 *
 * Shared so the clients cannot disagree, and so the ONLINE and OFFLINE paths
 * cannot either -- they arrive by different routes (an EVENT frame off the
 * wire, or a PongSimEvent from the local simulation) carrying the same kinds,
 * and a match should not sound different for being played against a server.
 */

#include "tone.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * The sound for an event kind, or false if that event makes no noise.
 *
 * `my_side` decides whether MATCH_OVER is a win or a loss; `winner` is the side
 * the event names. Everything else ignores both.
 */
bool pong_sfx_for_event(uint8_t kind, uint8_t winner, uint8_t my_side,
                        PongSfx *out);

#endif /* PONG_SFX_EVENTS_H */
