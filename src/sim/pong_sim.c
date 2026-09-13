#include "pong_sim.h"
#include "pong_trig.h"
/* pong_step_paddle lives with the client rather than here. The clients ran it
 * for prediction long before this file existed, and one definition is the whole
 * point -- a second copy is how a paddle starts rubber-banding. */
#include "client.h"

/* Field and paddle geometry in Q4, derived once so the expressions below read
 * like the TypeScript they mirror. */
#define FIELD_W_Q     (PONG_FIELD_W << PONG_Q4_SHIFT)
#define FIELD_H_Q     (PONG_FIELD_H << PONG_Q4_SHIFT)
#define PADDLE_W_Q    (PONG_PADDLE_W << PONG_Q4_SHIFT)
#define PADDLE_H_Q    (PONG_PADDLE_H << PONG_Q4_SHIFT)
#define PADDLE_HALF_Q (PADDLE_H_Q >> 1)
#define BALL_R_Q      (PONG_BALL_R << PONG_Q4_SHIFT)
#define PADDLE_X_L_Q  (PONG_PADDLE_X_L << PONG_Q4_SHIFT)
#define PADDLE_X_R_Q  (PONG_PADDLE_X_R << PONG_Q4_SHIFT)

/* How far off centre a hit can land and still count as contact. */
#define CONTACT_SPAN_Q (PADDLE_HALF_Q + BALL_R_Q)

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * xorshift32.
 *
 * uint32_t throughout, which is what makes this match the TypeScript: there the
 * shifts are written with >>> specifically to force unsigned 32-bit semantics
 * onto JavaScript's doubles. Using a signed type here would reintroduce exactly
 * the difference that was being avoided.
 */
static uint32_t next_rng(uint32_t s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s == 0 ? 0x1d872b41u : s;
}

