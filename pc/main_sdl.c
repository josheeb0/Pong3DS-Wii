/*
 * PC front end: window, input, clock, transport.
 *
 * One game, several interfaces. This drives src/ui/desktop.c -- a desktop
 * layout with one canvas -- rather than the handheld two-screen interface the
 * 3DS uses, for the same reason the browser client has its own: a window is not
 * a 3DS and pretending otherwise makes both worse. Everything underneath is
 * shared: the protocol, the netcode, the simulation and the renderer seam.
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "desktop.h"
#include "net_pc.h"
#include "client.h"
#include "pong_proto.h"

#define RX_CHUNK       2048
#define INPUT_HZ       60      /* a LAN socket, so no reason to send less */
#define PING_EVERY_MS  2000
#define CFG_PATH       "pong-pc.cfg"

typedef struct {
    PongNetPC  *net;
    PongClient  client;
    DeskHud     hud;
    PongView    view;

    char        server[128];
    uint16_t    port;
    char        name[17];
    char        room[8];

    uint8_t     acc[RX_CHUNK * 2];
    size_t      acc_len;
    uint32_t    input_seq;
    uint32_t    last_input_ms, last_ping_ms;
    /*
     * Two message slots, not one.
     *
     * They were the same buffer, and a gamepad-connected notice arriving after
     * a failed connect replaced the reason it failed -- the error screen read
     * "CONNECTION FAILED / DualSense Wireless Controller connected", which is
     * worse than useless. A failure explains itself until dismissed; a toast is
     * passing traffic.
     */
    char        error[192];
    char        toast[192];

    /* Text entry state, for the server and name fields. */
    bool        editing;
    int         edit_target;
    char        edit_buf[128];

    SDL_Gamepad *pad;
    uint32_t    fps, fps_count, fps_window;
    bool        rtt_demo;   /* --demo: keep the fake match state alive */
} App;

/* ------------------------------------------------------------------ config */

