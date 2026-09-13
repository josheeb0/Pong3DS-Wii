/*
 * The desktop interface. See desktop.h for why it is separate.
 *
 * LAYOUT. One canvas, laid out for the window it is given. The playfield keeps
 * the simulation's 800x480 aspect and is letterboxed into the window rather
 * than stretched -- the server simulates in those proportions, and a stretched
 * field would make the ball's angles read wrong even though the physics were
 * right. Everything else is placed relative to that box.
 */

#include "desktop.h"
#include "pong_proto.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CLR_BG        pong_rgba(0x07, 0x09, 0x0d, 0xFF)
#define CLR_SURROUND  pong_rgba(0x04, 0x05, 0x08, 0xFF)
#define CLR_FIELD_HI  pong_rgba(0x0e, 0x17, 0x22, 0xFF)
#define CLR_FIELD_LO  pong_rgba(0x06, 0x09, 0x0e, 0xFF)
#define CLR_NET       pong_rgba(0x1e, 0x3a, 0x4d, 0xFF)
#define CLR_MINE      pong_rgba(0x7e, 0xe7, 0xff, 0xFF)
#define CLR_THEIRS    pong_rgba(0xff, 0x9d, 0xe2, 0xFF)
#define CLR_BALL      pong_rgba(0xff, 0xff, 0xff, 0xFF)
#define CLR_TEXT      pong_rgba(0xdc, 0xe9, 0xf5, 0xFF)
#define CLR_DIM       pong_rgba(0x95, 0xad, 0xc0, 0xFF)
#define CLR_FAINT     pong_rgba(0x5c, 0x74, 0x87, 0xFF)
#define CLR_WARN      pong_rgba(0xff, 0xc8, 0x78, 0xFF)
#define CLR_ACCENT    pong_rgba(0x7e, 0xe7, 0xff, 0xFF)
#define CLR_EDGE      pong_rgba(0x1a, 0x2b, 0x38, 0xFF)
#define CLR_PANEL     pong_rgba(0x0d, 0x13, 0x1b, 0xEE)

static PongColor mix(PongColor a, PongColor b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint8_t o[4];
    for (int i = 0; i < 4; i++) {
        uint8_t ca = (uint8_t)((a >> (i * 8)) & 0xFF);
        uint8_t cb = (uint8_t)((b >> (i * 8)) & 0xFF);
        o[i] = (uint8_t)(ca + (int)((cb - ca) * t));
    }
    return (PongColor)o[0] | ((PongColor)o[1] << 8) |
           ((PongColor)o[2] << 16) | ((PongColor)o[3] << 24);
}

/* ---------------------------------------------------------------- layout */

typedef struct { float x, y, w, h, scale; } FieldBox;

/*
 * The playfield's place in the window.
 *
 * Aspect preserved, 6% margin, pushed down a little to leave room for the
 * scoreboard above it. Recomputed every frame so resizing works without any
 * resize handling.
 */
static FieldBox field_box(int out_w, int out_h)
{
    const float aspect = (float)PONG_FIELD_W / (float)PONG_FIELD_H;
    float avail_w = (float)out_w * 0.94f;
    float avail_h = (float)out_h * 0.78f;

    float w = avail_w, h = w / aspect;
    if (h > avail_h) { h = avail_h; w = h * aspect; }

    FieldBox b;
    b.w = w;
    b.h = h;
    b.x = ((float)out_w - w) * 0.5f;
    b.y = (float)out_h * 0.16f + (avail_h - h) * 0.5f;
    b.scale = w / (float)PONG_FIELD_W;
    return b;
}

/** Field Q4 -> window pixels. */
static inline float fx(const FieldBox *b, int32_t q4)
{
    return b->x + ((float)q4 / 16.0f) * b->scale;
}
static inline float fy(const FieldBox *b, int32_t q4)
{
    return b->y + ((float)q4 / 16.0f) * b->scale;
}
static inline float fs(const FieldBox *b, float units) { return units * b->scale; }

/* ------------------------------------------------------------------ menu */

typedef struct { const char *label, *hint; } DeskLabel;

