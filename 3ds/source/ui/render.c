#include "render.h"
#include "pong_proto.h"
#include <stdio.h>
#include <string.h>

/* Q4 field units -> top-screen pixels: /16 for fixed point, /2 for the
 * 800x480 -> 400x240 mapping. One shift, exact at both extremes. */
#define TOPX(q) ((float)((q) >> (PONG_Q4_SHIFT + 1)))

#define CLR_BG        C2D_Color32(0x07, 0x09, 0x0d, 0xFF)
#define CLR_NET       C2D_Color32(0x1e, 0x3a, 0x4d, 0xFF)
#define CLR_MINE      C2D_Color32(0x7e, 0xe7, 0xff, 0xFF)
#define CLR_THEIRS    C2D_Color32(0xff, 0x9d, 0xe2, 0xFF)
#define CLR_BALL      C2D_Color32(0xff, 0xff, 0xff, 0xFF)
#define CLR_TEXT      C2D_Color32(0xdc, 0xe9, 0xf5, 0xFF)
#define CLR_DIM       C2D_Color32(0x6a, 0x86, 0x9b, 0xFF)
#define CLR_WARN      C2D_Color32(0xff, 0xc8, 0x78, 0xFF)
#define CLR_SCORE     C2D_Color32(0x2a, 0x44, 0x5a, 0xFF)
#define CLR_PANEL     C2D_Color32(0x0d, 0x11, 0x17, 0xFF)

/* Two buffers: one for strings that never change, one cleared every frame.
 * Forgetting to clear the dynamic buffer leaks glyphs until it fills and
 * C2D_TextParse silently starts failing. */
static C2D_TextBuf s_static;
static C2D_TextBuf s_dynamic;

static C2D_Text s_title, s_tapToStart, s_pressA;

static void mkstatic(C2D_Text *t, const char *s)
{
    C2D_TextParse(t, s_static, s);
    C2D_TextOptimize(t);
}

void pong_render_init(void)
{
    s_static = C2D_TextBufNew(512);
    s_dynamic = C2D_TextBufNew(4096);
    mkstatic(&s_title, "PONG MULTIPLAYER!");
    mkstatic(&s_tapToStart, "TAP THE TOUCH SCREEN\nTO CONTINUE");
    mkstatic(&s_pressA, "you are currently on 3ds");
}

void pong_render_exit(void)
{
    C2D_TextBufDelete(s_dynamic);
    C2D_TextBufDelete(s_static);
}

