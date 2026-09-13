#include "mixer.h"

#include <stdlib.h>
#include <string.h>

bool pong_mixer_init(PongMixer *m)
{
    memset(m, 0, sizeof *m);

    for (int i = 0; i < PONG_SFX_COUNT; i++) {
        size_t n = pong_tone_samples((PongSfx)i);
        m->pcm[i] = (int16_t *)malloc(n * sizeof(int16_t));
        if (!m->pcm[i]) { pong_mixer_exit(m); return false; }
        m->len[i] = pong_tone_render((PongSfx)i, m->pcm[i], n);
        if (m->len[i] == 0) { pong_mixer_exit(m); return false; }
    }

    m->ready = true;
    return true;
}

void pong_mixer_exit(PongMixer *m)
{
    for (int i = 0; i < PONG_SFX_COUNT; i++) {
        free(m->pcm[i]);
        m->pcm[i] = NULL;
        m->len[i] = 0;
    }
    m->ready = false;
}

void pong_mixer_play(PongMixer *m, PongSfx s)
{
    if (!m->ready || s < 0 || s >= PONG_SFX_COUNT) return;

    int slot = -1;
    for (int i = 0; i < PONG_MIX_VOICES; i++) {
        if (!m->voice[i].live) { slot = i; break; }
    }
    /* All busy: take them round-robin rather than always slot 0, so a burst
     * steals each voice once instead of retriggering one of them repeatedly. */
    if (slot < 0) slot = (int)(m->next_voice++ % PONG_MIX_VOICES);

    m->voice[slot].pcm  = m->pcm[s];
    m->voice[slot].len  = m->len[s];
    m->voice[slot].pos  = 0;
    m->voice[slot].live = true;
}

void pong_mixer_fill(PongMixer *m, int16_t *out, size_t frames)
{
    memset(out, 0, frames * sizeof(int16_t));
    if (!m->ready) return;

    for (int v = 0; v < PONG_MIX_VOICES; v++) {
        if (!m->voice[v].live) continue;

        size_t left = m->voice[v].len - m->voice[v].pos;
        size_t take = left < frames ? left : frames;
        const int16_t *src = m->voice[v].pcm + m->voice[v].pos;

        for (size_t i = 0; i < take; i++) {
            /* 32-bit sum, clamped once below. Summing into int16 would wrap,
             * and a wrap is not a quieter sound -- it is a full-scale
             * inversion, heard as a crack exactly when two things coincide. */
            int32_t mixed = (int32_t)out[i] + (int32_t)src[i];
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            out[i] = (int16_t)mixed;
        }

        m->voice[v].pos += take;
        if (m->voice[v].pos >= m->voice[v].len) m->voice[v].live = false;
    }
}

bool pong_mixer_active(const PongMixer *m)
{
    for (int v = 0; v < PONG_MIX_VOICES; v++) if (m->voice[v].live) return true;
    return false;
}
