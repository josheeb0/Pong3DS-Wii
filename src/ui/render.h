/*
 * The interface, drawn through the platform-agnostic renderer seam in gfx.h.
 *
 * Top    (400x240): the playfield, at exactly half field coordinates. The
 *                   800x480 field was chosen for precisely this -- the mapping
 *                   is one integer shift with no rounding.
 * Bottom (320x240): status, transport, and the touch strip, following the
 *                   original sketch ("TAP THE TOUCH SCREEN TO CONTINUE").
 */

#ifndef PONG_RENDER_H
#define PONG_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include "client.h"
#include "gfx.h"

typedef struct { float x, y, w, h; } PongRect;

/*
 * Menu items, in the order they appear.
 *
 * The renderer and the input handler share this list and the rect table below,
 * so a button that is drawn is always a button that can be pressed -- the two
 * cannot drift into disagreeing about where things are.
 */
typedef enum {
    MENU_QUICK = 0,   /* quick match: pair with anyone, 3DS vs browser preferred */
    MENU_ROOM,        /* join a room by code, so you can play someone specific */
    MENU_BOT,         /* practice against the CPU, on the server */
    MENU_LOCAL_AI,    /* practice against the CPU, right here -- no server */
    MENU_LOCAL_2P,    /* two people, one 3DS */
    MENU_NAME,        /* the name the other player sees */
    MENU_SERVER,      /* edit the server address */
    MENU_SOURCE,      /* toggle where updates come from */
    MENU_UPDATE,      /* check for a newer build */
    MENU_COUNT
} PongMenuItem;

extern const PongRect PONG_MENU_RECT[MENU_COUNT];

bool pong_ui_hit(const PongRect *r, float x, float y);

/** Which menu item is under a touch, or -1. */
int pong_ui_menu_hit(float x, float y);

typedef enum {
    SCREEN_TITLE = 0,
    SCREEN_CONNECTING,
    SCREEN_QUEUED,
    SCREEN_PLAY,
    SCREEN_GAMEOVER,
    SCREEN_ERROR,
} PongScreen;

typedef struct {
    PongScreen  screen;
    int         menu_sel;      /* highlighted item, for d-pad navigation */
    const char *room_code;     /* shown while waiting, so it can be read out */
    const char *update_src;    /* "game server" / "GitHub Releases" */
    uint32_t    build_id;
    /* Which difficulty the offline opponent plays at. Shown on the VS AI row
     * and changed with LEFT/RIGHT while that row is selected. */
    int         ai_level;
    const char *status_line;   /* transport description */
    const char *detail_line;   /* rtt / hz / opponent */
    const char *message;       /* errors, prompts */
    const char *diag;          /* multi-line per-transport diagnostic */
    uint32_t    rtt_ms;
    uint32_t    hz;
    bool        touch_hint;    /* pulse the "tap to continue" box */
    const char *server_addr;   /* shown in the address field on the title */
    bool        slow_mode;
    uint8_t     my_side;
    /* Both players' names, shown under the scores. The opponent's comes from
     * MATCH_START, so a browser player's chosen name appears on the console. */
    const char *my_name;
    const char *opp_name;
    uint32_t    frame;         /* for animation */
    /* Measured frames per second. On the HUD because "the ball is not smooth"
     * has two completely different causes -- the netcode or the frame rate --
     * and no amount of describing it from across a room distinguishes them. */
    uint32_t    fps;
} PongHud;

/** Draws one frame to both surfaces. Call between gfx frame begin/end. */
void pong_render_frame(const PongView *view, const PongHud *hud);

#endif /* PONG_RENDER_H */
