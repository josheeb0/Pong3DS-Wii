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
#define CLR_BTN       C2D_Color32(0x0e, 0x16, 0x1e, 0xFF)
#define CLR_BTN_SEL   C2D_Color32(0x16, 0x2c, 0x3a, 0xFF)
#define CLR_EDGE      C2D_Color32(0x1a, 0x2b, 0x38, 0xFF)
#define CLR_ACCENT    C2D_Color32(0x7e, 0xe7, 0xff, 0xFF)
#define CLR_ACCENT2   C2D_Color32(0xff, 0x9d, 0xe2, 0xFF)
#define CLR_GOOD      C2D_Color32(0x7d, 0xff, 0xa8, 0xFF)
#define CLR_HEADER    C2D_Color32(0x0a, 0x12, 0x19, 0xFF)
#define CLR_TRAIL     C2D_Color32(0x16, 0x24, 0x30, 0xFF)

/* Blends two colours. Used for the selection animation, so the highlight
 * travels rather than snapping between rows -- motion is most of what makes an
 * interface feel considered rather than assembled. */
static u32 mix(u32 a, u32 b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    u8 out[4];
    for (int i = 0; i < 4; i++) {
        u8 ca = (u8)((a >> (i * 8)) & 0xFF);
        u8 cb = (u8)((b >> (i * 8)) & 0xFF);
        out[i] = (u8)(ca + (cb - ca) * t);
    }
    return (u32)out[0] | ((u32)out[1] << 8) | ((u32)out[2] << 16) | ((u32)out[3] << 24);
}

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
    [MENU_SERVER] = {  12.0f, 184.0f,  96.0f, 28.0f },
    [MENU_SOURCE] = { 114.0f, 184.0f, 100.0f, 28.0f },
    [MENU_UPDATE] = { 220.0f, 184.0f,  88.0f, 28.0f },
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
    [MENU_SOURCE] = { "SOURCE",      NULL },
    [MENU_UPDATE] = { "UPDATE",      NULL },
};

/* Per-item glyphs, so the rows are distinguishable at a glance rather than
 * being five identical boxes with different words in them. */
static void icon_for(int item, float x, float y, u32 a, u32 b)
{
    switch (item) {
    case MENU_QUICK:   /* two paddles and a ball: a normal match */
        C2D_DrawRectSolid(x,         y,        0.0f, 2.0f, 12.0f, a);
        C2D_DrawRectSolid(x + 12.0f, y + 3.0f, 0.0f, 2.0f, 12.0f, b);
        C2D_DrawCircleSolid(x + 7.0f, y + 7.0f, 0.0f, 2.0f, CLR_BALL);
        break;
    case MENU_ROOM: {  /* a keypad: something you type */
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                C2D_DrawRectSolid(x + c * 5.0f, y + r * 5.0f, 0.0f, 3.0f, 3.0f,
                                  (r + c) % 2 ? b : a);
        break;
    }
    case MENU_BOT:     /* a blocky head: the CPU */
        C2D_DrawRectSolid(x + 1.0f, y + 1.0f, 0.0f, 12.0f, 11.0f, a);
        C2D_DrawRectSolid(x + 4.0f, y + 4.0f, 0.0f, 2.0f, 2.0f, CLR_BG);
        C2D_DrawRectSolid(x + 9.0f, y + 4.0f, 0.0f, 2.0f, 2.0f, CLR_BG);
        C2D_DrawRectSolid(x + 4.0f, y + 8.0f, 0.0f, 7.0f, 1.0f, CLR_BG);
        break;
    default:
        C2D_DrawRectSolid(x + 2.0f, y + 2.0f, 0.0f, 10.0f, 10.0f, a);
        break;
    }
}

