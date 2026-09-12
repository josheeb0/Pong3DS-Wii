#include "render.h"
#include "pong_proto.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

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
#define CLR_BTN       C2D_Color32(0x10, 0x19, 0x22, 0xFF)
#define CLR_BTN_SEL   C2D_Color32(0x15, 0x2a, 0x38, 0xFF)
#define CLR_EDGE      C2D_Color32(0x1c, 0x2f, 0x3e, 0xFF)
#define CLR_ACCENT    C2D_Color32(0x7e, 0xe7, 0xff, 0xFF)
#define CLR_ACCENT2   C2D_Color32(0xff, 0x9d, 0xe2, 0xFF)
#define CLR_GOOD      C2D_Color32(0x7d, 0xff, 0xa8, 0xFF)

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

/*
 * Same, but wrapped to a width.
 *
 * Needed because diagnostics carry things we do not control the length of --
 * hostnames, mbedTLS error strings -- and unwrapped they simply run off the
 * side of a 320px screen, which is exactly when you most need to read them.
 *
 * The wrap width is a trailing vararg and must come AFTER the colour, per
 * C2D_DrawText's contract.
 */
static void dyn_wrap(const char *s, u32 flags, float x, float y, float scale,
                     u32 color, float wrap_w)
{
    C2D_Text t;
    C2D_TextParse(&t, s_dynamic, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, flags | C2D_WithColor | C2D_WordWrap,
                 x, y, 0.5f, scale, scale, color, wrap_w);
}

/*
 * Touch regions for the title screen. Declared in the header as constants so
 * the drawing code and the hit-testing in main.c cannot drift apart -- a
 * button you can see but not press is a miserable bug to chase.
 */
/*
 * Bottom screen is 320x240. Three primary actions get full-width rows big
 * enough to hit with a thumb; the two utility actions share a smaller row at
 * the bottom, because they are things you do rarely and should not compete for
 * attention with "play".
 */
const PongRect PONG_MENU_RECT[MENU_COUNT] = {
    [MENU_QUICK]  = {  12.0f,  46.0f, 296.0f, 40.0f },
    [MENU_ROOM]   = {  12.0f,  92.0f, 296.0f, 40.0f },
    [MENU_BOT]    = {  12.0f, 138.0f, 296.0f, 40.0f },
    [MENU_SERVER] = {  12.0f, 186.0f, 180.0f, 30.0f },
    [MENU_UPDATE] = { 198.0f, 186.0f, 110.0f, 30.0f },
};

bool pong_ui_hit(const PongRect *r, float x, float y)
{
    return x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h;
}

int pong_ui_menu_hit(float x, float y)
{
    for (int i = 0; i < MENU_COUNT; i++) {
        if (pong_ui_hit(&PONG_MENU_RECT[i], x, y)) return i;
    }
    return -1;
}

/** A filled panel with a 1px edge; the shape everything in the UI is built from. */
static void panel(const PongRect *r, u32 fill, u32 edge)
{
    C2D_DrawRectSolid(r->x, r->y, 0.0f, r->w, r->h, fill);
    C2D_DrawRectSolid(r->x, r->y, 0.0f, r->w, 1.0f, edge);
    C2D_DrawRectSolid(r->x, r->y + r->h - 1.0f, 0.0f, r->w, 1.0f, edge);
    C2D_DrawRectSolid(r->x, r->y, 0.0f, 1.0f, r->h, edge);
    C2D_DrawRectSolid(r->x + r->w - 1.0f, r->y, 0.0f, 1.0f, r->h, edge);
}

/* ------------------------------------------------------------------ menu */

typedef struct { const char *label; const char *hint; } MenuLabel;

static const MenuLabel MENU_LABEL[MENU_COUNT] = {
    [MENU_QUICK]  = { "QUICK MATCH", "play whoever is waiting" },
    [MENU_ROOM]   = { "JOIN ROOM",   "same code = same game" },
    [MENU_BOT]    = { "VS CPU",      "practice offline-ish" },
    [MENU_SERVER] = { "SERVER",      NULL },
    [MENU_UPDATE] = { "UPDATE",      NULL },
};

