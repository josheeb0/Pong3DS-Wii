/**
 * Sound in the browser.
 *
 * A port of src/audio/tone.c, deliberately sample-for-sample rather than "a
 * WebAudio oscillator that sounds about right". An OscillatorNode with a
 * frequency ramp would have been a third of the code and would have made the
 * browser the one client that sounds different — which is exactly the kind of
 * difference nobody notices until they play two devices side by side.
 *
 * The spec table below is the same data as `PONG_TONE` in tone.c, and
 * web/test/sfx.test.ts parses that C file and fails if the two ever disagree.
 * A comment saying "keep these in sync" would not have survived the first edit.
 */

import { EventKind } from '../../../shared/gen/protocol';

export const TONE_RATE = 48000;

const WAVE_SQUARE = 0;
const WAVE_TRIANGLE = 1;

export interface ToneSpec {
  freqStart: number;
  freqEnd: number;
  ms: number;
  wave: number;
  volume: number;
}

/** Mirrors PONG_TONE in src/audio/tone.c. Checked by the test, not by hand. */
export const TONE: Record<string, ToneSpec> = {
  PADDLE:    { freqStart: 440, freqEnd: 660, ms: 55,  wave: WAVE_SQUARE,   volume: 200 },
  WALL:      { freqStart: 300, freqEnd: 220, ms: 45,  wave: WAVE_SQUARE,   volume: 160 },
  GOAL:      { freqStart: 520, freqEnd: 130, ms: 260, wave: WAVE_SQUARE,   volume: 190 },
  COUNTDOWN: { freqStart: 880, freqEnd: 880, ms: 70,  wave: WAVE_TRIANGLE, volume: 140 },
  WIN:       { freqStart: 440, freqEnd: 990, ms: 420, wave: WAVE_TRIANGLE, volume: 200 },
  LOSE:      { freqStart: 440, freqEnd: 160, ms: 480, wave: WAVE_TRIANGLE, volume: 190 },
};

export type SfxName = keyof typeof TONE;

/**
 * The envelope, in Q8, matching envelope_q8() in tone.c.
 *
 * The decay reaching exactly zero is the point: a waveform cut off mid-swing
 * steps from full amplitude to silence, and that discontinuity is an audible
 * click. WebAudio does not save you from it — it plays what it is given.
 */
function envelopeQ8(i: number, total: number): number {
  const attack = Math.floor(total / 16) || 1;
  if (i < attack) return Math.floor((i * 256) / attack);

  const left = total - i;
  const span = total - attack;
  if (span === 0) return 0;
  return Math.floor((left * 256) / span);
}

/** Renders one sound as samples in -1..1, by the same integer maths as the C. */
export function renderTone(spec: ToneSpec): Float32Array<ArrayBuffer> {
  const n = spec.ms * (TONE_RATE / 1000);
  /* Explicitly over an ArrayBuffer, not ArrayBufferLike: copyToChannel refuses
   * a possibly-shared buffer, and the default inference is the wider type. */
  const out = new Float32Array(new ArrayBuffer(n * 4));

  let phase = 0;
  for (let i = 0; i < n; i++) {
    let f = spec.freqStart + Math.trunc(((spec.freqEnd - spec.freqStart) * i) / n);
    if (f < 1) f = 1;

    /* Q16 phase, so the sweep needs no floating point — and matches the C. */
    const step = Math.trunc((f * 65536) / TONE_RATE);
    phase = (phase + step) & 0xffff;

    let sample: number;
    if (spec.wave === WAVE_SQUARE) {
      sample = phase < 0x8000 ? 32767 : -32767;
    } else {
      sample = phase < 0x8000 ? phase * 4 - 0x10000 : 0x30000 - phase * 4;
      if (sample > 32767) sample = 32767;
      if (sample < -32767) sample = -32767;
    }

    let v = Math.trunc((sample * spec.volume) / 255);
    v = Math.trunc((v * envelopeQ8(i, n)) / 256);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;

    out[i] = v / 32768;
  }

  return out;
}

/** The sound an event makes, mirroring pong_sfx_for_event() in sfx_events.c. */
export function sfxForEvent(kind: number, winner: number, mySide: number): SfxName | null {
  switch (kind) {
    case EventKind.PADDLE_HIT: return 'PADDLE';
    case EventKind.WALL_HIT: return 'WALL';
    case EventKind.GOAL: return 'GOAL';
    case EventKind.COUNTDOWN_START: return 'COUNTDOWN';
    case EventKind.MATCH_OVER: return winner === mySide ? 'WIN' : 'LOSE';
    /* Leaving and rejoining stay silent: a sound there fires every time a
     * flaky connection blips. */
    default: return null;
  }
}

/* ------------------------------------------------------------------ output */

let ctx: AudioContext | null = null;
let buffers: Partial<Record<SfxName, AudioBuffer>> = {};
let enabled = true;

/**
 * Browsers refuse to start audio until the user has interacted with the page,
 * so this is called from the first click rather than at load. Calling it early
 * does not fail loudly — it leaves a context stuck in "suspended", which plays
 * nothing and reports no error, and is a genuinely confusing way to have no
 * sound.
 */
export function initSfx(): void {
  if (ctx) { void ctx.resume(); return; }

  const Ctor = window.AudioContext ?? (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
  if (!Ctor) return;               /* no WebAudio: play silently */

  try {
    ctx = new Ctor({ sampleRate: TONE_RATE });
  } catch {
    /* Some browsers refuse a non-native rate. Take whatever they give and let
     * the tones be a few cents off rather than having no sound at all. */
    try { ctx = new Ctor(); } catch { return; }
  }

  for (const name of Object.keys(TONE) as SfxName[]) {
    const pcm = renderTone(TONE[name]!);
    const buf = ctx.createBuffer(1, pcm.length, TONE_RATE);
    buf.copyToChannel(pcm, 0);
    buffers[name] = buf;
  }
}

export function setSfxEnabled(on: boolean): void { enabled = on; }
export function sfxEnabled(): boolean { return enabled; }

/** Plays a sound. Silent and harmless if audio never started. */
export function playSfx(name: SfxName): void {
  if (!enabled || !ctx) return;
  const buf = buffers[name];
  if (!buf) return;

  /* A fresh source per play: AudioBufferSourceNode is single-use by design,
   * which also means overlapping sounds mix for free. */
  const src = ctx.createBufferSource();
  src.buffer = buf;
  src.connect(ctx.destination);
  src.start();
}
