/*
 * The renderer seam.
 *
 * Everything the game draws goes through this handful of calls, so the UI is
 * written once and each platform supplies a backend. The surface is small on
 * purpose -- rectangles, a gradient, a circle and text -- because that is
 * genuinely all Pong needs, and a small seam is one that three backends can
 * actually agree on.
 *
 * TWO LOGICAL SURFACES, ALWAYS.
 *
 * The 3DS has two physical screens, 400x240 and 320x240, and the whole
 * interface is laid out in those coordinates. Rather than rewrite that for
 * hardware with one screen, every backend presents both surfaces and decides
 * for itself where they go: the 3DS maps them to its two panels, while the Vita
 * and a PC window place them side by side or stacked and scale to fit.
 *
 * The alternative -- a single canvas with per-platform layout -- would mean the
 * UI code carrying a branch for every target, which is the thing this exists to
 * avoid.
 *
 * COORDINATES are surface pixels, origin top-left, and are the same numbers on
 * every platform. A backend's scaling is its own business.
 */

#ifndef PONG_GFX_H
#define PONG_GFX_H

#include <stdint.h>
#include <stdbool.h>

/** 0xAABBGGRR, matching citro2d's packing so the 3DS backend is a cast. */
typedef uint32_t PongColor;

static inline PongColor pong_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (PongColor)r | ((PongColor)g << 8) | ((PongColor)b << 16) | ((PongColor)a << 24);
}

typedef enum {
    PONG_SURFACE_TOP = 0,    /* 400x240 -- the handheld playfield */
    PONG_SURFACE_BOTTOM,     /* 320x240 -- handheld menu, status, touch */
    /*
     * The whole output, in real pixels.
     *
     * For platforms whose interface is not a pair of handheld screens. A
     * desktop window is not a 3DS and should not pretend to be one, any more
     * than the browser client does -- it gets one canvas and lays itself out
     * for the space it actually has. Use pong_gfx_output_size() for the extent.
     */
    PONG_SURFACE_FULL,
    PONG_SURFACE_COUNT
} PongSurface;

#define PONG_TOP_W     400.0f
#define PONG_TOP_H     240.0f
#define PONG_BOTTOM_W  320.0f
#define PONG_BOTTOM_H  240.0f

/* Text alignment, mirroring what the 3DS layout already used. */
#define PONG_ALIGN_LEFT    0
#define PONG_ALIGN_CENTER  1
#define PONG_ALIGN_RIGHT   2

/**
 * Brings up a window or the console's screens.
 *
 * `title` is used where a window has one and ignored elsewhere.
 */
bool pong_gfx_init(const char *title);

/**
 * Requests a specific output size, where the backend has one to give.
 *
 * Only the windowed backends honour it; a console's screen is its screen. Used
 * to render the other platforms' layouts for documentation, since the layout
 * rule is shared and therefore genuinely previewable.
 */
void pong_gfx_request_size(int w, int h);

/**
 * Tells the backend a system dialog is on screen.
 *
 * Some platforms composite their own keyboard over the frame and need to be
 * given the chance each frame -- the Vita will simply never draw it otherwise,
 * which looks exactly like the application hanging. Platforms with nothing to
 * do here ignore it.
 */
void pong_gfx_system_dialog(bool active);

/** Real pixel extent of the output, for PONG_SURFACE_FULL layout. */
void pong_gfx_output_size(int *w, int *h);

/**
 * Turns text entry on or off.
 *
 * Through the seam because SDL3 starts text input against a specific WINDOW,
 * and the window belongs to the backend -- the front end has no handle to pass
 * and passing NULL silently does nothing, which is exactly the bug this
 * replaces: typing into the server and name fields produced no characters on
 * any desktop platform, with no error to notice.
 *
 * Returns false where the backend has no such concept, which the consoles do
 * not: they open their own system keyboard instead.
 */
bool pong_gfx_text_input(bool enabled);
void pong_gfx_exit(void);

/** One frame, both surfaces. Draw between these. */
void pong_gfx_frame_begin(void);
void pong_gfx_frame_end(void);

/**
 * Selects a surface and clears it, in one call.
 *
 * One call rather than two because citro2d clears a target rather than a
 * scene, so the two are inseparable there; splitting them would have given the
 * 3DS backend an API it could not implement honestly. Each surface is drawn
 * exactly once per frame, so nothing is lost.
 */
void pong_gfx_surface_begin(PongSurface s, PongColor clear);

void pong_gfx_rect(float x, float y, float w, float h, PongColor c);

/** Corner colours, in the order top-left, top-right, bottom-left, bottom-right. */
void pong_gfx_rect_grad(float x, float y, float w, float h,
                        PongColor tl, PongColor tr, PongColor bl, PongColor br);

void pong_gfx_circle(float cx, float cy, float r, PongColor c);

/**
 * Draws one line of text.
 *
 * `scale` is in the 3DS's units, where 1.0 is roughly a 30px line, because that
 * is what every existing call site is written in. A backend whose font differs
 * is expected to match that height rather than reinterpret the number.
 */
void pong_gfx_text(float x, float y, float scale, PongColor c, int align,
                   const char *s);

/** Same, wrapped to `wrap_w` pixels. Newlines in `s` are honoured either way. */
void pong_gfx_text_wrap(float x, float y, float scale, PongColor c, int align,
                        float wrap_w, const char *s);

/** Measures without drawing, so a panel can be sized from its text. */
void pong_gfx_text_size(const char *s, float scale, float *w, float *h);

/**
 * What this build is running on: "3ds", "vita", "pc".
 *
 * The interface says "you are currently on 3ds" under the title, which was a
 * literal until there was more than one answer. Belongs to the backend because
 * the backend is the only thing that knows.
 */
/**
 * Fullscreen, on the platforms that have a concept of it.
 *
 * A console IS its screen. The 3DS and the Vita have nothing to toggle, so
 * they report false here and the menu drops the row entirely rather than
 * offering a control that cannot do anything -- an option that visibly does
 * nothing is worse than no option.
 */
bool pong_gfx_fullscreen_supported(void);

/** True when the window is currently fullscreen. False where unsupported. */
bool pong_gfx_fullscreen_get(void);

/** No-op where unsupported, so callers need not guard every use. */
void pong_gfx_fullscreen_set(bool on);

const char *pong_gfx_platform_name(void);

/**
 * Saves the current frame to a file, if the backend can.
 *
 * Exists so a renderer change can be inspected rather than taken on trust --
 * "it compiles" says nothing about whether anything appeared on screen, and on
 * a console the feedback loop is a person carrying hardware to a computer.
 * Returns false where a backend has no way to do it.
 */
bool pong_gfx_screenshot(const char *path);

#endif /* PONG_GFX_H */
