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
#include "log.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

#define RX_CHUNK 2048
#define INPUT_HZ 30
#define PING_EVERY_MS 2000

static u32 *g_socBuf = NULL;

/* Kept in the binary even though nothing reads it, so the build is identifiable
 * with `strings` alone. */
static const char g_version_banner[] __attribute__((used)) = PONG_VERSION_BANNER;

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
    char message[352];   /* holds update messages, which can carry a release URL */
    char addr[PONG_ADDR_MAX];
    char room[PONG_ROOM_CODE_BYTES + 1];
    int  menu_sel;
} App;

static void send_hello(App *a);
static void send_join(App *a, uint8_t mode);

/** Records a failure with its full per-transport diagnostic. */
static void log_diag(const char *what, const char *detail)
{
    pong_log_section(what);
    if (detail && detail[0]) pong_log("%s", detail);
}

/*
 * Asks for a room code.
 *
 * Both players type the SAME code -- there is no "create then share an
 * assigned code" round trip, because that would need a new server message and
 * the code is more useful when a person chooses it: it can be agreed out loud
 * before either console is even switched on.
 */
static bool ask_room_code(App *a)
{
    char buf[PONG_ROOM_CODE_BYTES + 1];
    snprintf(buf, sizeof buf, "%s", a->room);

    static SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_QWERTY, 2, PONG_ROOM_CODE_BYTES);
    swkbdSetInitialText(&kb, buf);
    swkbdSetHintText(&kb, "room code, e.g. PONG42");
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "Join", true);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);

    if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_RIGHT) return false;

    /* Upper-case and strip anything that is not alphanumeric, so a code read
     * aloud and typed with different capitalisation still matches. */
    size_t w = 0;
    for (size_t i = 0; buf[i] && w < PONG_ROOM_CODE_BYTES; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) a->room[w++] = (char)c;
    }
    a->room[w] = '\0';

    if (w == 0) {
        snprintf(a->message, sizeof a->message, "room code must have letters or digits");
        return false;
    }
    return true;
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

/*
 * Manual update check.
 *
 * Deliberately not automatic on launch: a blocking HTTPS round trip before the
 * menu even draws would make a cold start feel broken, and during development
 * `make send` is the fast path anyway.
 */
static void do_update_check(App *a)
{
    PongUpdateInfo up;
    snprintf(a->message, sizeof a->message, "checking for updates...");
    pong_log_section("update check");

    {
        char lbl[80];
        pong_log("source         : %s",
                 pong_update_target_label(a->cfg.update_source, a->cfg.gh_owner,
                                          a->cfg.gh_repo, lbl, sizeof lbl));
    }
    PongUpdateResult r = pong_update_check(&a->cfg.net, PONG_BUILD_ID,
                                           a->cfg.update_source,
                                           a->cfg.gh_owner, a->cfg.gh_repo, &up);
    pong_log("local build    : %lu", (unsigned long)up.local_build);
    pong_log("remote build   : %lu", (unsigned long)up.remote_build);
    pong_log("result         : %s", up.message);

    if (r != PONG_UPDATE_AVAILABLE) {
        snprintf(a->message, sizeof a->message, "%s", up.message);
        return;
    }

    PongUpdateResult d = pong_update_download(&a->cfg.net, &up, PONG_DSX_PATH,
                                              a->message, sizeof a->message);
    pong_log("download       : %s", a->message);

    /*
     * The .3dsx on the SD card is now current. An installed .cia is NOT: a
     * title cannot install another title without am:u, which belongs to FBI.
     * Say where to get it rather than implying the running build was replaced.
     */
    if (d == PONG_UPDATE_DONE && up.release_url[0]) {
        snprintf(a->message, sizeof a->message,
                 "3dsx updated to %lu. CIA: %s",
                 (unsigned long)up.remote_build, up.release_url);
    }
}