/** Parses and draws a throwaway string from the per-frame buffer. */
static void dyn(const char *s, u32 flags, float x, float y, float scale, u32 color)
{
    C2D_Text t;
    C2D_TextParse(&t, s_dynamic, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, flags | C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

/* ------------------------------------------------------------------- top */

static void draw_playfield(const PongView *view, const PongHud *hud)
{
    /* Centre net. */
    for (int y = 0; y < 240; y += 16) {
        C2D_DrawRectSolid(199.0f, (float)y, 0.0f, 2.0f, 9.0f, CLR_NET);
    }

    if (!view || !view->valid) {
        dyn("CONNECTING...", C2D_AlignCenter, 200.0f, 108.0f, 0.7f, CLR_TEXT);
        return;
    }

    /* Score, behind the play. */
    char sc[16];
    snprintf(sc, sizeof sc, "%u", view->score_l);
    dyn(sc, C2D_AlignCenter, 150.0f, 8.0f, 1.1f, CLR_SCORE);
    snprintf(sc, sizeof sc, "%u", view->score_r);
    dyn(sc, C2D_AlignCenter, 250.0f, 8.0f, 1.1f, CLR_SCORE);

    const float pw = (float)PONG_PADDLE_W / 2.0f;   /* field px -> screen px */
    const float ph = (float)PONG_PADDLE_H / 2.0f;

    float lx = (float)PONG_PADDLE_X_L / 2.0f - pw / 2.0f;
    float rx = (float)PONG_PADDLE_X_R / 2.0f - pw / 2.0f;
    float ly = TOPX(view->left_y) - ph / 2.0f;
    float ry = TOPX(view->right_y) - ph / 2.0f;

    C2D_DrawRectSolid(lx, ly, 0.0f, pw, ph, hud->my_side == 0 ? CLR_MINE : CLR_THEIRS);
    C2D_DrawRectSolid(rx, ry, 0.0f, pw, ph, hud->my_side == 1 ? CLR_MINE : CLR_THEIRS);

    /* Phase banners. */
    if (view->state == PONG_MATCH_STATE_COUNTDOWN) {
        dyn("GET READY", C2D_AlignCenter, 200.0f, 100.0f, 0.9f, CLR_TEXT);
    } else if (view->state == PONG_MATCH_STATE_GAME_OVER) {
        bool won = (hud->my_side == 0) ? (view->score_l > view->score_r)
                                       : (view->score_r > view->score_l);
        dyn(won ? "YOU WIN!" : "YOU LOSE", C2D_AlignCenter, 200.0f, 100.0f, 0.9f,
            won ? CLR_MINE : CLR_THEIRS);
    } else if (view->state == PONG_MATCH_STATE_OPP_LOST) {
        dyn("OPPONENT LEFT", C2D_AlignCenter, 200.0f, 100.0f, 0.7f, CLR_WARN);
    }

    /* Ball LAST: circles force a citro2d state change, so batching every other
     * shape before them is measurably cheaper. */
    if (view->state == PONG_MATCH_STATE_PLAY || view->state == PONG_MATCH_STATE_GOAL_FREEZE) {
        C2D_DrawCircleSolid(TOPX(view->ball_x), TOPX(view->ball_y), 0.0f,
                            (float)PONG_BALL_R / 2.0f, CLR_BALL);
    }

    if (view->starved) {
        C2D_DrawCircleSolid(390.0f, 10.0f, 0.0f, 3.0f, CLR_WARN);
    }
}

static void draw_top(const PongView *view, const PongHud *hud)
{
    switch (hud->screen) {
    case SCREEN_TITLE:
        C2D_DrawText(&s_title, C2D_AlignCenter | C2D_WithColor,
                     200.0f, 70.0f, 0.5f, 0.9f, 0.9f, CLR_TEXT);
        C2D_DrawText(&s_pressA, C2D_AlignRight | C2D_WithColor,
                     392.0f, 214.0f, 0.5f, 0.42f, 0.42f, CLR_DIM);
        dyn("X = check for updates", C2D_AlignCenter, 200.0f, 150.0f, 0.42f, CLR_DIM);
        break;

    case SCREEN_CONNECTING:
        C2D_DrawText(&s_title, C2D_AlignCenter | C2D_WithColor,
                     200.0f, 60.0f, 0.5f, 0.8f, 0.8f, CLR_TEXT);
        dyn(hud->message ? hud->message : "CONNECTING...",
            C2D_AlignCenter, 200.0f, 120.0f, 0.6f, CLR_DIM);
        break;

    case SCREEN_QUEUED:
        C2D_DrawText(&s_title, C2D_AlignCenter | C2D_WithColor,
                     200.0f, 60.0f, 0.5f, 0.8f, 0.8f, CLR_TEXT);
        dyn("WAITING FOR AN OPPONENT", C2D_AlignCenter, 200.0f, 120.0f, 0.6f, CLR_TEXT);
        dyn("a CPU opponent is offered after 15s",
            C2D_AlignCenter, 200.0f, 145.0f, 0.42f, CLR_DIM);
        break;

    case SCREEN_ERROR:
        dyn("CONNECTION FAILED", C2D_AlignCenter, 200.0f, 90.0f, 0.8f, CLR_WARN);
        dyn(hud->message ? hud->message : "", C2D_AlignCenter, 200.0f, 125.0f, 0.45f, CLR_DIM);
        break;

    case SCREEN_PLAY:
    case SCREEN_GAMEOVER:
    default:
        draw_playfield(view, hud);
        break;
    }
}

/* ---------------------------------------------------------------- bottom */

static void draw_bottom(const PongView *view, const PongHud *hud)
{
    (void)view;

    /* Status panel. */
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 46.0f, CLR_PANEL);

    if (hud->status_line && hud->status_line[0]) {
        dyn(hud->status_line, 0, 8.0f, 6.0f, 0.44f, CLR_MINE);
    }
    if (hud->detail_line && hud->detail_line[0]) {
        dyn(hud->detail_line, 0, 8.0f, 24.0f, 0.42f, CLR_DIM);
    }
    if (hud->slow_mode) {
        dyn("SLOW", C2D_AlignRight, 312.0f, 6.0f, 0.42f, CLR_WARN);
    }

    if (hud->screen == SCREEN_PLAY) {
        /* Touch strip. Drawing the control surface explicitly makes it obvious
         * the bottom screen is the paddle, which the sketch implies. */
        C2D_DrawRectSolid(6.0f, 54.0f, 0.0f, 308.0f, 180.0f,
                          C2D_Color32(0x0f, 0x16, 0x1e, 0xFF));
        for (int i = 0; i < 5; i++) {
            float y = 54.0f + 36.0f * (float)i;
            C2D_DrawRectSolid(6.0f, y, 0.0f, 308.0f, 1.0f, C2D_Color32(0x16, 0x24, 0x30, 0xFF));
        }
        dyn("SLIDE TO MOVE  ·  D-PAD / CIRCLE PAD ALSO WORK",
            C2D_AlignCenter, 160.0f, 60.0f, 0.38f, CLR_DIM);
        dyn("START = quit", C2D_AlignCenter, 160.0f, 216.0f, 0.38f, CLR_DIM);
    } else {
        /* "TAP THE TOUCH SCREEN TO CONTINUE" -- straight from the sketch. */
        bool on = ((hud->frame / 30) % 2) == 0;
        u32 border = on ? CLR_MINE : CLR_NET;
        float x = 24.0f, y = 70.0f, w = 272.0f, h = 100.0f;
        C2D_DrawRectSolid(x, y, 0.0f, w, 2.0f, border);
        C2D_DrawRectSolid(x, y + h - 2.0f, 0.0f, w, 2.0f, border);
        C2D_DrawRectSolid(x, y, 0.0f, 2.0f, h, border);
        C2D_DrawRectSolid(x + w - 2.0f, y, 0.0f, 2.0f, h, border);

        C2D_DrawText(&s_tapToStart, C2D_AlignCenter | C2D_WithColor,
                     160.0f, 96.0f, 0.5f, 0.55f, 0.55f, CLR_TEXT);

        if (hud->message && hud->message[0] && hud->screen != SCREEN_ERROR) {
            dyn(hud->message, C2D_AlignCenter, 160.0f, 190.0f, 0.4f, CLR_DIM);
        }
    }
}

void pong_render_frame(C3D_RenderTarget *top, C3D_RenderTarget *bot,
                       const PongView *view, const PongHud *hud)
{
    /* Both screens cleared and drawn inside ONE frame. C2D_SceneBegin handles
     * the per-screen projection switch. */
    C2D_TextBufClear(s_dynamic);

    C2D_TargetClear(top, CLR_BG);
    C2D_SceneBegin(top);
    draw_top(view, hud);

    C2D_TargetClear(bot, CLR_BG);
    C2D_SceneBegin(bot);
    draw_bottom(view, hud);
}
