/*
 * Tests for the synthesised sound effects.
 *
 * Audio is the hardest thing in this project to verify, because the obvious
 * check -- did a speaker make a noise -- needs hardware nobody has in CI. But
 * the PCM is produced by deterministic integer arithmetic, so the WAVEFORM can
 * be asserted on directly, on any machine, with no audio device at all.
 *
 * The check that matters most is the last sample. A blip cut off mid-swing
 * steps from full amplitude to silence, and that discontinuity is an audible
 * click on every platform. It is the classic bug in synthesised sound, it is
 * inaudible in a diff, and it is trivially checkable here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "../../src/audio/tone.h"
#include "../../src/audio/mixer.h"
#include "../../src/audio/sfx_events.h"
#include "pong_proto.h"
#include "../../src/sim/pong_sim.h"

static int fails = 0;

static void check(bool ok, const char *what, const char *detail)
{
    if (!ok) { printf("  FAIL %-40s %s\n", what, detail ? detail : ""); fails++; }
    else printf("  ok   %-40s %s\n", what, detail ? detail : "");
}

static const char *NAME[PONG_SFX_COUNT] = {
    [PONG_SFX_PADDLE] = "PADDLE", [PONG_SFX_WALL] = "WALL",
    [PONG_SFX_GOAL] = "GOAL", [PONG_SFX_COUNTDOWN] = "COUNTDOWN",
    [PONG_SFX_WIN] = "WIN", [PONG_SFX_LOSE] = "LOSE",
};

int main(void)
{
    char d[128];
    int16_t *buf = malloc(PONG_TONE_RATE * 2 * sizeof *buf);   /* 2s is ample */
    if (!buf) { printf("  FAIL out of memory\n"); return 1; }

    printf("=== every sound renders, at the length it claims ===\n");
    for (int i = 0; i < PONG_SFX_COUNT; i++) {
        size_t want = pong_tone_samples((PongSfx)i);
        size_t got = pong_tone_render((PongSfx)i, buf, PONG_TONE_RATE * 2);
        snprintf(d, sizeof d, "%zu samples, %u ms", got, PONG_TONE[i].ms);
        check(got == want && got > 0, NAME[i], d);
    }

    printf("\n=== no sound ends on a click ===\n");
    for (int i = 0; i < PONG_SFX_COUNT; i++) {
        size_t n = pong_tone_render((PongSfx)i, buf, PONG_TONE_RATE * 2);
        /* The envelope must have reached silence. Anything above a whisper
         * here is a step discontinuity into the silence that follows. */
        int32_t last = buf[n - 1] < 0 ? -buf[n - 1] : buf[n - 1];
        snprintf(d, sizeof d, "final sample %d", (int)last);
        check(last < 512, NAME[i], d);
    }

    printf("\n=== and none starts on one either ===\n");
    for (int i = 0; i < PONG_SFX_COUNT; i++) {
        pong_tone_render((PongSfx)i, buf, PONG_TONE_RATE * 2);
        int32_t first = buf[0] < 0 ? -buf[0] : buf[0];
        snprintf(d, sizeof d, "first sample %d", (int)first);
        check(first < 512, NAME[i], d);
    }

    printf("\n=== each is actually audible, and never clips ===\n");
    for (int i = 0; i < PONG_SFX_COUNT; i++) {
        size_t n = pong_tone_render((PongSfx)i, buf, PONG_TONE_RATE * 2);
        int32_t peak = 0;
        long long energy = 0;
        for (size_t k = 0; k < n; k++) {
            int32_t a = buf[k] < 0 ? -buf[k] : buf[k];
            if (a > peak) peak = a;
            energy += a;
        }
        long long mean = energy / (long long)n;

        snprintf(d, sizeof d, "peak %d, mean %lld", (int)peak, mean);
        /* Loud enough to hear over a room, quiet enough not to be a square
         * wave pinned at full scale, which distorts on small speakers. */
        check(peak > 4000 && peak <= 32767 && mean > 200, NAME[i], d);
    }

    printf("\n=== the two frequent sounds are told apart ===\n");
    {
        /*
         * PADDLE and WALL happen within a fifth of a second of each other all
         * the time. If they shared a pitch range the rally would sound like
         * one repeated noise and carry no information.
         */
        const PongToneSpec *p = &PONG_TONE[PONG_SFX_PADDLE];
        const PongToneSpec *w = &PONG_TONE[PONG_SFX_WALL];
        snprintf(d, sizeof d, "paddle %u-%u Hz, wall %u-%u Hz",
                 p->freq_start, p->freq_end, w->freq_start, w->freq_end);
        /* Paddle sweeps up, wall sweeps down, and their ranges do not meet. */
        check(p->freq_end > p->freq_start, "paddle sweeps up", NULL);
        check(w->freq_end < w->freq_start, "wall sweeps down", NULL);
        check(w->freq_start < p->freq_start, "and they do not share a range", d);
    }

    printf("\n=== a buffer too small is refused, not overrun ===\n");
    {
        size_t n = pong_tone_samples(PONG_SFX_GOAL);
        memset(buf, 0x7f, 64 * sizeof *buf);
        size_t got = pong_tone_render(PONG_SFX_GOAL, buf, n - 1);
        check(got == 0, "short buffer writes nothing", NULL);
        /* Under ASan an overrun would already have aborted; this catches a
         * partial write that stayed in bounds. */
        check(buf[0] == (int16_t)0x7f7f, "and leaves it untouched", NULL);
    }

    printf("\n=== an unknown sound is handled, not indexed ===\n");
    {
        check(pong_tone_samples((PongSfx)PONG_SFX_COUNT) == 0, "count is out of range", NULL);
        check(pong_tone_render((PongSfx)99, buf, PONG_TONE_RATE) == 0, "nonsense id", NULL);
    }

    printf("\n=== events map to sounds, the same online and off ===\n");
    {
        PongSfx got;
        check(pong_sfx_for_event(PONG_EVENT_KIND_PADDLE_HIT, 0, 0, &got) &&
              got == PONG_SFX_PADDLE, "paddle hit", NULL);
        check(pong_sfx_for_event(PONG_EVENT_KIND_WALL_HIT, 0, 0, &got) &&
              got == PONG_SFX_WALL, "wall hit", NULL);
        check(pong_sfx_for_event(PONG_EVENT_KIND_GOAL, 0, 0, &got) &&
              got == PONG_SFX_GOAL, "goal", NULL);

        /* The same event means opposite things to the two players. */
        check(pong_sfx_for_event(PONG_EVENT_KIND_MATCH_OVER, 0, 0, &got) &&
              got == PONG_SFX_WIN, "match over, I won", NULL);
        check(pong_sfx_for_event(PONG_EVENT_KIND_MATCH_OVER, 1, 0, &got) &&
              got == PONG_SFX_LOSE, "match over, they won", NULL);
        check(pong_sfx_for_event(PONG_EVENT_KIND_MATCH_OVER, 1, 1, &got) &&
              got == PONG_SFX_WIN, "and from the other side", NULL);

        /* A flaky connection must not make a noise every time it blips. */
        check(!pong_sfx_for_event(PONG_EVENT_KIND_OPP_LEFT, 0, 0, &got),
              "opponent leaving is silent", NULL);
        check(!pong_sfx_for_event(PONG_EVENT_KIND_OPP_REJOINED, 0, 0, &got),
              "and so is rejoining", NULL);
        check(!pong_sfx_for_event(200, 0, 0, &got), "an unknown kind is silent", NULL);

        /*
         * The local simulation's event kinds and the protocol's must agree,
         * because this one function is fed by both. If they ever diverged, an
         * offline match would make the wrong noises -- or none.
         */
        /* Cast because they are deliberately DIFFERENT enum types -- one is
         * the simulation's, one is the wire's -- and the claim being made is
         * about their numeric values, not their types. gcc is right to want
         * that said out loud. */
        check((int)PONG_SIM_EV_PADDLE_HIT == (int)PONG_EVENT_KIND_PADDLE_HIT &&
              (int)PONG_SIM_EV_WALL_HIT   == (int)PONG_EVENT_KIND_WALL_HIT &&
              (int)PONG_SIM_EV_GOAL       == (int)PONG_EVENT_KIND_GOAL &&
              (int)PONG_SIM_EV_MATCH_OVER == (int)PONG_EVENT_KIND_MATCH_OVER,
              "sim and wire event kinds are the same numbers", NULL);
    }

    printf("\n=== the mixer ===\n");
    {
        PongMixer m;
        check(pong_mixer_init(&m), "renders every sound at startup", NULL);

        int16_t mix[256];
        check(!pong_mixer_active(&m), "silent before anything plays", NULL);

        pong_mixer_play(&m, PONG_SFX_PADDLE);
        check(pong_mixer_active(&m), "a sound makes it active", NULL);
        pong_mixer_fill(&m, mix, 256);
        int32_t any = 0;
        for (int i = 0; i < 256; i++) if (mix[i]) any = 1;
        check(any, "and produces samples", NULL);

        /*
         * Overlap is the normal case in Pong: a paddle hit lands while the
         * previous wall bounce is still decaying. A mixer that restarted one
         * channel would cut the first off mid-swing -- the same click the
         * envelope exists to prevent.
         */
        pong_mixer_play(&m, PONG_SFX_WALL);
        pong_mixer_play(&m, PONG_SFX_GOAL);
        int live = 0;
        for (int i = 0; i < PONG_MIX_VOICES; i++) if (m.voice[i].live) live++;
        snprintf(d, sizeof d, "%d voices sounding", live);
        check(live == 3, "three sounds overlap rather than replacing", d);

        /* More at once than there are voices must not corrupt anything. */
        for (int i = 0; i < 20; i++) pong_mixer_play(&m, PONG_SFX_PADDLE);
        pong_mixer_fill(&m, mix, 256);
        check(true, "a burst beyond the voice count is survivable", NULL);

        /* Every sound at once, at the loudest point, must clamp not wrap. */
        {
            PongMixer loud;
            pong_mixer_init(&loud);
            for (int i = 0; i < PONG_SFX_COUNT; i++)
                pong_mixer_play(&loud, (PongSfx)i);

            /*
             * Measured as a signed range, not an absolute value.
             *
             * The first version of this took |sample| and demanded <= 32767,
             * which fails on -32768 -- a perfectly legal int16 and exactly what
             * the clamp produces at full negative scale. It was the TEST that
             * was wrong, and it is worth keeping the note: asymmetric int16
             * range catches people out precisely here.
             */
            int32_t lo = 0, hi = 0, sign_flips = 0, prev = 0;
            for (int block = 0; block < 40; block++) {
                pong_mixer_fill(&loud, mix, 256);
                for (int i = 0; i < 256; i++) {
                    if (mix[i] > hi) hi = mix[i];
                    if (mix[i] < lo) lo = mix[i];
                    /* A wrap shows up as a jump from near +full to near -full
                     * between adjacent samples. */
                    if (prev > 30000 && mix[i] < -30000) sign_flips++;
                    if (prev < -30000 && mix[i] > 30000) sign_flips++;
                    prev = mix[i];
                }
            }
            snprintf(d, sizeof d, "range %d..%d, %d wrap-shaped jumps",
                     (int)lo, (int)hi, (int)sign_flips);
            check(lo >= -32768 && hi <= 32767 && sign_flips == 0,
                  "everything at once clamps, never wraps", d);
            pong_mixer_exit(&loud);
        }

        pong_mixer_exit(&m);
        check(!m.ready, "and tears down", NULL);
    }

    free(buf);
    printf("\n");
    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASSED: sound synthesis\n");
    return 0;
}