static void draw_menu(const PongHud *hud)
{
    /* The highlight eases toward the selection instead of jumping. Held in a
     * static because it is presentation state with no meaning to the rest of
     * the program -- putting it in the app struct would imply otherwise. */
    static float sel_y = 0.0f;
    static bool  sel_init = false;
    const PongRect *target = &PONG_MENU_RECT[hud->menu_sel];
    if (!sel_init) { sel_y = target->y; sel_init = true; }
    sel_y += (target->y - sel_y) * 0.35f;

    /* Header. */
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 32.0f, CLR_HEADER);
    C2D_DrawRectSolid(0.0f, 31.0f, 0.0f, 320.0f, 1.0f, CLR_ACCENT);
    dyn("PONG", 0, 12.0f, 5.0f, 0.62f, CLR_ACCENT);
    dyn("MULTIPLAYER", 0, 62.0f, 9.0f, 0.44f, CLR_TEXT);

    char b[48];
    if (hud->build_id == 0) {
        /* A local build is not a release and should never be mistaken for one. */
        snprintf(b, sizeof b, "DEV BUILD");
        dyn(b, C2D_AlignRight, 310.0f, 10.0f, 0.38f, CLR_WARN);
    } else {
        snprintf(b, sizeof b, "BUILD %lu", (unsigned long)hud->build_id);
        dyn(b, C2D_AlignRight, 310.0f, 10.0f, 0.38f, CLR_ACCENT);
    }

    for (int i = 0; i < MENU_COUNT; i++) {
        const PongRect *r = &PONG_MENU_RECT[i];
        bool sel = (hud->menu_sel == i);
        bool primary = (i <= MENU_BOT);

        panel(r, sel ? CLR_BTN_SEL : CLR_BTN, sel ? CLR_ACCENT : CLR_EDGE);

        if (primary) {
            icon_for(i, r->x + 14.0f, r->y + (r->h - 14.0f) / 2.0f,
                     sel ? CLR_ACCENT : CLR_DIM, sel ? CLR_ACCENT2 : CLR_DIM);
            dyn(MENU_LABEL[i].label, 0, r->x + 40.0f, r->y + 5.0f, 0.5f,
                sel ? CLR_TEXT : CLR_DIM);
            if (MENU_LABEL[i].hint) {
                dyn(MENU_LABEL[i].hint, 0, r->x + 40.0f, r->y + 22.0f, 0.33f,
                    sel ? CLR_DIM : mix(CLR_DIM, CLR_BG, 0.45f));
            }
            /* A chevron on the active row, pointing at the thing A will do. */
            if (sel) {
                float cx = r->x + r->w - 18.0f, cy = r->y + r->h / 2.0f;
                for (int k = 0; k < 5; k++) {
                    C2D_DrawRectSolid(cx + k, cy - 5.0f + k, 0.0f, 1.5f, 2.0f, CLR_ACCENT);
                    C2D_DrawRectSolid(cx + k, cy + 4.0f - k, 0.0f, 1.5f, 2.0f, CLR_ACCENT);
                }
            }
        } else {
            dyn(MENU_LABEL[i].label, C2D_AlignCenter,
                r->x + r->w / 2.0f, r->y + 7.0f, 0.42f, sel ? CLR_TEXT : CLR_DIM);
        }
    }

    /* The travelling highlight: a bar down the left edge of the active row. */
    C2D_DrawRectSolid(12.0f, sel_y, 0.0f, 3.0f,
                      PONG_MENU_RECT[hud->menu_sel].h, CLR_ACCENT);

    if (hud->server_addr && hud->server_addr[0]) {
        dyn_wrap(hud->server_addr, 0, 14.0f, 214.0f, 0.3f, CLR_DIM, 190.0f);
    }
    if (hud->update_src && hud->update_src[0]) {
        char us[72];
        snprintf(us, sizeof us, "updates: %s", hud->update_src);
        dyn(us, C2D_AlignRight, 308.0f, 214.0f, 0.3f, CLR_DIM);
    }

    if (hud->message && hud->message[0]) {
        dyn_wrap(hud->message, 0, 10.0f, 226.0f, 0.3f, CLR_WARN, 300.0f);
    } else {
        char v[64];
        if (hud->build_id == 0) {
            snprintf(v, sizeof v, "dev build  ·  protocol v%d  ·  D-PAD + A or tap",
                     PONG_PROTOCOL_VERSION);
        } else {
            snprintf(v, sizeof v, "build %lu  ·  protocol v%d  ·  D-PAD + A or tap",
                     (unsigned long)hud->build_id, PONG_PROTOCOL_VERSION);
        }
        dyn(v, C2D_AlignCenter, 160.0f, 228.0f, 0.29f, mix(CLR_DIM, CLR_BG, 0.25f));
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
        C2D_DrawRectSolid(199.0f, (float)y, 0.0f, 2.0f, 9.0f, CLR_TRAIL);
    }
    C2D_DrawRectSolid(24.0f,  ly - 20.0f, 0.0f, 5.0f, 40.0f, dim);
    C2D_DrawRectSolid(371.0f, ry - 20.0f, 0.0f, 5.0f, 40.0f, dim2);

    /* A short trail behind the ball. Rectangles rather than circles: circles
     * force a citro2d state change each time, and at four of them per frame
     * behind a menu that is not worth paying for. */
    for (int k = 6; k >= 1; k--) {
        float tt = t - (float)k * 2.0f;
        float tx = 200.0f + 150.0f * sinf(tt * 0.013f);
        float ty = 120.0f + 80.0f  * sinf(tt * 0.021f);
        C2D_DrawRectSolid(tx - 1.5f, ty - 1.5f, 0.0f, 3.0f, 3.0f,
                          mix(CLR_TRAIL, CLR_BG, (float)k / 7.0f));
    }
    C2D_DrawCircleSolid(bx, by, 0.0f, 4.0f, C2D_Color32(0x24, 0x36, 0x46, 0xFF));
}