int32_t pong_sim_isqrt(int32_t n)
{
    if (n <= 0) return 0;
    int32_t x = n;
    int32_t y = (x + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return x;
}

int32_t pong_sim_ball_speed_q4(const PongSim *s)
{
    /* The product fits comfortably: the per-axis speed ceiling is 176, so the
     * sum of squares cannot approach the 32-bit limit. */
    int32_t vx = s->ball_vx_q4;
    int32_t vy = s->ball_vy_q4;
    return pong_sim_isqrt(vx * vx + vy * vy);
}

/*
 * Sets velocity from an angle index and a speed.
 *
 * `idx` indexes the shared quarter-wave table, which is generated for C and
 * TypeScript from one source, so a bounce cannot leave at different angles on
 * different platforms.
 */
static void set_velocity(PongSim *s, int32_t idx, int32_t speed_q4,
                         int32_t dir_x, int32_t dir_y)
{
    int32_t i = clamp32(idx < 0 ? -idx : idx, 0, PONG_TRIG_STEPS);
    int32_t cos = PONG_COS_Q12[i];
    int32_t sin = PONG_SIN_Q12[i];

    s->ball_vx_q4 = dir_x * ((speed_q4 * cos) >> 12);
    s->ball_vy_q4 = dir_y * ((speed_q4 * sin) >> 12);

    /* A perfectly horizontal rally is dull and a perfectly vertical one never
     * ends; nudge vx so the ball always crosses the field eventually. */
    if (s->ball_vx_q4 == 0) s->ball_vx_q4 = dir_x * 8;
}

void pong_sim_create(PongSim *s, uint32_t seed, uint8_t win_score,
                     int32_t speed_max_q4)
{
    s->tick = 0;
    s->state = PONG_SIM_COUNTDOWN;
    s->ball_x_q4 = FIELD_W_Q >> 1;
    s->ball_y_q4 = FIELD_H_Q >> 1;
    s->ball_vx_q4 = 0;
    s->ball_vy_q4 = 0;
    s->left_y_q4 = FIELD_H_Q >> 1;
    s->right_y_q4 = FIELD_H_Q >> 1;
    s->score_l = 0;
    s->score_r = 0;
    /* xorshift32 is absorbing at zero: a zero seed would serve an identical
     * ball for the whole match. */
    s->rng = seed ? seed : 0x9e3779b9u;
    s->phase_until = PONG_COUNTDOWN_TICKS;
    s->speed_max_q4 = speed_max_q4;
    s->rally_hits = 0;
    s->win_score = win_score;
}

/** Centres the ball and gives it a fresh serve toward `dir_x`. */
static void serve(PongSim *s, int32_t dir_x)
{
    s->ball_x_q4 = FIELD_W_Q >> 1;
    s->ball_y_q4 = FIELD_H_Q >> 1;
    s->rally_hits = 0;

    s->rng = next_rng(s->rng);
    /* Within about 30 degrees, so the opening ball is always returnable. */
    int32_t idx = (int32_t)(s->rng % 22u);
    int32_t dir_y = (s->rng & 0x10000u) ? 1 : -1;
    set_velocity(s, idx, PONG_BALL_SPEED_START_Q4, dir_x, dir_y);
}

/*
 * Ball-versus-paddle overlap, both treated as boxes.
 *
 * Tunnelling is impossible by construction: the per-tick ball step is capped at
 * PONG_BALL_SPEED_MAX_Q4 (176), below the paddle width (192), so no tick can
 * step the ball across a paddle without landing inside it. The host test
 * asserts that inequality, so raising the speed cannot silently break it.
 */
static bool overlaps_paddle(int32_t ball_x, int32_t ball_y,
                            int32_t pad_x, int32_t pad_y)
{
    int32_t dx = ball_x - pad_x; if (dx < 0) dx = -dx;
    if (dx > (PADDLE_W_Q >> 1) + BALL_R_Q) return false;
    int32_t dy = ball_y - pad_y; if (dy < 0) dy = -dy;
    if (dy > CONTACT_SPAN_Q) return false;
    return true;
}

/*
 * Reflects the ball off a paddle: classic Atari, where the outgoing ANGLE is a
 * pure function of where the ball struck and speed ramps a fixed step per hit.
 * Paddle motion is ignored.
 *
 * The C division truncates toward zero and so does JavaScript's Math.trunc, so
 * a hit above centre and its mirror below produce mirrored angles on both. A
 * language that floored instead would put a one-step bias into every upward
 * bounce, which is the kind of difference that shows up as "the online game
 * feels different from practice" rather than as a test failure.
 */
static void apply_paddle_bounce(PongSim *s, int32_t pad_y, int32_t dir_x)
{
    int32_t offset = clamp32(s->ball_y_q4 - pad_y, -CONTACT_SPAN_Q, CONTACT_SPAN_Q);

    int32_t idx = (offset * PONG_BALL_MAX_ANGLE_STEPS) / CONTACT_SPAN_Q;
    int32_t dir_y = (offset == 0) ? (s->ball_vy_q4 >= 0 ? 1 : -1)
                                  : (offset > 0 ? 1 : -1);

    s->rally_hits++;
    int32_t speed = pong_sim_ball_speed_q4(s) + PONG_BALL_SPEED_STEP_Q4;
    if (speed > s->speed_max_q4) speed = s->speed_max_q4;

    set_velocity(s, idx, speed, dir_x, dir_y);
}

/** Ends a point: stops the ball, and either ends the match or freezes. */
static int end_point(PongSim *s, PongSimEvent *events, int n)
{
    s->ball_vx_q4 = 0;
    s->ball_vy_q4 = 0;

    if (s->score_l >= s->win_score || s->score_r >= s->win_score) {
        s->state = PONG_SIM_GAME_OVER;
        if (events) {
            events[n].kind = PONG_SIM_EV_MATCH_OVER;
            events[n].a = (uint8_t)(s->score_l > s->score_r ? 0 : 1);
            events[n].b = s->score_l;
            events[n].c = s->score_r;
        }
        return n + 1;
    }

    s->state = PONG_SIM_GOAL_FREEZE;
    s->phase_until = s->tick + PONG_GOAL_FREEZE_TICKS;
    return n;
}

int pong_sim_step(PongSim *s, const PongSimInput *in_l, const PongSimInput *in_r,
                  PongSimEvent *events)
{
    int n = 0;
    s->tick++;

    /* Paddles move on EVERY tick, in every phase. That is what makes a client
     * sending input at 10Hz look continuous: between its updates the paddle
     * keeps gliding toward the last stated target. */
    s->left_y_q4 = pong_step_paddle(s->left_y_q4, in_l->target_y_q4);
    s->right_y_q4 = pong_step_paddle(s->right_y_q4, in_r->target_y_q4);

    if (s->state == PONG_SIM_COUNTDOWN) {
        if (s->tick >= s->phase_until) {
            s->state = PONG_SIM_PLAY;
            s->rng = next_rng(s->rng);
            serve(s, (s->rng & 1u) ? 1 : -1);
        }
        return n;
    }

    if (s->state == PONG_SIM_GOAL_FREEZE) {
        if (s->tick >= s->phase_until) {
            s->state = PONG_SIM_PLAY;
            /* Served toward whoever conceded. The ball is resting past the edge
             * it left through, so a ball on the left half means the left player
             * conceded and receives. */
            serve(s, s->ball_x_q4 < (FIELD_W_Q >> 1) ? -1 : 1);
        }
        return n;
    }

    if (s->state != PONG_SIM_PLAY) return n;

    s->ball_x_q4 += s->ball_vx_q4;
    s->ball_y_q4 += s->ball_vy_q4;

    /* Walls. Reflecting the overshoot rather than clamping keeps the ball's
     * path symmetrical about the wall, which is what stops it crawling along
     * the rail at shallow angles. */
    if (s->ball_y_q4 - BALL_R_Q < 0 && s->ball_vy_q4 < 0) {
        s->ball_y_q4 = BALL_R_Q + (BALL_R_Q - s->ball_y_q4);
        s->ball_vy_q4 = -s->ball_vy_q4;
        if (events) { events[n].kind = PONG_SIM_EV_WALL_HIT; events[n].a = 0;
                      events[n].b = 0; events[n].c = 0; }
        n++;
    } else if (s->ball_y_q4 + BALL_R_Q > FIELD_H_Q && s->ball_vy_q4 > 0) {
        int32_t over = s->ball_y_q4 + BALL_R_Q - FIELD_H_Q;
        s->ball_y_q4 = FIELD_H_Q - BALL_R_Q - over;
        s->ball_vy_q4 = -s->ball_vy_q4;
        if (events) { events[n].kind = PONG_SIM_EV_WALL_HIT; events[n].a = 1;
                      events[n].b = 0; events[n].c = 0; }
        n++;
    }

    /* Only the paddle the ball is travelling toward is tested, so a ball
     * leaving a paddle cannot immediately collide with it again. */
    if (s->ball_vx_q4 < 0 &&
        overlaps_paddle(s->ball_x_q4, s->ball_y_q4, PADDLE_X_L_Q, s->left_y_q4)) {
        s->ball_x_q4 = PADDLE_X_L_Q + (PADDLE_W_Q >> 1) + BALL_R_Q;
        apply_paddle_bounce(s, s->left_y_q4, +1);
        if (events) { events[n].kind = PONG_SIM_EV_PADDLE_HIT; events[n].a = 0;
                      events[n].b = (uint8_t)(s->rally_hits & 0xffu); events[n].c = 0; }
        n++;
    } else if (s->ball_vx_q4 > 0 &&
               overlaps_paddle(s->ball_x_q4, s->ball_y_q4, PADDLE_X_R_Q, s->right_y_q4)) {
        s->ball_x_q4 = PADDLE_X_R_Q - (PADDLE_W_Q >> 1) - BALL_R_Q;
        apply_paddle_bounce(s, s->right_y_q4, -1);
        if (events) { events[n].kind = PONG_SIM_EV_PADDLE_HIT; events[n].a = 1;
                      events[n].b = (uint8_t)(s->rally_hits & 0xffu); events[n].c = 0; }
        n++;
    }

    if (s->ball_x_q4 + BALL_R_Q < 0) {
        s->score_r++;
        if (events) { events[n].kind = PONG_SIM_EV_GOAL; events[n].a = 1;
                      events[n].b = s->score_l; events[n].c = s->score_r; }
        n++;
        n = end_point(s, events, n);
    } else if (s->ball_x_q4 - BALL_R_Q > FIELD_W_Q) {
        s->score_l++;
        if (events) { events[n].kind = PONG_SIM_EV_GOAL; events[n].a = 0;
                      events[n].b = s->score_l; events[n].c = s->score_r; }
        n++;
        n = end_point(s, events, n);
    }

    return n;
}