/*
 * A little paddle-and-ball glyph, drawn with primitives.
 *
 * citro2d has no icon support and pulling in a spritesheet for five glyphs
 * would mean a t3x pipeline and a bigger romfs, for something three rectangles
 * express perfectly well.
 */
static void icon_pong(float x, float y, u32 a, u32 b)
{
    C2D_DrawRectSolid(x,          y,        0.0f, 2.0f, 12.0f, a);
    C2D_DrawRectSolid(x + 12.0f,  y + 3.0f, 0.0f, 2.0f, 12.0f, b);
    C2D_DrawRectSolid(x + 6.0f,   y + 7.0f, 0.0f, 3.0f, 3.0f,  CLR_BALL);
}

static void draw_menu(const PongHud *hud)
{
    /* Header. */
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 36.0f, CLR_PANEL);
    C2D_DrawRectSolid(0.0f, 35.0f, 0.0f, 320.0f, 1.0f, CLR_EDGE);
    dyn("PONG MULTIPLAYER!", 0, 12.0f, 6.0f, 0.52f, CLR_TEXT);

    char b[48];
    snprintf(b, sizeof b, "build %lu", (unsigned long)hud->build_id);
    dyn(b, C2D_AlignRight, 308.0f, 10.0f, 0.36f, CLR_DIM);

    for (int i = 0; i < MENU_COUNT; i++) {
        const PongRect *r = &PONG_MENU_RECT[i];
        bool sel = (hud->menu_sel == i);
        bool primary = (i <= MENU_BOT);

        panel(r, sel ? CLR_BTN_SEL : CLR_BTN, sel ? CLR_ACCENT : CLR_EDGE);

        /* An accent bar on the selected row, so the highlight reads instantly
         * rather than relying on a subtle fill difference. */
        if (sel) C2D_DrawRectSolid(r->x, r->y, 0.0f, 3.0f, r->h, CLR_ACCENT);

        if (primary) {
            icon_pong(r->x + 14.0f, r->y + (r->h - 15.0f) / 2.0f,
                      sel ? CLR_ACCENT : CLR_DIM, sel ? CLR_ACCENT2 : CLR_DIM);
            dyn(MENU_LABEL[i].label, 0, r->x + 38.0f, r->y + 6.0f, 0.5f,
                sel ? CLR_TEXT : CLR_DIM);
            if (MENU_LABEL[i].hint) {
                dyn(MENU_LABEL[i].hint, 0, r->x + 38.0f, r->y + 23.0f, 0.34f, CLR_DIM);
            }
        } else {
            dyn(MENU_LABEL[i].label, C2D_AlignCenter,
                r->x + r->w / 2.0f, r->y + 8.0f, 0.42f, sel ? CLR_TEXT : CLR_DIM);
        }
    }

    /* The server this will connect to -- the single most useful thing to see
     * before pressing anything. */
    if (hud->server_addr && hud->server_addr[0]) {
        dyn_wrap(hud->server_addr, 0, 20.0f, 195.0f, 0.34f, CLR_DIM, 164.0f);
    }

    if (hud->message && hud->message[0]) {
        dyn_wrap(hud->message, 0, 10.0f, 222.0f, 0.34f, CLR_WARN, 300.0f);
    } else {
        dyn("D-PAD + A, or tap", C2D_AlignCenter, 160.0f, 224.0f, 0.32f, CLR_DIM);
    }
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

/*
 * A dim demo rally behind the title.
 *
 * Purely decorative and purely local -- it is not the simulation and never
 * touches the network. Integer-free on purpose: nothing here has to agree with
 * the server, so readability wins over the fixed-point discipline the real
 * simulation needs.
 */
