/*
 * Interpolation and extrapolation, on the host.
 *
 * The 3DS client's view of the world is the part a player actually sees, and
 * it is also the part that is hardest to judge on hardware -- "it looks jagged"
 * is a real report but not a measurement. These tests assert the shape of the
 * motion directly: that a starved client keeps moving, that it moves the way
 * the server would move it, and that it refuses to keep guessing forever.
 *
 * Runs natively because client.c has no libctru dependency.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/game/client.h"

static int failures = 0;

static void ok(int cond, const char *what, const char *detail)
{
    printf("  %-4s %-46s %s\n", cond ? "ok" : "FAIL", what, detail ? detail : "");
    if (!cond) failures++;
}

/* Feeds one snapshot as if it had arrived from the wire. */
static void feed(PongClient *c, uint32_t tick, int32_t bx, int32_t by,
                 int32_t vx, int32_t vy, uint8_t state, uint32_t now_ms)
{
    PongSNAPSHOT s;
    memset(&s, 0, sizeof s);
    s.tick = tick;
    s.ball_xq4 = (int16_t)bx;  s.ball_yq4 = (int16_t)by;
    s.ball_vxq4 = (int16_t)vx; s.ball_vyq4 = (int16_t)vy;
    s.left_yq4  = (int16_t)(PONG_FIELD_H_Q4 / 2);
    s.right_yq4 = (int16_t)(PONG_FIELD_H_Q4 / 2);
    s.state = state;
    pong_client_on_snapshot(c, &s, now_ms);
}

/* Starts a match on the given side, so the view has somewhere to put us. */
static void start_match(PongClient *c, uint8_t side)
{
    PongMATCH_START m;
    memset(&m, 0, sizeof m);
    m.match_id = 1;
    m.your_side = side;
    m.win_score = 11;
    pong_client_on_match_start(c, &m);
}

/* Pins the clock so the tests are about interpolation, not clock estimation. */
static void sync_clock(PongClient *c, uint32_t tick, uint32_t now_ms)
{
    PongPONG p;
    memset(&p, 0, sizeof p);
    p.client_time_ms = now_ms;
    p.server_time_ms = tick * 1000 / PONG_TICK_HZ;
    p.server_tick = tick;
    pong_client_on_pong(c, &p, now_ms);
}

