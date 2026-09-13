#ifndef PONG_LOCAL_H
#define PONG_LOCAL_H

/*
 * A match played entirely on this device.
 *
 * Two shapes, one driver: against the built-in AI, or against a second person
 * sharing the hardware. The difference is only where the right-hand paddle's
 * intent comes from, so everything else -- the simulation, the timestep, the
 * view handed to the renderer -- is common.
 *
 * There is no network, no prediction and no interpolation here. Those exist to
 * paper over a server being somewhere else; the authority is in this process,
 * so the view is read straight out of the simulation. That also means no
 * render delay: a local match is the most responsive the game can be.
 */

#include <stdint.h>
#include <stdbool.h>

#include "pong_sim.h"
#include "pong_bot.h"
#include "client.h"   /* PongView, shared with the online path */

typedef enum {
    PONG_LOCAL_VS_AI = 0,   /* right paddle is the built-in opponent */
    PONG_LOCAL_VS_HUMAN,    /* right paddle is a second person, same device */
} PongLocalMode;

typedef struct {
    PongSim       sim;
    PongBot       bot;
    PongLocalMode mode;
    bool          active;

    /* Each side's latest stated intent, absolute and in Q4 -- the same shape
     * the network path sends, so the input code does not care which mode it is
     * feeding. */
    int32_t target_l, target_r;

    /* Fixed-timestep accumulator, in microseconds. */
    uint32_t accum_us;
} PongLocal;

/** Begins a match. `level` is ignored in VS_HUMAN. */
void pong_local_start(PongLocal *l, PongLocalMode mode, PongBotLevel level,
                      uint32_t seed, uint8_t win_score);

/** States a side's desired paddle centre. `side` is 0 left, 1 right. */
void pong_local_set_target(PongLocal *l, int side, int32_t target_q4);

/**
 * Advances the match by real elapsed time.
 *
 * Steps a FIXED number of microseconds per tick regardless of frame rate, so a
 * 3DS at 60fps, a desktop at 144 and a window dragged across a screen all play
 * the same game. A frame that took far too long is truncated rather than
 * simulated in full: catching up on a second of missed ticks would freeze the
 * frame that tries, and then have more to catch up on.
 */
void pong_local_advance(PongLocal *l, uint32_t dt_ms);

/** Fills a view for the renderer, straight from the simulation. */
void pong_local_view(const PongLocal *l, PongView *out);

#endif /* PONG_LOCAL_H */
