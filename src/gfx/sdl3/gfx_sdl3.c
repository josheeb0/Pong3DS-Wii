/*
 * SDL3 backend, for PC.
 *
 * TWO SURFACES ON ONE WINDOW.
 *
 * The console has two screens; a window has one. Rather than pick an
 * arrangement and hope, both are measured and the one that makes the playfield
 * larger wins: side by side (720x240 together) on anything wide, stacked
 * (400x480) on anything tall. On a 16:9 window side-by-side wins comfortably,
 * and on a portrait window stacked does -- which is the right answer in each
 * case without a special case for either.
 *
 * TEXT is an embedded 8x8 bitmap font rather than SDL_ttf. It has to look the
 * same here, on the Vita and on the console, and two font rasterisers do not
 * agree about metrics -- a layout that fits on one would clip on another. The
 * glyphs are uploaded once as an atlas and drawn as scaled quads.
 */

#include "gfx.h"
#include "font8x8.h"

#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>

/* A 3DS scale of 1.0 is about a 30px line. 24 here: a PC window is bigger and
 * further away, and matching 30 exactly made every panel overflow, since this
 * font is fixed-width where the console's is proportional. */
#define LINE_AT_SCALE_1  24.0f
#define GLYPH_ADVANCE_PX 6      /* of the 8 columns; these glyphs sit left */

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_font;     /* 95 glyphs in a row, 8px each */

typedef struct { float x, y, w, h, scale; } Viewport;
static Viewport s_vp[PONG_SURFACE_COUNT];
static PongSurface s_current = PONG_SURFACE_TOP;

const char *pong_gfx_platform_name(void) { return "pc"; }

/* Surface coords -> window coords. */
static inline float vx(float x) { return s_vp[s_current].x + x * s_vp[s_current].scale; }
static inline float vy(float y) { return s_vp[s_current].y + y * s_vp[s_current].scale; }
static inline float vs(float v) { return v * s_vp[s_current].scale; }

static void set_color(PongColor c)
{
    SDL_SetRenderDrawColor(s_ren,
                           (Uint8)(c & 0xFF), (Uint8)((c >> 8) & 0xFF),
                           (Uint8)((c >> 16) & 0xFF), (Uint8)((c >> 24) & 0xFF));
}

/*
 * Places both surfaces in the window.
 *
 * Called on resize as well as at startup, so the window can be dragged about
 * without the layout going stale.
 */
static void layout(void)
{
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(s_ren, &w, &h);
    if (w <= 0 || h <= 0) return;

    const float gap = 8.0f;

    /* Side by side, tops aligned. */
    float side_w = PONG_TOP_W + gap + PONG_BOTTOM_W;
    float side_h = (PONG_TOP_H > PONG_BOTTOM_H) ? PONG_TOP_H : PONG_BOTTOM_H;
    float side_scale = SDL_min((float)w / side_w, (float)h / side_h);

    /* Stacked, centred horizontally. */
    float stack_w = (PONG_TOP_W > PONG_BOTTOM_W) ? PONG_TOP_W : PONG_BOTTOM_W;
    float stack_h = PONG_TOP_H + gap + PONG_BOTTOM_H;
    float stack_scale = SDL_min((float)w / stack_w, (float)h / stack_h);

    /* FULL is the window itself: no offset, no scale, real pixels. */
    s_vp[PONG_SURFACE_FULL] = (Viewport){ 0.0f, 0.0f, (float)w, (float)h, 1.0f };

    if (side_scale >= stack_scale) {
        float sc = side_scale;
        float total_w = side_w * sc, total_h = side_h * sc;
        float ox = ((float)w - total_w) * 0.5f, oy = ((float)h - total_h) * 0.5f;
        s_vp[PONG_SURFACE_TOP]    = (Viewport){ ox, oy, PONG_TOP_W * sc, PONG_TOP_H * sc, sc };
        s_vp[PONG_SURFACE_BOTTOM] = (Viewport){ ox + (PONG_TOP_W + gap) * sc, oy,
                                                PONG_BOTTOM_W * sc, PONG_BOTTOM_H * sc, sc };
    } else {
        float sc = stack_scale;
        float total_w = stack_w * sc, total_h = stack_h * sc;
        float ox = ((float)w - total_w) * 0.5f, oy = ((float)h - total_h) * 0.5f;
        s_vp[PONG_SURFACE_TOP]    = (Viewport){ ox + (stack_w - PONG_TOP_W) * 0.5f * sc, oy,
                                                PONG_TOP_W * sc, PONG_TOP_H * sc, sc };
        s_vp[PONG_SURFACE_BOTTOM] = (Viewport){ ox + (stack_w - PONG_BOTTOM_W) * 0.5f * sc,
                                                oy + (PONG_TOP_H + gap) * sc,
                                                PONG_BOTTOM_W * sc, PONG_BOTTOM_H * sc, sc };
    }
}

