/*
 * Pong3DS -- cross-play Pong against a browser.
 *
 * Structure of a frame:
 *   1. read input          (touch is absolute, which matches the protocol)
 *   2. drain the network   (never blocks; the HTTPS path has a worker thread)
 *   3. update prediction   (our paddle at 60fps, the world interpolated in the past)
 *   4. draw both screens   (one C3D_FrameBegin/End pair)
 *
 * The top screen is the playfield at exactly half field coordinates; the bottom
 * screen is status plus the touch strip, following the original sketch.
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "pong_proto.h"
#include "client.h"
#include "net.h"
#include "render.h"
#include "config.h"
#include "update.h"
#include "addr.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

#define RX_CHUNK 2048
#define INPUT_HZ 30
#define PING_EVERY_MS 2000

static u32 *g_socBuf = NULL;

static void socShutdown(void) { socExit(); }

/* --------------------------------------------------------------------- app */

typedef struct {
    PongConfig cfg;
    PongNet   *net;
    PongClient client;
    PongScreen screen;

    /* Partial frame carried between reads: a TCP read can split anywhere, and
     * the header's length field is what makes carrying the tail safe. */
    uint8_t  acc[RX_CHUNK * 2];
    size_t   acc_len;

    uint32_t input_seq;
    uint32_t last_input_ms;
    uint32_t last_ping_ms;
    uint32_t frame;

    char status[96];
    char detail[96];
    char message[192];   /* holds update-check messages, which are verbose */
    char addr[PONG_ADDR_MAX];
} App;

static void send_hello(App *a);
static void send_join(App *a, uint8_t mode);

/*
 * Mirrors a diagnostic to the SD card.
 *
 * A TLS error string plus its hex code does not comfortably fit on a 320px
 * screen, and it is exactly the text you need to read carefully. Writing it to
 * a file means it can be read on a computer instead of squinted at, and it
 * survives the console being switched off.
 */
static void log_diag(const char *what, const char *detail)
{
    FILE *f = fopen(PONG_LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "[%llu] %s\n", (unsigned long long)osGetTime(), what);
    if (detail && detail[0]) fprintf(f, "%s\n", detail);
    fprintf(f, "----\n");
    fclose(f);
}

/** Opens the software keyboard and applies whatever the user typed. */
static void edit_server_address(App *a)
{
    char buf[PONG_ADDR_MAX];
    snprintf(buf, sizeof buf, "%s", a->addr);

    if (!pong_addr_prompt(buf, sizeof buf, a->addr)) return;   /* cancelled */

    char err[96];
    PongNetConfig probe = a->cfg.net;
    if (!pong_addr_parse(buf, &probe, err, sizeof err)) {
        snprintf(a->message, sizeof a->message, "%s", err);
        return;
    }

    a->cfg.net = probe;
    pong_addr_format(&a->cfg.net, a->addr, sizeof a->addr);
    /* Persist immediately: typing an address on a 3DS keyboard once is enough. */
    pong_config_save(&a->cfg);
    snprintf(a->message, sizeof a->message, "saved");
}

static void begin_connect(App *a)
{
    a->screen = SCREEN_CONNECTING;
    snprintf(a->message, sizeof a->message, "connecting...");
    a->net = pong_net_open(&a->cfg.net);
    if (a->net && pong_net_state(a->net) != PONG_LINK_FAILED) {
        send_hello(a);
        send_join(a, PONG_JOIN_MODE_QUICKMATCH);
        a->screen = SCREEN_QUEUED;
    } else {
        snprintf(a->message, sizeof a->message, "%s",
                 a->net ? pong_net_error(a->net) : "out of memory");
        a->screen = SCREEN_ERROR;
    }
}

static bool send_frame(App *a, const uint8_t *buf, size_t len)
{
    return pong_net_send(a->net, buf, len);
}

static void send_hello(App *a)
{
    PongHELLO h;
    memset(&h, 0, sizeof h);
    h.platform = PONG_PLATFORM_N3DS;
    h.transport = PONG_TRANSPORT_KIND_TCP;
    h.caps = 0;
    h.build_id = 1;
    pong_pad_bytes(h.name, sizeof h.name, a->cfg.player_name);

    uint8_t buf[64];
    size_t n = pong_write_hello(buf, sizeof buf, 1, &h);
    send_frame(a, buf, n);
}

static void send_join(App *a, uint8_t mode)
{
    PongJOIN j;
    memset(&j, 0, sizeof j);
    j.mode = mode;
    uint8_t buf[64];
    size_t n = pong_write_join(buf, sizeof buf, 2, &j);
    send_frame(a, buf, n);
}

/* ---------------------------------------------------------------- receive */

