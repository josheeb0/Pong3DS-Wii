#ifndef PONG_MIXER_H
#define PONG_MIXER_H

/*
 * The part of playback that is the same everywhere.
 *
 * Every backend has to render the sounds once, keep them, and mix whatever is
 * currently sounding into an output buffer. Only the last step -- handing that
 * buffer to the hardware -- actually differs, so the rest lives here rather
 * than three times.
 *
 * Mixing matters because Pong overlaps sounds constantly: a paddle hit lands
 * while the previous wall bounce is still decaying, and a goal starts over
 * both. A backend that simply restarted one channel would cut the previous
 * sound off mid-swing, which is the same click the envelope exists to avoid.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "tone.h"

/** How many sounds may overlap. Beyond this the oldest is replaced. */
#define PONG_MIX_VOICES 4

typedef struct {
    int16_t *pcm[PONG_SFX_COUNT];
    size_t   len[PONG_SFX_COUNT];

    struct { const int16_t *pcm; size_t len, pos; bool live; } voice[PONG_MIX_VOICES];

    uint32_t next_voice;
    bool     ready;
} PongMixer;

/** Renders every sound into freshly allocated buffers. False if out of memory. */
bool pong_mixer_init(PongMixer *m);

void pong_mixer_exit(PongMixer *m);

/** Starts a sound on a free voice, stealing the oldest if none is free. */
void pong_mixer_play(PongMixer *m, PongSfx s);

/**
 * Mixes the live voices into `out`, overwriting it.
 *
 * Sums into 32-bit and clamps once at the end: summing into int16 wraps, and a
 * wrap is not a quieter sound, it is a full-scale inversion -- a loud crack
 * exactly when two things happen at once, which is exactly when Pong is
 * interesting.
 */
void pong_mixer_fill(PongMixer *m, int16_t *out, size_t frames);

/** True while any voice is still sounding, so a backend can idle. */
bool pong_mixer_active(const PongMixer *m);

#endif /* PONG_MIXER_H */
