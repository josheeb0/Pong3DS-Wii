/*
 * citro2d rendering for both screens.
 *
 * Top    (400x240): the playfield, at exactly half field coordinates. The
 *                   800x480 field was chosen for precisely this -- the mapping
 *                   is one integer shift with no rounding.
 * Bottom (320x240): status, transport, and the touch strip, following the
 *                   original sketch ("TAP THE TOUCH SCREEN TO CONTINUE").
 */

#ifndef PONG_RENDER_H
#define PONG_RENDER_H

#include <citro2d.h>
#include <stdbool.h>
#include "client.h"

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
    const char *status_line;   /* transport description */
    const char *detail_line;   /* rtt / hz / opponent */
    const char *message;       /* errors, prompts */
    const char *diag;          /* multi-line per-transport diagnostic */
    uint32_t    rtt_ms;
    uint32_t    hz;
    bool        touch_hint;    /* pulse the "tap to continue" box */
    bool        slow_mode;
    uint8_t     my_side;
    uint32_t    frame;         /* for animation */
} PongHud;

void pong_render_init(void);
void pong_render_exit(void);

/** Draws one frame to both screens. Call between C3D_FrameBegin/End. */
void pong_render_frame(C3D_RenderTarget *top, C3D_RenderTarget *bot,
                       const PongView *view, const PongHud *hud);

#endif /* PONG_RENDER_H */
