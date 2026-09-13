/*
 * PS Vita front end.
 *
 * Uses the DESKTOP interface, not the handheld one. The Vita has a single
 * 960x544 screen, so emulating the 3DS's two panels would waste most of it --
 * the same reasoning that gives the browser and the PC their own layouts.
 *
 * What this file supplies is what a platform owns: a window, input, a clock, a
 * transport and a keyboard. Everything else -- the protocol, the simulation,
 * the netcode, the interface -- is the code already running on four other
 * platforms.
 */

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "desktop.h"
#include "pong_local.h"
#include "net_pc.h"
#include "ime_vita.h"
#include "client.h"
#include "pong_proto.h"

#define RX_CHUNK       2048
#define INPUT_HZ       60
#define PING_EVERY_MS  2000
#define CFG_PATH       "ux0:data/pong_vita.cfg"
#define LOG_PATH       "ux0:data/pong_vita.log"

/* The front touchscreen reports in its own units, not pixels. */
#define TOUCH_MAX_X 1920.0f
#define TOUCH_MAX_Y 1088.0f

typedef struct {
    PongNetPC  *net;
    PongClient  client;
    DeskHud     hud;
    PongView    view;

    char        server[64];
    uint16_t    port;
    char        name[17];
    char        room[8];

    uint8_t     acc[RX_CHUNK * 2];
    size_t      acc_len;
    uint32_t    input_seq;
    uint32_t    last_input_ms, last_ping_ms;

    char        error[192];
    char        toast[192];

    int         editing;        /* which DeskItem is being typed into, or -1 */
    char        edit_buf[128];

    uint32_t    fps, fps_count, fps_window;

    /* A match played on this Vita. When active it is the authority and the
     * network client is not consulted. */
    PongLocal   local;
    bool        in_local;
    int32_t     p2_target;
} App;

static void vlog(const char *msg)
{
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

/** Milliseconds since boot. The clock only has to be monotonic and fine. */
static uint32_t now_ms(void)
{
    SceRtcTick t;
    sceRtcGetCurrentTick(&t);
    return (uint32_t)(t.tick / 1000u);
}

/* ------------------------------------------------------------------ config */

static void cfg_load(App *a)
{
    snprintf(a->server, sizeof a->server, "192.168.4.29");
    a->port = 8787;
    snprintf(a->name, sizeof a->name, "VITA PLAYER");

    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = '\0';
        if      (!strcmp(line, "server")) snprintf(a->server, sizeof a->server, "%s", v);
        else if (!strcmp(line, "port"))   a->port = (uint16_t)atoi(v);
        else if (!strcmp(line, "name"))   snprintf(a->name, sizeof a->name, "%s", v);
    }
    fclose(f);
}

static void cfg_save(const App *a)
{
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "server=%s\nport=%u\nname=%s\n", a->server, (unsigned)a->port, a->name);
    fclose(f);
}

/* ----------------------------------------------------------------- network */

static bool send_frame(App *a, const uint8_t *buf, size_t n)
{
    return a->net && pong_pc_send(a->net, buf, n);
}

static void send_hello(App *a)
{
    PongHELLO h;
    memset(&h, 0, sizeof h);
    h.platform  = PONG_PLATFORM_VITA;
    h.transport = PONG_TRANSPORT_KIND_TCP;
    h.build_id  = 1;
    pong_pad_bytes(h.name, sizeof h.name, a->name);

    uint8_t buf[64];
    send_frame(a, buf, pong_write_hello(buf, sizeof buf, 1, &h));
}

static void send_join(App *a, uint8_t mode)
{
    PongJOIN j;
    memset(&j, 0, sizeof j);
    j.mode = mode;
    pong_pad_bytes(j.room_code, sizeof j.room_code, a->room);

    uint8_t buf[64];
    send_frame(a, buf, pong_write_join(buf, sizeof buf, 2, &j));
}

static void begin_connect(App *a, uint8_t mode)
{
    if (a->net) { pong_pc_close(a->net); a->net = NULL; }
    pong_client_init(&a->client);
    a->acc_len = 0;

    char err[160];
    a->hud.screen = DESK_CONNECTING;

    a->net = pong_pc_connect(a->server, a->port, 3000, err, sizeof err);
    if (!a->net) {
        snprintf(a->error, sizeof a->error, "%s", err);
        vlog(err);
        a->hud.screen = DESK_ERROR;
        return;
    }
    a->toast[0] = '\0';
    send_hello(a);
    send_join(a, mode);
    a->hud.screen = DESK_WAITING;
}