static void begin_connect(App *a, uint8_t join_mode)
{
    pong_log_section("connect attempt");
    pong_log("target         : %s  mode=%u  room='%s'",
             a->addr, (unsigned)join_mode, a->room);
    a->screen = SCREEN_CONNECTING;
    snprintf(a->message, sizeof a->message, "connecting...");
    a->net = pong_net_open(&a->cfg.net);
    if (a->net && pong_net_state(a->net) != PONG_LINK_FAILED) {
        pong_log("transport      : %s", pong_net_describe(a->net));
        send_hello(a);
        send_join(a, join_mode);
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
    if (mode == PONG_JOIN_MODE_ROOM_CODE) {
        pong_pad_bytes(j.room_code, sizeof j.room_code, a->room);
    }
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
                pong_log_section("match started");
                pong_log("seat           : %s", m.your_side == 0 ? "LEFT" : "RIGHT");
                pong_log("opponent       : %s (platform %u)",
                         a->client.opp_name, (unsigned)m.opp_platform);
                pong_log("first to       : %u%s", (unsigned)m.win_score,
                         (m.match_flags & PONG_MATCH_FLAG_SLOW_MODE) ? "  [SLOW MODE]" : "");
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

    /* Opened before anything else can fail, so a failure during startup is
     * still recorded. */
    pong_log_open();
    pong_log_system(PONG_BUILD_ID, PONG_PROTOCOL_VERSION);

    pong_config_load(&app.cfg);
    pong_addr_format(&app.cfg.net, app.addr, sizeof app.addr);
    app.screen = SCREEN_TITLE;

    pong_log_section("configuration");
    pong_log("server address : %s", app.addr);
    pong_log("mode           : %s",
             app.cfg.net.mode == PONG_MODE_LAN ? "lan"
             : app.cfg.net.mode == PONG_MODE_WEB ? "web" : "auto");
    pong_log("web            : %s:%u tls=%d verify=%d path=%s",
             app.cfg.net.web_host, (unsigned)app.cfg.net.web_port,
             app.cfg.net.web_tls ? 1 : 0, app.cfg.net.web_verify ? 1 : 0,
             app.cfg.net.web_path);
    pong_log("lan            : %s:%u subnet='%s'",
             app.cfg.net.lan_host[0] ? app.cfg.net.lan_host : "(unset)",
             (unsigned)app.cfg.net.lan_port, app.cfg.net.lan_subnet);
    pong_log("player name    : %s", app.cfg.player_name);
    {
        char lbl[80];
        pong_log("update source  : %s",
                 pong_update_target_label(app.cfg.update_source, app.cfg.gh_owner,
                                          app.cfg.gh_repo, lbl, sizeof lbl));
    }

    /* Sockets. The buffer must be page-aligned and becomes inaccessible to us
     * while SOC is up, so it is never freed before socExit(). */
    g_socBuf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    bool net_ready = false;
    if (g_socBuf && R_SUCCEEDED(socInit(g_socBuf, SOC_BUFFERSIZE))) {
        atexit(socShutdown);
        net_ready = true;
        pong_log_section("network");
        pong_log("socInit        : ok (%d KB buffer)", SOC_BUFFERSIZE / 1024);
        pong_log_network_identity();
    } else {
        snprintf(app.message, sizeof app.message, "no network (is wifi on?)");
        pong_log_section("network");
        pong_log("socInit        : FAILED (buffer %s) -- wifi off?",
                 g_socBuf ? "allocated" : "ALLOCATION FAILED");
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
            /* D-pad moves the highlight; A activates it. Touch does both at
             * once. Supporting both matters because the buttons are on the
             * bottom screen but the game is played with the pad. */
            if (kDown & KEY_DOWN) app.menu_sel = (app.menu_sel + 1) % MENU_COUNT;
            if (kDown & KEY_UP)   app.menu_sel = (app.menu_sel + MENU_COUNT - 1) % MENU_COUNT;

            int activate = -1;
            if (kDown & KEY_A) activate = app.menu_sel;
            if (kDown & KEY_TOUCH) {
                touchPosition tp;
                hidTouchRead(&tp);
                int hit = pong_ui_menu_hit((float)tp.px, (float)tp.py);
                if (hit >= 0) { app.menu_sel = hit; activate = hit; }
            }

            if (activate >= 0 && net_ready) {
                app.message[0] = '\0';
                switch (activate) {
                case MENU_QUICK:
                    app.room[0] = '\0';   /* no code to display for a quickmatch */
                    begin_connect(&app, PONG_JOIN_MODE_QUICKMATCH);
                    break;
                case MENU_BOT:
                    app.room[0] = '\0';
                    begin_connect(&app, PONG_JOIN_MODE_VS_BOT);
                    break;
                case MENU_ROOM:
                    if (ask_room_code(&app)) begin_connect(&app, PONG_JOIN_MODE_ROOM_CODE);
                    break;
                case MENU_SERVER: edit_server_address(&app); break;
                case MENU_SOURCE: {
                    /* Cycle the presets. A hand-edited config that matches none
                     * of them starts the cycle from the top rather than being
                     * silently rewritten to something adjacent. */
                    int idx = pong_update_target_index(app.cfg.update_source,
                                                       app.cfg.gh_owner,
                                                       app.cfg.gh_repo);
                    idx = (idx + 1) % PONG_UPDATE_TARGET_COUNT;

                    const PongUpdateTarget *t = &PONG_UPDATE_TARGETS[idx];
                    app.cfg.update_source = t->source;
                    if (t->owner) {
                        snprintf(app.cfg.gh_owner, sizeof app.cfg.gh_owner, "%s", t->owner);
                        snprintf(app.cfg.gh_repo, sizeof app.cfg.gh_repo, "%s", t->repo);
                    }
                    pong_config_save(&app.cfg);
                    snprintf(app.message, sizeof app.message, "updates from %s", t->label);
                    break;
                }
                case MENU_UPDATE: do_update_check(&app); break;
                default: break;
                }
            } else if (activate >= 0) {
                snprintf(app.message, sizeof app.message, "no network -- is wifi on?");
            }
        }

        if (app.screen == SCREEN_ERROR) {
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
        hud.menu_sel = app.menu_sel;
        hud.build_id = PONG_BUILD_ID;
        /* Only meaningful while waiting in a room someone else must join. */
        hud.room_code = app.room[0] ? app.room : NULL;
        static char src_label[80];
        hud.update_src = pong_update_target_label(app.cfg.update_source,
                                                  app.cfg.gh_owner, app.cfg.gh_repo,
                                                  src_label, sizeof src_label);
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
    pong_log_section("session end");
    pong_log_close();
    pong_render_exit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