static const DeskLabel ITEM[DESK_ITEM_COUNT] = {
    [DESK_ITEM_QUICK]  = { "QUICK MATCH", "play whoever is waiting" },
    [DESK_ITEM_ROOM]   = { "JOIN ROOM",   "same code = same game" },
    [DESK_ITEM_BOT]    = { "VS CPU",      "practice against the server bot" },
    [DESK_ITEM_SERVER] = { "SERVER",      "where to connect" },
    [DESK_ITEM_NAME]   = { "NAME",        "what the other player sees" },
    [DESK_ITEM_QUIT]   = { "QUIT",        NULL },
};

/* Menu geometry, shared by drawing and hit-testing so a row that is drawn is
 * always a row that can be clicked. */
/*
 * How big the title block is, which the menu has to start below.
 *
 * Scaled down on a short screen. The first version used fixed sizes suited to
 * a desktop window, and on the Vita's 544px display the menu was clamped
 * upward until it sat on top of the tagline -- the layout had no idea the
 * header existed.
 */
static float title_scale(int out_h) { return (out_h < 620) ? 2.1f : 3.0f; }

static float title_block_h(int out_h)
{
    /* Title, the spaced-out MULTIPLAYER, and the tagline, plus a gap. */
    float t = title_scale(out_h);
    return (float)out_h * 0.06f + t * 24.0f + 30.0f + 22.0f + 24.0f;
}

/** Row height, shrunk to fit rather than overflowing. */
static float menu_row_h(int out_h)
{
    const float gap = 8.0f;
    float avail = (float)out_h - title_block_h(out_h) - 72.0f;  /* 72: hint line */
    float rh = (avail - (DESK_ITEM_COUNT - 1) * gap) / (float)DESK_ITEM_COUNT;
    if (rh > 52.0f) rh = 52.0f;
    if (rh < 34.0f) rh = 34.0f;      /* below this the two lines stop fitting */
    return rh;
}

static void menu_row(int out_w, int out_h, int i, float *x, float *y, float *w, float *h)
{
    float rw = (float)out_w * 0.46f;
    if (rw < 320.0f) rw = 320.0f;
    if (rw > 560.0f) rw = 560.0f;

    const float gap = 8.0f;
    const float rh = menu_row_h(out_h);
    const float total = DESK_ITEM_COUNT * rh + (DESK_ITEM_COUNT - 1) * gap;

    /*
     * Measured DOWN from the title rather than up from the bottom.
     *
     * The block has one origin, not six -- clamping each row separately was an
     * earlier bug that opened a gap in the middle of the menu. Starting below
     * the header is what stops the other failure, where a short screen pushed
     * the whole block up into the title.
     */
    float start = title_block_h(out_h);
    float slack = ((float)out_h - 72.0f) - (start + total);
    if (slack > 0.0f) start += slack * 0.5f;      /* centre in what is left */

    *x = ((float)out_w - rw) * 0.5f;
    *y = start + (float)i * (rh + gap);
    *w = rw;
    *h = rh;
}

int pong_desk_item_at(float mx, float my, int out_w, int out_h)
{
    for (int i = 0; i < DESK_ITEM_COUNT; i++) {
        float x, y, w, h;
        menu_row(out_w, out_h, i, &x, &y, &w, &h);
        if (mx >= x && mx <= x + w && my >= y && my <= y + h) return i;
    }
    return -1;
}

int32_t pong_desk_paddle_from_mouse(float my, int out_h)
{
    FieldBox b = field_box(out_h * 16 / 9, out_h);   /* only the vertical matters */
    float t = (my - b.y) / (b.h > 0.0f ? b.h : 1.0f);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (int32_t)(t * (float)PONG_FIELD_H_Q4);
}

/* ------------------------------------------------------------ playfield */

#define TRAIL_LEN 18
static float s_tx[TRAIL_LEN], s_ty[TRAIL_LEN];
static int   s_thead = 0;
static bool  s_tlive = false;

static void trail_reset(float x, float y)
{
    for (int i = 0; i < TRAIL_LEN; i++) { s_tx[i] = x; s_ty[i] = y; }
    s_thead = 0;
    s_tlive = true;
}

