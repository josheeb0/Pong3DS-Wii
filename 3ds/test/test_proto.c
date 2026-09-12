/*
 * Host-side codec conformance test.
 *
 * Compiles 3ds/source/net/pong_proto.c with the HOST compiler (clang/gcc) and
 * checks it against byte vectors produced by the TypeScript encoder. This is
 * how C<->TS wire compatibility is proven on a laptop, with no 3DS and no
 * devkitARM in the loop.
 *
 * It also structurally enforces the Wii seam: pong_proto.c must not include any
 * libctru header, or this translation unit would not build here at all.
 *
 *   make -C 3ds/test -f Makefile.host run
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../source/net/pong_proto.h"
#include "golden_data.h"

static int failures = 0;

#define CHECK(cond, ...)                        \
    do {                                        \
        if (!(cond)) {                          \
            printf("  FAIL: ");                 \
            printf(__VA_ARGS__);                \
            printf("\n");                       \
            failures++;                         \
        }                                       \
    } while (0)

/* ------------------------------------------------------------------------- */
/* 1. Golden vectors: every field of every message, against the TS encoder.    */
static void test_golden(void)
{
    printf("golden vectors (%d messages)\n", PONG_GOLDEN_COUNT);
    int rc = pong_golden_check_all();
    CHECK(rc == 0, "golden vector mismatch (see FAIL lines above)");
    if (rc == 0) printf("  ok: all %d messages match TypeScript byte-for-byte\n", PONG_GOLDEN_COUNT);
}

/* ------------------------------------------------------------------------- */
/* 2. Framing: the property that lets one protocol ride every transport.       */
static void test_stream_framing(void)
{
    printf("stream framing\n");

    /* Three frames concatenated, exactly as they arrive on a TCP stream or in
     * one HTTP response body. */
    uint8_t stream[256];
    size_t n = 0;

    PongINPUT in = { .input_seq = 7, .last_tick_seen = 1234, .desired_yq4 = -500, .buttons = 1, .flags = 0 };
    PongPING pg = { .client_time_ms = 99, .echo_server_time_ms = 5 };
    PongLEAVE lv = { 0 };

    n += pong_write_input(stream + n, sizeof stream - n, 1, &in);
    n += pong_write_ping(stream + n, sizeof stream - n, 2, &pg);
    n += pong_write_leave(stream + n, sizeof stream - n, 3, &lv);

    size_t off = 0;
    int count = 0;
    uint8_t types[8];
    while (off < n) {
        PongFrame fr;
        size_t used = 0;
        PongParseResult r = pong_parse_frame(stream + off, n - off, &fr, &used);
        CHECK(r == PONG_PARSE_OK, "parse frame %d returned %d", count, (int)r);
        if (r != PONG_PARSE_OK) break;
        types[count++] = fr.type;
        off += used;
    }
    CHECK(count == 3, "expected 3 frames, got %d", count);
    CHECK(off == n, "consumed %zu of %zu bytes", off, n);
    CHECK(types[0] == PONG_MSG_INPUT && types[1] == PONG_MSG_PING && types[2] == PONG_MSG_LEAVE,
          "frame order wrong");

    /* Signed round-trip: a negative paddle target must survive as negative.
     * Getting this wrong (reading i16 as u16) is a classic and would show up as
     * the paddle teleporting to the bottom of the field. */
    PongFrame fr;
    size_t used = 0;
    PongINPUT back;
    pong_parse_frame(stream, n, &fr, &used);
    CHECK(pong_read_input(fr.payload, fr.length, &back), "decode INPUT");
    CHECK(back.desired_yq4 == -500, "signed i16 round-trip: got %d want -500", back.desired_yq4);
    CHECK(back.last_tick_seen == 1234, "u32 round-trip: got %u", back.last_tick_seen);

    if (failures == 0) printf("  ok: 3 frames popped from one buffer, signed fields intact\n");
}

/* ------------------------------------------------------------------------- */
/* 3. Partial frames: a TCP read can split anywhere. The parser must say       */
/*    NEED_MORE rather than misparsing, for every possible split point.        */
static void test_partial_frames(void)
{
    printf("partial frame handling\n");

    uint8_t buf[64];
    PongSNAPSHOT s = {
        .tick = 4242, .ack_input_seq = 11,
        .ball_xq4 = 1000, .ball_yq4 = -2000, .ball_vxq4 = 80, .ball_vyq4 = -30,
        .left_yq4 = 3840, .right_yq4 = 3840,
        .score_l = 3, .score_r = 5, .state = 2, .flags = 1,
    };
    size_t n = pong_write_snapshot(buf, sizeof buf, 0, &s);
    CHECK(n == PONG_HEADER_BYTES + PONG_SNAPSHOT_BYTES, "snapshot framed size %zu", n);

    int bad = 0;
    for (size_t cut = 0; cut < n; cut++) {
        PongFrame fr;
        size_t used = 0;
        if (pong_parse_frame(buf, cut, &fr, &used) != PONG_PARSE_NEED_MORE) bad++;
    }
    CHECK(bad == 0, "%d of %zu truncations did not report NEED_MORE", bad, n);

    PongFrame fr; size_t used = 0;
    CHECK(pong_parse_frame(buf, n, &fr, &used) == PONG_PARSE_OK, "full frame should parse");

    PongSNAPSHOT back;
    CHECK(pong_read_snapshot(fr.payload, fr.length, &back), "decode SNAPSHOT");
    CHECK(back.ball_yq4 == -2000, "negative ballY: got %d", back.ball_yq4);
    CHECK(back.ball_vyq4 == -30, "negative ballVY: got %d", back.ball_vyq4);
    CHECK(back.tick == 4242, "tick: got %u", back.tick);

    if (failures == 0) printf("  ok: all %zu truncation points report NEED_MORE\n", n);
}

