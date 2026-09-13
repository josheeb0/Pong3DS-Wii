#include "render.h"
#include "pong_version.h"
#include "pong_proto.h"
#include "pong_bot.h"
#include "gfx.h"

#define CREDIT_TEXT "made by josheeb0 on github :)"
#include <stdio.h>
#include <string.h>
#include <math.h>

/*
 * Q4 field units -> top-screen pixels: /16 for fixed point, /2 for the
 * 800x480 -> 400x240 mapping.
 *
 * A float divide, NOT the shift this used to be. The shift truncated every
 * position to a whole pixel, so a ball crossing the screen moved in integer
 * steps and visibly stair-stepped even when the netcode was feeding perfectly
 * smooth input -- half of "it looks jagged" was here rather than in the
 * network. citro2d takes floats and the GPU interpolates, so keeping the
 * sub-pixel part costs nothing and is the single largest smoothness win
 * available.
 *
 * 32 = 16 (Q4) * 2 (field -> screen), exact in binary, so this is still exact
 * at both extremes.
 */
#define TOPX(q) ((float)(q) * (1.0f / 32.0f))

#define CLR_BG        pong_rgba(0x07, 0x09, 0x0d, 0xFF)
#define CLR_NET       pong_rgba(0x1e, 0x3a, 0x4d, 0xFF)
#define CLR_MINE      pong_rgba(0x7e, 0xe7, 0xff, 0xFF)
#define CLR_THEIRS    pong_rgba(0xff, 0x9d, 0xe2, 0xFF)
#define CLR_BALL      pong_rgba(0xff, 0xff, 0xff, 0xFF)
#define CLR_TEXT      pong_rgba(0xdc, 0xe9, 0xf5, 0xFF)
/* Lifted from 0x6a869b. The New 3DS XL stretches the same 320x240 panel over
 * 4.18 inches, so it is the lowest pixel density of any 3DS -- text that read
 * fine in an emulator at 3x is mush on the hardware. Secondary text needs real
 * contrast, not a polite grey. */
#define CLR_DIM       pong_rgba(0x95, 0xad, 0xc0, 0xFF)
#define CLR_FAINT     pong_rgba(0x5c, 0x74, 0x87, 0xFF)
#define CLR_WARN      pong_rgba(0xff, 0xc8, 0x78, 0xFF)
#define CLR_SCORE     pong_rgba(0x2a, 0x44, 0x5a, 0xFF)
#define CLR_PANEL     pong_rgba(0x0d, 0x11, 0x17, 0xFF)
#define CLR_BTN       pong_rgba(0x0e, 0x16, 0x1e, 0xFF)
#define CLR_BTN_SEL   pong_rgba(0x16, 0x2c, 0x3a, 0xFF)
#define CLR_EDGE      pong_rgba(0x1a, 0x2b, 0x38, 0xFF)
#define CLR_ACCENT    pong_rgba(0x7e, 0xe7, 0xff, 0xFF)
#define CLR_ACCENT2   pong_rgba(0xff, 0x9d, 0xe2, 0xFF)
#define CLR_GOOD      pong_rgba(0x7d, 0xff, 0xa8, 0xFF)
#define CLR_HEADER    pong_rgba(0x0a, 0x12, 0x19, 0xFF)
#define CLR_TRAIL     pong_rgba(0x16, 0x24, 0x30, 0xFF)
#define CLR_FIELD_HI  pong_rgba(0x0d, 0x15, 0x1f, 0xFF)
#define CLR_FIELD_LO  pong_rgba(0x05, 0x07, 0x0b, 0xFF)
#define CLR_GLOW_MINE pong_rgba(0x7e, 0xe7, 0xff, 0x30)
#define CLR_GLOW_THRS pong_rgba(0xff, 0x9d, 0xe2, 0x30)
#define CLR_BALL_GLOW pong_rgba(0xff, 0xff, 0xff, 0x28)

/* Blends two colours. Used for the selection animation, so the highlight
 * travels rather than snapping between rows -- motion is most of what makes an
 * interface feel considered rather than assembled. */
static PongColor mix(PongColor a, PongColor b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint8_t out[4];
    for (int i = 0; i < 4; i++) {
        uint8_t ca = (uint8_t)((a >> (i * 8)) & 0xFF);
        uint8_t cb = (uint8_t)((b >> (i * 8)) & 0xFF);
        out[i] = (uint8_t)(ca + (cb - ca) * t);
    }
    return (PongColor)out[0] | ((PongColor)out[1] << 8) | ((PongColor)out[2] << 16) | ((PongColor)out[3] << 24);
}

/*
 * Text helpers.
 *
 * These were a pair of citro2d text buffers with pre-parsed strings for the
 * handful that never change. That optimisation belonged to citro2d and moved
 * into the backend with it; four strings a frame is not worth an abstraction
 * that every platform would have to reimplement.
 */
