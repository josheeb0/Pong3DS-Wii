/*
 * PS Vita front end.
 *
 * NOT YET COMPILED -- see vita/Makefile. Mirrors pc/main_sdl.c: it supplies the
 * window, input and clock, and drives the same ui/render.c the console does.
 */

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <string.h>

#include "gfx.h"
#include "render.h"

int main(void)
{
    if (!pong_gfx_init("Pong")) return 1;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    PongHud hud;
    memset(&hud, 0, sizeof hud);
    hud.screen = SCREEN_TITLE;
    hud.my_name = "VITA PLAYER";
    hud.opp_name = "";
    hud.server_addr = "pong.wardcrew.com";

    PongView view;
    memset(&view, 0, sizeof view);

    SceCtrlData pad, prev;
    memset(&prev, 0, sizeof prev);

    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned pressed = pad.buttons & ~prev.buttons;
        prev = pad;

        if (pressed & SCE_CTRL_START) break;
        if (pressed & SCE_CTRL_DOWN) hud.menu_sel = (hud.menu_sel + 1) % MENU_COUNT;
        if (pressed & SCE_CTRL_UP)   hud.menu_sel = (hud.menu_sel + MENU_COUNT - 1) % MENU_COUNT;

        hud.frame++;

        pong_gfx_frame_begin();
        pong_render_frame(&view, &hud);
        pong_gfx_frame_end();
    }

    pong_gfx_exit();
    sceKernelExitProcess(0);
    return 0;
}
