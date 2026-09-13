#include "pong_local.h"

#include <string.h>

/*
 * One tick, in microseconds.
 *
 * 1000000/60 is 16666.67, and this truncates to 16666, which runs the match at
 * 60.0024Hz -- about one tick fast every seven minutes. That is deliberate:
 * rounding the other way loses a tick at the same rate, and there is nothing on
 * the other end of this match to stay in step with. Online play cares about the
 * server's clock; a local match only has to be smooth.
 */
#define TICK_US 16666u

/*
 * The most ticks one frame may simulate.
 *
 * Without this, a frame delayed by a second -- alt-tab, a Vita suspending, an
 * SD card stalling the 3DS -- would try to simulate sixty ticks at once, take
 * longer than a frame to do it, and arrive at the next frame with even more to
 * catch up on. Time is dropped instead. The match runs slightly slow for one
 * frame, which nobody notices, rather than locking up, which everybody does.
 */
#define MAX_STEPS_PER_FRAME 8

void pong_local_start(PongLocal *l, PongLocalMode mode, PongBotLevel level,
                      uint32_t seed, uint8_t win_score)
{
    memset(l, 0, sizeof *l);
    l->mode = mode;
    l->active = true;
    l->accum_us = 0;

    pong_sim_create(&l->sim, seed, win_score, PONG_BALL_SPEED_MAX_Q4);

    /* The full speed ceiling, always. The lowered one exists online to keep a
     * player on a 200ms polling transport in the game; nobody here is. */

    l->target_l = l->sim.left_y_q4;
    l->target_r = l->sim.right_y_q4;

    /* The bot always plays the right-hand paddle, so "your side" is left in
     * every local match and the renderer needs no special case. */
    if (mode == PONG_LOCAL_VS_AI) pong_bot_init(&l->bot, 1, level, seed);
}

void pong_local_set_target(PongLocal *l, int side, int32_t target_q4)
{
    if (side == 0) l->target_l = target_q4;
    else l->target_r = target_q4;
}

void pong_local_advance(PongLocal *l, uint32_t dt_ms)
{
    if (!l->active) return;

    l->accum_us += dt_ms * 1000u;

    int steps = 0;
    while (l->accum_us >= TICK_US && steps < MAX_STEPS_PER_FRAME) {
        l->accum_us -= TICK_US;
        steps++;

        PongSimInput in_l = { l->target_l, 0 };
        PongSimInput in_r;

        if (l->mode == PONG_LOCAL_VS_AI) {
            /* The bot decides from the state BEFORE this tick, which is the
             * same information a remote player's input carries: nobody gets to
             * see the tick they are acting on. */
            in_r = pong_bot_think(&l->bot, &l->sim);
        } else {
            in_r.target_y_q4 = l->target_r;
            in_r.buttons = 0;
        }

        pong_sim_step(&l->sim, &in_l, &in_r, NULL);
    }

    /* Whatever could not be simulated is discarded rather than carried, so a
     * long stall cannot leave a debt that the next frames have to pay. */
    if (steps >= MAX_STEPS_PER_FRAME) l->accum_us = 0;
}

void pong_local_view(const PongLocal *l, PongView *out)
{
    memset(out, 0, sizeof *out);
    out->ball_x = l->sim.ball_x_q4;
    out->ball_y = l->sim.ball_y_q4;
    out->left_y = l->sim.left_y_q4;
    out->right_y = l->sim.right_y_q4;
    out->score_l = l->sim.score_l;
    out->score_r = l->sim.score_r;
    out->state = l->sim.state;
    out->flags = 0;
    /* Neither starved nor extrapolated by construction: this IS the authority,
     * so there is nothing to run ahead of and nothing to run out of. */
    out->starved = false;
    out->extrapolated = false;
    out->valid = l->active;
}