static void leave_match(App *a)
{
    if (a->net) { pong_pc_close(a->net); a->net = NULL; }
    a->acc_len = 0;
    a->hud.screen = DESK_MENU;
    a->toast[0] = '\0';
}

static void pump_network(App *a, uint32_t now)
{
    if (!a->net) return;

    uint8_t chunk[RX_CHUNK];
    int got = pong_pc_recv(a->net, chunk, sizeof chunk);
    if (got < 0) {
        snprintf(a->error, sizeof a->error, "disconnected");
        leave_match(a);
        a->hud.screen = DESK_ERROR;
        return;
    }
    if (got > 0) {
        size_t space = sizeof a->acc - a->acc_len;
        size_t take = ((size_t)got < space) ? (size_t)got : space;
        memcpy(a->acc + a->acc_len, chunk, take);
        a->acc_len += take;
    }

    size_t off = 0;
    while (off < a->acc_len) {
        PongFrame fr;
        size_t used = 0;
        PongParseResult r = pong_parse_frame(a->acc + off, a->acc_len - off, &fr, &used);
        if (r == PONG_PARSE_NEED_MORE) break;
        if (r != PONG_PARSE_OK) {
            snprintf(a->error, sizeof a->error, "protocol desync (%d)", (int)r);
            a->acc_len = 0;
            return;
        }

        switch (fr.type) {
        case PONG_MSG_SNAPSHOT: {
            PongSNAPSHOT s;
            if (pong_read_snapshot(fr.payload, fr.length, &s)) {
                pong_client_on_snapshot(&a->client, &s, now);
                if (a->hud.screen == DESK_WAITING) a->hud.screen = DESK_PLAY;
                if (s.state == PONG_MATCH_STATE_GAME_OVER) a->hud.screen = DESK_GAMEOVER;
            }
            break;
        }
        case PONG_MSG_MATCH_START: {
            PongMATCH_START m;
            if (pong_read_match_start(fr.payload, fr.length, &m)) {
                pong_client_on_match_start(&a->client, &m);
                a->hud.screen = DESK_PLAY;
                a->toast[0] = '\0';
            }
            break;
        }
        case PONG_MSG_PONG: {
            PongPONG p;
            if (pong_read_pong(fr.payload, fr.length, &p)) {
                pong_client_on_pong(&a->client, &p, now);
            }
            break;
        }
        case PONG_MSG_EVENT: {
            PongEVENT e;
            if (pong_read_event(fr.payload, fr.length, &e) &&
                e.kind == PONG_EVENT_KIND_OPP_LEFT) {
                snprintf(a->toast, sizeof a->toast, "opponent left");
            }
            break;
        }
        case PONG_MSG_BYE: {
            PongBYE b;
            if (pong_read_bye(fr.payload, fr.length, &b)) {
                snprintf(a->error, sizeof a->error, "disconnected (code %u)", b.code);
                leave_match(a);
                a->hud.screen = DESK_ERROR;
            }
            break;
        }
        default: break;
        }
        off += used;
    }

    if (off > 0) {
        memmove(a->acc, a->acc + off, a->acc_len - off);
        a->acc_len -= off;
    }
}

static void send_input(App *a, uint32_t now)
{
    if (!a->net) return;
    if (now - a->last_input_ms < 1000 / INPUT_HZ) return;
    a->last_input_ms = now;

    PongINPUT in;
    memset(&in, 0, sizeof in);
    in.input_seq      = ++a->input_seq;
    in.last_tick_seen = a->client.newest_tick;
    in.desired_yq4    = (int16_t)a->client.target_y;

    uint8_t buf[64];
    send_frame(a, buf, pong_write_input(buf, sizeof buf,
                                        (uint16_t)(a->input_seq & 0xffff), &in));

    if (now - a->last_ping_ms >= PING_EVERY_MS) {
        a->last_ping_ms = now;
        PongPING p;
        memset(&p, 0, sizeof p);
        p.client_time_ms = now;
        send_frame(a, buf, pong_write_ping(buf, sizeof buf, 0, &p));
    }
}

/* ------------------------------------------------------------------- input */