static void pump_network(App *a, uint32_t now_ms)
{
    uint8_t chunk[RX_CHUNK];
    size_t got = pong_net_recv(a->net, chunk, sizeof chunk);

    if (got > 0) {
        size_t space = sizeof a->acc - a->acc_len;
        size_t take = got < space ? got : space;
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
            /* Magic and version exist precisely so garbage is distinguishable
             * from a partial frame. There is no safe resync, so drop it all. */
            snprintf(a->message, sizeof a->message, "protocol desync (%d)", (int)r);
            a->acc_len = 0;
            return;
        }

        switch (fr.type) {
        case PONG_MSG_SNAPSHOT: {
            PongSNAPSHOT s;
            if (pong_read_snapshot(fr.payload, fr.length, &s)) {
                pong_client_on_snapshot(&a->client, &s, now_ms);
                if (a->screen == SCREEN_QUEUED || a->screen == SCREEN_CONNECTING) {
                    a->screen = SCREEN_PLAY;
                }
                if (s.state == PONG_MATCH_STATE_GAME_OVER) a->screen = SCREEN_GAMEOVER;
            }
            break;
        }
        case PONG_MSG_MATCH_START: {
            PongMATCH_START m;
            if (pong_read_match_start(fr.payload, fr.length, &m)) {
                pong_client_on_match_start(&a->client, &m);
                a->screen = SCREEN_PLAY;
                a->message[0] = '\0';
            }
            break;
        }
        case PONG_MSG_PONG: {
            PongPONG p;
            if (pong_read_pong(fr.payload, fr.length, &p)) {
                pong_client_on_pong(&a->client, &p, now_ms);
            }
            break;
        }
        case PONG_MSG_EVENT: {
            PongEVENT e;
            if (pong_read_event(fr.payload, fr.length, &e)) {
                if (e.kind == PONG_EVENT_KIND_OPP_LEFT) {
                    snprintf(a->message, sizeof a->message, "opponent left");
                }
            }
            break;
        }
        case PONG_MSG_BYE: {
            PongBYE b;
            if (pong_read_bye(fr.payload, fr.length, &b)) {
                snprintf(a->message, sizeof a->message, "disconnected (code %u)", b.code);
                a->screen = SCREEN_ERROR;
            }
            break;
        }
        case PONG_MSG_WELCOME:
        default:
            break;
        }

        off += used;
    }

    if (off > 0) {
        memmove(a->acc, a->acc + off, a->acc_len - off);
        a->acc_len -= off;
    }
}

/* ------------------------------------------------------------------ input */

static void read_input(App *a, u32 kHeld)
{
    /*
     * Touch is absolute and maps exactly: the bottom screen is 240px tall, the
     * field is 480 field-px == 7680 Q4, so Q4 = py * 32. One multiply, no
     * rounding, and it lines up with what the renderer draws.
     */
    if (kHeld & KEY_TOUCH) {
        touchPosition tp;
        hidTouchRead(&tp);
        pong_client_set_target(&a->client, (int32_t)tp.py * 32);
        return;
    }

    /* Circle pad: integrate locally at 60fps, so pad feel stays full-rate even
     * when the network is 10Hz. Gain chosen so full deflection exactly
     * saturates the server's speed clamp -- pushing harder cannot help, which
     * makes the control predictable. */
    circlePosition cp;
    hidCircleRead(&cp);
    if (cp.dy > 20 || cp.dy < -20) {
        int32_t step = (-(int32_t)cp.dy * PONG_MAX_PADDLE_SPEED_Q4) / 156;
        pong_client_set_target(&a->client, a->client.target_y + step);
        return;
    }

    /* D-pad moves at exactly the clamp speed. KEY_UP/KEY_DOWN also cover the
     * circle pad, so this is checked after the analog read. */
    if (kHeld & KEY_UP) {
        pong_client_set_target(&a->client, a->client.target_y - PONG_MAX_PADDLE_SPEED_Q4);
    } else if (kHeld & KEY_DOWN) {
        pong_client_set_target(&a->client, a->client.target_y + PONG_MAX_PADDLE_SPEED_Q4);
    }
}