static void draw_top(const PongView *view, const PongHud *hud)
{
    switch (hud->screen) {
    case SCREEN_TITLE: {
        draw_attract(hud);

        /* Letterbox bands: they frame the title and, conveniently, stop the
         * demo rally competing with the text for attention. */
        C2D_DrawRectSolid(0.0f, 60.0f, 0.0f, 400.0f, 74.0f,
                          C2D_Color32(0x06, 0x0a, 0x0e, 0xE8));
        C2D_DrawRectSolid(0.0f, 60.0f, 0.0f, 400.0f, 1.0f, CLR_EDGE);
        C2D_DrawRectSolid(0.0f, 133.0f, 0.0f, 400.0f, 1.0f, CLR_EDGE);

        dyn("PONG", C2D_AlignCenter, 200.0f, 66.0f, 1.5f, CLR_ACCENT);
        dyn("M U L T I P L A Y E R", C2D_AlignCenter, 200.0f, 108.0f, 0.5f, CLR_TEXT);
        dyn("cross-play with any browser",
            C2D_AlignCenter, 200.0f, 146.0f, 0.4f, CLR_DIM);
        {
            char vb[64];
            if (hud->build_id == 0) snprintf(vb, sizeof vb, "DEV BUILD");
            else snprintf(vb, sizeof vb, "BUILD %lu", (unsigned long)hud->build_id);
            dyn(vb, 0, 10.0f, 214.0f, 0.4f,
                hud->build_id == 0 ? CLR_WARN : CLR_DIM);
        }

        C2D_DrawText(&s_pressA, C2D_AlignRight | C2D_WithColor,
                     392.0f, 214.0f, 0.5f, 0.4f, 0.4f, CLR_DIM);
        break;
    }

    case SCREEN_CONNECTING:
        C2D_DrawText(&s_title, C2D_AlignCenter | C2D_WithColor,
                     200.0f, 60.0f, 0.5f, 0.8f, 0.8f, CLR_TEXT);
        dyn(hud->message ? hud->message : "CONNECTING...",
            C2D_AlignCenter, 200.0f, 120.0f, 0.6f, CLR_DIM);
        break;

    case SCREEN_QUEUED:
        draw_attract(hud);
        if (hud->room_code && hud->room_code[0]) {
            dyn("ROOM CODE", C2D_AlignCenter, 200.0f, 48.0f, 0.46f, CLR_DIM);

            /* One boxed character each, so it reads as a code to be
             * transcribed rather than a word, and ambiguous glyphs are easier
             * to pick apart when read aloud. */
            int n = (int)strlen(hud->room_code);
            if (n > 8) n = 8;
            float bw = 34.0f, gap = 6.0f;
            float total = n * bw + (n - 1) * gap;
            float x0 = 200.0f - total / 2.0f;
            for (int i = 0; i < n; i++) {
                PongRect cell = { x0 + i * (bw + gap), 74.0f, bw, 46.0f };
                panel(&cell, CLR_BTN, CLR_ACCENT);
                char ch[2] = { hud->room_code[i], 0 };
                dyn(ch, C2D_AlignCenter, cell.x + bw / 2.0f, cell.y + 7.0f,
                    1.0f, CLR_ACCENT);
            }

            dyn("enter this code on the other device",
                C2D_AlignCenter, 200.0f, 134.0f, 0.42f, CLR_TEXT);

            /* Three dots cycling: says "still waiting" without a spinner that
             * would need its own animation state. */
            int phase = (hud->frame / 20) % 4;
            for (int i = 0; i < 3; i++) {
                C2D_DrawCircleSolid(190.0f + i * 10.0f, 166.0f, 0.0f, 3.0f,
                                    i < phase ? CLR_ACCENT : CLR_EDGE);
            }
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