/* Online a target goes to the network client; locally it goes into the
 * simulation. Routed once so the stick, the d-pad and the touch screen do not
 * each have to know which kind of match is running. */
static void set_my_target(App *a, int32_t t)
{
    if (t < 0) t = 0;
    if (t > PONG_FIELD_H_Q4) t = PONG_FIELD_H_Q4;
    if (a->in_local) pong_local_set_target(&a->local, 0, t);
    else pong_client_set_target(&a->client, t);
}

static int32_t my_target(const App *a)
{
    return a->in_local ? a->local.target_l : a->client.target_y;
}

static void nudge_paddle(App *a, float dir, float dt)
{
    int32_t step = (int32_t)(dir * (float)PONG_MAX_PADDLE_SPEED_Q4 * 60.0f * dt);
    set_my_target(a, my_target(a) + step);
}

/*
 * Starts a match on this device.
 *
 * The seed is the clock, so consecutive matches do not open with the same
 * serve. The local player is always the left paddle, which is why the renderer
 * needs no special case for offline play.
 */
static void begin_local(App *a, PongLocalMode mode)
{
    pong_local_start(&a->local, mode, (PongBotLevel)a->hud.ai_level,
                     now_ms() | 1u, PONG_WIN_SCORE);
    a->in_local = true;
    a->p2_target = PONG_FIELD_H_Q4 / 2;
    a->hud.screen = DESK_PLAY;
    a->hud.my_side = 0;
    a->hud.opp_name = (mode == PONG_LOCAL_VS_AI)
                    ? pong_desk_ai_level_name(a->hud.ai_level)
                    : "PLAYER 2";
}

static void end_local(App *a)
{
    a->in_local = false;
    a->local.active = false;
    a->hud.screen = DESK_MENU;
}

static void open_editor(App *a, int item)
{
    const char *title = (item == DESK_ITEM_SERVER) ? "Server  host:port"
                      : (item == DESK_ITEM_NAME)   ? "Your name"
                      : "Room code";
    char initial[128];
    if (item == DESK_ITEM_SERVER) {
        snprintf(initial, sizeof initial, "%s:%u", a->server, (unsigned)a->port);
    } else if (item == DESK_ITEM_NAME) {
        snprintf(initial, sizeof initial, "%s", a->name);
    } else {
        initial[0] = '\0';
    }

    unsigned cap = (item == DESK_ITEM_NAME) ? (unsigned)(sizeof a->name - 1)
                 : (item == DESK_ITEM_ROOM) ? (unsigned)(sizeof a->room - 1)
                                            : 32u;

    if (pong_ime_open(title, initial, cap)) {
        a->editing = item;
        /* The system composites the keyboard, but only if the backend gives it
         * the chance each frame. Without this it never draws and the game
         * looks frozen. */
        pong_gfx_system_dialog(true);
    } else {
        snprintf(a->toast, sizeof a->toast, "keyboard unavailable");
    }
}

static void commit_editor(App *a, const char *text)
{
    if (a->editing == DESK_ITEM_SERVER) {
        char host[64];
        unsigned port = 0;
        if (sscanf(text, "%63[^:]:%u", host, &port) == 2 && port > 0 && port < 65536) {
            snprintf(a->server, sizeof a->server, "%s", host);
            a->port = (uint16_t)port;
        } else {
            snprintf(a->server, sizeof a->server, "%s", text);
            a->port = 8787;   /* the raw TCP port, which is what this speaks */
        }
        snprintf(a->toast, sizeof a->toast, "server %s:%u", a->server, (unsigned)a->port);
    } else if (a->editing == DESK_ITEM_NAME) {
        snprintf(a->name, sizeof a->name, "%s", text);
        snprintf(a->toast, sizeof a->toast, "you are '%s'", a->name);
    } else if (a->editing == DESK_ITEM_ROOM) {
        snprintf(a->room, sizeof a->room, "%s", text);
    }
    cfg_save(a);

    int was = a->editing;
    a->editing = -1;
    if (was == DESK_ITEM_ROOM && a->room[0]) begin_connect(a, PONG_JOIN_MODE_ROOM_CODE);
}

