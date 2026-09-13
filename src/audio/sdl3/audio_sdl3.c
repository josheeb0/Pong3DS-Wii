#include "audio.h"
#include "mixer.h"

#include <SDL3/SDL.h>
#include <string.h>

/*
 * SDL3 output.
 *
 * SDL3 pulls through a callback on its own thread, which is the only reason
 * this file needs to think about threading at all: pong_audio_play() is called
 * from the game loop while the callback is mixing. The lock is held for the
 * few microseconds it takes to touch a voice slot, which is far less than the
 * buffer it is feeding, so it cannot cause an underrun.
 */

static SDL_AudioStream *s_stream;
static SDL_Mutex       *s_lock;
static PongMixer        s_mix;
static bool             s_enabled = true;
static bool             s_ready;

static void SDLCALL feed(void *ud, SDL_AudioStream *stream,
                         int additional, int total)
{
    (void)ud; (void)total;
    if (additional <= 0) return;

    /* Frames, not bytes: mono int16. */
    int frames = additional / (int)sizeof(int16_t);
    while (frames > 0) {
        int16_t chunk[512];
        int take = frames < 512 ? frames : 512;

        SDL_LockMutex(s_lock);
        pong_mixer_fill(&s_mix, chunk, (size_t)take);
        SDL_UnlockMutex(s_lock);

        SDL_PutAudioStreamData(stream, chunk, take * (int)sizeof(int16_t));
        frames -= take;
    }
}

bool pong_audio_init(void)
{
    if (s_ready) return true;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("audio: %s -- continuing without sound", SDL_GetError());
        return false;
    }

    if (!pong_mixer_init(&s_mix)) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    s_lock = SDL_CreateMutex();
    if (!s_lock) { pong_mixer_exit(&s_mix); SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }

    SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 1, PONG_TONE_RATE };
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                         &spec, feed, NULL);
    if (!s_stream) {
        SDL_Log("audio: %s -- continuing without sound", SDL_GetError());
        SDL_DestroyMutex(s_lock); s_lock = NULL;
        pong_mixer_exit(&s_mix);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    SDL_ResumeAudioStreamDevice(s_stream);
    s_ready = true;
    return true;
}

void pong_audio_exit(void)
{
    if (!s_ready) return;
    SDL_DestroyAudioStream(s_stream);
    s_stream = NULL;
    SDL_DestroyMutex(s_lock);
    s_lock = NULL;
    pong_mixer_exit(&s_mix);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    s_ready = false;
}

void pong_audio_play(PongSfx s)
{
    if (!s_ready || !s_enabled) return;
    SDL_LockMutex(s_lock);
    pong_mixer_play(&s_mix, s);
    SDL_UnlockMutex(s_lock);
}

/* Nothing to do: SDL pulls through feed() on its own thread, so there is no buffer for the game loop to refill. */
void pong_audio_update(void) { }

void pong_audio_set_enabled(bool on) { s_enabled = on; }
bool pong_audio_enabled(void) { return s_enabled; }
