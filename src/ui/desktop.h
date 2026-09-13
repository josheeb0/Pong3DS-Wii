/*
 * The desktop interface.
 *
 * A separate interface from the handheld one in render.h, not a port of it.
 * The 3DS lays out two small screens with a touch panel; a desktop window is
 * one large canvas with a keyboard and a pad, and the browser client is already
 * its own thing for the same reason. Sharing the layout would have meant both
 * being slightly wrong.
 *
 * What IS shared is everything below the interface: the renderer seam, the
 * protocol, the netcode and the simulation. Only the arrangement differs.
 */

#ifndef PONG_DESKTOP_H
#define PONG_DESKTOP_H

#include <stdbool.h>
#include <stdint.h>
#include "client.h"
#include "gfx.h"

typedef enum {
    DESK_MENU = 0,
    DESK_CONNECTING,
    DESK_WAITING,
    DESK_PLAY,
    DESK_GAMEOVER,
    DESK_ERROR,
} DeskScreen;

/* Menu entries, in the order they are drawn. */
typedef enum {
    DESK_ITEM_QUICK = 0,
    DESK_ITEM_ROOM,
    DESK_ITEM_BOT,
    DESK_ITEM_LOCAL_AI,   /* offline, against the built-in opponent */
    DESK_ITEM_LOCAL_2P,   /* offline, two people on this device */
    DESK_ITEM_SERVER,
    DESK_ITEM_NAME,
    DESK_ITEM_FULLSCREEN,
    DESK_ITEM_QUIT,
    DESK_ITEM_COUNT
} DeskItem;

/*
 * Not every item exists on every platform, so the menu is addressed two ways.
 *
 * `DeskItem` is the identity -- what a row MEANS -- and is what `sel` holds and
 * what callers compare against. The visible ROW is where it sits on screen, and
 * a hidden item has none. Keeping them apart is what lets the Vita drop the
 * fullscreen row without renumbering everything after it, and without a caller
 * having to know the row it is looking at is not the item it wants.
 */

/** "EASY" / "NORMAL" / "HARD" for the VS AI row's value. Takes an int so the
 *  UI header does not have to know the simulation's enum. */
const char *pong_desk_ai_level_name(int level);

/** How many difficulty levels there are, for cycling. */
int pong_desk_ai_level_count(void);

/** Whether this platform offers the item at all. */
bool pong_desk_item_shown(DeskItem it);

/** Rows actually drawn. */
int pong_desk_rows(void);

/** Screen row for an item, or -1 when the platform does not offer it. */
int pong_desk_row_of(DeskItem it);

/** The item at a screen row, or DESK_ITEM_COUNT when the row is past the end. */
DeskItem pong_desk_item_of_row(int row);

/**
 * The next shown item in `dir` (+1 down, -1 up), wrapping.
 *
 * Menu movement goes through here so that a hidden row cannot be landed on,
 * which is the failure mode of skipping it only in the drawing code.
 */
DeskItem pong_desk_step(DeskItem cur, int dir);

typedef struct {
    DeskScreen  screen;
    int         sel;
    /* Which difficulty the offline opponent plays at. Shown as the value on the
     * VS AI row and cycled with left/right, rather than occupying a row of its
     * own -- the menu is already nine rows and the Vita's screen is 544px. */
    int         ai_level;

    const char *my_name;
    const char *opp_name;
    const char *server_addr;
    const char *room_code;
    const char *message;
    const char *status_line;   /* transport */

    uint32_t    rtt_ms;
    uint32_t    snap_hz;
    uint32_t    fps;
    uint32_t    buf_ms;
    uint8_t     my_side;
    uint32_t    frame;
    bool        gamepad;       /* a pad is attached, so show pad hints */
    bool        editing;       /* a text field is open; dim the rest */
    const char *edit_label;
    const char *edit_text;
} DeskHud;

/** Hit-testing for the mouse, in output pixels. -1 for nothing. */
int pong_desk_item_at(float x, float y, int out_w, int out_h);

/** Maps a mouse Y in the window to a paddle target in field units. */
int32_t pong_desk_paddle_from_mouse(float y, int out_h);

void pong_desk_frame(const PongView *view, const DeskHud *hud);

#endif /* PONG_DESKTOP_H */
