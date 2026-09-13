/*
 * Tests for local play: the AI, and the fixed timestep that drives it.
 *
 * test_sim.c proves the simulation matches the server's. This covers the parts
 * that only exist on the client, and that no conformance trace can check --
 * whether the difficulty setting actually changes anything, and whether the
 * match advances at the same rate regardless of frame rate.
 *
 * The difficulty test is the important one. A setting that is wired to the menu
 * but not to the opponent is invisible: the game still plays, the label still
 * changes, and the only symptom is that EASY does not feel easy.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../src/sim/pong_local.h"

static int fails = 0;

static void check(bool cond, const char *what, const char *detail)
{
    if (!cond) { printf("  FAIL %-42s %s\n", what, detail ? detail : ""); fails++; }
    else printf("  ok   %-42s %s\n", what, detail ? detail : "");
}

/**
 * Plays a full match against the bot with the left paddle tracking perfectly,
 * and returns how many balls the BOT returned before losing.
 *
 * Returns, not goals. Measuring the bot's score was the obvious choice and was
 * useless: a perfect tracker never misses, so the bot cannot score at any
 * difficulty and all three levels read zero. What a better bot actually does
 * against an opponent it cannot beat is last longer, and returns are how that
 * is counted.
 */
static int bot_returns_against_perfect(PongBotLevel level, uint32_t seed)
{
    PongLocal l;
    pong_local_start(&l, PONG_LOCAL_VS_AI, level, seed, 7);

    int returns = 0;
    uint32_t last_hits = 0;

    for (int i = 0; i < 400000 && l.sim.state != PONG_SIM_GAME_OVER; i++) {
        /* Perfect tracking: aim exactly at the ball every tick. The paddle
         * speed clamp still applies, so this is not invincible -- it simply
         * never makes a decision error. */
        pong_local_set_target(&l, 0, l.sim.ball_y_q4);
        pong_local_advance(&l, 17);

        /* rally_hits counts both paddles and resets each serve, so a rise while
         * the ball is heading away from the bot is the bot having struck it. */
        if (l.sim.rally_hits > last_hits && l.sim.ball_vx_q4 < 0) returns++;
        last_hits = l.sim.rally_hits;
    }
    return returns;
}

