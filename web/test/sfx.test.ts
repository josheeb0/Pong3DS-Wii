import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { TONE, renderTone, sfxForEvent, TONE_RATE, type SfxName } from '../src/game/sfx';
import { EventKind } from '../../shared/gen/protocol';

/**
 * The browser's sound is a port of src/audio/tone.c, and a port is a promise
 * that two things stay the same. The comment saying so would not survive the
 * first edit, so this parses the C and checks.
 *
 * The properties below are the same ones 3ds/test/test_tone.c asserts. The
 * important one is that nothing ends on a click: a waveform cut off mid-swing
 * steps from full amplitude to silence, which is audible on every platform and
 * which WebAudio will happily play for you.
 */

/** Pulls the PONG_TONE table out of the C, so drift fails here. */
function specsFromC(): Record<string, { freqStart: number; freqEnd: number; ms: number; wave: number; volume: number }> {
  const src = readFileSync(new URL('../../src/audio/tone.c', import.meta.url), 'utf8');
  const out: Record<string, { freqStart: number; freqEnd: number; ms: number; wave: number; volume: number }> = {};

  // [PONG_SFX_PADDLE] = { 440, 660,  55, PONG_WAVE_SQUARE,   200 },
  const re = /\[PONG_SFX_(\w+)\]\s*=\s*\{\s*(\d+),\s*(\d+),\s*(\d+),\s*PONG_WAVE_(\w+),\s*(\d+)\s*\}/g;
  for (let m = re.exec(src); m; m = re.exec(src)) {
    out[m[1]!] = {
      freqStart: Number(m[2]),
      freqEnd: Number(m[3]),
      ms: Number(m[4]),
      wave: m[5] === 'SQUARE' ? 0 : 1,
      volume: Number(m[6]),
    };
  }
  return out;
}

describe('the browser sound matches the C', () => {
  const fromC = specsFromC();

  it('found the table in tone.c at all', () => {
    // If the C table is reformatted past what the regex understands, every
    // comparison below would vacuously pass on an empty object.
    expect(Object.keys(fromC).length).toBe(Object.keys(TONE).length);
    expect(Object.keys(fromC).length).toBeGreaterThan(0);
  });

  it('has the same sounds, with the same numbers', () => {
    expect(Object.keys(TONE).sort()).toEqual(Object.keys(fromC).sort());
    for (const name of Object.keys(TONE) as SfxName[]) {
      expect({ name, ...TONE[name] }).toEqual({ name, ...fromC[name] });
    }
  });
});

describe('the waveform', () => {
  const names = Object.keys(TONE) as SfxName[];

  it('is the length the spec claims', () => {
    for (const n of names) {
      expect(renderTone(TONE[n]!).length).toBe(TONE[n]!.ms * (TONE_RATE / 1000));
    }
  });

  it('never ends on a click', () => {
    for (const n of names) {
      const pcm = renderTone(TONE[n]!);
      expect(Math.abs(pcm[pcm.length - 1]!)).toBeLessThan
        ? expect(Math.abs(pcm[pcm.length - 1]!)).toBeLessThan(512 / 32768)
        : undefined;
    }
  });

  it('never starts on one either', () => {
    for (const n of names) {
      expect(Math.abs(renderTone(TONE[n]!)[0]!)).toBeLessThan(512 / 32768);
    }
  });

  it('is audible and never clips', () => {
    for (const n of names) {
      const pcm = renderTone(TONE[n]!);
      let peak = 0;
      for (const v of pcm) peak = Math.max(peak, Math.abs(v));
      expect(peak).toBeGreaterThan(4000 / 32768);
      expect(peak).toBeLessThanOrEqual(1);
    }
  });
});

describe('events become sounds', () => {
  it('maps the ones that make a noise', () => {
    expect(sfxForEvent(EventKind.PADDLE_HIT, 0, 0)).toBe('PADDLE');
    expect(sfxForEvent(EventKind.WALL_HIT, 0, 0)).toBe('WALL');
    expect(sfxForEvent(EventKind.GOAL, 0, 0)).toBe('GOAL');
  });

  it('tells a win from a loss by side', () => {
    // The same event means opposite things to the two players.
    expect(sfxForEvent(EventKind.MATCH_OVER, 0, 0)).toBe('WIN');
    expect(sfxForEvent(EventKind.MATCH_OVER, 1, 0)).toBe('LOSE');
    expect(sfxForEvent(EventKind.MATCH_OVER, 1, 1)).toBe('WIN');
  });

  it('stays silent for connection noise', () => {
    // A sound here fires every time a flaky connection blips.
    expect(sfxForEvent(EventKind.OPP_LEFT, 0, 0)).toBeNull();
    expect(sfxForEvent(EventKind.OPP_REJOINED, 0, 0)).toBeNull();
    expect(sfxForEvent(200, 0, 0)).toBeNull();
  });
});
