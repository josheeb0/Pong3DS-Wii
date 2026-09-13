#include "audio.h"
#include "mixer.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

/*
 * Vita output, via sceAudioOut.
 *
 * sceAudioOutOutput BLOCKS until the port wants more, which makes it a natural
 * fit for a dedicated thread and a terrible fit for the game loop -- calling it
 * from there would pace the whole game at the audio buffer rate.
 *
 * The grain is fixed at open time and the port wants a specific one; 1024 is
 * accepted at 48kHz, which is also about 21ms, so a sound starts within a
 * frame of the hit that caused it.
 */

#define GRAIN 1024

static int          s_port = -1;
static SceUID       s_thread = -1;
static PongMixer    s_mix;
static volatile bool s_running;
static bool         s_enabled = true;
static bool         s_ready;

static int audio_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;

    int16_t block[GRAIN];
    while (s_running) {
        pong_mixer_fill(&s_mix, block, GRAIN);
        /* Blocks until the port has drained, which is what paces this loop --
         * there is deliberately no sleep here. */
        sceAudioOutOutput(s_port, block);
    }
    return sceKernelExitDeleteThread(0);
}

bool pong_audio_init(void)
{
    if (s_ready) return true;

    if (!pong_mixer_init(&s_mix)) return false;

    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN,
                                 PONG_TONE_RATE, SCE_AUDIO_OUT_MODE_MONO);
    if (s_port < 0) { pong_mixer_exit(&s_mix); return false; }

    sceAudioOutSetVolume(s_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                         (int[]){ SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB });

    s_running = true;
    s_thread = sceKernelCreateThread("pong_audio", audio_thread,
                                     0x10000100, 0x10000, 0, 0, NULL);
    if (s_thread < 0) {
        s_running = false;
        sceAudioOutReleasePort(s_port);
        s_port = -1;
        pong_mixer_exit(&s_mix);
        return false;
    }
    sceKernelStartThread(s_thread, 0, NULL);

    s_ready = true;
    return true;
}

void pong_audio_exit(void)
{
    if (!s_ready) return;

    /* The thread is blocked inside sceAudioOutOutput, so it cannot see the flag
     * until that returns. Waiting for it is what stops the port being released
     * out from under a write in progress. */
    s_running = false;
    sceKernelWaitThreadEnd(s_thread, NULL, NULL);
    s_thread = -1;

    sceAudioOutReleasePort(s_port);
    s_port = -1;
    pong_mixer_exit(&s_mix);
    s_ready = false;
}

void pong_audio_play(PongSfx s)
{
    if (!s_ready || !s_enabled) return;
    /*
     * Written from the game thread while the audio thread mixes.
     *
     * Deliberately unlocked. The worst interleaving starts a voice one grain
     * late or lets a burst steal a slot that was about to free -- both
     * inaudible -- and the alternative is taking a mutex on the thread that
     * must never block, to protect a handful of word-sized writes.
     */
    pong_mixer_play(&s_mix, s);
}

/* Nothing to do: a dedicated thread blocks on sceAudioOutOutput, so there is no buffer for the game loop to refill. */
void pong_audio_update(void) { }

void pong_audio_set_enabled(bool on) { s_enabled = on; }
bool pong_audio_enabled(void) { return s_enabled; }