static void draw_attract(const PongHud *hud)
{
    float t = (float)hud->frame;
    float bx = 200.0f + 150.0f * sinf(t * 0.013f);
    float by = 120.0f + 80.0f  * sinf(t * 0.021f);

    /* Paddles lazily track the ball, always a little behind. */
    float ly = 120.0f + 70.0f * sinf(t * 0.021f - 0.6f);
    float ry = 120.0f + 70.0f * sinf(t * 0.021f - 1.1f);

    u32 dim  = C2D_Color32(0x12, 0x20, 0x2c, 0xFF);
    u32 dim2 = C2D_Color32(0x22, 0x18, 0x28, 0xFF);

    for (int y = 0; y < 240; y += 16) {
        C2D_DrawRectSolid(199.0f, (float)y, 0.0f, 2.0f, 9.0f,
                          C2D_Color32(0x11, 0x1c, 0x24, 0xFF));
    }
    C2D_DrawRectSolid(24.0f,  ly - 20.0f, 0.0f, 5.0f, 40.0f, dim);
    C2D_DrawRectSolid(371.0f, ry - 20.0f, 0.0f, 5.0f, 40.0f, dim2);
    C2D_DrawCircleSolid(bx, by, 0.0f, 4.0f, C2D_Color32(0x1e, 0x2c, 0x38, 0xFF));
}

static void draw_top(const PongView *view, const PongHud *hud)
{
    switch (hud->screen) {
    case SCREEN_TITLE:
        draw_attract(hud);
        C2D_DrawText(&s_title, C2D_AlignCenter | C2D_WithColor,
                     200.0f, 78.0f, 0.5f, 0.9f, 0.9f, CLR_TEXT);
        dyn("cross-play with any browser",
            C2D_AlignCenter, 200.0f, 112.0f, 0.42f, CLR_DIM);
        C2D_DrawText(&s_pressA, C2D_AlignRight | C2D_WithColor,
                     392.0f, 214.0f, 0.5f, 0.42f, 0.42f, CLR_DIM);
        break;

    case SCREEN_CONNECTING:
        C2D_DrawText(&s_title, C2D_AlignCenter | C2D_WithColor,
                     200.0f, 60.0f, 0.5f, 0.8f, 0.8f, CLR_TEXT);
        dyn(hud->message ? hud->message : "CONNECTING...",
            C2D_AlignCenter, 200.0f, 120.0f, 0.6f, CLR_DIM);
        break;

    case SCREEN_QUEUED:
        draw_attract(hud);
        if (hud->room_code && hud->room_code[0]) {
            /* Big, because the whole point is reading it to someone else. */
            dyn("ROOM CODE", C2D_AlignCenter, 200.0f, 52.0f, 0.5f, CLR_DIM);
            dyn(hud->room_code, C2D_AlignCenter, 200.0f, 78.0f, 1.6f, CLR_ACCENT);
            dyn("enter this on the other device",
                C2D_AlignCenter, 200.0f, 150.0f, 0.44f, CLR_TEXT);
        } else {
            dyn("WAITING FOR AN OPPONENT", C2D_AlignCenter, 200.0f, 100.0f, 0.62f, CLR_TEXT);
            dyn("a CPU opponent is offered after 15s",
                C2D_AlignCenter, 200.0f, 132.0f, 0.42f, CLR_DIM);
        }
        break;

    case SCREEN_ERROR:
        dyn("CONNECTION FAILED", C2D_AlignCenter, 200.0f, 30.0f, 0.8f, CLR_WARN);
        /* Every path's result, so the failure is diagnosable from the screen
         * rather than by guessing. A fallback's error alone is not enough. */
        if (hud->diag && hud->diag[0]) {
            /* Top screen is 400 wide; leave a 12px margin each side. */
            dyn_wrap(hud->diag, 0, 12.0f, 62.0f, 0.42f, CLR_DIM, 376.0f);
        } else if (hud->message) {
            dyn_wrap(hud->message, 0, 12.0f, 100.0f, 0.42f, CLR_DIM, 376.0f);
        }
        dyn("also written to sd:/3ds/pong3ds.log",
            C2D_AlignCenter, 200.0f, 196.0f, 0.38f, CLR_DIM);
        dyn("A / TAP = back", C2D_AlignCenter, 200.0f, 216.0f, 0.42f, CLR_DIM);
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
        dyn_wrap(hud->status_line, 0, 8.0f, 6.0f, 0.42f, CLR_MINE, 250.0f);
    }
    if (hud->detail_line && hud->detail_line[0]) {
        dyn_wrap(hud->detail_line, 0, 8.0f, 24.0f, 0.40f, CLR_DIM, 304.0f);
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
    } else if (hud->screen == SCREEN_TITLE) {
        draw_menu(hud);
    } else {
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
