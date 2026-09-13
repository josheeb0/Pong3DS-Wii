#include "audio.h"
#include "mixer.h"

#include <3ds.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

/*
 * 3DS output, via ndsp.
 *
 * ndsp does NOT pull through a callback the way SDL does. It plays wave buffers
 * that must live in LINEAR memory -- memory the DSP can reach -- and it is the
 * caller's job to keep queueing them. So this keeps two buffers in flight and
 * refills whichever the DSP has finished with, which pong_audio_update() does
 * once a frame from the game loop.
 *
 * linearAlloc rather than malloc is not a preference. A wave buffer in ordinary
 * heap memory is not visible to the DSP, and the symptom is silence with no
 * error anywhere -- ndsp reports success and plays nothing.
 */

#define BLOCK_FRAMES 1024   /* ~21ms at 48kHz: short enough to feel immediate */

static ndspWaveBuf s_wave[2];
static int16_t    *s_block[2];
static PongMixer   s_mix;
static bool        s_enabled = true;
static bool        s_ready;

bool pong_audio_init(void)
{
    if (s_ready) return true;

    if (R_FAILED(ndspInit())) {
        /* A console with no sound still plays Pong. */
        return false;
    }

    if (!pong_mixer_init(&s_mix)) { ndspExit(); return false; }

    ndspSetOutputMode(NDSP_OUTPUT_MONO);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);
    ndspChnSetRate(0, (float)PONG_TONE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof mix);
    mix[0] = 1.0f;   /* front left  */
    mix[1] = 1.0f;   /* front right */
    ndspChnSetMix(0, mix);

    for (int i = 0; i < 2; i++) {
        s_block[i] = (int16_t *)linearAlloc(BLOCK_FRAMES * sizeof(int16_t));
        if (!s_block[i]) {
            for (int k = 0; k < i; k++) linearFree(s_block[k]);
            pong_mixer_exit(&s_mix);
            ndspExit();
            return false;
        }
        memset(s_block[i], 0, BLOCK_FRAMES * sizeof(int16_t));
        memset(&s_wave[i], 0, sizeof s_wave[i]);
        s_wave[i].data_vaddr = s_block[i];
        s_wave[i].nsamples   = BLOCK_FRAMES;
        s_wave[i].status     = NDSP_WBUF_DONE;
    }

    s_ready = true;
    return true;
}

void pong_audio_exit(void)
{
    if (!s_ready) return;
    ndspChnWaveBufClear(0);
    for (int i = 0; i < 2; i++) linearFree(s_block[i]);
    pong_mixer_exit(&s_mix);
    ndspExit();
    s_ready = false;
}

/*
 * Refills any finished buffer and requeues it. Called once a frame.
 *
 * Silence is still queued when nothing is playing: stopping the channel and
 * restarting it on the next hit costs a wave buffer of latency exactly when the
 * sound needs to be immediate, and an idle DSP channel costs nothing.
 */
void pong_audio_update(void)
{
    if (!s_ready) return;

    for (int i = 0; i < 2; i++) {
        if (s_wave[i].status != NDSP_WBUF_DONE &&
            s_wave[i].status != NDSP_WBUF_FREE) continue;

        pong_mixer_fill(&s_mix, s_block[i], BLOCK_FRAMES);
        /* The DSP reads this memory directly, so it has to be flushed out of
         * the CPU's cache first or it plays whatever was there before. */
        DSP_FlushDataCache(s_block[i], BLOCK_FRAMES * sizeof(int16_t));
        ndspChnWaveBufAdd(0, &s_wave[i]);
    }
}

void pong_audio_play(PongSfx s)
{
    if (!s_ready || !s_enabled) return;
    pong_mixer_play(&s_mix, s);
}

void pong_audio_set_enabled(bool on) { s_enabled = on; }
bool pong_audio_enabled(void) { return s_enabled; }