int main(void)
{
    printf("\n=== ball keeps moving when snapshots stop ===\n");
    {
        PongClient c;
        pong_client_init(&c);
        start_match(&c, 0);

        const int32_t vx = 64, vy = 0;             /* Q4 per tick, level flight */
        int32_t bx = PONG_FIELD_W_Q4 / 4, by = PONG_FIELD_H_Q4 / 2;

        uint32_t now = 1000;
        for (uint32_t t = 100; t <= 130; t++) {
            sync_clock(&c, t, now);
            feed(&c, t, bx, by, vx, vy, PONG_MATCH_STATE_PLAY, now);
            bx += vx;
            now += 1000 / PONG_TICK_HZ;
        }

        /*
         * Nothing further arrives; the clock keeps running. BOTH samples are
         * taken past the newest snapshot on purpose: sampling the first one
         * mid-history would let plain clamping separate them, and the test
         * would pass without any prediction happening at all. It did, when I
         * first wrote it that way.
         */
        PongView v1, v2;
        sync_clock(&c, 130 + 9, now + 150);
        pong_client_update(&c, now + 150, &v1);
        sync_clock(&c, 130 + 12, now + 200);
        pong_client_update(&c, now + 200, &v2);

        ok(v1.valid && v2.valid, "view stays valid with no new snapshots", NULL);
        ok(v1.extrapolated, "first sample is already past the newest snapshot", NULL);

        char d[80];
        snprintf(d, sizeof d, "%ld -> %ld", (long)v1.ball_x, (long)v2.ball_x);
        ok(v2.ball_x > v1.ball_x, "ball advances instead of freezing", d);
        ok(v2.extrapolated, "and says it is predicted, not observed", NULL);
    }

    printf("\n=== prediction is capped, not open-ended ===\n");
    {
        PongClient c;
        pong_client_init(&c);
        start_match(&c, 0);

        const int32_t vx = 64;
        int32_t bx = PONG_FIELD_W_Q4 / 4, by = PONG_FIELD_H_Q4 / 2;
        uint32_t now = 1000;
        for (uint32_t t = 100; t <= 130; t++) {
            sync_clock(&c, t, now);
            feed(&c, t, bx, by, vx, 0, PONG_MATCH_STATE_PLAY, now);
            bx += vx;
            now += 1000 / PONG_TICK_HZ;
        }

        PongView a, b;
        sync_clock(&c, 130 + 30, now + 500);     /* half a second adrift */
        pong_client_update(&c, now + 500, &a);
        sync_clock(&c, 130 + 120, now + 2000);   /* two seconds adrift */
        pong_client_update(&c, now + 2000, &b);

        char d[80];
        snprintf(d, sizeof d, "500ms=%ld  2000ms=%ld", (long)a.ball_x, (long)b.ball_x);
        ok(a.ball_x == b.ball_x, "stops predicting past the cap", d);

        int32_t last_seen = bx - vx;
        int32_t ahead = b.ball_x - last_seen;
        snprintf(d, sizeof d, "%ld Q4 ahead (<= %d)", (long)ahead, 9 * 64);
        ok(ahead > 0 && ahead <= 10 * vx, "and predicts at most ~150ms of travel", d);
    }

    printf("\n=== prediction respects the walls ===\n");
    {
        PongClient c;
        pong_client_init(&c);
        start_match(&c, 0);

        /* Aimed hard at the top wall from close to it. */
        const int32_t vy = -96;
        int32_t by = (PONG_BALL_R << PONG_Q4_SHIFT) + 32;
        uint32_t now = 1000;
        for (uint32_t t = 100; t <= 130; t++) {
            sync_clock(&c, t, now);
            feed(&c, t, PONG_FIELD_W_Q4 / 2, by, 0, vy, PONG_MATCH_STATE_PLAY, now);
            now += 1000 / PONG_TICK_HZ;
        }

        PongView v;
        sync_clock(&c, 130 + 9, now + 150);
        pong_client_update(&c, now + 150, &v);

        int32_t lo = PONG_BALL_R << PONG_Q4_SHIFT;
        int32_t hi = PONG_FIELD_H_Q4 - lo;
        char d[80];
        snprintf(d, sizeof d, "y=%ld in [%ld,%ld]", (long)v.ball_y, (long)lo, (long)hi);
        ok(v.ball_y >= lo && v.ball_y <= hi, "predicted ball never leaves the field", d);
    }

    printf("\n=== a ball that is not in play is not predicted ===\n");
    {
        PongClient c;
        pong_client_init(&c);
        start_match(&c, 0);

        uint32_t now = 1000;
        for (uint32_t t = 100; t <= 130; t++) {
            sync_clock(&c, t, now);
            /* Velocity still set, but the match is frozen after a goal. */
            feed(&c, t, PONG_FIELD_W_Q4 / 2, PONG_FIELD_H_Q4 / 2, 64, 0,
                 PONG_MATCH_STATE_GOAL_FREEZE, now);
            now += 1000 / PONG_TICK_HZ;
        }

        PongView v1, v2;
        pong_client_update(&c, now, &v1);
        sync_clock(&c, 130 + 12, now + 200);
        pong_client_update(&c, now + 200, &v2);

        ok(v1.ball_x == v2.ball_x, "a frozen ball stays put", NULL);
        ok(!v2.extrapolated, "and is not reported as predicted", NULL);
    }

    if (failures) {
        printf("\nFAILED: %d check(s)\n\n", failures);
        return 1;
    }
    printf("\nPASSED: interpolation and extrapolation\n\n");
    return 0;
}
