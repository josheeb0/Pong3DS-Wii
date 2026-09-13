#include "tone.h"

/*
 * The sounds.
 *
 * Tuned to be told apart with the console's speaker at half volume in a room
 * with other noise, which is the actual listening condition. That means short,
 * and it means the two frequent sounds -- paddle and wall -- sit far enough
 * apart in pitch to be distinguishable when they happen a fifth of a second
 * from each other.
 *
 * Everything sweeps rather than holding a pitch. A held square wave reads as a
 * beep from a menu; a swept one reads as an impact, which is what these are.
 */
const PongToneSpec PONG_TONE[PONG_SFX_COUNT] = {
    /* Paddle: bright, upward, very short. The one you hear most. */
    [PONG_SFX_PADDLE]    = { 440, 660,  55, PONG_WAVE_SQUARE,   200 },
    /* Wall: lower and downward, so a rally off the rails does not sound like
     * a return you did not make. */
    [PONG_SFX_WALL]      = { 300, 220,  45, PONG_WAVE_SQUARE,   160 },
    /* Goal: a long fall. Unmistakably an ending. */
    [PONG_SFX_GOAL]      = { 520, 130, 260, PONG_WAVE_SQUARE,   190 },
    /* Countdown: a flat, soft tick that does not compete with the music of
     * the rally about to start. */
    [PONG_SFX_COUNTDOWN] = { 880, 880,  70, PONG_WAVE_TRIANGLE, 140 },
    /* Win and lose are the same shape in opposite directions, which is the
     * clearest way to make two results feel like a pair. */
    [PONG_SFX_WIN]       = { 440, 990, 420, PONG_WAVE_TRIANGLE, 200 },
    [PONG_SFX_LOSE]      = { 440, 160, 480, PONG_WAVE_TRIANGLE, 190 },
};

size_t pong_tone_samples(PongSfx s)
{
    if (s < 0 || s >= PONG_SFX_COUNT) return 0;
    return (size_t)PONG_TONE[s].ms * (PONG_TONE_RATE / 1000);
}

/*
 * Envelope, in Q8.
 *
 * A short attack so the sound starts at once, and a decay to EXACTLY zero by
 * the final sample. The end matters: a waveform cut off mid-swing steps
 * straight from full amplitude to silence, and that discontinuity is an
 * audible click on every platform. It is the classic bug in synthesised
 * blips, and it is only avoidable by construction.
 */
static int32_t envelope_q8(size_t i, size_t total)
{
    const size_t attack = total / 16 ? total / 16 : 1;
    if (i < attack) return (int32_t)((i * 256) / attack);

    size_t left = total - i;          /* total..1 */
    size_t span = total - attack;
    if (span == 0) return 0;
    return (int32_t)((left * 256) / span);
}

size_t pong_tone_render(PongSfx s, int16_t *out, size_t cap)
{
    if (s < 0 || s >= PONG_SFX_COUNT || !out) return 0;

    const PongToneSpec *t = &PONG_TONE[s];
    const size_t n = pong_tone_samples(s);
    if (n == 0 || cap < n) return 0;

    /* Phase in Q16 of a full period, so the sweep needs no floating point. */
    uint32_t phase = 0;

    for (size_t i = 0; i < n; i++) {
        /* Linear sweep from start to end across the whole sound. */
        int32_t f = (int32_t)t->freq_start +
                    (int32_t)(((int32_t)t->freq_end - (int32_t)t->freq_start) *
                              (int32_t)i) / (int32_t)n;
        if (f < 1) f = 1;

        uint32_t step = (uint32_t)(((uint64_t)f << 16) / PONG_TONE_RATE);
        phase = (phase + step) & 0xFFFF;

        int32_t sample;
        if (t->wave == PONG_WAVE_SQUARE) {
            sample = (phase < 0x8000) ? 32767 : -32767;
        } else {
            /* Triangle: up for half a period, down for the other half. */
            int32_t p = (int32_t)phase;
            sample = (p < 0x8000) ? (p * 4 - 0x10000) : (0x30000 - p * 4);
            if (sample > 32767) sample = 32767;
            if (sample < -32767) sample = -32767;
        }

        int32_t v = (sample * (int32_t)t->volume) / 255;
        v = (v * envelope_q8(i, n)) / 256;

        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }

    return n;
}
