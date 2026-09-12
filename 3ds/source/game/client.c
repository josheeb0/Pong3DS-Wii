#include "client.h"
#include <string.h>

#define FIELD_H_Q4    PONG_FIELD_H_Q4
#define PADDLE_HALF   ((PONG_PADDLE_H << PONG_Q4_SHIFT) / 2)

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * The paddle step, identical to shared/sim/paddle.ts and to the server.
 *
 * This is the contract that makes local prediction exact: the input is an
 * ABSOLUTE target and the clamp is deterministic, so running it here produces
 * the same number the server will. If it ever drifts from the TypeScript, the
 * symptom is your own paddle lagging and snapping under load.
 */
int32_t pong_step_paddle(int32_t current_q4, int32_t target_q4)
{
    const int32_t lo = PADDLE_HALF;
    const int32_t hi = FIELD_H_Q4 - PADDLE_HALF;
    int32_t target = clamp32(target_q4, lo, hi);
    int32_t delta = clamp32(target - current_q4,
                            -PONG_MAX_PADDLE_SPEED_Q4, PONG_MAX_PADDLE_SPEED_Q4);
    return clamp32(current_q4 + delta, lo, hi);
}

void pong_client_init(PongClient *c)
{
    memset(c, 0, sizeof *c);
    c->my_y = FIELD_H_Q4 / 2;
    c->my_server_y = FIELD_H_Q4 / 2;
    c->target_y = FIELD_H_Q4 / 2;
    c->win_score = PONG_WIN_SCORE;
    c->render_delay_ms = 100;
}

void pong_client_reset_match(PongClient *c)
{
    c->head = 0;
    c->count = 0;
    c->newest_tick = 0;
    c->gap_count = 0;
    c->gap_head = 0;
    c->last_arrival_ms = 0;
    c->my_server_tick = 0;
    c->snapshots_seen = 0;
}

void pong_client_set_target(PongClient *c, int32_t target_q4)
{
    c->target_y = clamp32(target_q4, PADDLE_HALF, FIELD_H_Q4 - PADDLE_HALF);
}

/* ---------------------------------------------------------------- snapshots */

void pong_client_on_snapshot(PongClient *c, const PongSNAPSHOT *s, uint32_t now_ms)
{
    /* Every transport can deliver duplicates or reorder. Inserting a snapshot
     * behind the newest would make the interpolator walk backwards. */
    if (c->count > 0 && s->tick <= c->newest_tick) return;

    if (c->last_arrival_ms != 0) {
        c->gaps[c->gap_head] = now_ms - c->last_arrival_ms;
        c->gap_head = (c->gap_head + 1) % 32;
        if (c->gap_count < 32) c->gap_count++;
    }
    c->last_arrival_ms = now_ms;

    PongSnap *e = &c->ring[c->head];
    e->tick = s->tick;
    e->ball_x = s->ball_xq4;
    e->ball_y = s->ball_yq4;
    e->ball_vx = s->ball_vxq4;
    e->ball_vy = s->ball_vyq4;
    e->left_y = s->left_yq4;
    e->right_y = s->right_yq4;
    e->score_l = s->score_l;
    e->score_r = s->score_r;
    e->state = s->state;
    e->flags = s->flags;
    e->arrived_ms = now_ms;

    c->head = (c->head + 1) % PONG_CLIENT_RING;
    if (c->count < PONG_CLIENT_RING) c->count++;
    c->newest_tick = s->tick;
    c->snapshots_seen++;

    /* Our own paddle's authoritative position, for reconciliation. */
    int32_t mine = (c->my_side == 0) ? s->left_yq4 : s->right_yq4;
    if (s->tick >= c->my_server_tick) {
        c->my_server_tick = s->tick;
        c->my_server_y = mine;
    }
}

void pong_client_on_match_start(PongClient *c, const PongMATCH_START *m)
{
    pong_client_reset_match(c);
    c->matched = true;
    c->my_side = m->your_side;
    c->win_score = m->win_score;
    c->slow_mode = (m->match_flags & PONG_MATCH_FLAG_SLOW_MODE) != 0;
    c->opp_platform = m->opp_platform;
    memcpy(c->opp_name, m->opp_name, PONG_NAME_BYTES);
    c->opp_name[PONG_NAME_BYTES] = '\0';
}

/* -------------------------------------------------------------------- clock */

/*
 * Minimum-RTT clock estimation.
 *
 * The least-queued exchange is the least distorted by buffering, which is
 * exactly the noise we are trying to see through. A mean would be dragged
 * around by it.
 */
