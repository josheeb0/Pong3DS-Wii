#ifndef PONG_TONE_H
#define PONG_TONE_H

/*
 * The sound design, shared by every platform.
 *
 * The game was silent on all five clients. Rather than ship sample files --
 * an asset pipeline, six more things in the .3dsx, and a licence question --
 * the blips are SYNTHESISED from a table of specs, in integer arithmetic, by
 * code every platform compiles. A paddle hit therefore sounds identical on a
 * 3DS and in a browser because it is the same arithmetic, not because two
 * people exported the same wav.
 *
 * Backends render each sound ONCE at startup and keep the PCM. They are a few
 * kilobytes each and never change, so synthesising per-hit would be work done
 * sixty times a rally for no reason.
 */

#include <stdint.h>
#include <stddef.h>

/** One rate everywhere, chosen because the Vita's audio port wants 48kHz. */
#define PONG_TONE_RATE 48000

typedef enum {
    PONG_WAVE_SQUARE = 0,   /* hard and percussive: hits */
    PONG_WAVE_TRIANGLE,     /* softer: countdown, results */
} PongWave;

typedef struct {
    uint16_t freq_start;   /* Hz at the start */
    uint16_t freq_end;     /* Hz at the end; sweeps linearly if different */
    uint16_t ms;
    uint8_t  wave;         /* PongWave */
    uint8_t  volume;       /* 0..255 of full scale */
} PongToneSpec;

/** The sounds the game makes. */
typedef enum {
    PONG_SFX_PADDLE = 0,
    PONG_SFX_WALL,
    PONG_SFX_GOAL,
    PONG_SFX_COUNTDOWN,
    PONG_SFX_WIN,
    PONG_SFX_LOSE,
    PONG_SFX_COUNT,
} PongSfx;

/** The spec for each sound. */
extern const PongToneSpec PONG_TONE[PONG_SFX_COUNT];

/** Samples a sound occupies, for sizing a buffer before rendering it. */
size_t pong_tone_samples(PongSfx s);

/**
 * Renders one sound as mono signed 16-bit PCM at PONG_TONE_RATE.
 *
 * Returns the samples written, which is zero if the buffer is too small.
 * Deterministic and integer-only, so the host test can assert on the waveform
 * itself rather than on whether a speaker made a noise.
 */
size_t pong_tone_render(PongSfx s, int16_t *out, size_t cap);

#endif /* PONG_TONE_H */