static void dyn(const char *s, int align, float x, float y, float scale, PongColor color)
{
    pong_gfx_text(x, y, scale, color, align, s);
}

/*
 * Same, but wrapped to a width.
 *
 * Needed because diagnostics carry things we do not control the length of --
 * hostnames, mbedTLS error strings -- and unwrapped they simply run off the
 * side of a 320px screen, which is exactly when you most need to read them.
 */
static void dyn_wrap(const char *s, int align, float x, float y, float scale,
                     PongColor color, float wrap_w)
{
    pong_gfx_text_wrap(x, y, scale, color, align, wrap_w, s);
}

/*
 * Touch regions for the title screen. Declared in the header as constants so
 * the drawing code and the hit-testing in main.c cannot drift apart -- a
 * button you can see but not press is a miserable bug to chase.
 */
/*
 * Bottom screen is 320x240 on every 3DS -- the XL does not add pixels, it
 * stretches the same panel over 4.18 inches. That makes it the least dense
 * screen in the family (~132 PPI against ~165 on the original), so the limit on
 * this layout is legibility per pixel, not space.
 *
 * Consequences, both learned the hard way on hardware:
 *   - nothing below ~13px of text height is comfortably readable, which with
 *     citro2d's system font (about 30px at scale 1.0) is a floor of ~0.44;
 *   - a touch target under about 30px is awkward with a thumb, and this is a
 *     screen people use with a thumb while holding the console.
 *
 * So: three tall primary rows, one utility row that still clears 30px, and a
 * status line. Everything that used to be drawn at 0.29-0.38 is either bigger
 * now or gone.
 */
/*
 * Five ways to start a match now, not three, on a screen that is still 320x240.
 *
 * The rows drop from 44px to 32 rather than the list scrolling. 30px is the
 * floor -- it is what a thumb needs, and it is what decided the utility row's
 * height -- so 32 keeps a margin while fitting five rows and the utility strip
 * in the 200px between the header and the bottom edge.
 */
