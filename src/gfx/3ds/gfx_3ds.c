/*
 * citro2d backend.
 *
 * Nearly a direct mapping, which is the point: the seam was shaped around what
 * this hardware already did well, so the platform that has to be fast pays
 * almost nothing for the abstraction. PongColor is deliberately the same 32-bit
 * packing as C2D_Color32, so colours are a cast rather than a conversion.
 */

#include "gfx.h"

#include <3ds.h>
#include <citro2d.h>
#include <string.h>

static C3D_RenderTarget *s_target[PONG_SURFACE_COUNT];
static C2D_TextBuf s_buf;          /* cleared every frame */
static bool s_ready = false;

const char *pong_gfx_platform_name(void) { return "3ds"; }

void pong_gfx_output_size(int *w, int *h)
{
    /* The handheld UI never asks; reported for completeness. */
    if (w) *w = (int)PONG_TOP_W;
    if (h) *h = (int)PONG_TOP_H;
}

void pong_gfx_request_size(int w, int h)
{
    (void)w; (void)h;   /* the console's screens are its screens */
}

bool pong_gfx_screenshot(const char *path)
{
    /* Not implemented: the console has no framebuffer readback path worth the
     * code, and its screen can be photographed. Says so rather than pretending. */
    (void)path;
    return false;
}

bool pong_gfx_init(const char *title)
{
    (void)title;   /* the console has no window to name */
    if (s_ready) return true;

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    s_target[PONG_SURFACE_TOP]    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_target[PONG_SURFACE_BOTTOM] = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    /* Generous: every string drawn in a frame lives here until it is cleared,
     * and running out makes C2D_TextParse fail silently -- text simply stops
     * appearing, with nothing to indicate why. */
    s_buf = C2D_TextBufNew(4096);

    s_ready = true;
    return true;
}

void pong_gfx_exit(void)
{
    if (!s_ready) return;
    C2D_TextBufDelete(s_buf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    s_ready = false;
}

void pong_gfx_frame_begin(void)
{
    C2D_TextBufClear(s_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void pong_gfx_frame_end(void)
{
    C3D_FrameEnd(0);
}

void pong_gfx_surface_begin(PongSurface s, PongColor clear)
{
    C2D_TargetClear(s_target[s], (u32)clear);
    C2D_SceneBegin(s_target[s]);
}

void pong_gfx_rect(float x, float y, float w, float h, PongColor c)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, (u32)c);
}

void pong_gfx_rect_grad(float x, float y, float w, float h,
                        PongColor tl, PongColor tr, PongColor bl, PongColor br)
{
    C2D_DrawRectangle(x, y, 0.0f, w, h, (u32)tl, (u32)tr, (u32)bl, (u32)br);
}

void pong_gfx_circle(float cx, float cy, float r, PongColor c)
{
    C2D_DrawCircleSolid(cx, cy, 0.0f, r, (u32)c);
}

static u32 align_flags(int align)
{
    if (align == PONG_ALIGN_CENTER) return C2D_AlignCenter;
    if (align == PONG_ALIGN_RIGHT)  return C2D_AlignRight;
    return 0;
}

void pong_gfx_text(float x, float y, float scale, PongColor c, int align,
                   const char *s)
{
    if (!s || !s[0]) return;
    C2D_Text t;
    C2D_TextParse(&t, s_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, align_flags(align) | C2D_WithColor,
                 x, y, 0.5f, scale, scale, (u32)c);
}

void pong_gfx_text_wrap(float x, float y, float scale, PongColor c, int align,
                        float wrap_w, const char *s)
{
    if (!s || !s[0]) return;
    C2D_Text t;
    C2D_TextParse(&t, s_buf, s);
    C2D_TextOptimize(&t);
    /* The wrap width is a trailing vararg and must come AFTER the colour, per
     * C2D_DrawText's contract. Getting that wrong compiles and then draws
     * nothing, which is a miserable afternoon. */
    C2D_DrawText(&t, align_flags(align) | C2D_WithColor | C2D_WordWrap,
                 x, y, 0.5f, scale, scale, (u32)c, wrap_w);
}

void pong_gfx_text_size(const char *s, float scale, float *w, float *h)
{
    float tw = 0.0f, th = 0.0f;
    if (s && s[0]) {
        C2D_Text t;
        C2D_TextParse(&t, s_buf, s);
        C2D_TextOptimize(&t);
        C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
    }
    if (w) *w = tw;
    if (h) *h = th;
}
