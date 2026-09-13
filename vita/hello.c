/*
 * The smallest thing that proves the Vita build works.
 *
 * No game code, no shared UI, no stdio, no font atlas -- vita2d and the pad,
 * and nothing else. If this runs, the toolchain, the link line, the heap and
 * the packaging are all correct and any remaining fault is mine. If it does
 * not, the problem is underneath everything and there is no point debugging
 * the renderer.
 *
 * Three crashes went into the full build before it was worth admitting that
 * the platform layer itself had never been proven in isolation.
 */

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#define RGBA(r, g, b) RGBA8((r), (g), (b), 0xFF)

int main(void)
{
    vita2d_init();
    vita2d_set_clear_color(RGBA(0x10, 0x12, 0x16));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    SceCtrlData pad;
    unsigned frame = 0;

    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;

        vita2d_start_drawing();
        vita2d_clear_screen();

        /* A moving bar, so a frozen frame is distinguishable from a live one.
         * A static image cannot tell you whether the loop is running. */
        float x = (float)(frame % 900);
        vita2d_draw_rectangle(x, 240.0f, 60.0f, 60.0f, RGBA(0x7e, 0xe7, 0xff));

        /* Corner markers: if the screen is the wrong size or the origin is
         * somewhere unexpected, these say so immediately. */
        vita2d_draw_rectangle(0.0f, 0.0f, 40.0f, 40.0f, RGBA(0xff, 0x9d, 0xe2));
        vita2d_draw_rectangle(920.0f, 504.0f, 40.0f, 40.0f, RGBA(0x7d, 0xff, 0xa8));

        vita2d_end_drawing();
        vita2d_swap_buffers();
        frame++;
    }

    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