/** Builds the glyph atlas: 95 glyphs side by side, white on transparent. */
static bool build_font(void)
{
    const int n = FONT8X8_LAST - FONT8X8_FIRST + 1;
    const int W = n * 8, H = 8;

    SDL_Surface *surf = SDL_CreateSurface(W, H, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return false;

    Uint32 *px = (Uint32 *)surf->pixels;
    const int pitch = surf->pitch / 4;
    memset(px, 0, (size_t)surf->pitch * (size_t)H);

    for (int g = 0; g < n; g++) {
        for (int row = 0; row < 8; row++) {
            uint8_t bits = FONT8X8[g][row];
            for (int col = 0; col < 8; col++) {
                if (bits & (1 << col)) px[row * pitch + g * 8 + col] = 0xFFFFFFFFu;
            }
        }
    }

    s_font = SDL_CreateTextureFromSurface(s_ren, surf);
    SDL_DestroySurface(surf);
    if (!s_font) return false;
    /* Nearest, deliberately: this is an 8x8 bitmap face and smoothing it makes
     * small text mushy rather than smooth. */
    SDL_SetTextureScaleMode(s_font, SDL_SCALEMODE_NEAREST);
    return true;
}

static int s_req_w = 1280, s_req_h = 560;

bool pong_gfx_text_input(bool enabled)
{
    if (!s_win) return false;
    /* The window argument is the whole point: SDL3 scopes text input to a
     * window, and NULL is accepted and does nothing. */
    return enabled ? SDL_StartTextInput(s_win) : SDL_StopTextInput(s_win);
}

void pong_gfx_system_dialog(bool active)
{
    (void)active;   /* SDL composites its own text input */
}

void pong_gfx_output_size(int *w, int *h)
{
    int ow = 0, oh = 0;
    if (s_ren) SDL_GetRenderOutputSize(s_ren, &ow, &oh);
    if (w) *w = ow;
    if (h) *h = oh;
}

void pong_gfx_request_size(int w, int h)
{
    if (w > 0 && h > 0) { s_req_w = w; s_req_h = h; }
    if (s_win) { SDL_SetWindowSize(s_win, s_req_w, s_req_h); layout(); }
}

bool pong_gfx_init(const char *title)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_CreateWindowAndRenderer(title ? title : "Pong", s_req_w, s_req_h,
                                     SDL_WINDOW_RESIZABLE, &s_win, &s_ren)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_BLEND);
    /* Vsync on: the simulation is 60Hz and the interpolator is written around
     * that, so rendering at 122fps spends the machine to redraw states that
     * have not changed. */
    SDL_SetRenderVSync(s_ren, 1);
    if (!build_font()) {
        fprintf(stderr, "font atlas: %s\n", SDL_GetError());
        return false;
    }
    layout();
    return true;
}

void pong_gfx_exit(void)
{
    if (s_font) SDL_DestroyTexture(s_font);
    if (s_ren)  SDL_DestroyRenderer(s_ren);
    if (s_win)  SDL_DestroyWindow(s_win);
    SDL_Quit();
}

void pong_gfx_frame_begin(void)
{
    layout();   /* cheap, and keeps a resized window correct */
    /* The letterbox around the surfaces. Distinct from the field colour so it
     * reads as "outside the console" rather than as part of the game. */
    SDL_SetRenderDrawColor(s_ren, 0x10, 0x12, 0x16, 0xFF);
    SDL_RenderClear(s_ren);
}

void pong_gfx_frame_end(void)
{
    SDL_RenderPresent(s_ren);
}

bool pong_gfx_screenshot(const char *path)
{
    /* Read back before the present, strictly speaking, but SDL keeps the
     * target readable and this is a diagnostic rather than a capture pipeline. */
    SDL_Surface *shot = SDL_RenderReadPixels(s_ren, NULL);
    if (!shot) {
        fprintf(stderr, "screenshot: %s\n", SDL_GetError());
        return false;
    }
    bool ok = SDL_SaveBMP(shot, path);
    SDL_DestroySurface(shot);
    return ok;
}

void pong_gfx_surface_begin(PongSurface s, PongColor clear)
{
    s_current = s;
    /* Clipped so a surface cannot draw over its neighbour -- on the console
     * that is guaranteed by there being two screens, and here it is not. */
    SDL_Rect clip = { (int)s_vp[s].x, (int)s_vp[s].y, (int)s_vp[s].w, (int)s_vp[s].h };
    SDL_SetRenderClipRect(s_ren, &clip);
    set_color(clear);
    SDL_FRect r = { s_vp[s].x, s_vp[s].y, s_vp[s].w, s_vp[s].h };
    SDL_RenderFillRect(s_ren, &r);
}

void pong_gfx_rect(float x, float y, float w, float h, PongColor c)
{
    set_color(c);
    SDL_FRect r = { vx(x), vy(y), vs(w), vs(h) };
    SDL_RenderFillRect(s_ren, &r);
}

