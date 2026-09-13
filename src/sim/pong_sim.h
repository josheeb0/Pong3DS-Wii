#ifndef PONG_SIM_H
#define PONG_SIM_H

/*
 * The authoritative Pong simulation, in C.
 *
 * A line-for-line port of server/src/sim/pong.ts, which until now had no
 * counterpart on the client: the clients carried prediction and interpolation,
 * which answer "where will the server say the ball is", not "where IS the
 * ball". Local play needs the second question answered without a server.
 *
 * It is a PORT rather than a second implementation, and that distinction is
 * load-bearing. A local match runs the same rules as an online one -- the same
 * bounce angles, the same speed ramp, the same serve -- so practising offline
 * teaches you the game you will actually play. tools/gen-protocol.mjs emits a
 * trace from the TypeScript simulation and 3ds/test/test_sim.c replays it here,
 * so a divergence is a failed build rather than a game that feels subtly wrong.
 *
 * Entirely integer, no allocation, no I/O, no platform headers: the 3DS, the
 * Vita and the desktops all compile this file unchanged.
 */

#include <stdint.h>
#include <stdbool.h>

#include "pong_proto.h"

/** Phases, matching the protocol's MatchState. */
enum {
    PONG_SIM_WAITING     = 0,
    PONG_SIM_COUNTDOWN   = 1,
    PONG_SIM_PLAY        = 2,
    PONG_SIM_GOAL_FREEZE = 3,
    PONG_SIM_GAME_OVER   = 4,
};

/** Event kinds a caller may care about, matching the protocol's EventKind. */
enum {
    PONG_SIM_EV_GOAL       = 1,
    PONG_SIM_EV_MATCH_OVER = 2,
    PONG_SIM_EV_PADDLE_HIT = 6,
    PONG_SIM_EV_WALL_HIT   = 7,
};

typedef struct {
    uint8_t kind;
    uint8_t a, b, c;
} PongSimEvent;

/** At most this many events can come out of one tick: a wall hit, a paddle hit,
 *  a goal and the match ending cannot all happen together, but the bound is
 *  generous rather than exact so the caller never has to check. */
#define PONG_SIM_MAX_EVENTS 4

typedef struct {
    uint32_t tick;
    uint8_t  state;

    int32_t ball_x_q4, ball_y_q4;
    int32_t ball_vx_q4, ball_vy_q4;
    int32_t left_y_q4, right_y_q4;

    uint8_t score_l, score_r;

    /** xorshift32 state; never zero. */
    uint32_t rng;
    /** Tick at which COUNTDOWN or GOAL_FREEZE ends. */
    uint32_t phase_until;
    /** Ball speed ceiling. Lowered online when a player is on a slow transport;
     *  a local match has no such player, so it is always the full value. */
    int32_t speed_max_q4;
    /** Paddle hits this rally, for the speed ramp. */
    uint32_t rally_hits;
    uint8_t win_score;
} PongSim;

/** One player's intent for a tick. Absolute target, exactly as on the wire. */
typedef struct {
    int32_t target_y_q4;
    uint8_t buttons;
} PongSimInput;

/**
 * Starts a match in COUNTDOWN, ball centred, paddles centred.
 *
 * `seed` selects the serve sequence. Zero is replaced, because xorshift32 is
 * absorbing at zero and a zero seed would serve the same ball forever.
 */
void pong_sim_create(PongSim *s, uint32_t seed, uint8_t win_score,
                     int32_t speed_max_q4);

/**
 * Advances the match by exactly one tick.
 *
 * `events` may be NULL. When it is not, up to PONG_SIM_MAX_EVENTS are written
 * and the count is returned; otherwise the return is still the count that would
 * have been written.
 */
int pong_sim_step(PongSim *s, const PongSimInput *in_l, const PongSimInput *in_r,
                  PongSimEvent *events);

/** Ball speed magnitude in Q4. Integer sqrt, so it cannot drift. */
int32_t pong_sim_ball_speed_q4(const PongSim *s);

/** Integer square root, exact over the range this simulation uses. */
int32_t pong_sim_isqrt(int32_t n);

#endif /* PONG_SIM_H */
