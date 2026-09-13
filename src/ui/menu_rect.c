#include "render.h"

/*
 * Five ways to start a match on a 320x240 screen, without the rows getting so
 * short that the two lines of text collide.
 *
 * They did collide. Stacking five 32px rows put the label at y+2 and the hint
 * at y+18, and citro2d's font is 30px at scale 1.0 -- so a 0.56 label runs to
 * y+18.8 and lands on top of a hint that starts at y+18. The rows have to be
 * about 44px for two readable lines, and five of those plus the utility strip
 * does not fit in 200px.
 *
 * So the secondary modes go side by side. QUICK MATCH keeps the full width
 * because it is the thing most people press; the other four pair off into two
 * rows. Every button is at least 44px tall and at least 146 wide, both well
 * past what a thumb needs.
 */
const PongRect PONG_MENU_RECT[MENU_COUNT] = {
    [MENU_QUICK]    = {  10.0f,  36.0f, 300.0f, 46.0f },

    [MENU_ROOM]     = {  10.0f,  88.0f, 146.0f, 44.0f },
    [MENU_BOT]      = { 164.0f,  88.0f, 146.0f, 44.0f },

    [MENU_LOCAL_AI] = {  10.0f, 138.0f, 146.0f, 44.0f },
    [MENU_LOCAL_2P] = { 164.0f, 138.0f, 146.0f, 44.0f },

    /* Four utility buttons across 300px: 72 wide with 4px gaps. */
    [MENU_NAME]   = {  10.0f, 190.0f,  72.0f, 28.0f },
    [MENU_SERVER] = {  86.0f, 190.0f,  72.0f, 28.0f },
    [MENU_SOURCE] = { 162.0f, 190.0f,  72.0f, 28.0f },
    [MENU_UPDATE] = { 238.0f, 190.0f,  72.0f, 28.0f },
};

/*
 * In its own file so 3ds/test/test_menu_layout.c can link THIS table rather
 * than a copy of the numbers. render.c cannot be linked into a host test
 * without stubbing eight graphics entry points, and a test that re-declares
 * the geometry it is checking agrees with itself forever -- including after
 * the real layout changes. The same reason updatetarget.c was split out.
 */
