/*
 * Proves the C simulation agrees with the TypeScript one, tick for tick.
 *
 * src/sim/pong_sim.c is a port of server/src/sim/pong.ts, and exists so that a
 * local match -- against the built-in AI or a second player on the same device
 * -- plays by the same rules as an online one. That is a claim about two
 * separate implementations, and the only honest way to hold it is to run both
 * and compare.
 *
 * sim_trace.h is a recording of the TypeScript simulation: 6000 ticks with the
 * inputs that produced them. This replays those inputs through the C and
 * compares every field of the state on every tick.
 *
 * A mismatch is reported at the FIRST tick it happens, with both values, and
 * stops. A physics divergence cascades -- one wrong bounce makes every
 * subsequent tick wrong -- so a report of "6000 failures" would say nothing and
 * the first one says everything.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../src/sim/pong_sim.h"
#include "sim_trace.h"

static int fails = 0;

/** Reports the first field that differs, and says which tick. */
static int cmp_row(uint32_t i, const PongSim *s, const PongSimTraceRow *w)
{
#define FIELD(name, got, want)                                                 \
    if ((int64_t)(got) != (int64_t)(want)) {                                   \
        printf("  FAIL tick %u: %s = %lld, TypeScript says %lld\n",            \
               (unsigned)i, name, (long long)(got), (long long)(want));        \
        return 1;                                                              \
    }

    FIELD("tick",       s->tick,        w->tick)
    FIELD("state",      s->state,       w->state)
    FIELD("ball_x",     s->ball_x_q4,   w->ball_x)
    FIELD("ball_y",     s->ball_y_q4,   w->ball_y)
    FIELD("ball_vx",    s->ball_vx_q4,  w->ball_vx)
    FIELD("ball_vy",    s->ball_vy_q4,  w->ball_vy)
    FIELD("left_y",     s->left_y_q4,   w->left_y)
    FIELD("right_y",    s->right_y_q4,  w->right_y)
    FIELD("score_l",    s->score_l,     w->score_l)
    FIELD("score_r",    s->score_r,     w->score_r)
    FIELD("rng",        s->rng,         w->rng)
    FIELD("rally_hits", s->rally_hits,  w->rally_hits)
#undef FIELD
    return 0;
}

int main(void)
{
    printf("=== the C simulation against the TypeScript one ===\n");

    /*
     * Tunnelling. The ball can only be caught if no single tick steps it clean
     * across a paddle, which holds because the speed ceiling is below the
     * paddle width. Asserted rather than assumed, so raising the ceiling one
     * day fails here instead of producing a game where fast balls pass through
     * paddles roughly one time in twenty.
     */
    if (PONG_BALL_SPEED_MAX_Q4 >= PONG_PADDLE_W << PONG_Q4_SHIFT) {
        printf("  FAIL ball speed %d can step across a %d-wide paddle\n",
               PONG_BALL_SPEED_MAX_Q4, PONG_PADDLE_W << PONG_Q4_SHIFT);
        fails++;
    } else {
        printf("  ok   ball step %d stays under paddle width %d\n",
               PONG_BALL_SPEED_MAX_Q4, PONG_PADDLE_W << PONG_Q4_SHIFT);
    }

    PongSim s;
    pong_sim_create(&s, PONG_SIM_TRACE_SEED, PONG_SIM_TRACE_WIN_SCORE,
                    PONG_BALL_SPEED_MAX_Q4);

    uint32_t goals = 0, paddle_hits = 0, wall_hits = 0, diverged = 0;

    for (uint32_t i = 0; i < PONG_SIM_TRACE_TICKS; i++) {
        const PongSimTraceRow *w = &PONG_SIM_TRACE[i];

        PongSimInput l = { w->target_l, 0 };
        PongSimInput r = { w->target_r, 0 };
        PongSimEvent ev[PONG_SIM_MAX_EVENTS];
        int n = pong_sim_step(&s, &l, &r, ev);

        for (int k = 0; k < n; k++) {
            if (ev[k].kind == PONG_SIM_EV_GOAL) goals++;
            else if (ev[k].kind == PONG_SIM_EV_PADDLE_HIT) paddle_hits++;
            else if (ev[k].kind == PONG_SIM_EV_WALL_HIT) wall_hits++;
        }

        if (cmp_row(i, &s, w)) { diverged = 1; fails++; break; }
    }

    if (!diverged) {
        printf("  ok   %d ticks identical in every field\n", PONG_SIM_TRACE_TICKS);
        printf("  ok   final score %u-%u, matching TypeScript\n",
               s.score_l, s.score_r);
    }

    /*
     * A trace that ran but exercised nothing would pass the comparison above
     * while proving very little, so the interesting paths are counted here too.
     * The generator refuses to emit a trace that fails these; asserting them in
     * C as well is what keeps a hand-edited header honest.
     *
     * The thresholds are floors that catch a trace which stopped exercising a
     * path, not coverage targets. They can be low because divergence CASCADES:
     * one wrong bounce puts the ball somewhere else for every remaining tick,
     * so a single mismatched angle fails thousands of comparisons, not one.
     * The power of this test is the exact tick-by-tick compare above; these
     * only guarantee the trace still contains the situations at all.
     */
    printf("\n=== the trace covers what matters ===\n");
    struct { const char *what; uint32_t got; uint32_t least; } cov[] = {
        { "goals scored",   goals,       2 },
        { "paddle bounces", paddle_hits, 12 },
        { "wall bounces",   wall_hits,   10 },
    };
    for (size_t k = 0; k < sizeof cov / sizeof cov[0]; k++) {
        if (cov[k].got < cov[k].least) {
            printf("  FAIL only %u %s; the trace is not exercising it\n",
                   cov[k].got, cov[k].what);
            fails++;
        } else {
            printf("  ok   %u %s\n", cov[k].got, cov[k].what);
        }
    }

    /* Both goal directions. Scoring is two branches -- past x<0 and past
     * x>FIELD_W -- and a trace that only ever left one end would let a port
     * have the other backwards. */
    if (s.score_l == 0 || s.score_r == 0) {
        printf("  FAIL finished %u-%u; only one goal direction covered\n",
               s.score_l, s.score_r);
        fails++;
    } else {
        printf("  ok   both ends conceded (%u-%u)\n", s.score_l, s.score_r);
    }

    /* The match must have ENDED, or the game-over branch is untested. */
    if (s.state != PONG_SIM_GAME_OVER) {
        printf("  FAIL trace ended in state %u, not GAME_OVER\n", s.state);
        fails++;
    } else {
        printf("  ok   reached GAME_OVER\n");
    }

    printf("\n");
    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASSED: C simulation agrees with TypeScript\n");
    return 0;
}
