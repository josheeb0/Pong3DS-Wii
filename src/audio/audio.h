#ifndef PONG_AUDIO_H
#define PONG_AUDIO_H

/*
 * The audio seam, in the same spirit as src/gfx/gfx.h.
 *
 * Sound design lives in tone.c and is shared; only OUTPUT differs per platform,
 * and it differs a lot -- ndsp wants linear memory and wave buffers, the Vita
 * wants a thread feeding a port at a fixed grain, SDL wants a stream. So the
 * seam is deliberately tiny: render everything once, then "play this one".
 *
 * Every function is safe to call when audio failed to start. A console with no
 * sound is a console that still plays Pong, and a missing speaker must never be
 * the reason a match does not run -- so init returning false is information,
 * not an error to handle at every call site.
 */

#include <stdbool.h>
#include "tone.h"

/** Starts audio. False means the game runs silently, which is not a failure. */
bool pong_audio_init(void);

void pong_audio_exit(void);

/**
 * Called once a frame from the game loop.
 *
 * Exists for the 3DS, where ndsp does not pull through a callback: buffers have
 * to be refilled and requeued by whoever owns the loop. SDL and the Vita are
 * pulled by a callback and a thread respectively, so it does nothing there --
 * but every client calls it, because a seam whose contract changes per platform
 * is not a seam.
 */
void pong_audio_update(void);

/** Plays a sound, or does nothing if audio is off or unavailable. */
void pong_audio_play(PongSfx s);

/** Sound on/off, for the settings the player controls. */
void pong_audio_set_enabled(bool on);
bool pong_audio_enabled(void);

#endif /* PONG_AUDIO_H */
