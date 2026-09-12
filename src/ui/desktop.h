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
    DESK_ITEM_SERVER,
    DESK_ITEM_NAME,
    DESK_ITEM_QUIT,
    DESK_ITEM_COUNT
} DeskItem;

typedef struct {
    DeskScreen  screen;
    int         sel;

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
