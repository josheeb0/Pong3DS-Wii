/*
 * Host integration test: plays a real match against the live server using the
 * SAME C code the 3DS runs.
 *
 * `test_proto` proves the codec agrees with TypeScript on bytes. This proves
 * the layer above it -- snapshot ring, interpolation, clock sync, paddle
 * prediction -- actually works against a real server, without needing a 3DS in
 * the loop. Everything here except the socket calls is the console's code
 * verbatim, which is why client.c deliberately has no libctru dependency.
 *
 *   make -C 3ds/test -f Makefile.host play        (server must be running)
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

#include "../source/net/pong_proto.h"
#include "../source/game/client.h"

static const char *HOST = "127.0.0.1";
static int PORT = 8787;
static int SECONDS = 10;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int connect_tcp(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) HOST = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) PORT = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) SECONDS = atoi(argv[++i]);
    }

    /* A match opens with a 3s countdown (COUNTDOWN_TICKS at 60Hz), so a window
     * shorter than this leaves too little actual play for the thresholds below
     * to mean anything -- and produces a confusing failure rather than a
     * useful one. */
    if (SECONDS < 8) {
        printf("refusing to run for %ds: the match countdown alone is %ds.\n",
               SECONDS, PONG_COUNTDOWN_TICKS / PONG_TICK_HZ);
        printf("use --seconds 8 or more.\n");
        return 2;
    }

    printf("=== 3DS client integration test ===\n");
    printf("connecting to %s:%d for %ds\n\n", HOST, PORT, SECONDS);

    int fd = connect_tcp(HOST, PORT);
    if (fd < 0) {
        printf("FAILED: cannot reach %s:%d -- is the server running?\n", HOST, PORT);
        return 1;
    }

    PongClient c;
    pong_client_init(&c);

    uint8_t acc[8192];
    size_t acc_len = 0;
    uint8_t out[256];

    /* ---- HELLO + JOIN (vs bot, so the test never waits for a human) ------ */
    {
        PongHELLO h;
        memset(&h, 0, sizeof h);
        h.platform = PONG_PLATFORM_N3DS;
        h.transport = PONG_TRANSPORT_KIND_TCP;
        h.build_id = 1;
        pong_pad_bytes(h.name, sizeof h.name, "HOSTTEST");
        size_t n = pong_write_hello(out, sizeof out, 1, &h);
        if (write(fd, out, n) != (ssize_t)n) { printf("FAILED: write HELLO\n"); return 1; }

        PongJOIN j;
        memset(&j, 0, sizeof j);
        j.mode = PONG_JOIN_MODE_VS_BOT;
        n = pong_write_join(out, sizeof out, 2, &j);
        if (write(fd, out, n) != (ssize_t)n) { printf("FAILED: write JOIN\n"); return 1; }
    }

    uint32_t start = now_ms();
    uint32_t last_input = 0, last_ping = 0;
    uint32_t input_seq = 0;

    int welcome = 0, match_start = 0, snapshots = 0, pongs = 0, events = 0;
    int views_valid = 0, frames = 0;
    int32_t min_ball_x = INT32_MAX, max_ball_x = INT32_MIN;
    int paddle_moved = 0;
    int32_t first_my_y = -1;

    while (now_ms() - start < (uint32_t)SECONDS * 1000) {
        uint32_t t = now_ms();

        /* ---- receive ---- */
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&p, 1, 5) > 0 && (p.revents & POLLIN)) {
            ssize_t r = read(fd, acc + acc_len, sizeof acc - acc_len);
            if (r <= 0) { printf("FAILED: connection closed\n"); return 1; }
            acc_len += (size_t)r;
        }

        size_t off = 0;
        while (off < acc_len) {
            PongFrame fr;
            size_t used = 0;
            PongParseResult pr = pong_parse_frame(acc + off, acc_len - off, &fr, &used);
            if (pr == PONG_PARSE_NEED_MORE) break;
            if (pr != PONG_PARSE_OK) { printf("FAILED: parse error %d\n", (int)pr); return 1; }

            switch (fr.type) {
            case PONG_MSG_WELCOME: welcome++; break;
            case PONG_MSG_MATCH_START: {
                PongMATCH_START m;
                if (pong_read_match_start(fr.payload, fr.length, &m)) {
                    pong_client_on_match_start(&c, &m);
                    match_start++;
                    printf("  MATCH_START: seat %s, first to %u%s\n",
                           m.your_side == 0 ? "LEFT" : "RIGHT", m.win_score,
                           (m.match_flags & PONG_MATCH_FLAG_SLOW_MODE) ? ", SLOW" : "");
                }
                break;
            }
            case PONG_MSG_SNAPSHOT: {
                PongSNAPSHOT s;
                if (pong_read_snapshot(fr.payload, fr.length, &s)) {
                    pong_client_on_snapshot(&c, &s, t);
                    snapshots++;
                }
                break;
            }
            case PONG_MSG_PONG: {
                PongPONG pg;
                if (pong_read_pong(fr.payload, fr.length, &pg)) {
                    pong_client_on_pong(&c, &pg, t);
                    pongs++;
                }
                break;
            }
            case PONG_MSG_EVENT: events++; break;
            default: break;
            }
            off += used;
        }
        if (off > 0) { memmove(acc, acc + off, acc_len - off); acc_len -= off; }

        /* ---- update (the console's own prediction + interpolation) ---- */
        PongView v;
        pong_client_update(&c, t, &v);
        frames++;
        if (v.valid) {
            views_valid++;
            if (v.ball_x < min_ball_x) min_ball_x = v.ball_x;
            if (v.ball_x > max_ball_x) max_ball_x = v.ball_x;
            /* Track the ball, as the 3DS input code does. */
            pong_client_set_target(&c, v.ball_y);
            if (first_my_y < 0) first_my_y = c.my_y;
            else if (c.my_y != first_my_y) paddle_moved = 1;
        }

        /* ---- send ---- */
        if (t - last_input >= 33) {
            last_input = t;
            PongINPUT in;
            memset(&in, 0, sizeof in);
            in.input_seq = ++input_seq;
            in.last_tick_seen = c.newest_tick;
            in.desired_yq4 = (int16_t)c.target_y;
            size_t n = pong_write_input(out, sizeof out, (uint16_t)input_seq, &in);
            if (write(fd, out, n) != (ssize_t)n) { printf("FAILED: write INPUT\n"); return 1; }
        }
        if (t - last_ping >= 2000) {
            last_ping = t;
            PongPING pg;
            memset(&pg, 0, sizeof pg);
            pg.client_time_ms = t;
            size_t n = pong_write_ping(out, sizeof out, 0, &pg);
            if (write(fd, out, n) != (ssize_t)n) { printf("FAILED: write PING\n"); return 1; }
        }
    }

    close(fd);

    /* ------------------------------------------------------------ report -- */
    printf("\n--- results over %ds ---\n", SECONDS);
    printf("WELCOME         : %d\n", welcome);
    printf("MATCH_START     : %d\n", match_start);
    printf("snapshots       : %d (%.1f/s)\n", snapshots, (double)snapshots / SECONDS);
    printf("PONGs           : %d\n", pongs);
    printf("EVENTs          : %d\n", events);
    printf("frames rendered : %d (%.0f fps equivalent)\n", frames, (double)frames / SECONDS);
    printf("valid views     : %d\n", views_valid);
    printf("min RTT         : %ums\n", c.min_rtt);
    printf("clock locked    : %s\n", c.have_offset ? "yes" : "no");
    printf("render delay    : %ums\n", c.render_delay_ms);
    printf("ball x range    : %d..%d Q4 (%d..%d px)\n",
           min_ball_x, max_ball_x, min_ball_x >> 4, max_ball_x >> 4);
    printf("score           : %u - %u\n",
           c.count ? c.ring[(c.head - 1 + PONG_CLIENT_RING) % PONG_CLIENT_RING].score_l : 0,
           c.count ? c.ring[(c.head - 1 + PONG_CLIENT_RING) % PONG_CLIENT_RING].score_r : 0);

    int fails = 0;
    #define REQUIRE(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

    REQUIRE(welcome >= 1, "no WELCOME -- the server did not greet us");
    REQUIRE(match_start >= 1, "no MATCH_START -- matchmaking did not seat us");
    REQUIRE(snapshots > SECONDS * 10, "too few snapshots -- state is not flowing");
    REQUIRE(pongs >= 1, "no PONG -- clock sync never completed");
    REQUIRE(c.have_offset, "clock never locked");
    REQUIRE(views_valid > frames / 2, "interpolator produced mostly invalid views");
    /* The ball must actually traverse the field, which proves interpolation is
     * producing motion rather than a frozen or clamped position. */
    REQUIRE(max_ball_x - min_ball_x > (200 << 4), "ball barely moved -- interpolation suspect");
    REQUIRE(paddle_moved, "our predicted paddle never moved");

    printf("\n");
    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASSED: the 3DS client code plays a real match\n");
    return 0;
}
