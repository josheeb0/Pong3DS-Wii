#ifndef PONG_BOT_H
#define PONG_BOT_H

/*
 * A practice opponent that runs on the client.
 *
 * A port of server/src/game/bot.ts, so the offline opponent behaves like the
 * one VS CPU gives you online rather than being a second, different AI with its
 * own feel.
 *
 * Unlike the simulation it plays in, this is NOT required to be bit-identical
 * to the TypeScript. Nothing is shared across a network here -- the bot's
 * decisions are inputs to a match that exists only on this device -- so where
 * the TypeScript uses a fractional drift this uses integer arithmetic. The
 * behaviour is indistinguishable; the guarantee is deliberately weaker because
 * paying for it would buy nothing.
 */

#include <stdint.h>
#include "pong_sim.h"

/** How hard the opponent is. Difficulty is skill, plus how long it takes to
 *  react and how far off it aims -- all three move together. */
typedef enum {
    PONG_BOT_EASY = 0,
    PONG_BOT_NORMAL,
    PONG_BOT_HARD,
    PONG_BOT_LEVEL_COUNT,
} PongBotLevel;

/** "EASY" / "NORMAL" / "HARD", for the menu. */
const char *pong_bot_level_name(PongBotLevel level);

typedef struct {
    uint8_t  side;      /* 0 = left, 1 = right */
    int32_t  skill_q8;  /* 0..256; higher tracks tighter and reacts sooner */
    int32_t  target;
    int32_t  cooldown;
    uint32_t rng;
} PongBot;

/** Seeds the bot. `seed` only varies the jitter, never the difficulty. */
void pong_bot_init(PongBot *b, uint8_t side, PongBotLevel level, uint32_t seed);

/** The bot's input for this tick, given what it can see. */
PongSimInput pong_bot_think(PongBot *b, const PongSim *s);

#endif /* PONG_BOT_H */