static void send_input(App *a, uint32_t now_ms)
{
    if (now_ms - a->last_input_ms < 1000 / INPUT_HZ) return;
    a->last_input_ms = now_ms;

    PongINPUT in;
    memset(&in, 0, sizeof in);
    in.input_seq = ++a->input_seq;
    in.last_tick_seen = a->client.newest_tick;
    in.desired_yq4 = (int16_t)a->client.target_y;
    in.buttons = 0;
    in.flags = 0;

    uint8_t buf[64];
    size_t n = pong_write_input(buf, sizeof buf, (uint16_t)(a->input_seq & 0xffff), &in);
    send_frame(a, buf, n);

    if (now_ms - a->last_ping_ms >= PING_EVERY_MS) {
        a->last_ping_ms = now_ms;
        PongPING p;
        memset(&p, 0, sizeof p);
        p.client_time_ms = now_ms;
        size_t m = pong_write_ping(buf, sizeof buf, 0, &p);
        send_frame(a, buf, m);
    }
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    romfsInit();
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    pong_render_init();

    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    static App app;
    memset(&app, 0, sizeof app);
    pong_client_init(&app.client);
    pong_config_load(&app.cfg);
    pong_addr_format(&app.cfg.net, app.addr, sizeof app.addr);
    app.screen = SCREEN_TITLE;

    /* Sockets. The buffer must be page-aligned and becomes inaccessible to us
     * while SOC is up, so it is never freed before socExit(). */
    g_socBuf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    bool net_ready = false;
    if (g_socBuf && R_SUCCEEDED(socInit(g_socBuf, SOC_BUFFERSIZE))) {
        atexit(socShutdown);
        net_ready = true;
    } else {
        snprintf(app.message, sizeof app.message, "no network (is wifi on?)");
        app.screen = SCREEN_ERROR;
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        if (kDown & KEY_START) break;

        uint32_t now = (uint32_t)osGetTime();
        app.frame++;

        /* ---- state machine ---------------------------------------------- */
        if (app.screen == SCREEN_TITLE) {
            /* Tapping the address field opens the keyboard; tapping CONNECT (or
             * A) starts the game. Hit-testing uses the same rects the renderer
             * draws, so what you see is what you can press. */
            if (net_ready && (kDown & KEY_TOUCH)) {
                touchPosition tp;
                hidTouchRead(&tp);
                float tx = (float)tp.px, ty = (float)tp.py;
                if (pong_ui_hit(&PONG_UI_ADDR_BOX, tx, ty)) {
                    edit_server_address(&app);
                } else if (pong_ui_hit(&PONG_UI_CONNECT_BTN, tx, ty)) {
                    begin_connect(&app);
                }
            }
            if (net_ready && (kDown & KEY_A)) begin_connect(&app);

            /* X checks for a newer build. Deliberately manual rather than
             * automatic on launch: a blocking HTTPS round trip before the title
             * screen even draws would make a cold start feel broken, and the
             * dev loop uses `make send` anyway. */
            if (net_ready && (kDown & KEY_X)) {
                PongUpdateInfo up;
                snprintf(app.message, sizeof app.message, "checking for updates...");
                PongUpdateResult r = pong_update_check(&app.cfg.net, PONG_BUILD_ID, &up);
                if (r == PONG_UPDATE_AVAILABLE) {
                    PongUpdateResult d = pong_update_download(&app.cfg.net, &up,
                                                              PONG_DSX_PATH,
                                                              app.message, sizeof app.message);
                    /*
                     * The .3dsx on the SD card is now current. An installed
                     * .cia is NOT: a title cannot install another title without
                     * am:u access, which belongs to FBI. So point at the release
                     * rather than implying the running build was replaced.
                     */
                    if (d == PONG_UPDATE_DONE && up.release_url[0]) {
                        snprintf(app.message, sizeof app.message,
                                 "3dsx updated. For the CIA: %s", up.release_url);
                    }
                } else {
                    snprintf(app.message, sizeof app.message, "%s", up.message);
                }
            }
        } else if (app.screen == SCREEN_ERROR) {
            if (kDown & (KEY_TOUCH | KEY_A)) {
                if (app.net) { pong_net_close(app.net); app.net = NULL; }
                app.acc_len = 0;
                app.message[0] = '\0';
                app.screen = SCREEN_TITLE;
            }
        } else if (app.screen == SCREEN_GAMEOVER) {
            if (kDown & (KEY_TOUCH | KEY_A)) {
                pong_client_reset_match(&app.client);
                send_join(&app, PONG_JOIN_MODE_QUICKMATCH);
                app.screen = SCREEN_QUEUED;
            }
        }

        /* ---- network + simulation ---------------------------------------- */
        if (app.net) {
            pump_network(&app, now);

            if (pong_net_state(app.net) == PONG_LINK_FAILED && app.screen != SCREEN_ERROR) {
                /* The detail lives in pong_net_diag(); this is just the headline. */
                snprintf(app.message, sizeof app.message, "%s", pong_net_error(app.net));
                log_diag("connection failed", pong_net_diag(app.net));
                app.screen = SCREEN_ERROR;
            }

            if (app.screen == SCREEN_PLAY || app.screen == SCREEN_QUEUED) {
                read_input(&app, kHeld);
                send_input(&app, now);
            }
        }

        PongView view;
        pong_client_update(&app.client, now, &view);

        /* ---- hud ---------------------------------------------------------- */
        PongHud hud;
        memset(&hud, 0, sizeof hud);
        hud.screen = app.screen;
        hud.frame = app.frame;
        hud.my_side = app.client.my_side;
        hud.slow_mode = app.client.slow_mode;
        hud.message = app.message;
        hud.diag = app.net ? pong_net_diag(app.net) : NULL;
        hud.server_addr = app.addr;

        if (app.net) {
            snprintf(app.status, sizeof app.status, "%s", pong_net_describe(app.net));
            snprintf(app.detail, sizeof app.detail, "RTT %lums  %luHz  +%lums  vs %s",
                     (unsigned long)pong_net_rtt(app.net),
                     (unsigned long)pong_net_hz(app.net),
                     (unsigned long)app.client.render_delay_ms,
                     app.client.opp_name[0] ? app.client.opp_name : "?");
            hud.status_line = app.status;
            hud.detail_line = app.detail;
        } else {
            hud.status_line = "not connected";
            hud.detail_line = app.cfg.net.lan_host;
        }

        /* ---- draw --------------------------------------------------------- */
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        pong_render_frame(top, bot, &view, &hud);
        C3D_FrameEnd(0);
    }

    if (app.net) pong_net_close(app.net);
    pong_render_exit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