static void trail_push(float x, float y)
{
    if (!s_tlive) { trail_reset(x, y); return; }
    int prev = (s_thead + TRAIL_LEN - 1) % TRAIL_LEN;
    float dx = x - s_tx[prev], dy = y - s_ty[prev];
    /* A serve teleports the ball; smearing across that reads as a glitch. */
    if (dx * dx + dy * dy > 200.0f * 200.0f) { trail_reset(x, y); return; }
    s_tx[s_thead] = x;
    s_ty[s_thead] = y;
    s_thead = (s_thead + 1) % TRAIL_LEN;
}

static void draw_paddle(float x, float y, float w, float h, PongColor body)
{
    PongColor glow = (body & 0x00FFFFFFu) | 0x30000000u;
    pong_gfx_rect(x - 5.0f, y - 5.0f, w + 10.0f, h + 10.0f, glow);
    pong_gfx_rect(x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, glow);
    pong_gfx_rect_grad(x, y, w, h * 0.5f,
                       mix(body, CLR_BG, 0.35f), mix(body, CLR_BG, 0.35f), body, body);
    pong_gfx_rect_grad(x, y + h * 0.5f, w, h * 0.5f,
                       body, body, mix(body, CLR_BG, 0.35f), mix(body, CLR_BG, 0.35f));
}

static void draw_field(const PongView *view, const DeskHud *hud, int out_w, int out_h)
{
    FieldBox b = field_box(out_w, out_h);

    pong_gfx_rect_grad(b.x, b.y, b.w, b.h,
                       CLR_FIELD_HI, CLR_FIELD_HI, CLR_FIELD_LO, CLR_FIELD_LO);

    /* Goal lines in each player's colour, so which end is yours is obvious. */
    bool mine_left = (hud->my_side == 0);
    pong_gfx_rect(b.x, b.y, 2.0f, b.h, mix(mine_left ? CLR_MINE : CLR_THEIRS, CLR_BG, 0.4f));
    pong_gfx_rect(b.x + b.w - 2.0f, b.y, 2.0f, b.h,
                  mix(mine_left ? CLR_THEIRS : CLR_MINE, CLR_BG, 0.4f));
    /* A frame, so the field reads as a table rather than a gradient. */
    pong_gfx_rect(b.x, b.y, b.w, 2.0f, CLR_EDGE);
    pong_gfx_rect(b.x, b.y + b.h - 2.0f, b.w, 2.0f, CLR_EDGE);

    float dash = b.h / 17.0f;
    for (float y = b.y + dash * 0.25f; y < b.y + b.h - dash * 0.3f; y += dash) {
        float d = fabsf((y - (b.y + b.h * 0.5f)) / (b.h * 0.5f));
        pong_gfx_rect(b.x + b.w * 0.5f - 1.5f, y, 3.0f, dash * 0.5f,
                      mix(CLR_NET, CLR_FIELD_LO, d * 0.7f));
    }

    if (!view || !view->valid) return;

    float pw = fs(&b, (float)PONG_PADDLE_W);
    float ph = fs(&b, (float)PONG_PADDLE_H);
    if (pw < 6.0f) pw = 6.0f;

    draw_paddle(fx(&b, PONG_PADDLE_X_L << PONG_Q4_SHIFT) - pw * 0.5f,
                fy(&b, view->left_y) - ph * 0.5f, pw, ph,
                mine_left ? CLR_MINE : CLR_THEIRS);
    draw_paddle(fx(&b, PONG_PADDLE_X_R << PONG_Q4_SHIFT) - pw * 0.5f,
                fy(&b, view->right_y) - ph * 0.5f, pw, ph,
                mine_left ? CLR_THEIRS : CLR_MINE);

    if (view->state == PONG_MATCH_STATE_PLAY ||
        view->state == PONG_MATCH_STATE_GOAL_FREEZE) {
        float bx = fx(&b, view->ball_x), by = fy(&b, view->ball_y);
        float br = fs(&b, (float)PONG_BALL_R);
        if (br < 3.0f) br = 3.0f;

        if (view->state == PONG_MATCH_STATE_PLAY) trail_push(bx, by);

        for (int i = 0; i < TRAIL_LEN; i++) {
            int idx = (s_thead + i) % TRAIL_LEN;
            float age = (float)i / (float)TRAIL_LEN;
            float sz = br * (0.2f + 0.7f * age);
            pong_gfx_rect(s_tx[idx] - sz, s_ty[idx] - sz, sz * 2.0f, sz * 2.0f,
                          pong_rgba(0xff, 0xff, 0xff, (uint8_t)(age * age * 80.0f)));
        }
        pong_gfx_circle(bx, by, br * 2.4f, pong_rgba(0xff, 0xff, 0xff, 0x22));
        pong_gfx_circle(bx, by, br * 1.6f, pong_rgba(0xff, 0xff, 0xff, 0x2A));
        pong_gfx_circle(bx, by, br, CLR_BALL);
    } else {
        s_tlive = false;
    }
}

