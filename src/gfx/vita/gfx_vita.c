/*
 * PS Vita backend, on vita2d.
 *
 * NOT COMPILED OR RUN. VitaSDK was not installed on the machine this was
 * written on, so unlike the 3DS and SDL3 backends -- both of which were built
 * and, for SDL3, rendered and inspected -- this one is careful reading of the
 * vita2d API rather than anything that has executed. Treat the first build as
 * the real review. Anywhere it is wrong, the seam is still right: only this
 * file changes.
 *
 * LAYOUT. The Vita's screen is 960x544 and the interface is two surfaces of
 * 400x240 and 320x240. Side by side (728x240 with a gap) scales to 1.31 and
 * gives a 525px playfield; stacked (400x488) scales to 1.11 and gives 445.
 * Side by side wins on this hardware, and the same measurement is made at
 * runtime rather than hardcoded, so the rule matches the SDL3 backend and a
 * future device with a different screen gets the right answer for free.
 *
 * TEXT is the same embedded 8x8 font the PC backend uses, drawn as scaled
 * quads, so all three platforms agree about metrics. vita2d has its own font
 * support, and using it would reintroduce exactly the per-platform layout
 * divergence this avoids.
 */

#include "gfx.h"
#include "font8x8.h"

#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/common_dialog.h>
#include <string.h>
#include <stdio.h>

#define VITA_W 960.0f
#define VITA_H 544.0f

#define LINE_AT_SCALE_1  24.0f
#define GLYPH_ADVANCE_PX 6

typedef struct { float x, y, w, h, scale; } Viewport;
static Viewport s_vp[PONG_SURFACE_COUNT];
static PongSurface s_current = PONG_SURFACE_TOP;
static vita2d_texture *s_font;
static bool s_ready = false;

/* A console is its screen: there is no window to grow, so the menu leaves the
 * row out rather than showing a toggle that does nothing. */
bool pong_gfx_fullscreen_supported(void) { return false; }
bool pong_gfx_fullscreen_get(void) { return true; }
void pong_gfx_fullscreen_set(bool on) { (void)on; }

const char *pong_gfx_platform_name(void) { return "vita"; }

static inline float vx(float x) { return s_vp[s_current].x + x * s_vp[s_current].scale; }
static inline float vy(float y) { return s_vp[s_current].y + y * s_vp[s_current].scale; }
static inline float vs(float v) { return v * s_vp[s_current].scale; }

/* PongColor is 0xAABBGGRR; vita2d's RGBA8 helper takes components. */
static inline unsigned int v2d(PongColor c)
{
    return RGBA8((unsigned)(c & 0xFF), (unsigned)((c >> 8) & 0xFF),
                 (unsigned)((c >> 16) & 0xFF), (unsigned)((c >> 24) & 0xFF));
}

static void layout(void)
{
    /* The whole screen, unscaled. The Vita has one display and an interface
     * built for it should use all of it rather than pretending to be two
     * handheld panels side by side. */
    s_vp[PONG_SURFACE_FULL] = (Viewport){ 0.0f, 0.0f, VITA_W, VITA_H, 1.0f };

    const float gap = 8.0f;

    float side_w = PONG_TOP_W + gap + PONG_BOTTOM_W;
    float side_h = (PONG_TOP_H > PONG_BOTTOM_H) ? PONG_TOP_H : PONG_BOTTOM_H;
    float side_scale = VITA_W / side_w;
    if (VITA_H / side_h < side_scale) side_scale = VITA_H / side_h;

    float stack_w = (PONG_TOP_W > PONG_BOTTOM_W) ? PONG_TOP_W : PONG_BOTTOM_W;
    float stack_h = PONG_TOP_H + gap + PONG_BOTTOM_H;
    float stack_scale = VITA_W / stack_w;
    if (VITA_H / stack_h < stack_scale) stack_scale = VITA_H / stack_h;

    if (side_scale >= stack_scale) {
        float sc = side_scale;
        float ox = (VITA_W - side_w * sc) * 0.5f;
        float oy = (VITA_H - side_h * sc) * 0.5f;
        s_vp[PONG_SURFACE_TOP]    = (Viewport){ ox, oy, PONG_TOP_W * sc, PONG_TOP_H * sc, sc };
        s_vp[PONG_SURFACE_BOTTOM] = (Viewport){ ox + (PONG_TOP_W + gap) * sc, oy,
                                                PONG_BOTTOM_W * sc, PONG_BOTTOM_H * sc, sc };
    } else {
        float sc = stack_scale;
        float ox = (VITA_W - stack_w * sc) * 0.5f;
        float oy = (VITA_H - stack_h * sc) * 0.5f;
        s_vp[PONG_SURFACE_TOP]    = (Viewport){ ox + (stack_w - PONG_TOP_W) * 0.5f * sc, oy,
                                                PONG_TOP_W * sc, PONG_TOP_H * sc, sc };
        s_vp[PONG_SURFACE_BOTTOM] = (Viewport){ ox + (stack_w - PONG_BOTTOM_W) * 0.5f * sc,
                                                oy + (PONG_TOP_H + gap) * sc,
                                                PONG_BOTTOM_W * sc, PONG_BOTTOM_H * sc, sc };
    }
}