const PongRect PONG_MENU_RECT[MENU_COUNT] = {
    [MENU_QUICK]    = {  10.0f,  36.0f, 300.0f, 32.0f },
    [MENU_ROOM]     = {  10.0f,  71.0f, 300.0f, 32.0f },
    [MENU_BOT]      = {  10.0f, 106.0f, 300.0f, 32.0f },
    [MENU_LOCAL_AI] = {  10.0f, 141.0f, 300.0f, 32.0f },
    [MENU_LOCAL_2P] = {  10.0f, 176.0f, 300.0f, 32.0f },
    /* Four utility buttons across 300px: 72 wide with 4px gaps. */
    [MENU_NAME]   = {  10.0f, 212.0f,  72.0f, 24.0f },
    [MENU_SERVER] = {  86.0f, 212.0f,  72.0f, 24.0f },
    [MENU_SOURCE] = { 162.0f, 212.0f,  72.0f, 24.0f },
    [MENU_UPDATE] = { 238.0f, 212.0f,  72.0f, 24.0f },
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
static void panel(const PongRect *r, PongColor fill, PongColor edge)
{
    pong_gfx_rect(r->x, r->y, r->w, r->h, fill);
    pong_gfx_rect(r->x, r->y, r->w, 1.0f, edge);
    pong_gfx_rect(r->x, r->y + r->h - 1.0f, r->w, 1.0f, edge);
    pong_gfx_rect(r->x, r->y, 1.0f, r->h, edge);
    pong_gfx_rect(r->x + r->w - 1.0f, r->y, 1.0f, r->h, edge);
}

/* ------------------------------------------------------------------ menu */

typedef struct { const char *label; const char *hint; } MenuLabel;

static const MenuLabel MENU_LABEL[MENU_COUNT] = {
    [MENU_QUICK]  = { "QUICK MATCH", "play whoever is waiting" },
    [MENU_ROOM]   = { "JOIN ROOM",   "same code = same game" },
    [MENU_BOT]      = { "VS CPU",    "the server's bot" },
    [MENU_LOCAL_AI] = { "VS AI",     "offline, no server" },
    [MENU_LOCAL_2P] = { "2 PLAYERS", "D-PAD vs A/B/X/Y" },
    [MENU_NAME]   = { "NAME",        NULL },
    [MENU_SERVER] = { "SERVER",      NULL },
    [MENU_SOURCE] = { "SOURCE",      NULL },
    [MENU_UPDATE] = { "UPDATE",      NULL },
};

/* Per-item glyphs, so the rows are distinguishable at a glance rather than
 * being five identical boxes with different words in them. */
static void icon_for(int item, float x, float y, PongColor a, PongColor b)
{
    switch (item) {
    case MENU_QUICK:   /* two paddles and a ball: a normal match */
        pong_gfx_rect(x,         y, 2.0f, 12.0f, a);
        pong_gfx_rect(x + 12.0f, y + 3.0f, 2.0f, 12.0f, b);
        pong_gfx_circle(x + 7.0f, y + 7.0f, 2.0f, CLR_BALL);
        break;
    case MENU_LOCAL_AI:  /* one paddle and a chip: a machine on this device */
        pong_gfx_rect(x, y, 2.0f, 12.0f, a);
        pong_gfx_rect(x + 8.0f, y + 2.0f, 8.0f, 8.0f, b);
        pong_gfx_rect(x + 10.0f, y + 4.0f, 4.0f, 4.0f, CLR_BG);
        break;
    case MENU_LOCAL_2P:  /* two paddles, no ball between: two people, one box */
        pong_gfx_rect(x,         y, 2.0f, 12.0f, a);
        pong_gfx_rect(x + 4.0f,  y + 3.0f, 2.0f, 9.0f, a);
        pong_gfx_rect(x + 10.0f, y + 3.0f, 2.0f, 9.0f, b);
        pong_gfx_rect(x + 14.0f, y, 2.0f, 12.0f, b);
        break;
    case MENU_ROOM: {  /* a keypad: something you type */
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                pong_gfx_rect(x + c * 5.0f, y + r * 5.0f, 3.0f, 3.0f,
                                  (r + c) % 2 ? b : a);
        break;
    }
    case MENU_BOT:     /* a blocky head: the CPU */
        pong_gfx_rect(x + 1.0f, y + 1.0f, 12.0f, 11.0f, a);
        pong_gfx_rect(x + 4.0f, y + 4.0f, 2.0f, 2.0f, CLR_BG);
        pong_gfx_rect(x + 9.0f, y + 4.0f, 2.0f, 2.0f, CLR_BG);
        pong_gfx_rect(x + 4.0f, y + 8.0f, 7.0f, 1.0f, CLR_BG);
        break;
    default:
        pong_gfx_rect(x + 2.0f, y + 2.0f, 10.0f, 10.0f, a);
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
    pong_gfx_rect_grad(0.0f, 0.0f, 320.0f, 32.0f,
                      CLR_HEADER, CLR_HEADER, CLR_BG, CLR_BG);
    pong_gfx_rect(0.0f, 31.0f, 320.0f, 1.0f, CLR_ACCENT);
    dyn("PONG", 0, 12.0f, 3.0f, 0.72f, CLR_ACCENT);
    dyn("MULTIPLAYER", 0, 70.0f, 9.0f, 0.5f, CLR_TEXT);

    char b[48];
    if (hud->build_id == 0) {
        /* A local build is not a release and should never be mistaken for one. */
        snprintf(b, sizeof b, "DEV");
        dyn(b, PONG_ALIGN_RIGHT, 310.0f, 9.0f, 0.5f, CLR_WARN);
    } else {
        snprintf(b, sizeof b, "BUILD %lu", (unsigned long)hud->build_id);
        dyn(b, PONG_ALIGN_RIGHT, 310.0f, 9.0f, 0.5f, CLR_ACCENT);
    }

    for (int i = 0; i < MENU_COUNT; i++) {
        const PongRect *r = &PONG_MENU_RECT[i];
        bool sel = (hud->menu_sel == i);
        bool primary = (i <= MENU_LOCAL_2P);

        panel(r, sel ? CLR_BTN_SEL : CLR_BTN, sel ? CLR_ACCENT : CLR_EDGE);

        if (primary) {
            icon_for(i, r->x + 16.0f, r->y + (r->h - 14.0f) / 2.0f,
                     sel ? CLR_ACCENT : CLR_DIM, sel ? CLR_ACCENT2 : CLR_DIM);
            /* Both lines moved up with the row height: at 32px the old y+24
             * hint hung 2px past the bottom edge. */
            dyn(MENU_LABEL[i].label, 0, r->x + 46.0f, r->y + 2.0f, 0.56f,
                sel ? CLR_TEXT : CLR_DIM);
            if (MENU_LABEL[i].hint) {
                dyn(MENU_LABEL[i].hint, 0, r->x + 46.0f, r->y + 18.0f, 0.42f,
                    sel ? CLR_DIM : CLR_FAINT);
            }
            /* The offline opponent's difficulty, on its own row. Changed with
             * LEFT/RIGHT, which is why it sits where a value would and not in
             * the hint: a hint is prose, this is a setting. */
            if (i == MENU_LOCAL_AI) {
                dyn(pong_bot_level_name((PongBotLevel)hud->ai_level),
                    PONG_ALIGN_RIGHT, r->x + r->w - 34.0f, r->y + 9.0f, 0.5f,
                    sel ? CLR_ACCENT : CLR_FAINT);
            }
            /* A chevron on the active row, pointing at the thing A will do. */
            if (sel) {
                float cx = r->x + r->w - 20.0f, cy = r->y + r->h / 2.0f;
                for (int k = 0; k < 6; k++) {
                    pong_gfx_rect(cx + k, cy - 6.0f + k, 2.0f, 2.5f, CLR_ACCENT);
                    pong_gfx_rect(cx + k, cy + 5.0f - k, 2.0f, 2.5f, CLR_ACCENT);
                }
            }
        } else {
            dyn(MENU_LABEL[i].label, PONG_ALIGN_CENTER,
                r->x + r->w / 2.0f, r->y + 7.0f, 0.5f, sel ? CLR_TEXT : CLR_DIM);
        }
    }

    /* The travelling highlight: a bar down the left edge of the active row. */
    pong_gfx_rect(10.0f, sel_y, 3.0f,
                      PONG_MENU_RECT[hud->menu_sel].h, CLR_ACCENT);

    /*
     * Status line.
     *
     * One line, one job. This used to be three separate rows at 0.29-0.33 --
     * the server address, the update source, and a build/protocol footer -- and
     * the address in particular ran off the side of the screen where it was
     * needed most. A message, when there is one, replaces the lot: nothing here
     * matters while the console is trying to tell you something.
     */
    if (hud->message && hud->message[0]) {
        dyn_wrap(hud->message, 0, 10.0f, 214.0f, 0.44f, CLR_WARN, 300.0f);
    } else if (hud->server_addr && hud->server_addr[0]) {
        /* The address starts after the label, measured rather than at a fixed
         * 62px. Backends do not share a font -- the PC one is fixed-width and
         * wider -- and a hardcoded offset that fit on the console overlapped
         * the address everywhere else. */
        const float lx = 10.0f;
        float lw = 0.0f, lh = 0.0f;
        pong_gfx_text_size("SERVER", 0.44f, &lw, &lh);
        dyn("SERVER", PONG_ALIGN_LEFT, lx, 215.0f, 0.44f, CLR_FAINT);
        float ax = lx + lw + 6.0f;
        dyn_wrap(hud->server_addr, PONG_ALIGN_LEFT, ax, 213.0f, 0.46f, CLR_DIM,
                 310.0f - ax);
    }
}

/* ------------------------------------------------------------------- top */

/*
 * Ball trail.
 *
 * Kept here rather than in the client because it is presentation only: nothing
 * else in the program has any use for where the ball was half a second ago, and
 * putting it in the shared view would imply the simulation cared.
 *
 * Screen coordinates, not field coordinates, so a resolution change would
 * invalidate it -- which is fine, there is exactly one resolution.
 */
#define TRAIL_LEN 14
static float s_trail_x[TRAIL_LEN], s_trail_y[TRAIL_LEN];
static int   s_trail_head = 0;
static bool  s_trail_live = false;

static void trail_reset(float x, float y)
{
    for (int i = 0; i < TRAIL_LEN; i++) { s_trail_x[i] = x; s_trail_y[i] = y; }
    s_trail_head = 0;
    s_trail_live = true;
}

static void trail_push(float x, float y)
{
    if (!s_trail_live) { trail_reset(x, y); return; }
    /* A serve or a goal teleports the ball. Dragging a trail across that reads
     * as a glitch, so a big jump restarts the trail rather than smearing. */
    int prev = (s_trail_head + TRAIL_LEN - 1) % TRAIL_LEN;
    float dx = x - s_trail_x[prev], dy = y - s_trail_y[prev];
    if (dx * dx + dy * dy > 60.0f * 60.0f) { trail_reset(x, y); return; }
    s_trail_x[s_trail_head] = x;
    s_trail_y[s_trail_head] = y;
    s_trail_head = (s_trail_head + 1) % TRAIL_LEN;
}

/** Paddle with a soft glow behind it, so it reads as lit rather than painted. */
static void draw_paddle(float x, float y, float w, float h, PongColor body, PongColor glow)
{
    pong_gfx_rect(x - 3.0f, y - 3.0f, w + 6.0f, h + 6.0f, glow);
    pong_gfx_rect(x - 1.5f, y - 1.5f, w + 3.0f, h + 3.0f, glow);
    /* Vertical sheen: brighter in the middle, so a flat rectangle gets some
     * shape without needing a texture. */
    pong_gfx_rect_grad(x, y, w, h * 0.5f, mix(body, CLR_BG, 0.35f), mix(body, CLR_BG, 0.35f), body, body);
    pong_gfx_rect_grad(x, y + h * 0.5f, w, h * 0.5f, body, body, mix(body, CLR_BG, 0.35f), mix(body, CLR_BG, 0.35f));
}

static void draw_playfield(const PongView *view, const PongHud *hud)
{
    /* Field: a shallow vertical gradient rather than flat black, which gives
     * the play area an edge without drawing a box around it. */
    pong_gfx_rect_grad(0.0f, 0.0f, 400.0f, 240.0f,
                      CLR_FIELD_HI, CLR_FIELD_HI, CLR_FIELD_LO, CLR_FIELD_LO);

    /* Goal lines. */
    pong_gfx_rect(0.0f, 0.0f, 1.0f, 240.0f, mix(CLR_MINE, CLR_BG, 0.55f));
    pong_gfx_rect(399.0f, 0.0f, 1.0f, 240.0f, mix(CLR_THEIRS, CLR_BG, 0.55f));

    /* Centre net, fading toward the top and bottom so the eye is drawn to the
     * middle of the field where the play is. */
    for (int y = 4; y < 240; y += 15) {
        float d = fabsf((float)y - 120.0f) / 120.0f;
        pong_gfx_rect(199.0f, (float)y, 2.0f, 8.0f,
                          mix(CLR_NET, CLR_FIELD_LO, d * 0.75f));
    }

    if (!view || !view->valid) {
        dyn("CONNECTING...", PONG_ALIGN_CENTER, 200.0f, 104.0f, 0.75f, CLR_TEXT);
        s_trail_live = false;
        return;
    }

    /* Score, behind the play and deliberately large: it is the one thing you
     * want to read without looking away from the ball. */
    char sc[16];
    snprintf(sc, sizeof sc, "%u", view->score_l);
    dyn(sc, PONG_ALIGN_CENTER, 150.0f, 6.0f, 1.35f, CLR_SCORE);
    snprintf(sc, sizeof sc, "%u", view->score_r);
    dyn(sc, PONG_ALIGN_CENTER, 250.0f, 6.0f, 1.35f, CLR_SCORE);

    /*
     * Whose score is whose.
     *
     * Two numbers on a screen do not say which one is yours, and on a console
     * you cannot see the other player to work it out. Each name sits under its
     * own score, and yours is drawn in your paddle's colour so the link between
     * "the blue paddle" and "me" needs no explanation.
     */
    {
        bool left_is_mine = (hud->my_side == 0);
        const char *ln = left_is_mine ? hud->my_name : hud->opp_name;
        const char *rn = left_is_mine ? hud->opp_name : hud->my_name;
        PongColor lc = left_is_mine ? CLR_MINE : CLR_THEIRS;
        PongColor rc = left_is_mine ? CLR_THEIRS : CLR_MINE;
        /* Dimmed toward the background: this is furniture, and a bright name
         * beside a moving ball competes with the ball. */
        if (ln && ln[0]) dyn(ln, PONG_ALIGN_CENTER, 150.0f, 48.0f, 0.46f, mix(lc, CLR_BG, 0.55f));
        if (rn && rn[0]) dyn(rn, PONG_ALIGN_CENTER, 250.0f, 48.0f, 0.46f, mix(rc, CLR_BG, 0.55f));
    }

    const float pw = (float)PONG_PADDLE_W / 2.0f;
    const float ph = (float)PONG_PADDLE_H / 2.0f;

    float lx = (float)PONG_PADDLE_X_L / 2.0f - pw / 2.0f;
    float rx = (float)PONG_PADDLE_X_R / 2.0f - pw / 2.0f;
    float ly = TOPX(view->left_y) - ph / 2.0f;
    float ry = TOPX(view->right_y) - ph / 2.0f;

    bool mine_left = (hud->my_side == 0);
    draw_paddle(lx, ly, pw, ph, mine_left ? CLR_MINE : CLR_THEIRS,
                mine_left ? CLR_GLOW_MINE : CLR_GLOW_THRS);
    draw_paddle(rx, ry, pw, ph, mine_left ? CLR_THEIRS : CLR_MINE,
                mine_left ? CLR_GLOW_THRS : CLR_GLOW_MINE);

    /* Phase banners, on a backing strip so they stay readable over the play. */
    const char *banner = NULL;
    PongColor banner_clr = CLR_TEXT;
    if (view->state == PONG_MATCH_STATE_COUNTDOWN) {
        banner = "GET READY";
    } else if (view->state == PONG_MATCH_STATE_GAME_OVER) {
        bool won = mine_left ? (view->score_l > view->score_r)
                             : (view->score_r > view->score_l);
        banner = won ? "YOU WIN!" : "YOU LOSE";
        banner_clr = won ? CLR_MINE : CLR_THEIRS;
    } else if (view->state == PONG_MATCH_STATE_OPP_LOST) {
        banner = "OPPONENT LEFT";
        banner_clr = CLR_WARN;
    }
    if (banner) {
        pong_gfx_rect(0.0f, 96.0f, 400.0f, 44.0f,
                          pong_rgba(0x06, 0x0a, 0x0e, 0xD8));
        pong_gfx_rect(0.0f, 96.0f, 400.0f, 1.0f, banner_clr);
        pong_gfx_rect(0.0f, 139.0f, 400.0f, 1.0f, banner_clr);
        dyn(banner, PONG_ALIGN_CENTER, 200.0f, 104.0f, 0.95f, banner_clr);
    }

    /* Ball and its trail LAST: circles force a citro2d state change, so every
     * other shape batches ahead of them. The trail is rectangles for the same
     * reason -- fourteen circles a frame is not worth what it costs. */
    if (view->state == PONG_MATCH_STATE_PLAY ||
        view->state == PONG_MATCH_STATE_GOAL_FREEZE) {
        float bx = TOPX(view->ball_x), by = TOPX(view->ball_y);
        float br = (float)PONG_BALL_R / 2.0f;

        if (view->state == PONG_MATCH_STATE_PLAY) trail_push(bx, by);

        for (int i = 0; i < TRAIL_LEN; i++) {
            int idx = (s_trail_head + i) % TRAIL_LEN;
            float age = (float)i / (float)TRAIL_LEN;    /* 0 oldest, 1 newest */
            float sz = br * (0.25f + 0.65f * age);
            PongColor c = pong_rgba(0xff, 0xff, 0xff, (uint8_t)(age * age * 70.0f));
            pong_gfx_rect(s_trail_x[idx] - sz, s_trail_y[idx] - sz, sz * 2.0f, sz * 2.0f, c);
        }

        pong_gfx_circle(bx, by, br * 2.2f, CLR_BALL_GLOW);
        pong_gfx_circle(bx, by, br * 1.5f, CLR_BALL_GLOW);
        pong_gfx_circle(bx, by, br, CLR_BALL);
    } else {
        s_trail_live = false;
    }

    /*
     * Starved means we are drawing a predicted ball because nothing has
     * arrived. Worth surfacing -- if the ball looks wrong, this says whether
     * the network or the game is responsible -- but small, because it is
     * diagnostic and the match is not.
     */
    if (view->starved) {
        pong_gfx_circle(391.0f, 9.0f, 3.5f, CLR_WARN);
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

    PongColor dim  = pong_rgba(0x12, 0x20, 0x2c, 0xFF);
    PongColor dim2 = pong_rgba(0x22, 0x18, 0x28, 0xFF);

    for (int y = 0; y < 240; y += 16) {
        pong_gfx_rect(199.0f, (float)y, 2.0f, 9.0f, CLR_TRAIL);
    }
    pong_gfx_rect(24.0f,  ly - 20.0f, 5.0f, 40.0f, dim);
    pong_gfx_rect(371.0f, ry - 20.0f, 5.0f, 40.0f, dim2);

    /* A short trail behind the ball. Rectangles rather than circles: circles
     * force a citro2d state change each time, and at four of them per frame
     * behind a menu that is not worth paying for. */
    for (int k = 6; k >= 1; k--) {
        float tt = t - (float)k * 2.0f;
        float tx = 200.0f + 150.0f * sinf(tt * 0.013f);
        float ty = 120.0f + 80.0f  * sinf(tt * 0.021f);
        pong_gfx_rect(tx - 1.5f, ty - 1.5f, 3.0f, 3.0f,
                          mix(CLR_TRAIL, CLR_BG, (float)k / 7.0f));
    }
    pong_gfx_circle(bx, by, 4.0f, pong_rgba(0x24, 0x36, 0x46, 0xFF));
}

static void draw_top(const PongView *view, const PongHud *hud)
{
    switch (hud->screen) {
    case SCREEN_TITLE: {
        draw_attract(hud);

        /* Letterbox bands: they frame the title and, conveniently, stop the
         * demo rally competing with the text for attention. */
        pong_gfx_rect(0.0f, 60.0f, 400.0f, 74.0f,
                          pong_rgba(0x06, 0x0a, 0x0e, 0xE8));
        pong_gfx_rect(0.0f, 60.0f, 400.0f, 1.0f, CLR_EDGE);
        pong_gfx_rect(0.0f, 133.0f, 400.0f, 1.0f, CLR_EDGE);

        dyn("PONG", PONG_ALIGN_CENTER, 200.0f, 66.0f, 1.5f, CLR_ACCENT);
        dyn("M U L T I P L A Y E R", PONG_ALIGN_CENTER, 200.0f, 108.0f, 0.5f, CLR_TEXT);
        /* Every platform this actually plays against, not just the browser.
         * Shorter than the desktop client's wording rather than smaller: this
         * screen is 400px and the text is already at the size that was called
         * hard to read, so the words go rather than the scale. */
        dyn("plays across 3DS, Vita, PC, Mac, Linux and browsers",
            PONG_ALIGN_CENTER, 200.0f, 146.0f, 0.46f, CLR_DIM);
        {
            /* The build number is the only version this project has, and the
             * only one an update check can compare. Version names were tried
             * and removed: v1.1.1 reads as 1, which is not newer than 93. */
            char vb[64];
            if (hud->build_id == 0) snprintf(vb, sizeof vb, "DEV BUILD");
            else snprintf(vb, sizeof vb, "BUILD %lu", (unsigned long)hud->build_id);
            dyn(vb, 0, 10.0f, 212.0f, 0.46f,
                hud->build_id == 0 ? CLR_WARN : CLR_DIM);
        }

        {
            /* Was the literal "you are currently on 3ds", which stopped being
             * true the moment there was a second backend. */
            char who[48];
            snprintf(who, sizeof who, "you are currently on %s", pong_gfx_platform_name());
            dyn(who, PONG_ALIGN_RIGHT, 392.0f, 212.0f, 0.46f, CLR_DIM);
        }

        /*
         * Credit, top left. It sits over the attract rally rather than inside
         * the letterbox, so it gets its own backing strip -- dim text on a
         * moving ball is legible only about half the time, which is worse than
         * not showing it at all.
         *
         * The strip is measured from the text rather than given a fixed width,
         * so editing the string can never leave it clipped or floating on an
         * oversized box.
         */
        {
            const float cx = 8.0f, cy = 6.0f, sc = 0.46f, pad = 4.0f;
            float tw = 0.0f, th = 0.0f;
            pong_gfx_text_size(CREDIT_TEXT, sc, &tw, &th);
            pong_gfx_rect(cx - pad, cy - pad, tw + pad * 2.0f, th + pad * 2.0f,
                              pong_rgba(0x06, 0x0a, 0x0e, 0xC0));
            dyn(CREDIT_TEXT, PONG_ALIGN_LEFT, cx, cy, sc, CLR_DIM);
        }
        break;
    }

    case SCREEN_CONNECTING:
        dyn("PONG MULTIPLAYER!", PONG_ALIGN_CENTER, 200.0f, 60.0f, 0.8f, CLR_TEXT);
        dyn(hud->message ? hud->message : "CONNECTING...",
            PONG_ALIGN_CENTER, 200.0f, 120.0f, 0.6f, CLR_DIM);
        break;

    case SCREEN_QUEUED:
        draw_attract(hud);
        if (hud->room_code && hud->room_code[0]) {
            dyn("ROOM CODE", PONG_ALIGN_CENTER, 200.0f, 48.0f, 0.46f, CLR_DIM);

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
                dyn(ch, PONG_ALIGN_CENTER, cell.x + bw / 2.0f, cell.y + 7.0f,
                    1.0f, CLR_ACCENT);
            }

            dyn("enter this code on the other device",
                PONG_ALIGN_CENTER, 200.0f, 134.0f, 0.5f, CLR_TEXT);

            /* Three dots cycling: says "still waiting" without a spinner that
             * would need its own animation state. */
            int phase = (hud->frame / 20) % 4;
            for (int i = 0; i < 3; i++) {
                pong_gfx_circle(190.0f + i * 10.0f, 166.0f, 3.0f,
                                    i < phase ? CLR_ACCENT : CLR_EDGE);
            }
        } else {
            dyn("WAITING FOR AN OPPONENT", PONG_ALIGN_CENTER, 200.0f, 98.0f, 0.7f, CLR_TEXT);
            dyn("a CPU opponent is offered after 15s",
                PONG_ALIGN_CENTER, 200.0f, 134.0f, 0.5f, CLR_DIM);
        }
        break;

    case SCREEN_ERROR:
        dyn("CONNECTION FAILED", PONG_ALIGN_CENTER, 200.0f, 30.0f, 0.8f, CLR_WARN);
        /* Every path's result, so the failure is diagnosable from the screen
         * rather than by guessing. A fallback's error alone is not enough. */
        if (hud->diag && hud->diag[0]) {
            /* Top screen is 400 wide; leave a 12px margin each side. */
            dyn_wrap(hud->diag, 0, 12.0f, 60.0f, 0.47f, CLR_DIM, 376.0f);
        } else if (hud->message) {
            dyn_wrap(hud->message, 0, 12.0f, 96.0f, 0.47f, CLR_DIM, 376.0f);
        }
        dyn("also written to sd:/3ds/pong3ds.log",
            PONG_ALIGN_CENTER, 200.0f, 194.0f, 0.44f, CLR_FAINT);
        dyn("A / TAP = back", PONG_ALIGN_CENTER, 200.0f, 214.0f, 0.5f, CLR_DIM);
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

    /*
     * The title screen owns the whole bottom screen.
     *
     * This panel used to be drawn first on every screen and the menu painted
     * over it, so on the title screen the transport status ("not connected")
     * sat underneath the PONG MULTIPLAYER header in the same few pixels. Two
     * unrelated pieces of text in one place, neither readable. The menu already
     * has a header of its own and nothing to say about a connection that has
     * not been attempted yet.
     */
    if (hud->screen == SCREEN_TITLE) {
        draw_menu(hud);
        return;
    }

    /* Status panel. Taller than it was, because the two lines inside it are
     * bigger and cramping them was most of why they were hard to read. */
    pong_gfx_rect_grad(0.0f, 0.0f, 320.0f, 50.0f,
                      CLR_PANEL, CLR_PANEL, CLR_BG, CLR_BG);
    pong_gfx_rect(0.0f, 49.0f, 320.0f, 1.0f, CLR_EDGE);

    if (hud->status_line && hud->status_line[0]) {
        dyn_wrap(hud->status_line, 0, 8.0f, 4.0f, 0.5f, CLR_MINE, 244.0f);
    }
    if (hud->detail_line && hud->detail_line[0]) {
        dyn_wrap(hud->detail_line, 0, 8.0f, 26.0f, 0.46f, CLR_DIM, 304.0f);
    }
    if (hud->slow_mode) {
        dyn("SLOW", PONG_ALIGN_RIGHT, 312.0f, 4.0f, 0.5f, CLR_WARN);
    }

    if (hud->screen == SCREEN_PLAY) {
        /*
         * The touch strip IS the paddle, so it is drawn as a track with a
         * marker showing where your paddle currently is. Previously it was an
         * undifferentiated box with a caption, which said what to do without
         * showing that it was doing anything.
         */
        const float ty = 56.0f, th = 176.0f;
        pong_gfx_rect_grad(6.0f, ty, 308.0f, th,
                          pong_rgba(0x0f, 0x18, 0x22, 0xFF),
                          pong_rgba(0x0f, 0x18, 0x22, 0xFF),
                          pong_rgba(0x0a, 0x10, 0x17, 0xFF),
                          pong_rgba(0x0a, 0x10, 0x17, 0xFF));
        for (int i = 1; i < 5; i++) {
            float y = ty + (th / 5.0f) * (float)i;
            pong_gfx_rect(6.0f, y, 308.0f, 1.0f, CLR_TRAIL);
        }

        /* Marker at our own paddle's height, mapped from field to strip. */
        if (view && view->valid) {
            int32_t my_q4 = (hud->my_side == 0) ? view->left_y : view->right_y;
            float f = (float)my_q4 / (float)PONG_FIELD_H_Q4;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            float my = ty + f * th;
            pong_gfx_rect(6.0f, my - 9.0f, 308.0f, 18.0f,
                              pong_rgba(0x7e, 0xe7, 0xff, 0x22));
            pong_gfx_rect(6.0f, my - 1.5f, 308.0f, 3.0f, CLR_MINE);
            pong_gfx_rect(6.0f, my - 6.0f, 4.0f, 12.0f, CLR_MINE);
            pong_gfx_rect(310.0f, my - 6.0f, 4.0f, 12.0f, CLR_MINE);
        }

        dyn("SLIDE TO MOVE", 0, 10.0f, 220.0f, 0.46f, CLR_FAINT);
        dyn("B = LEAVE", PONG_ALIGN_RIGHT, 310.0f, 220.0f, 0.46f, CLR_FAINT);
    } else {
        dyn("TAP THE TOUCH SCREEN\nTO CONTINUE", PONG_ALIGN_CENTER,
            160.0f, 92.0f, 0.6f, CLR_TEXT);
        if (hud->message && hud->message[0] && hud->screen != SCREEN_ERROR) {
            dyn_wrap(hud->message, PONG_ALIGN_CENTER, 160.0f, 186.0f, 0.48f,
                     CLR_DIM, 300.0f);
        }
    }
}

void pong_render_frame(const PongView *view, const PongHud *hud)
{
    /* Both surfaces inside one frame. Which physical screens, windows or
     * viewports those become is the backend's problem, not this file's. */
    pong_gfx_surface_begin(PONG_SURFACE_TOP, CLR_BG);
    draw_top(view, hud);

    pong_gfx_surface_begin(PONG_SURFACE_BOTTOM, CLR_BG);
    draw_bottom(view, hud);
}