/** Score and names, above the field where a desktop game puts them. */
static void draw_scoreboard(const PongView *view, const DeskHud *hud, int out_w, int out_h)
{
    if (!view || !view->valid) return;
    FieldBox b = field_box(out_w, out_h);

    bool mine_left = (hud->my_side == 0);
    const char *ln = mine_left ? hud->my_name : hud->opp_name;
    const char *rn = mine_left ? hud->opp_name : hud->my_name;
    PongColor lc = mine_left ? CLR_MINE : CLR_THEIRS;
    PongColor rc = mine_left ? CLR_THEIRS : CLR_MINE;

    float cx = b.x + b.w * 0.5f;
    float y  = b.y - (float)out_h * 0.115f;

    char sc[16];
    snprintf(sc, sizeof sc, "%u", view->score_l);
    pong_gfx_text(cx - 44.0f, y, 2.0f, lc, PONG_ALIGN_RIGHT, sc);
    pong_gfx_text(cx, y + 8.0f, 1.2f, CLR_FAINT, PONG_ALIGN_CENTER, "-");
    snprintf(sc, sizeof sc, "%u", view->score_r);
    pong_gfx_text(cx + 44.0f, y, 2.0f, rc, PONG_ALIGN_LEFT, sc);

    if (ln && ln[0]) pong_gfx_text(cx - 44.0f, y + 54.0f, 0.7f, mix(lc, CLR_BG, 0.3f),
                                   PONG_ALIGN_RIGHT, ln);
    if (rn && rn[0]) pong_gfx_text(cx + 44.0f, y + 54.0f, 0.7f, mix(rc, CLR_BG, 0.3f),
                                   PONG_ALIGN_LEFT, rn);
}

/** A small chip of connection facts, bottom left, out of the way. */
static void draw_status_chip(const DeskHud *hud, int out_w, int out_h)
{
    (void)out_w;
    char line[160];
    snprintf(line, sizeof line, "%s   RTT %u   %u snap/s   buf %u   %u fps",
             hud->status_line ? hud->status_line : "",
             (unsigned)hud->rtt_ms, (unsigned)hud->snap_hz,
             (unsigned)hud->buf_ms, (unsigned)hud->fps);

    float w = 0.0f, h = 0.0f;
    pong_gfx_text_size(line, 0.6f, &w, &h);
    float x = 18.0f, y = (float)out_h - h - 18.0f;
    pong_gfx_rect(x - 10.0f, y - 6.0f, w + 20.0f, h + 12.0f, CLR_PANEL);
    pong_gfx_text(x, y, 0.6f, CLR_FAINT, PONG_ALIGN_LEFT, line);
}

static void draw_hint(const DeskHud *hud, int out_w, int out_h)
{
    const char *hint = hud->gamepad
        ? "LEFT STICK move    CROSS select    CIRCLE back    OPTIONS quit"
        : "MOUSE or ARROWS move    ENTER select    ESC back";
    pong_gfx_text((float)out_w * 0.5f, (float)out_h - 40.0f, 0.6f, CLR_FAINT,
                  PONG_ALIGN_CENTER, hint);
}

/* ------------------------------------------------------------------ menu */