/** The glyph atlas: 95 glyphs in a row, white, alpha from the bitmap. */
static bool build_font(void)
{
    const int n = FONT8X8_LAST - FONT8X8_FIRST + 1;
    s_font = vita2d_create_empty_texture_format((unsigned)(n * 8), 8,
                                                SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!s_font) return false;

    unsigned int *px = (unsigned int *)vita2d_texture_get_datap(s_font);
    if (!px) return false;          /* a texture can exist with no mapping */

    const unsigned int stride = vita2d_texture_get_stride(s_font) / 4;
    if (stride < (unsigned)(n * 8)) return false;   /* would write past the row */

    memset(px, 0, (size_t)stride * 8 * 4);

    for (int g = 0; g < n; g++) {
        for (int row = 0; row < 8; row++) {
            uint8_t bits = FONT8X8[g][row];
            for (int col = 0; col < 8; col++) {
                if (bits & (1 << col)) {
                    px[(unsigned)row * stride + (unsigned)(g * 8 + col)] = 0xFFFFFFFFu;
                }
            }
        }
    }
    return true;
}

/* Written to by init so a failure says which step, not just "false". */
char g_vita_gfx_error[96] = "";

bool pong_gfx_init(const char *title)
{
    (void)title;
    if (s_ready) return true;

    vita2d_init();
    vita2d_set_clear_color(RGBA8(0x10, 0x12, 0x16, 0xFF));

    if (!build_font()) {
        snprintf(g_vita_gfx_error, sizeof g_vita_gfx_error,
                 "font atlas failed (texture %ux8)",
                 (unsigned)((FONT8X8_LAST - FONT8X8_FIRST + 1) * 8));
        return false;
    }

    layout();
    s_ready = true;
    return true;
}

void pong_gfx_exit(void)
{
    if (!s_ready) return;
    if (s_font) vita2d_free_texture(s_font);
    vita2d_fini();
    s_ready = false;
}

void pong_gfx_frame_begin(void)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
}

static bool s_dialog = false;

void pong_gfx_system_dialog(bool active) { s_dialog = active; }

void pong_gfx_frame_end(void)
{
    vita2d_end_drawing();
    /* The IME is composited by the system, and only if it is given the chance
     * every frame between drawing and the swap. Skip this and the keyboard
     * never appears -- which reads as the application having frozen, because
     * from the player's side it has. */
    if (s_dialog) vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

bool pong_gfx_text_input(bool enabled)
{
    (void)enabled;
    return false;   /* the Vita opens its own IME instead */
}

void pong_gfx_output_size(int *w, int *h)
{
    if (w) *w = (int)VITA_W;
    if (h) *h = (int)VITA_H;
}

void pong_gfx_request_size(int w, int h)
{
    (void)w; (void)h;   /* 960x544, and not negotiable */
}

bool pong_gfx_screenshot(const char *path)
{
    (void)path;
    return false;   /* the console's own screenshot button is better than this */
}

void pong_gfx_surface_begin(PongSurface s, PongColor clear)
{
    s_current = s;
    vita2d_draw_rectangle(s_vp[s].x, s_vp[s].y, s_vp[s].w, s_vp[s].h, v2d(clear));
}

void pong_gfx_rect(float x, float y, float w, float h, PongColor c)
{
    vita2d_draw_rectangle(vx(x), vy(y), vs(w), vs(h), v2d(c));
}

void pong_gfx_rect_grad(float x, float y, float w, float h,
                        PongColor tl, PongColor tr, PongColor bl, PongColor br)
{
    /*
     * vita2d has no per-vertex-coloured rectangle, so this bands the rectangle
     * into horizontal strips.
     *
     * VERTICAL ONLY: tr and br are ignored. Every gradient the interface
     * actually draws is vertical -- the field wash, the header, the status
     * panel, the paddle sheen and the touch strip all pass the same colour for
     * both top corners and the same for both bottom -- so this is exact for
     * every real call rather than an approximation. A horizontal gradient
     * would silently come out wrong, which is why it is said here plainly: add
     * one and this needs a second axis first.
     *
     * Sixteen bands is enough that these shallow washes read as smooth. The
     * alternative is raw GXM for a background, which is not a trade worth
     * making.
     */
    enum { BANDS = 16 };
    for (int i = 0; i < BANDS; i++) {
        float t0 = (float)i / (float)BANDS;
        float t1 = (float)(i + 1) / (float)BANDS;
        PongColor top = tl, bot = bl;   /* left edge; horizontal blend below */
        (void)tr; (void)br;
        unsigned char o[4], a[4], b[4];
        for (int k = 0; k < 4; k++) {
            a[k] = (unsigned char)((top >> (k * 8)) & 0xFF);
            b[k] = (unsigned char)((bot >> (k * 8)) & 0xFF);
            o[k] = (unsigned char)(a[k] + (int)((b[k] - a[k]) * t0));
        }
        PongColor band = (PongColor)o[0] | ((PongColor)o[1] << 8) |
                         ((PongColor)o[2] << 16) | ((PongColor)o[3] << 24);
        vita2d_draw_rectangle(vx(x), vy(y + h * t0), vs(w), vs(h * (t1 - t0)), v2d(band));
    }
}

void pong_gfx_circle(float cx, float cy, float r, PongColor c)
{
    vita2d_draw_fill_circle(vx(cx), vy(cy), vs(r), v2d(c));
}

/* ------------------------------------------------------------------- text */

static float glyph_px(float scale) { return (LINE_AT_SCALE_1 * scale) / 8.0f; }
static float line_width(size_t len, float scale)
{
    return (float)len * (float)GLYPH_ADVANCE_PX * glyph_px(scale);
}

static void draw_line(const char *s, size_t len, float x, float y,
                      float scale, PongColor c, int align)
{
    if (len == 0) return;
    float gp = glyph_px(scale);
    float adv = (float)GLYPH_ADVANCE_PX * gp;
    float w = line_width(len, scale);

    float sx = x;
    if (align == PONG_ALIGN_CENTER) sx = x - w * 0.5f;
    else if (align == PONG_ALIGN_RIGHT) sx = x - w;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < FONT8X8_FIRST || ch > FONT8X8_LAST) ch = '?';
        vita2d_draw_texture_tint_part_scale(
            s_font,
            vx(sx + (float)i * adv), vy(y),
            (float)(ch - FONT8X8_FIRST) * 8.0f, 0.0f, 8.0f, 8.0f,
            vs(gp), vs(gp), v2d(c));
    }
}

