/*
 * Client-side netcode: snapshot history, interpolation, prediction, clock sync.
 *
 * A direct transcription of web/src/game/interp.ts and the prediction in
 * web/src/game/client.ts. Keeping the two implementations structurally
 * identical is deliberate -- they must agree with the same server, and a
 * reviewer should be able to read them side by side.
 *
 * Pure C99 with no libctru dependency beyond the caller supplying a millisecond
 * clock, so it compiles in the host test alongside the codec.
 */

#ifndef PONG_CLIENT_H
#define PONG_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "pong_proto.h"

/* One second of 30Hz history: enough to bracket any render delay we use. */
/* Client-side history depth. Distinct from PONG_SNAP_RING, which is the
 * server's ring size -- one second of 30Hz here is enough to bracket any
 * render delay we use. */
#define PONG_CLIENT_RING 48
#define PONG_CLOCK_SAMPLES 16

typedef struct {
    uint32_t tick;
    int16_t  ball_x, ball_y;
    int16_t  ball_vx, ball_vy;
    int16_t  left_y, right_y;
    uint8_t  score_l, score_r;
    uint8_t  state;
    uint8_t  flags;
    uint32_t arrived_ms;   /* local arrival, for measuring real cadence */
} PongSnap;

/* Interpolated world state, ready to draw. */
typedef struct {
    int32_t ball_x, ball_y;      /* Q4 */
    int32_t left_y, right_y;     /* Q4 */
    uint8_t score_l, score_r;
    uint8_t state;
    uint8_t flags;
    bool    starved;             /* ran past the newest snapshot */
    bool    extrapolated;        /* ball position is predicted, not interpolated */
    bool    valid;
} PongView;

typedef struct {
    uint32_t rtt;
    int32_t  offset_q8;  /* (server_tick - local_tick) in Q8 ticks */
} PongClockSample;

typedef struct {
    /* --- snapshot ring --- */
    PongSnap ring[PONG_CLIENT_RING];
    int      head;
    int      count;
    uint32_t newest_tick;

    /* --- arrival cadence, for sizing the render delay --- */
    uint32_t gaps[32];
    int      gap_count;
    int      gap_head;
    uint32_t last_arrival_ms;

    /* --- clock --- */
    PongClockSample samples[PONG_CLOCK_SAMPLES];
    int      sample_count;
    int      sample_head;
    int32_t  offset_q8;
    bool     have_offset;
    uint32_t min_rtt;

    /* --- our own paddle --- */
    uint8_t  my_side;
    int32_t  my_y;          /* Q4, locally predicted, what we draw */

    /*
     * Visual reconciliation for the ball.
     *
     * The ball is drawn at (authoritative + error), where the error is what was
     * left over when the last correction landed and is worked off over a few
     * frames. Without it, every arriving snapshot moves the ball instantly to
     * wherever the server says it is -- correct, and visible as a jolt at the
     * rate packets arrive, which is what a high-ping client actually sees.
     */
    int32_t  ball_off_x, ball_off_y;   /* Q4, decaying toward zero */
    int32_t  vis_x, vis_y;             /* Q4, what we drew last frame */
    uint32_t vis_basis_tick;           /* newest tick the drawn ball was built on */
    bool     vis_valid;
    int32_t  my_server_y;   /* Q4, last authoritative */
    uint32_t my_server_tick;
    int32_t  target_y;      /* Q4, what we ask the server for */

    /* --- match --- */
    bool     matched;
    uint8_t  win_score;
    bool     slow_mode;
    uint8_t  opp_platform;
    char     opp_name[PONG_NAME_BYTES + 1];

    /* --- stats for the bottom screen --- */
    uint32_t snapshots_seen;
    uint32_t render_delay_ms;

    /* Snapshots actually received per second -- the number that says whether
     * the server is batching history or sending one state per poll. Distinct
     * from the transport's request rate, which is what the HUD used to show. */
    uint32_t snap_hz;
    uint32_t snap_count;
    uint32_t snap_window_ms;
    uint32_t arrival_gap_ms;
} PongClient;

void pong_client_init(PongClient *c);
void pong_client_reset_match(PongClient *c);

/** Feeds one decoded snapshot. `now_ms` is the local clock. */
void pong_client_on_snapshot(PongClient *c, const PongSNAPSHOT *s, uint32_t now_ms);

/** Feeds a PONG for clock sync. */
void pong_client_on_pong(PongClient *c, const PongPONG *p, uint32_t now_ms);

/** Feeds MATCH_START. */
void pong_client_on_match_start(PongClient *c, const PongMATCH_START *m);

/** Estimated server tick right now, Q8 fixed point. */
int32_t pong_client_server_tick_q8(const PongClient *c, uint32_t now_ms);

/** How far behind live to render, from the cadence we actually observe. */
uint32_t pong_client_render_delay_ms(const PongClient *c);

/**
 * Advances local prediction and produces the frame to draw.
 * Call once per rendered frame.
 */
void pong_client_update(PongClient *c, uint32_t now_ms, PongView *out);

/** Sets the desired paddle centre, clamped to the field. */
void pong_client_set_target(PongClient *c, int32_t target_q4);

/** The paddle step the server also runs. Must stay identical to it. */
int32_t pong_step_paddle(int32_t current_q4, int32_t target_q4);

#endif /* PONG_CLIENT_H */