static void cfg_load(App *a)
{
    snprintf(a->server, sizeof a->server, "192.168.4.29");
    a->port = 8787;
    snprintf(a->name, sizeof a->name, "PC PLAYER");

    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = '\0';
        if (!strcmp(line, "server")) snprintf(a->server, sizeof a->server, "%s", v);
        else if (!strcmp(line, "port")) a->port = (uint16_t)atoi(v);
        else if (!strcmp(line, "name")) snprintf(a->name, sizeof a->name, "%s", v);
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
    h.platform = PONG_PLATFORM_PC;
    h.transport = PONG_TRANSPORT_KIND_TCP;
    h.build_id = 1;
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
        fprintf(stderr, "connect: %s\n", err);
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
            /* Magic and version exist so garbage is distinguishable from a
             * partial frame. There is no safe resync. */
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
    in.input_seq = ++a->input_seq;
    in.last_tick_seen = a->client.newest_tick;
    in.desired_yq4 = (int16_t)a->client.target_y;

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

static void nudge_paddle(App *a, float dir, float dt)
{
    /* Speed matched to the server's clamp, so holding a direction tracks what
     * the simulation will actually allow rather than outrunning it. */
    int32_t step = (int32_t)(dir * (float)PONG_MAX_PADDLE_SPEED_Q4 * 60.0f * dt);
    int32_t t = a->client.target_y + step;
    if (t < 0) t = 0;
    if (t > PONG_FIELD_H_Q4) t = PONG_FIELD_H_Q4;
    pong_client_set_target(&a->client, t);
}

static void open_editor(App *a, int target)
{
    a->editing = true;
    a->edit_target = target;
    if (target == DESK_ITEM_SERVER) {
        snprintf(a->edit_buf, sizeof a->edit_buf, "%s:%u", a->server, (unsigned)a->port);
    } else if (target == DESK_ITEM_NAME) {
        snprintf(a->edit_buf, sizeof a->edit_buf, "%s", a->name);
    } else {
        a->edit_buf[0] = '\0';
    }
    SDL_StartTextInput(NULL);
}

static void commit_editor(App *a)
{
    if (a->edit_target == DESK_ITEM_SERVER) {
        char host[128];
        unsigned port = 0;
        if (sscanf(a->edit_buf, "%127[^:]:%u", host, &port) == 2 && port > 0 && port < 65536) {
            snprintf(a->server, sizeof a->server, "%s", host);
            a->port = (uint16_t)port;
        } else {
            snprintf(a->server, sizeof a->server, "%s", a->edit_buf);
            a->port = 8787;   /* the raw TCP port, which is what this build speaks */
        }
        snprintf(a->toast, sizeof a->toast, "server %s:%u", a->server, (unsigned)a->port);
    } else if (a->edit_target == DESK_ITEM_NAME) {
        snprintf(a->name, sizeof a->name, "%s", a->edit_buf);
        snprintf(a->toast, sizeof a->toast, "you are '%s'", a->name);
    } else if (a->edit_target == DESK_ITEM_ROOM) {
        snprintf(a->room, sizeof a->room, "%s", a->edit_buf);
    }
    cfg_save(a);
    a->editing = false;
    SDL_StopTextInput(NULL);

    if (a->edit_target == DESK_ITEM_ROOM && a->room[0]) {
        begin_connect(a, PONG_JOIN_MODE_ROOM_CODE);
    }
}

static void activate(App *a)
{
    switch (a->hud.sel) {
    case DESK_ITEM_QUICK:  begin_connect(a, PONG_JOIN_MODE_QUICKMATCH); break;
    case DESK_ITEM_BOT:    begin_connect(a, PONG_JOIN_MODE_VS_BOT); break;
    case DESK_ITEM_ROOM:   open_editor(a, DESK_ITEM_ROOM); break;
    case DESK_ITEM_SERVER: open_editor(a, DESK_ITEM_SERVER); break;
    case DESK_ITEM_NAME:   open_editor(a, DESK_ITEM_NAME); break;
    default: break;
    }
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof app);
    cfg_load(&app);
    pong_client_init(&app.client);

    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--size") == 0) {
            int w = 0, h = 0;
            if (sscanf(argv[i + 1], "%dx%d", &w, &h) == 2) pong_gfx_request_size(w, h);
        }
    }

    /* --shot renders a few frames and saves one, so a rendering change can be
     * looked at rather than taken on trust. --demo fills in a plausible match
     * so the playfield can be captured without a server. */
    const char *shot = NULL;
    bool demo = false, autoplay = false;
    int shot_after = 8;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[i + 1];
        if (strcmp(argv[i], "--demo") == 0) demo = true;
        if (strcmp(argv[i], "--autoplay") == 0) autoplay = true;
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) shot_after = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            snprintf(app.name, sizeof app.name, "%s", argv[i + 1]);
    }

    if (!pong_gfx_init("Pong - cross-play")) return 1;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "gamepad subsystem: %s\n", SDL_GetError());
    }

    app.hud.screen = DESK_MENU;
    app.hud.my_name = app.name;
    app.hud.server_addr = app.server;
    app.hud.message = app.toast;   /* the error screen reads hud->message too */
    app.hud.opp_name = app.client.opp_name;

    if (demo) {
        app.hud.screen = DESK_PLAY;
        app.hud.opp_name = "BROWSER";
        app.client.my_side = 0;
        app.view.valid = true;
        app.view.state = PONG_MATCH_STATE_PLAY;
        app.view.ball_x = (PONG_FIELD_W_Q4 * 3) / 5;
        app.view.ball_y = PONG_FIELD_H_Q4 / 3;
        app.view.left_y = PONG_FIELD_H_Q4 / 2;
        app.view.right_y = PONG_FIELD_H_Q4 / 3;
        app.view.score_l = 7;
        app.view.score_r = 4;
        app.rtt_demo = true;
    }

    /* --autoplay starts a quick match immediately, so the whole path can be
     * exercised from a script rather than by a person pressing Enter. */
    if (autoplay) begin_connect(&app, PONG_JOIN_MODE_QUICKMATCH);

    int frames = 0;
    bool running = true;
    uint64_t prev_ticks = SDL_GetTicks();

    while (running) {
        uint32_t now = (uint32_t)SDL_GetTicks();
        float dt = (float)(now - (uint32_t)prev_ticks) / 1000.0f;
        prev_ticks = now;
        if (dt > 0.1f) dt = 0.1f;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;

            /* SDL3 knows the DualSense natively, so nothing here is
             * PS5-specific beyond the button names being the PS layout. */
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!app.pad) {
                    app.pad = SDL_OpenGamepad(e.gdevice.which);
                    if (app.pad) {
                        snprintf(app.toast, sizeof app.toast, "%s connected",
                                 SDL_GetGamepadName(app.pad));
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (app.pad && SDL_GetGamepadID(app.pad) == e.gdevice.which) {
                    SDL_CloseGamepad(app.pad);
                    app.pad = NULL;
                }
                break;

            case SDL_EVENT_TEXT_INPUT:
                if (app.editing) {
                    size_t have = strlen(app.edit_buf);
                    size_t cap = (app.edit_target == DESK_ITEM_NAME)
                                 ? sizeof app.name - 1 : sizeof app.edit_buf - 1;
                    size_t add = strlen(e.text.text);
                    if (have + add < cap) strcat(app.edit_buf, e.text.text);
                }
                break;

            case SDL_EVENT_KEY_DOWN:
                if (app.editing) {
                    if (e.key.key == SDLK_RETURN) commit_editor(&app);
                    else if (e.key.key == SDLK_ESCAPE) {
                        app.editing = false;
                        SDL_StopTextInput(NULL);
                    } else if (e.key.key == SDLK_BACKSPACE) {
                        size_t n = strlen(app.edit_buf);
                        if (n) app.edit_buf[n - 1] = '\0';
                    }
                    break;
                }
                switch (e.key.key) {
                case SDLK_ESCAPE:
                    if (app.hud.screen == DESK_MENU) running = false;
                    else leave_match(&app);
                    break;
                case SDLK_UP:
                    if (app.hud.screen == DESK_MENU)
                        app.hud.sel = (app.hud.sel + DESK_ITEM_COUNT - 1) % DESK_ITEM_COUNT;
                    break;
                case SDLK_DOWN:
                    if (app.hud.screen == DESK_MENU)
                        app.hud.sel = (app.hud.sel + 1) % DESK_ITEM_COUNT;
                    break;
                case SDLK_RETURN:
                    if (app.hud.screen == DESK_MENU) {
                        if (app.hud.sel == DESK_ITEM_QUIT) running = false;
                        else activate(&app);
                    } else if (app.hud.screen == DESK_GAMEOVER) {
                        begin_connect(&app, PONG_JOIN_MODE_QUICKMATCH);
                    }
                    break;
                default: break;
                }
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (app.editing) break;
                switch (e.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    if (app.hud.screen == DESK_MENU)
                        app.hud.sel = (app.hud.sel + DESK_ITEM_COUNT - 1) % DESK_ITEM_COUNT;
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    if (app.hud.screen == DESK_MENU)
                        app.hud.sel = (app.hud.sel + 1) % DESK_ITEM_COUNT;
                    break;
                case SDL_GAMEPAD_BUTTON_SOUTH:    /* cross */
                    if (app.hud.screen == DESK_MENU) {
                        if (app.hud.sel == DESK_ITEM_QUIT) running = false;
                        else activate(&app);
                    } else if (app.hud.screen == DESK_GAMEOVER) {
                        begin_connect(&app, PONG_JOIN_MODE_QUICKMATCH);
                    }
                    break;
                case SDL_GAMEPAD_BUTTON_EAST:     /* circle */
                    if (app.hud.screen != DESK_MENU) leave_match(&app);
                    break;
                case SDL_GAMEPAD_BUTTON_START:    /* options */
                    running = false;
                    break;
                default: break;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                if (app.editing) break;
                int w = 0, h = 0;
                pong_gfx_output_size(&w, &h);
                if (app.hud.screen == DESK_MENU) {
                    int hit = pong_desk_item_at(e.button.x, e.button.y, w, h);
                    if (hit >= 0) {
                        app.hud.sel = hit;
                        if (hit == DESK_ITEM_QUIT) running = false;
                        else activate(&app);
                    }
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                if (!app.editing && app.hud.screen == DESK_PLAY) {
                    int w = 0, h = 0;
                    pong_gfx_output_size(&w, &h);
                    (void)w;
                    pong_client_set_target(&app.client,
                                           pong_desk_paddle_from_mouse(e.motion.y, h));
                }
                break;

            default: break;
            }
        }

        /* Held inputs: the stick and the arrow keys move the paddle
         * continuously, where the mouse sets it absolutely. */
        if (!app.editing && app.hud.screen == DESK_PLAY) {
            const bool *keys = SDL_GetKeyboardState(NULL);
            float dir = 0.0f;
            if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) dir -= 1.0f;
            if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) dir += 1.0f;

            if (app.pad) {
                float ay = (float)SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
                /* A DualSense stick does not rest at exactly zero. */
                if (ay > 0.18f || ay < -0.18f) dir += ay;
                if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) dir -= 1.0f;
                if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) dir += 1.0f;
            }
            if (dir != 0.0f) nudge_paddle(&app, dir, dt);
        }

        pump_network(&app, now);
        send_input(&app, now);
        if (!app.rtt_demo) pong_client_update(&app.client, now, &app.view);

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
        app.hud.rtt_ms = app.client.min_rtt;
        app.hud.snap_hz = app.client.snap_hz;
        app.hud.buf_ms = app.client.render_delay_ms;
        app.hud.fps = app.fps;
        app.hud.my_side = app.client.my_side;
        app.hud.gamepad = (app.pad != NULL);
        app.hud.editing = app.editing;
        app.hud.edit_text = app.edit_buf;
        app.hud.edit_label = (app.edit_target == DESK_ITEM_SERVER) ? "SERVER  host:port"
                           : (app.edit_target == DESK_ITEM_NAME)   ? "YOUR NAME"
                           : "ROOM CODE";
        app.hud.room_code = app.room;
        /* The error screen shows the failure; every other screen shows the
         * passing notice. One field in the HUD, chosen here. */
        app.hud.message = (app.hud.screen == DESK_ERROR) ? app.error : app.toast;
        app.hud.frame++;

        pong_gfx_frame_begin();
        pong_desk_frame(&app.view, &app.hud);
        pong_gfx_frame_end();

        if (shot && ++frames >= shot_after) {
            if (!pong_gfx_screenshot(shot)) fprintf(stderr, "screenshot failed\n");
            else printf("wrote %s\n", shot);
            running = false;
        }
    }

    if (app.pad) SDL_CloseGamepad(app.pad);
    if (app.net) pong_pc_close(app.net);
    pong_gfx_exit();
    return 0;
}