static void draw_menu(const DeskHud *hud, int out_w, int out_h)
{
    float cx = (float)out_w * 0.5f;

    const float ts = title_scale(out_h);
    float ty = (float)out_h * 0.06f;
    pong_gfx_text(cx, ty, ts, CLR_ACCENT, PONG_ALIGN_CENTER, "PONG");
    ty += ts * 24.0f + 4.0f;
    pong_gfx_text(cx, ty, 0.8f, CLR_TEXT, PONG_ALIGN_CENTER, "M U L T I P L A Y E R");
    ty += 26.0f;
    pong_gfx_text(cx, ty, 0.62f, CLR_FAINT, PONG_ALIGN_CENTER,
                  "plays across 3DS, Vita, Windows, macOS, Linux and the browser");

    for (int i = 0; i < DESK_ITEM_COUNT; i++) {
        float x, y, w, h;
        menu_row(out_w, out_h, i, &x, &y, &w, &h);
        bool sel = (hud->sel == i);

        pong_gfx_rect(x, y, w, h, sel ? pong_rgba(0x16, 0x2c, 0x3a, 0xFF)
                                      : pong_rgba(0x0c, 0x12, 0x1a, 0xFF));
        PongColor edge = sel ? CLR_ACCENT : CLR_EDGE;
        pong_gfx_rect(x, y, w, 1.0f, edge);
        pong_gfx_rect(x, y + h - 1.0f, w, 1.0f, edge);
        pong_gfx_rect(x, y, 1.0f, h, edge);
        pong_gfx_rect(x + w - 1.0f, y, 1.0f, h, edge);
        if (sel) pong_gfx_rect(x, y, 4.0f, h, CLR_ACCENT);

        /* Both lines positioned from the row's own height: a 34px row on a
         * handheld and a 52px one in a window should both look deliberate. */
        const float label_s = (h >= 46.0f) ? 0.85f : 0.72f;
        const float hint_s  = (h >= 46.0f) ? 0.60f : 0.52f;
        pong_gfx_text(x + 20.0f, y + h * 0.12f, label_s, sel ? CLR_TEXT : CLR_DIM,
                      PONG_ALIGN_LEFT, ITEM[i].label);
        if (ITEM[i].hint) {
            pong_gfx_text(x + 20.0f, y + h * 0.12f + label_s * 24.0f + 2.0f, hint_s,
                          sel ? CLR_DIM : CLR_FAINT, PONG_ALIGN_LEFT, ITEM[i].hint);
        }

        /* The two settings rows show their current value on the right, which is
         * the whole reason to look at them. */
        const char *val = NULL;
        if (i == DESK_ITEM_SERVER) val = hud->server_addr;
        if (i == DESK_ITEM_NAME)   val = hud->my_name;
        if (val && val[0]) {
            pong_gfx_text(x + w - 20.0f, y + h * 0.5f - 8.0f, 0.62f,
                          sel ? CLR_ACCENT : CLR_FAINT, PONG_ALIGN_RIGHT, val);
        }
    }
}

/* ---------------------------------------------------------------- frame */