/* Line breaking is identical to the SDL3 backend on purpose: the two must
 * agree about where a string wraps, or a panel sized on one clips on the
 * other. Duplicated rather than shared only because each backend is meant to
 * be independently droppable. */
static void layout_text(const char *s, float scale, float wrap_w,
                        void (*emit)(const char *, size_t, int, void *), void *ctx)
{
    if (!s || !s[0]) return;
    float adv = (float)GLYPH_ADVANCE_PX * glyph_px(scale);
    int line = 0;
    const char *p = s;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t avail = nl ? (size_t)(nl - p) : strlen(p);

        if (wrap_w <= 0.0f || (float)avail * adv <= wrap_w) {
            emit(p, avail, line++, ctx);
        } else {
            size_t start = 0;
            while (start < avail) {
                size_t max_chars = (size_t)(wrap_w / adv);
                if (max_chars == 0) max_chars = 1;
                size_t take = (avail - start < max_chars) ? avail - start : max_chars;
                if (start + take < avail) {
                    size_t b = take;
                    while (b > 0 && p[start + b] != ' ') b--;
                    if (b > 0) take = b;
                }
                emit(p + start, take, line++, ctx);
                start += take;
                while (start < avail && p[start] == ' ') start++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

typedef struct { float x, y, scale, line_h; PongColor color; int align; } DrawCtx;

static void emit_draw(const char *s, size_t len, int line, void *vctx)
{
    DrawCtx *c = (DrawCtx *)vctx;
    draw_line(s, len, c->x, c->y + (float)line * c->line_h, c->scale, c->color, c->align);
}

typedef struct { float w, h, line_h, scale; } MeasCtx;

static void emit_measure(const char *s, size_t len, int line, void *vctx)
{
    (void)s;
    MeasCtx *m = (MeasCtx *)vctx;
    float w = line_width(len, m->scale);
    if (w > m->w) m->w = w;
    m->h = (float)(line + 1) * m->line_h;
}

void pong_gfx_text(float x, float y, float scale, PongColor c, int align, const char *s)
{
    pong_gfx_text_wrap(x, y, scale, c, align, 0.0f, s);
}

void pong_gfx_text_wrap(float x, float y, float scale, PongColor c, int align,
                        float wrap_w, const char *s)
{
    DrawCtx ctx = { x, y, scale, LINE_AT_SCALE_1 * scale, c, align };
    layout_text(s, scale, wrap_w, emit_draw, &ctx);
}

void pong_gfx_text_size(const char *s, float scale, float *w, float *h)
{
    MeasCtx m = { 0.0f, 0.0f, LINE_AT_SCALE_1 * scale, scale };
    layout_text(s, scale, 0.0f, emit_measure, &m);
    if (w) *w = m.w;
    if (h) *h = m.h;
}
