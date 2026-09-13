/*
 * Geometry checks for the 3DS menu.
 *
 * This exists because the 3DS cannot be built or seen from every machine that
 * works on this project, and a layout mistake is invisible until someone runs
 * it on hardware. The bug that prompted it was reported as "the ui on 3ds is
 * really clunky now, overlapping text": five modes had been squeezed into 32px
 * rows, and citro2d's font is 30px at scale 1.0, so a 0.56 label ran to y+18.8
 * while the hint below it started at y+18.
 *
 * None of that is visible in a diff. It IS arithmetic, so it can be checked
 * here on any machine, with no devkitARM and no console.
 *
 * The table comes from src/ui/menu_rect.c -- the real one the console draws,
 * not a copy. A test holding its own copy of the numbers agrees with itself
 * forever, including after the layout changes.
 */

#include <stdio.h>
#include <stdbool.h>

#include "../../src/ui/render.h"

/*
 * citro2d's default font is 30 pixels tall at scale 1.0, so a line drawn at
 * `scale` occupies 30*scale downward from its y. Every spacing decision in the
 * menu depends on this number.
 */
#define LINE_PX(scale) (30.0f * (scale))

/* The scales render.c actually uses, and the offsets it draws at. */
#define LABEL_Y   5.0f
#define HINT_Y   24.0f
#define WIDE_LABEL_S   0.60f
#define WIDE_HINT_S    0.44f
#define NARROW_LABEL_S 0.55f
#define NARROW_HINT_S  0.40f

/* The bottom screen, and the header the menu must start below. */
#define SCREEN_W 320.0f
#define SCREEN_H 240.0f
#define HEADER_H  32.0f

/* What a thumb needs. This is what set the original row height. */
#define TOUCH_MIN 28.0f

static int fails = 0;

static void check(bool ok, const char *what, const char *detail)
{
    if (!ok) { printf("  FAIL %-44s %s\n", what, detail ? detail : ""); fails++; }
    else printf("  ok   %-44s %s\n", what, detail ? detail : "");
}

static bool overlap(const PongRect *a, const PongRect *b)
{
    return !(a->x + a->w <= b->x || b->x + b->w <= a->x ||
             a->y + a->h <= b->y || b->y + b->h <= a->y);
}

static const char *NAME[MENU_COUNT] = {
    [MENU_QUICK] = "QUICK", [MENU_ROOM] = "ROOM", [MENU_BOT] = "VS CPU",
    [MENU_LOCAL_AI] = "VS AI", [MENU_LOCAL_2P] = "2 PLAYERS",
    [MENU_NAME] = "NAME", [MENU_SERVER] = "SERVER",
    [MENU_SOURCE] = "SOURCE", [MENU_UPDATE] = "UPDATE",
};

int main(void)
{
    char d[128];

    printf("=== every button is on the screen, below the header ===\n");
    for (int i = 0; i < MENU_COUNT; i++) {
        const PongRect *r = &PONG_MENU_RECT[i];
        snprintf(d, sizeof d, "%.0f,%.0f %.0fx%.0f", r->x, r->y, r->w, r->h);
        check(r->x >= 0.0f && r->y >= HEADER_H &&
              r->x + r->w <= SCREEN_W && r->y + r->h <= SCREEN_H,
              NAME[i], d);
    }

    printf("\n=== no two buttons overlap ===\n");
    {
        int clashes = 0;
        for (int i = 0; i < MENU_COUNT; i++)
            for (int j = i + 1; j < MENU_COUNT; j++)
                if (overlap(&PONG_MENU_RECT[i], &PONG_MENU_RECT[j])) {
                    printf("  FAIL %s overlaps %s\n", NAME[i], NAME[j]);
                    clashes++;
                }
        check(clashes == 0, "all nine are disjoint", NULL);
    }

    printf("\n=== a thumb can hit them ===\n");
    for (int i = 0; i < MENU_COUNT; i++) {
        const PongRect *r = &PONG_MENU_RECT[i];
        snprintf(d, sizeof d, "%.0fx%.0f", r->w, r->h);
        check(r->h >= TOUCH_MIN && r->w >= 64.0f, NAME[i], d);
    }

    /*
     * The check the reported bug would have failed.
     *
     * A row that draws a label at LABEL_Y and a hint at HINT_Y needs the label
     * to END before the hint STARTS, and the hint to end inside the row.
     */
    printf("\n=== the two text lines cannot collide ===\n");
    for (int i = 0; i <= MENU_LOCAL_2P; i++) {
        const PongRect *r = &PONG_MENU_RECT[i];
        bool wide = (r->w > 200.0f);
        float label_bottom = LABEL_Y + LINE_PX(wide ? WIDE_LABEL_S : NARROW_LABEL_S);
        float hint_bottom  = HINT_Y  + LINE_PX(wide ? WIDE_HINT_S  : NARROW_HINT_S);

        snprintf(d, sizeof d, "label ends %.1f, hint starts %.1f, ends %.1f in %.0f",
                 label_bottom, HINT_Y, hint_bottom, r->h);
        check(label_bottom <= HINT_Y && hint_bottom <= r->h, NAME[i], d);
    }

    /*
     * The utility row's single centred label, at y+7 scale 0.5.
     *
     * The two-line check below only ever looked at the primary rows, so nothing
     * verified that NAME/SERVER/SOURCE/UPDATE could hold their own text.
     */
    printf("\n=== a single label fits its button ===\n");
    for (int i = MENU_LOCAL_2P + 1; i < MENU_COUNT; i++) {
        const PongRect *r = &PONG_MENU_RECT[i];
        float bottom = 7.0f + LINE_PX(0.5f);
        snprintf(d, sizeof d, "text ends %.1f in %.0f", bottom, r->h);
        check(bottom <= r->h, NAME[i], d);
    }

    /*
     * Nothing may reach the status line.
     *
     * This is the check that was missing when the utility row was moved down
     * and grown to 28px: it ended at 218 while the server address starts at
     * 213, so the address -- the one line you need when a connection fails --
     * had buttons drawn through it. Reported as "the bottom text where it says
     * server and stuff slightly overlaps with the bottom of the buttons".
     */
    printf("\n=== nothing overlaps the status line ===\n");
    for (int i = 0; i < MENU_COUNT; i++) {
        const PongRect *r = &PONG_MENU_RECT[i];
        snprintf(d, sizeof d, "ends %.0f, status starts %.0f",
                 r->y + r->h, PONG_MENU_STATUS_Y);
        check(r->y + r->h <= PONG_MENU_STATUS_Y, NAME[i], d);
    }

    printf("\n=== the menu leaves the header alone ===\n");
    {
        float top = SCREEN_H;
        for (int i = 0; i < MENU_COUNT; i++)
            if (PONG_MENU_RECT[i].y < top) top = PONG_MENU_RECT[i].y;
        snprintf(d, sizeof d, "topmost button at y=%.0f, header ends %.0f", top, HEADER_H);
        check(top >= HEADER_H, "nothing sits under the title bar", d);
    }

    printf("\n");
    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASSED: 3DS menu layout\n");
    return 0;
}
