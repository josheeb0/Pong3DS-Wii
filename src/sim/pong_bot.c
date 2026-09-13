#include "pong_bot.h"

#define FIELD_H_Q     (PONG_FIELD_H << PONG_Q4_SHIFT)
#define PADDLE_HALF_Q ((PONG_PADDLE_H << PONG_Q4_SHIFT) >> 1)

/*
 * Difficulty, as a skill value in Q8.
 *
 * The values are measured, not guessed. 3ds/test/test_local.c plays each level
 * against a perfect tracker and counts the balls it returns before losing:
 *
 *     EASY 435    NORMAL 8647    HARD 18468
 *
 * The first attempt used 128/210/240, which reads like a sensible spread and
 * was not one -- NORMAL and HARD came out within 1% of each other, so the HARD
 * setting did nothing a player would notice. Skill saturates: past about 0.8
 * the bot already reaches nearly every returnable ball, and the remaining
 * difficulty has to come from the reaction delay, which is why the levels are
 * spaced widely at the bottom and only reach for the ceiling at the top.
 *
 * Perfect play is not offered, and could not be beaten if it were: the paddle
 * speed clamp applies equally to both sides, so a bot aiming exactly at the
 * ball returns everything physically returnable.
 */
static const int32_t SKILL_Q8[PONG_BOT_LEVEL_COUNT] = {
    [PONG_BOT_EASY]   = 80,    /* 0.31 -- beatable by someone who has never played */
    [PONG_BOT_NORMAL] = 150,   /* 0.59 -- a game worth winning */
    [PONG_BOT_HARD]   = 250,   /* 0.98 -- near the ceiling; punishes a loose return */
};

static const char *const LEVEL_NAME[PONG_BOT_LEVEL_COUNT] = {
    [PONG_BOT_EASY]   = "EASY",
    [PONG_BOT_NORMAL] = "NORMAL",
    [PONG_BOT_HARD]   = "HARD",
};

const char *pong_bot_level_name(PongBotLevel level)
{
    if (level < 0 || level >= PONG_BOT_LEVEL_COUNT) return "NORMAL";
    return LEVEL_NAME[level];
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t next_rng(PongBot *b)
{
    uint32_t s = b->rng;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    b->rng = s ? s : 0x1d872b41u;
    return b->rng;
}

void pong_bot_init(PongBot *b, uint8_t side, PongBotLevel level, uint32_t seed)
{
    b->side = side;
    b->skill_q8 = SKILL_Q8[(level < 0 || level >= PONG_BOT_LEVEL_COUNT)
                           ? PONG_BOT_NORMAL : level];
    b->target = FIELD_H_Q >> 1;
    b->cooldown = 0;
    /* Mixed with the side so two bots in the same match -- which the soak test
     * does -- do not make identical mistakes at identical moments. */
    b->rng = (seed ^ 0x2545f491u ^ (uint32_t)(side + 1)) | 1u;
}

PongSimInput pong_bot_think(PongBot *b, const PongSim *s)
{
    /*
     * Only tracks while the ball is coming toward it.
     *
     * This is most of what makes the bot feel like an opponent rather than a
     * servo: it commits to a position, and a ball that changes direction after
     * it has committed is one it has to recover from, exactly as a player does.
     */
    bool approaching = (b->side == 0) ? (s->ball_vx_q4 < 0) : (s->ball_vx_q4 > 0);

    if (b->cooldown > 0) {
        b->cooldown--;
    } else if (approaching) {
        /* Re-aims periodically rather than every tick, so it visibly reacts
         * rather than gliding. Lower skill aims further off and waits longer. */
        int32_t jitter_range = ((256 - b->skill_q8) * PADDLE_HALF_Q * 2) >> 8;
        int32_t jitter = 0;
        if (jitter_range > 0) {
            jitter = (int32_t)(next_rng(b) % (uint32_t)(jitter_range * 2)) - jitter_range;
        }
        b->target = s->ball_y_q4 + jitter;
        b->cooldown = 2 + (((256 - b->skill_q8) * 14) >> 8);
    } else {
        /* Drifts back toward the middle while the ball is away, which is both
         * what a player does and what stops it parking at the last ball's
         * height and starting the next rally already out of position. */
        b->target += ((FIELD_H_Q >> 1) - b->target) / 32;
    }

    PongSimInput in;
    in.target_y_q4 = clamp32(b->target, PADDLE_HALF_Q, FIELD_H_Q - PADDLE_HALF_Q);
    in.buttons = 0;
    return in;
}