static void activate(App *a, bool *running)
{
    switch (a->hud.sel) {
    case DESK_ITEM_QUICK:  begin_connect(a, PONG_JOIN_MODE_QUICKMATCH); break;
    case DESK_ITEM_BOT:    begin_connect(a, PONG_JOIN_MODE_VS_BOT); break;
    case DESK_ITEM_LOCAL_AI: begin_local(a, PONG_LOCAL_VS_AI); break;
    case DESK_ITEM_LOCAL_2P: begin_local(a, PONG_LOCAL_VS_HUMAN); break;
    case DESK_ITEM_ROOM:   open_editor(a, DESK_ITEM_ROOM); break;
    case DESK_ITEM_SERVER: open_editor(a, DESK_ITEM_SERVER); break;
    case DESK_ITEM_NAME:   open_editor(a, DESK_ITEM_NAME); break;
    case DESK_ITEM_QUIT:   *running = false; break;
    default: break;
    }
}

int main(void)
{
    FILE *f = fopen(LOG_PATH, "w");
    if (f) { fprintf(f, "pong-vita starting\n"); fclose(f); }

    App app;
    memset(&app, 0, sizeof app);
    app.editing = -1;
    app.hud.ai_level = PONG_BOT_NORMAL;
    cfg_load(&app);
    pong_client_init(&app.client);

    vlog("stage: gfx init");
    if (!pong_gfx_init("Pong")) {
        vlog("FAILED: pong_gfx_init");
        sceKernelExitProcess(0);
        return 1;
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    vlog("stage: input ready");

    app.hud.screen      = DESK_MENU;
    app.hud.my_name     = app.name;
    app.hud.opp_name    = app.client.opp_name;
    app.hud.server_addr = app.server;
    app.hud.room_code   = app.room;
    app.hud.gamepad     = true;    /* there is always a pad; show its hints */

    SceCtrlData pad, prev;
    memset(&pad, 0, sizeof pad);
    memset(&prev, 0, sizeof prev);

    bool running = true;
    uint32_t prev_ms = now_ms();
    bool touch_was_down = false;

    vlog("stage: entering main loop");

    while (running) {
        uint32_t now = now_ms();
        float dt = (float)(now - prev_ms) / 1000.0f;
        prev_ms = now;
        if (dt > 0.1f) dt = 0.1f;

        /* The keyboard is modal: nothing else reads input while it is up. */
        if (app.editing >= 0) {
            char text[128];
            PongImeResult r = pong_ime_poll(text, sizeof text);
            if (r != PONG_IME_PENDING) pong_gfx_system_dialog(false);
            if (r == PONG_IME_ACCEPTED)       commit_editor(&app, text);
            else if (r == PONG_IME_CANCELLED) app.editing = -1;
        } else {
            sceCtrlPeekBufferPositive(0, &pad, 1);
            unsigned pressed = pad.buttons & ~prev.buttons;
            prev = pad;

            if (pressed & SCE_CTRL_START) running = false;

            if (app.hud.screen == DESK_MENU) {
                if (pressed & SCE_CTRL_DOWN)
                    app.hud.sel = pong_desk_step(app.hud.sel, +1);
                if (pressed & SCE_CTRL_UP)
                    app.hud.sel = pong_desk_step(app.hud.sel, -1);
                if (pressed & SCE_CTRL_CROSS) activate(&app, &running);
            } else {
                if (pressed & SCE_CTRL_CIRCLE) {
                    if (app.in_local) end_local(&app);
                    else leave_match(&app);
                }
                /* Left/right change a row's value, which today is only the
                 * difficulty on the VS AI row. */
                if (app.hud.screen == DESK_MENU && app.hud.sel == DESK_ITEM_LOCAL_AI) {
                    int n = pong_desk_ai_level_count();
                    if (pressed & SCE_CTRL_RIGHT) app.hud.ai_level = (app.hud.ai_level + 1) % n;
                    if (pressed & SCE_CTRL_LEFT)  app.hud.ai_level = (app.hud.ai_level + n - 1) % n;
                }
                if (app.hud.screen == DESK_GAMEOVER && (pressed & SCE_CTRL_CROSS)) {
                    /* Again means the same KIND of match. */
                    if (app.in_local) begin_local(&app, app.local.mode);
                    else begin_connect(&app, PONG_JOIN_MODE_QUICKMATCH);
                }
            }

            /* Touch: taps pick a menu row, a held finger drives the paddle --
             * the same gesture the 3DS uses on its bottom screen. */
            SceTouchData touch;
            int n = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
            bool down = (n > 0 && touch.reportNum > 0);
            if (down) {
                float tx = (float)touch.report[0].x / TOUCH_MAX_X * 960.0f;
                float ty = (float)touch.report[0].y / TOUCH_MAX_Y * 544.0f;

                if (app.hud.screen == DESK_MENU) {
                    if (!touch_was_down) {           /* on press, not on hold */
                        int hit = pong_desk_item_at(tx, ty, 960, 544);
                        if (hit >= 0) {
                            app.hud.sel = hit;
                            activate(&app, &running);
                        }
                    }
                } else if (app.hud.screen == DESK_PLAY) {
                    set_my_target(&app, pong_desk_paddle_from_mouse(ty, 544));
                }
            }
            touch_was_down = down;

            /* Held stick or d-pad, for players who would rather not smear the
             * screen. Absolute from touch, relative from the stick. */
            if (app.hud.screen == DESK_PLAY) {
                float ay = ((float)pad.ly - 128.0f) / 128.0f;
                float dir = 0.0f;
                if (ay > 0.2f || ay < -0.2f) dir += ay;
                if (pad.buttons & SCE_CTRL_UP)   dir -= 1.0f;
                if (pad.buttons & SCE_CTRL_DOWN) dir += 1.0f;
                if (dir != 0.0f) nudge_paddle(&app, dir, dt);

                /*
                 * Player two, on a Vita that has to seat both of them.
                 *
                 * The RIGHT stick and the face buttons, because the machine has
                 * a second stick and the two hands are already apart -- the
                 * left half of the controls stays exactly what a solo player
                 * uses, so nothing has to be relearned to play alone again.
                 * Triangle is up and cross is down, matching their physical
                 * positions rather than any convention.
                 */
                if (app.in_local && app.local.mode == PONG_LOCAL_VS_HUMAN) {
                    float ry = ((float)pad.ry - 128.0f) / 128.0f;
                    float d2 = 0.0f;
                    if (ry > 0.2f || ry < -0.2f) d2 += ry;
                    if (pad.buttons & SCE_CTRL_TRIANGLE) d2 -= 1.0f;
                    if (pad.buttons & SCE_CTRL_CROSS)    d2 += 1.0f;
                    if (d2 != 0.0f) {
                        int32_t step = (int32_t)(d2 * (float)PONG_MAX_PADDLE_SPEED_Q4 * 60.0f * dt);
                        int32_t t = app.p2_target + step;
                        if (t < 0) t = 0;
                        if (t > PONG_FIELD_H_Q4) t = PONG_FIELD_H_Q4;
                        app.p2_target = t;
                    }
                    pong_local_set_target(&app.local, 1, app.p2_target);
                }
            }
        }

        if (app.in_local) {
            /* The authority is here, so the view comes straight out of the
             * simulation -- no snapshot ring, no render delay. */
            pong_local_advance(&app.local, (uint32_t)(dt * 1000.0f));
            pong_local_view(&app.local, &app.view);
            if (app.local.sim.state == PONG_SIM_GAME_OVER)
                app.hud.screen = DESK_GAMEOVER;
        } else {
            pump_network(&app, now);
            send_input(&app, now);
            pong_client_update(&app.client, now, &app.view);
        }

        if (app.fps_window == 0) app.fps_window = now;
        app.fps_count++;
        if (now - app.fps_window >= 1000) {
            app.fps = app.fps_count;
            app.fps_count = 0;
            app.fps_window = now;
        }

        char status[96];
        snprintf(status, sizeof status, "TCP %s:%u", app.server, (unsigned)app.port);
        app.hud.status_line = status;
        app.hud.rtt_ms  = app.client.min_rtt;
        app.hud.snap_hz = app.client.snap_hz;
        app.hud.buf_ms  = app.client.render_delay_ms;
        app.hud.fps     = app.fps;
        app.hud.my_side = app.client.my_side;
        app.hud.message = (app.hud.screen == DESK_ERROR) ? app.error : app.toast;
        app.hud.frame++;

        pong_gfx_frame_begin();
        pong_desk_frame(&app.view, &app.hud);
        pong_gfx_frame_end();
    }

    vlog("stage: exiting");
    if (app.net) pong_pc_close(app.net);
    pong_gfx_exit();
    sceKernelExitProcess(0);
    return 0;
}