void pong_gfx_rect_grad(float x, float y, float w, float h,
                        PongColor tl, PongColor tr, PongColor bl, PongColor br)
{
    /* Real per-vertex interpolation via RenderGeometry, rather than banding it
     * into strips: the gradients here are subtle and banding would be the only
     * thing anyone noticed about them. */
    const float x0 = vx(x), y0 = vy(y), x1 = vx(x + w), y1 = vy(y + h);
    PongColor cols[4] = { tl, tr, bl, br };
    float xs[4] = { x0, x1, x0, x1 };
    float ys[4] = { y0, y0, y1, y1 };

    SDL_Vertex v[4];
    for (int i = 0; i < 4; i++) {
        v[i].position.x = xs[i];
        v[i].position.y = ys[i];
        v[i].color.r = (float)( cols[i]        & 0xFF) / 255.0f;
        v[i].color.g = (float)((cols[i] >> 8)  & 0xFF) / 255.0f;
        v[i].color.b = (float)((cols[i] >> 16) & 0xFF) / 255.0f;
        v[i].color.a = (float)((cols[i] >> 24) & 0xFF) / 255.0f;
        v[i].tex_coord.x = 0.0f;
        v[i].tex_coord.y = 0.0f;
    }
    const int idx[6] = { 0, 1, 2, 1, 3, 2 };
    SDL_RenderGeometry(s_ren, NULL, v, 4, idx, 6);
}

void pong_gfx_circle(float cx, float cy, float r, PongColor c)
{
    /* A fan. SDL has no circle primitive, and the ball is the one thing on
     * screen whose roundness anyone will look at. */
    enum { SEGMENTS = 24 };
    SDL_Vertex v[SEGMENTS + 1];
    int idx[SEGMENTS * 3];

    const float R = (float)((c      ) & 0xFF) / 255.0f;
    const float G = (float)((c >> 8 ) & 0xFF) / 255.0f;
    const float B = (float)((c >> 16) & 0xFF) / 255.0f;
    const float A = (float)((c >> 24) & 0xFF) / 255.0f;

    v[0].position.x = vx(cx);
    v[0].position.y = vy(cy);
    v[0].color.r = R; v[0].color.g = G; v[0].color.b = B; v[0].color.a = A;
    v[0].tex_coord.x = v[0].tex_coord.y = 0.0f;

    for (int i = 0; i < SEGMENTS; i++) {
        float a = (float)i / (float)SEGMENTS * 6.28318530718f;
        v[i + 1].position.x = vx(cx + SDL_cosf(a) * r);
        v[i + 1].position.y = vy(cy + SDL_sinf(a) * r);
        v[i + 1].color.r = R; v[i + 1].color.g = G;
        v[i + 1].color.b = B; v[i + 1].color.a = A;
        v[i + 1].tex_coord.x = v[i + 1].tex_coord.y = 0.0f;

        idx[i * 3 + 0] = 0;
        idx[i * 3 + 1] = i + 1;
        idx[i * 3 + 2] = (i + 1) % SEGMENTS + 1;
    }
    SDL_RenderGeometry(s_ren, NULL, v, SEGMENTS + 1, idx, SEGMENTS * 3);
}

/* ------------------------------------------------------------------- text */

static float glyph_px(float scale) { return (LINE_AT_SCALE_1 * scale) / 8.0f; }

/* Width of one line, in surface pixels. Fixed-width font, so the characters
 * themselves do not matter -- only how many there are. */
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

    SDL_SetTextureColorMod(s_font, (Uint8)(c & 0xFF), (Uint8)((c >> 8) & 0xFF),
                           (Uint8)((c >> 16) & 0xFF));
    SDL_SetTextureAlphaMod(s_font, (Uint8)((c >> 24) & 0xFF));

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < FONT8X8_FIRST || ch > FONT8X8_LAST) ch = '?';
        SDL_FRect src = { (float)(ch - FONT8X8_FIRST) * 8.0f, 0.0f, 8.0f, 8.0f };
        SDL_FRect dst = { vx(sx + (float)i * adv), vy(y), vs(8.0f * gp), vs(8.0f * gp) };
        SDL_RenderTexture(s_ren, s_font, &src, &dst);
    }
}

/*
 * Lays a string out into lines, honouring newlines and an optional wrap width,
 * and hands each line to `emit`. Shared so that drawing and measuring cannot
 * disagree about where the breaks fall -- which they would, written twice.
 */
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
                /* Break on a space when there is one, so words stay whole. */
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

typedef struct {
    float x, y, scale, line_h;
    PongColor color;
    int align;
} DrawCtx;

static void emit_draw(const char *s, size_t len, int line, void *vctx)
{
    DrawCtx *c = (DrawCtx *)vctx;
    draw_line(s, len, c->x, c->y + (float)line * c->line_h, c->scale, c->color, c->align);
}

typedef struct { float w, h, line_h, scale; } MeasCtx;

static void emit_measure(const char *s, size_t len, int line, void *vctx)
{
    (void)s;   /* fixed width: only the count matters */
    MeasCtx *m = (MeasCtx *)vctx;
    float w = line_width(len, m->scale);
    if (w > m->w) m->w = w;
    m->h = (float)(line + 1) * m->line_h;
}

void pong_gfx_text(float x, float y, float scale, PongColor c, int align,
                   const char *s)
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