/* ------------------------------------------------------------------------- */
/* 4. Malformed input must be rejected, not trusted. This runs against bytes   */
/*    arriving from the network, so it is a security boundary, not a nicety.   */
static void test_rejects_garbage(void)
{
    printf("malformed input rejection\n");

    uint8_t buf[64];
    PongPING p = { .client_time_ms = 1, .echo_server_time_ms = 2 };
    size_t n = pong_write_ping(buf, sizeof buf, 0, &p);

    PongFrame fr; size_t used;

    uint8_t bad_magic[64]; memcpy(bad_magic, buf, n); bad_magic[0] = 'X';
    CHECK(pong_parse_frame(bad_magic, n, &fr, &used) == PONG_PARSE_BAD_MAGIC, "bad magic not caught");

    uint8_t bad_ver[64]; memcpy(bad_ver, buf, n); bad_ver[2] = 99;
    CHECK(pong_parse_frame(bad_ver, n, &fr, &used) == PONG_PARSE_BAD_VERSION, "bad version not caught");

    /* Length that disagrees with the declared type: the type table catches it. */
    uint8_t bad_len[64]; memcpy(bad_len, buf, n); bad_len[4] = 99;
    CHECK(pong_parse_frame(bad_len, n, &fr, &used) == PONG_PARSE_BAD_LENGTH, "type/length mismatch not caught");

    /* A decoder handed the wrong length must refuse rather than read past. */
    PongPING out;
    CHECK(!pong_read_ping(buf + PONG_HEADER_BYTES, 3, &out), "short payload not rejected");

    /* Writers must refuse an undersized destination rather than overflow it. */
    uint8_t tiny[4];
    CHECK(pong_write_ping(tiny, sizeof tiny, 0, &p) == 0, "undersized buffer not refused");

    if (failures == 0) printf("  ok: magic, version, length and capacity all enforced\n");
}

/* ------------------------------------------------------------------------- */
/* 5. The constants the 3DS renderer depends on.                               */
static void test_constants(void)
{
    printf("constants and coordinate mapping\n");

    CHECK(PONG_FIELD_W_Q4 == 12800, "FIELD_W_Q4 = %d", PONG_FIELD_W_Q4);
    CHECK(PONG_FIELD_H_Q4 == 7680, "FIELD_H_Q4 = %d", PONG_FIELD_H_Q4);
    CHECK(PONG_FIELD_W_Q4 < 32767 && PONG_FIELD_H_Q4 < 32767, "field must fit in i16");

    /* The 800x480 -> 400x240 mapping must be exact at both extremes, which is
     * the whole reason the field is 2x the top screen. */
    CHECK(PONG_Q4_TO_TOPSCREEN(0) == 0, "origin maps to 0");
    CHECK(PONG_Q4_TO_TOPSCREEN(PONG_FIELD_W_Q4) == 400, "right edge -> %d, want 400",
          PONG_Q4_TO_TOPSCREEN(PONG_FIELD_W_Q4));
    CHECK(PONG_Q4_TO_TOPSCREEN(PONG_FIELD_H_Q4) == 240, "bottom edge -> %d, want 240",
          PONG_Q4_TO_TOPSCREEN(PONG_FIELD_H_Q4));

    /* Max ball step must stay under one diameter or a per-tick test can tunnel. */
    CHECK(PONG_BALL_SPEED_MAX_Q4 < (PONG_BALL_R * 2) << PONG_Q4_SHIFT,
          "max ball speed %d >= diameter %d (tunneling risk)",
          PONG_BALL_SPEED_MAX_Q4, (PONG_BALL_R * 2) << PONG_Q4_SHIFT);

    if (failures == 0) printf("  ok: field fits i16, screen mapping exact, no tunneling\n");
}

int main(void)
{
    printf("=== pong_proto host conformance test ===\n\n");
    test_golden();
    test_stream_framing();
    test_partial_frames();
    test_rejects_garbage();
    test_constants();
    printf("\n");
    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASSED: C codec agrees with TypeScript\n");
    return 0;
}
