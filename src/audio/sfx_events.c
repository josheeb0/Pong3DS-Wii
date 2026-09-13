#include "sfx_events.h"
#include "pong_proto.h"

bool pong_sfx_for_event(uint8_t kind, uint8_t winner, uint8_t my_side,
                        PongSfx *out)
{
    switch (kind) {
    case PONG_EVENT_KIND_PADDLE_HIT:      *out = PONG_SFX_PADDLE;    return true;
    case PONG_EVENT_KIND_WALL_HIT:        *out = PONG_SFX_WALL;      return true;
    case PONG_EVENT_KIND_GOAL:            *out = PONG_SFX_GOAL;      return true;
    case PONG_EVENT_KIND_COUNTDOWN_START: *out = PONG_SFX_COUNTDOWN; return true;

    case PONG_EVENT_KIND_MATCH_OVER:
        /* The same event means opposite things to the two players, which is the
         * whole reason this takes a side. */
        *out = (winner == my_side) ? PONG_SFX_WIN : PONG_SFX_LOSE;
        return true;

    /* Someone leaving or rejoining is a message, not a noise. A sound here
     * would fire on a flaky connection every time it blipped. */
    default:
        return false;
    }
}