void pong_desk_frame(const PongView *view, const DeskHud *hud)
{
    int w = 0, h = 0;
    pong_gfx_output_size(&w, &h);
    if (w <= 0 || h <= 0) return;

    pong_gfx_surface_begin(PONG_SURFACE_FULL, CLR_SURROUND);

    switch (hud->screen) {
    case DESK_MENU:
        draw_menu(hud, w, h);
        draw_hint(hud, w, h);
        break;

    case DESK_CONNECTING:
        pong_gfx_text((float)w * 0.5f, (float)h * 0.44f, 1.2f, CLR_TEXT,
                      PONG_ALIGN_CENTER, "CONNECTING");
        pong_gfx_text((float)w * 0.5f, (float)h * 0.44f + 44.0f, 0.7f, CLR_DIM,
                      PONG_ALIGN_CENTER, hud->server_addr ? hud->server_addr : "");
        break;

    case DESK_WAITING: {
        draw_field(view, hud, w, h);
        /* Banded across the FIELD, not the window: a strip running out past the
         * table edge reads as a broken overlay rather than part of the game. */
        FieldBox fb = field_box(w, h);
        pong_gfx_rect(fb.x, fb.y + fb.h * 0.36f, fb.w, 110.0f, CLR_PANEL);
        pong_gfx_text(fb.x + fb.w * 0.5f, fb.y + fb.h * 0.36f + 14.0f, 1.1f, CLR_TEXT,
                      PONG_ALIGN_CENTER, "WAITING FOR AN OPPONENT");
        if (hud->room_code && hud->room_code[0]) {
            char rc[64];
            snprintf(rc, sizeof rc, "room  %s", hud->room_code);
            pong_gfx_text(fb.x + fb.w * 0.5f, fb.y + fb.h * 0.36f + 62.0f, 0.85f,
                          CLR_ACCENT, PONG_ALIGN_CENTER, rc);
        }
        draw_status_chip(hud, w, h);
        break;
    }

    case DESK_PLAY:
        draw_field(view, hud, w, h);
        draw_scoreboard(view, hud, w, h);
        draw_status_chip(hud, w, h);
        break;

    case DESK_GAMEOVER: {
        draw_field(view, hud, w, h);
        draw_scoreboard(view, hud, w, h);
        bool won = view && ((hud->my_side == 0) ? (view->score_l > view->score_r)
                                                : (view->score_r > view->score_l));
        FieldBox fb = field_box(w, h);
        pong_gfx_rect(fb.x, fb.y + fb.h * 0.38f, fb.w, 96.0f, CLR_PANEL);
        pong_gfx_text(fb.x + fb.w * 0.5f, fb.y + fb.h * 0.38f + 16.0f, 1.4f,
                      won ? CLR_MINE : CLR_THEIRS, PONG_ALIGN_CENTER,
                      won ? "YOU WIN" : "YOU LOSE");
        pong_gfx_text(fb.x + fb.w * 0.5f, fb.y + fb.h * 0.38f + 62.0f, 0.7f, CLR_DIM,
                      PONG_ALIGN_CENTER, "ENTER play again    ESC menu");
        break;
    }

    case DESK_ERROR:
        pong_gfx_text((float)w * 0.5f, (float)h * 0.38f, 1.2f, CLR_WARN,
                      PONG_ALIGN_CENTER, "CONNECTION FAILED");
        if (hud->message) {
            pong_gfx_text_wrap((float)w * 0.5f, (float)h * 0.38f + 50.0f, 0.7f,
                               CLR_DIM, PONG_ALIGN_CENTER, (float)w * 0.7f,
                               hud->message);
        }
        pong_gfx_text((float)w * 0.5f, (float)h * 0.38f + 140.0f, 0.7f, CLR_FAINT,
                      PONG_ALIGN_CENTER, "ESC to go back");
        break;
    }

    /*
     * Transient messages sit at the BOTTOM, not the top.
     *
     * They were centred near the top, which put them straight over the
     * scoreboard -- the first capture of this screen showed a pad-connected
     * notice covering the score, with only the tops of the digits visible. The
     * score is the one thing that must never be obscured, and a toast is the
     * one thing that can go anywhere.
     */
    if (hud->screen != DESK_ERROR && hud->message && hud->message[0]) {
        float tw = 0.0f, th = 0.0f;
        pong_gfx_text_size(hud->message, 0.7f, &tw, &th);
        float bx = ((float)w - tw) * 0.5f;
        float by = (float)h - th - 66.0f;
        pong_gfx_rect(bx - 14.0f, by - 8.0f, tw + 28.0f, th + 16.0f, CLR_PANEL);
        pong_gfx_text((float)w * 0.5f, by, 0.7f, CLR_WARN, PONG_ALIGN_CENTER, hud->message);
    }

    /* Text entry, drawn over everything because it is a modal thing. */
    if (hud->editing) {
        pong_gfx_rect(0.0f, 0.0f, (float)w, (float)h, pong_rgba(0x00, 0x00, 0x00, 0xC0));
        float bw = (float)w * 0.6f, bh = 150.0f;
        float bx = ((float)w - bw) * 0.5f, by = ((float)h - bh) * 0.5f;
        pong_gfx_rect(bx, by, bw, bh, pong_rgba(0x0d, 0x15, 0x1f, 0xFF));
        pong_gfx_rect(bx, by, bw, 2.0f, CLR_ACCENT);
        pong_gfx_text(bx + 24.0f, by + 22.0f, 0.7f, CLR_DIM, PONG_ALIGN_LEFT,
                      hud->edit_label ? hud->edit_label : "");
        char shown[160];
        snprintf(shown, sizeof shown, "%s_", hud->edit_text ? hud->edit_text : "");
        pong_gfx_text(bx + 24.0f, by + 58.0f, 1.0f, CLR_TEXT, PONG_ALIGN_LEFT, shown);
        pong_gfx_text(bx + 24.0f, by + 108.0f, 0.6f, CLR_FAINT, PONG_ALIGN_LEFT,
                      "ENTER save    ESC cancel");
    }
}