void pong_client_on_pong(PongClient *c, const PongPONG *p, uint32_t now_ms)
{
    if (now_ms < p->client_time_ms) return;
    uint32_t rtt = now_ms - p->client_time_ms;
    if (rtt > 5000) return;

    /* Where the server's tick counter stood when the reply reached us, in Q8
     * ticks. TICK_MS is 1000/60; multiply before dividing to keep precision. */
    int32_t half_rtt_ticks_q8 = (int32_t)((rtt / 2) * PONG_TICK_HZ * 256 / 1000);
    int32_t server_tick_q8 = (int32_t)(p->server_tick * 256) + half_rtt_ticks_q8;
    int32_t local_tick_q8 = (int32_t)((uint64_t)now_ms * PONG_TICK_HZ * 256 / 1000);

    c->samples[c->sample_head].rtt = rtt;
    c->samples[c->sample_head].offset_q8 = server_tick_q8 - local_tick_q8;
    c->sample_head = (c->sample_head + 1) % PONG_CLOCK_SAMPLES;
    if (c->sample_count < PONG_CLOCK_SAMPLES) c->sample_count++;

    int best = 0;
    for (int i = 1; i < c->sample_count; i++) {
        if (c->samples[i].rtt < c->samples[best].rtt) best = i;
    }
    c->min_rtt = c->samples[best].rtt;

    if (!c->have_offset) {
        c->offset_q8 = c->samples[best].offset_q8;
        c->have_offset = true;
    } else {
        /* Ease toward the new estimate. A jump would make the render cursor
         * leap, which reads as a stutter even when the new estimate is better. */
        int32_t delta = c->samples[best].offset_q8 - c->offset_q8;
        if (delta > 16) delta = 16;
        if (delta < -16) delta = -16;
        c->offset_q8 += delta;
    }
}

int32_t pong_client_server_tick_q8(const PongClient *c, uint32_t now_ms)
{
    int32_t local_q8 = (int32_t)((uint64_t)now_ms * PONG_TICK_HZ * 256 / 1000);
    return local_q8 + c->offset_q8;
}

/* ------------------------------------------------------------- render delay */

uint32_t pong_client_render_delay_ms(const PongClient *c)
{
    if (c->gap_count < 4) return 100;

    /* Median of the observed arrival gaps. Derived from what we actually get
     * rather than the transport's nominal rate: a polling client receives
     * batched 30Hz snapshots but receives them in ~100ms bursts, so the buffer
     * must absorb the burst spacing, not the sample spacing. */
    uint32_t tmp[32];
    memcpy(tmp, c->gaps, sizeof(uint32_t) * (size_t)c->gap_count);
    for (int i = 1; i < c->gap_count; i++) {
        uint32_t k = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = k;
    }
    uint32_t median = tmp[c->gap_count / 2];
    uint32_t delay = median * 2 + 20;
    if (delay < 50) delay = 50;
    if (delay > 250) delay = 250;
    return delay;
}

/* ------------------------------------------------------------ interpolation */

static const PongSnap *ring_at(const PongClient *c, int i)
{
    int idx = (c->head - c->count + i + PONG_CLIENT_RING * 2) % PONG_CLIENT_RING;
    return &c->ring[idx];
}

static int32_t lerp32(int32_t a, int32_t b, int32_t t_q8)
{
    return a + (int32_t)(((int64_t)(b - a) * t_q8) >> 8);
}

/*
 * How far past the newest snapshot we are willing to predict, in Q8 ticks.
 *
 * 9 ticks is 150ms. Beyond that a prediction is more likely to be wrong than
 * useful -- the ball may have hit a paddle we have not heard about -- and the
 * correction when the truth arrives is worse than the stutter it was hiding.
 */
#define EXTRAPOLATE_MAX_Q8 (9 * 256)

/*
 * Folds a predicted Y back inside the walls, matching the server's reflection.
 *
 * A loop rather than a single fold: a long prediction at a steep angle can
 * cross both walls, and folding once would leave the ball outside the field,
 * which is far more visible than the stutter this exists to remove.
 */
static int32_t reflect_y(int32_t y)
{
    const int32_t lo = PONG_BALL_R << PONG_Q4_SHIFT;
    const int32_t hi = PONG_FIELD_H_Q4 - lo;
    if (hi <= lo) return y;
    for (int guard = 0; guard < 8; guard++) {
        if (y < lo)      y = lo + (lo - y);
        else if (y > hi) y = hi - (y - hi);
        else break;
    }
    return clamp32(y, lo, hi);
}