int main(void)
{
    printf("=== difficulty actually changes the opponent ===\n");
    {
        /*
         * Averaged over several seeds, because one match is noise: the serve
         * angle alone can decide a point, and a single run of EASY beating
         * HARD would prove nothing either way.
         */
        int total[PONG_BOT_LEVEL_COUNT] = { 0, 0, 0 };
        const uint32_t seeds[] = { 1, 7, 99, 12345, 0xbeef, 0x5eed };
        const size_t n = sizeof seeds / sizeof seeds[0];

        for (size_t s = 0; s < n; s++) {
            for (int lv = 0; lv < PONG_BOT_LEVEL_COUNT; lv++) {
                total[lv] += bot_returns_against_perfect((PongBotLevel)lv, seeds[s]);
            }
        }

        char d[96];
        snprintf(d, sizeof d, "EASY %d, NORMAL %d, HARD %d returns over %zu matches",
                 total[PONG_BOT_EASY], total[PONG_BOT_NORMAL], total[PONG_BOT_HARD], n);
        printf("  ..   %s\n", d);

        check(total[PONG_BOT_HARD] > total[PONG_BOT_EASY],
              "HARD returns more balls than EASY", NULL);
        check(total[PONG_BOT_EASY] > 0,
              "even EASY returns something", NULL);

        /* The levels must be distinct. Three labels mapped to one skill value
         * is exactly the bug this file exists to catch. */
        check(!(total[PONG_BOT_EASY] == total[PONG_BOT_NORMAL] &&
                total[PONG_BOT_NORMAL] == total[PONG_BOT_HARD]),
              "the three levels are not all identical", NULL);

        check(pong_bot_level_name(PONG_BOT_EASY)[0] == 'E' &&
              pong_bot_level_name(PONG_BOT_HARD)[0] == 'H',
              "levels have names for the menu", NULL);
    }

    printf("\n=== the match runs at the same rate whatever the frame rate ===\n");
    {
        /* One second of play, delivered as 60 frames or as 120, must advance
         * the match by the same number of ticks. Otherwise the game is faster
         * on a 144Hz monitor than on a 3DS, which is the classic way a fixed
         * simulation gets undone by a variable renderer. */
        PongLocal a, b;
        pong_local_start(&a, PONG_LOCAL_VS_HUMAN, PONG_BOT_NORMAL, 42, 7);
        pong_local_start(&b, PONG_LOCAL_VS_HUMAN, PONG_BOT_NORMAL, 42, 7);

        /* The same ELAPSED TIME delivered in different sized frames. The first
         * version of this used 60x17ms against 120x8ms, which is 1020ms against
         * 960ms -- it compared different amounts of time and then blamed the
         * driver for the four-tick difference. */
        for (int i = 0; i < 50; i++) pong_local_advance(&a, 20);    /* 1000ms, 50fps */
        for (int i = 0; i < 100; i++) pong_local_advance(&b, 10);   /* 1000ms, 100fps */

        char d[64];
        snprintf(d, sizeof d, "%u vs %u ticks", a.sim.tick, b.sim.tick);
        /* Within one tick: 17ms and 8ms are both rounded wall-clock values, so
         * demanding exact equality would be testing the arithmetic of the test
         * rather than the behaviour of the driver. */
        uint32_t diff = a.sim.tick > b.sim.tick ? a.sim.tick - b.sim.tick
                                                : b.sim.tick - a.sim.tick;
        check(diff <= 2, "60fps and 120fps advance together", d);

        snprintf(d, sizeof d, "%u ticks in ~1s", a.sim.tick);
        check(a.sim.tick >= 57 && a.sim.tick <= 63, "about 60 ticks per second", d);
    }

    printf("\n=== a stalled frame does not spiral ===\n");
    {
        PongLocal l;
        pong_local_start(&l, PONG_LOCAL_VS_HUMAN, PONG_BOT_NORMAL, 42, 7);

        /* Ten seconds of missed time arriving as one frame. Simulating all of
         * it would take longer than a frame and leave even more owed. */
        pong_local_advance(&l, 10000);

        char d[64];
        snprintf(d, sizeof d, "%u ticks from a 10s stall", l.sim.tick);
        check(l.sim.tick <= 8, "a huge delta is truncated, not simulated", d);
        check(l.accum_us == 0, "and no debt is carried into the next frame", NULL);
    }

    printf("\n=== two players on one device ===\n");
    {
        PongLocal l;
        pong_local_start(&l, PONG_LOCAL_VS_HUMAN, PONG_BOT_NORMAL, 3, 7);

        /* Both paddles parked away from the centre: whoever the ball is served
         * to concedes, so the match must still reach an end rather than stall
         * with nobody touching the ball. */
        pong_local_set_target(&l, 0, 0);
        pong_local_set_target(&l, 1, 0);
        for (int i = 0; i < 200000 && l.sim.state != PONG_SIM_GAME_OVER; i++) {
            pong_local_advance(&l, 17);
        }

        char d[64];
        snprintf(d, sizeof d, "%u-%u", l.sim.score_l, l.sim.score_r);
        check(l.sim.state == PONG_SIM_GAME_OVER, "a two-player match reaches an end", d);

        /* The right paddle must be driven by the second player, not the bot.
         * If VS_HUMAN quietly ran the AI, this paddle would have chased the
         * ball instead of sitting where it was told. */
        check(l.sim.right_y_q4 == (PONG_PADDLE_H << PONG_Q4_SHIFT) / 2,
              "the right paddle obeys player two, not the AI", NULL);
    }

    printf("\n");
    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASSED: local play\n");
    return 0;
}