void pong_client_update(PongClient *c, uint32_t now_ms, PongView *out)
{
    memset(out, 0, sizeof *out);

    int32_t server_q8 = pong_client_server_tick_q8(c, now_ms);

    /* --- our paddle: predicted forward from the last authoritative tick ---- */
    int32_t ticks_since_q8 = server_q8 - (int32_t)(c->my_server_tick * 256);
    if (ticks_since_q8 < 0) ticks_since_q8 = 0;
    if (ticks_since_q8 > 64 * 256) ticks_since_q8 = 64 * 256;

    int32_t predicted = c->my_server_y;
    int whole = ticks_since_q8 >> 8;
    for (int i = 0; i < whole; i++) predicted = pong_step_paddle(predicted, c->target_y);
    int32_t frac_q8 = ticks_since_q8 & 0xff;
    if (frac_q8 > 0) {
        const int32_t lo = PADDLE_HALF;
        const int32_t hi = FIELD_H_Q4 - PADDLE_HALF;
        int32_t target = clamp32(c->target_y, lo, hi);
        int32_t max_step = (PONG_MAX_PADDLE_SPEED_Q4 * frac_q8) >> 8;
        int32_t delta = clamp32(target - predicted, -max_step, max_step);
        predicted = clamp32(predicted + delta, lo, hi);
    }

    /* Reconcile: snap on a large error (the server rejected or clamped
     * something), smooth a small one (sub-tick noise). */
    int32_t err = predicted - c->my_y;
    if (err > 64 || err < -64) c->my_y = predicted;
    else c->my_y += err / 4;

    /* --- everything else: sampled in the past ----------------------------- */
    c->render_delay_ms = pong_client_render_delay_ms(c);
    c->arrival_gap_ms = (c->gap_count > 0) ? c->gaps[(c->gap_head + 31) % 32] : 0;

    if (c->count == 0) {
        out->valid = false;
        out->left_y = out->right_y = FIELD_H_Q4 / 2;
        return;
    }

    int32_t delay_ticks_q8 = (int32_t)(c->render_delay_ms * PONG_TICK_HZ * 256 / 1000);
    int32_t cursor_q8 = server_q8 - delay_ticks_q8;

    const PongSnap *oldest = ring_at(c, 0);
    const PongSnap *newest = ring_at(c, c->count - 1);
    int32_t oldest_q8 = (int32_t)(oldest->tick * 256);
    int32_t newest_q8 = (int32_t)(newest->tick * 256);

    out->starved = cursor_q8 > newest_q8 + 128;

    /*
     * Running past the newest snapshot used to clamp the cursor, which froze
     * the ball until the next packet and then jumped it to wherever it had got
     * to. On a bursty transport that is most of the visible motion, and it is
     * what "jagged" actually looks like: hold, jump, hold, jump.
     *
     * The ball is the one thing we can honestly predict. Between contacts it is
     * linear motion plus wall reflection and nothing else, and every snapshot
     * carries its velocity, so extrapolating forward reproduces exactly what
     * the server is doing rather than inventing plausible motion.
     *
     * Paddles are deliberately NOT extrapolated. A paddle's velocity says
     * nothing about whether the player is about to stop, so predicting it
     * overshoots and then snaps back -- worse than being slightly stale, which
     * nobody can see.
     */
    int32_t ahead_q8 = cursor_q8 - newest_q8;
    if (ahead_q8 > EXTRAPOLATE_MAX_Q8) ahead_q8 = EXTRAPOLATE_MAX_Q8;

    if (cursor_q8 < oldest_q8) cursor_q8 = oldest_q8;
    if (cursor_q8 > newest_q8) cursor_q8 = newest_q8;

    const PongSnap *a = oldest;
    const PongSnap *b = newest;
    for (int i = 0; i < c->count; i++) {
        const PongSnap *s = ring_at(c, i);
        int32_t s_q8 = (int32_t)(s->tick * 256);
        if (s_q8 <= cursor_q8) a = s;
        if (s_q8 >= cursor_q8) { b = s; break; }
    }

    int32_t span = (int32_t)(b->tick * 256) - (int32_t)(a->tick * 256);
    int32_t t_q8 = (span > 0) ? (((cursor_q8 - (int32_t)(a->tick * 256)) << 8) / span) : 0;
    if (t_q8 < 0) t_q8 = 0;
    if (t_q8 > 256) t_q8 = 256;

    /* A keyframe marks a discontinuity -- a serve, a goal, the end of a
     * countdown. Interpolating across one would drag the ball smoothly from
     * where it died to where it was re-served, which reads as a glitch. */
    bool crosses_keyframe = (b->flags & PONG_SNAP_FLAG_KEYFRAME) != 0 && span > 0;

    if (crosses_keyframe) {
        out->ball_x = b->ball_x;
        out->ball_y = b->ball_y;
    } else {
        out->ball_x = lerp32(a->ball_x, b->ball_x, t_q8);
        out->ball_y = lerp32(a->ball_y, b->ball_y, t_q8);
    }

    /* Past the end of what we hold, carry the ball forward ourselves. Only
     * while actually playing: during a serve or a goal the stored velocity
     * describes a ball that is not moving yet. */
    if (ahead_q8 > 0 && newest->state == PONG_MATCH_STATE_PLAY &&
        !(newest->flags & PONG_SNAP_FLAG_KEYFRAME)) {
        out->ball_x = newest->ball_x + (((int32_t)newest->ball_vx * ahead_q8) >> 8);
        out->ball_y = newest->ball_y + (((int32_t)newest->ball_vy * ahead_q8) >> 8);
        out->ball_y = reflect_y(out->ball_y);
        out->extrapolated = true;
    }
    out->left_y = lerp32(a->left_y, b->left_y, t_q8);
    out->right_y = lerp32(a->right_y, b->right_y, t_q8);
    out->score_l = b->score_l;
    out->score_r = b->score_r;
    out->state = b->state;
    out->flags = b->flags;
    out->valid = true;

    /* Our own paddle is drawn from local prediction, never the interpolated
     * value -- using the latter would add the render delay to our OWN input,
     * which is the lag a player notices first. */
    if (c->my_side == 0) out->left_y = c->my_y;
    else out->right_y = c->my_y;
}
