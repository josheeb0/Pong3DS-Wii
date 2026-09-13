/**
 * Canvas renderer.
 *
 * Pure drawing: it is handed a view and paints it. No state, no allocation in
 * the hot path -- an allocation per frame here is a GC pause you will see as a
 * stutter every few seconds.
 *
 * The field is 800x480 Q4 units; the canvas is scaled once with setTransform so
 * everything below draws in raw field pixels and never does per-shape maths.
 */

import { C, MatchState as Phase, SnapFlag } from '../../../shared/gen/protocol';
import type { View } from './interp';

const Q = C.Q4_SHIFT;
const toPx = (q: number): number => q / (1 << Q);

export interface Palette {
  bg: string;
  net: string;
  paddleMine: string;
  paddleTheirs: string;
  ball: string;
  text: string;
  glow: string;
}

export const NEON: Palette = {
  bg: '#07090d',
  net: 'rgba(120, 200, 255, 0.18)',
  paddleMine: '#7ee7ff',
  paddleTheirs: '#ff9de2',
  ball: '#ffffff',
  text: 'rgba(220, 240, 255, 0.9)',
  glow: 'rgba(126, 231, 255, 0.35)',
};

/*
 * The same values the handheld and desktop clients use, so the three look like
 * one game rather than three ports of it. They are literals here rather than
 * shared constants because the C side packs colours as 0xAABBGGRR and canvas
 * wants CSS strings; a shared table would have to be translated either way.
 */
const FIELD_HI = '#0e1722';
const FIELD_LO = '#06090e';
const GHOST_L = 'rgba(27, 51, 68, 1)';
const GHOST_R = 'rgba(56, 36, 64, 1)';
const GHOST_BALL = 'rgba(53, 77, 96, 1)';

/** Recent ball positions, for the trail. Module state because the renderer is
 *  called fresh each frame and has nowhere else to keep it. */
const TRAIL_LEN = 10;
const trailX = new Float32Array(TRAIL_LEN);
const trailY = new Float32Array(TRAIL_LEN);
let trailHead = 0;
let trailLive = false;

function trailPush(x: number, y: number): void {
  if (!trailLive) {
    trailX.fill(x);
    trailY.fill(y);
    trailLive = true;
    trailHead = 0;
    return;
  }
  trailX[trailHead] = x;
  trailY[trailHead] = y;
  trailHead = (trailHead + 1) % TRAIL_LEN;
}

export interface RenderInput {
  view: View | null;
  /** Our locally predicted paddle, which overrides the interpolated one. */
  myYQ4: number;
  mySide: number;
  countdownSecs: number;
  /**
   * What to show when there is no match to draw.
   *
   * This used to be hardcoded to "CONNECTING...", which meant an idle client
   * sitting happily at the lobby looked permanently stuck -- the single most
   * misleading thing the UI did.
   */
  idleText?: string;
  palette?: Palette;
}

export function resizeCanvas(canvas: HTMLCanvasElement): number {
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const rect = canvas.getBoundingClientRect();
  const w = Math.max(1, Math.round(rect.width * dpr));
  const h = Math.max(1, Math.round(rect.height * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  return dpr;
}

export function render(ctx: CanvasRenderingContext2D, canvas: HTMLCanvasElement, input: RenderInput): void {
  const pal = input.palette ?? NEON;
  const cw = canvas.width;
  const ch = canvas.height;

  // Letterbox the 5:3 field into whatever shape the canvas is.
  const scale = Math.min(cw / C.FIELD_W, ch / C.FIELD_H);
  const ox = (cw - C.FIELD_W * scale) / 2;
  const oy = (ch - C.FIELD_H * scale) / 2;

  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = pal.bg;
  ctx.fillRect(0, 0, cw, ch);

  ctx.setTransform(scale, 0, 0, scale, ox, oy);

  // The field is a gradient, not a flat fill, and it is the single change that
  // makes this read as a table rather than a black rectangle. Same treatment as
  // the handheld and desktop clients.
  const g = ctx.createLinearGradient(0, 0, 0, C.FIELD_H);
  g.addColorStop(0, FIELD_HI);
  g.addColorStop(1, FIELD_LO);
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, C.FIELD_W, C.FIELD_H);

  const mineLeft = input.mySide === 0;

  // Goal lines in each player's colour, so which end is yours needs no caption.
  ctx.fillStyle = mineLeft ? 'rgba(126,231,255,0.40)' : 'rgba(255,157,226,0.40)';
  ctx.fillRect(0, 0, 2, C.FIELD_H);
  ctx.fillStyle = mineLeft ? 'rgba(255,157,226,0.40)' : 'rgba(126,231,255,0.40)';
  ctx.fillRect(C.FIELD_W - 2, 0, 2, C.FIELD_H);

  // Top and bottom rails.
  ctx.fillStyle = 'rgba(26,43,56,1)';
  ctx.fillRect(0, 0, C.FIELD_W, 2);
  ctx.fillRect(0, C.FIELD_H - 2, C.FIELD_W, 2);

  // The net fades toward the rails rather than running at one opacity. A flat
  // dashed line draws the eye to the edges; this keeps it in the middle where
  // the play is.
  const dash = C.FIELD_H / 17;
  for (let y = dash * 0.25; y < C.FIELD_H - dash * 0.3; y += dash) {
    const d = Math.abs((y - C.FIELD_H / 2) / (C.FIELD_H / 2));
    ctx.fillStyle = `rgba(30,58,77,${(1 - d * 0.7).toFixed(3)})`;
    ctx.fillRect(C.FIELD_W / 2 - 1.5, y, 3, dash * 0.5);
  }

  const v = input.view;
  if (!v) {
    // A ghost match, rather than text on an empty field. The handheld plays one
    // behind its menu and it is what makes that screen feel alive; the browser
    // has the same dead space and had nothing in it.
    drawAttract(ctx);
    drawCentreText(ctx, pal, input.idleText ?? 'READY', 26);
    trailLive = false;
    return;
  }

  // Paddles. Ours is the locally predicted one -- drawing the interpolated
  // version would add the render delay to our OWN input, which is exactly the
  // lag players notice most.
  const leftY = input.mySide === 0 ? input.myYQ4 : v.leftY;
  const rightY = input.mySide === 1 ? input.myYQ4 : v.rightY;

  drawPaddle(ctx, C.PADDLE_X_L, toPx(leftY), input.mySide === 0 ? pal.paddleMine : pal.paddleTheirs, pal);
  drawPaddle(ctx, C.PADDLE_X_R, toPx(rightY), input.mySide === 1 ? pal.paddleMine : pal.paddleTheirs, pal);

  // Score, behind the play.
  ctx.font = '600 64px ui-monospace, SFMono-Regular, Menlo, monospace';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  ctx.fillStyle = 'rgba(200, 230, 255, 0.16)';
  ctx.fillText(String(v.scoreL), C.FIELD_W / 2 - 70, 24);
  ctx.fillText(String(v.scoreR), C.FIELD_W / 2 + 70, 24);

  // Ball last: it is the thing the eye tracks, and on the 3DS the equivalent
  // ordering is a hard requirement rather than a preference.
  if (v.state === Phase.PLAY || v.state === Phase.GOAL_FREEZE) {
    const bx = toPx(v.ballX);
    const by = toPx(v.ballY);

    if (v.state === Phase.PLAY) trailPush(bx, by);

    // Squares, oldest and smallest first. Alpha rises with the square of age so
    // the tail disappears quickly instead of smearing.
    for (let i = 0; i < TRAIL_LEN; i++) {
      const idx = (trailHead + i) % TRAIL_LEN;
      const age = i / TRAIL_LEN;
      const sz = C.BALL_R * (0.2 + 0.7 * age);
      // `?? 0` rather than a non-null assertion: the arrays are fixed-length and
      // idx is always in range, but a silent 0 is a harmless dot in the corner
      // while an assertion that turns out wrong is a crash mid-rally.
      const tx = trailX[idx] ?? 0;
      const ty = trailY[idx] ?? 0;
      ctx.fillStyle = `rgba(255,255,255,${(age * age * 0.32).toFixed(3)})`;
      ctx.fillRect(tx - sz, ty - sz, sz * 2, sz * 2);
    }

    ctx.shadowColor = pal.glow;
    ctx.shadowBlur = 24;
    ctx.fillStyle = pal.ball;
    ctx.beginPath();
    ctx.arc(bx, by, C.BALL_R, 0, Math.PI * 2);
    ctx.fill();
    ctx.shadowBlur = 0;
  } else {
    trailLive = false;
  }

  if (v.state === Phase.COUNTDOWN) {
    drawCentreText(ctx, pal, input.countdownSecs > 0 ? String(input.countdownSecs) : 'GET READY', 72);
  } else if (v.state === Phase.GAME_OVER) {
    drawCentreText(ctx, pal, v.scoreL > v.scoreR ? 'LEFT WINS' : 'RIGHT WINS', 40);
  } else if (v.state === Phase.OPP_LOST) {
    drawCentreText(ctx, pal, 'OPPONENT LEFT', 32);
  }

  if ((v.flags & SnapFlag.SLOW_MODE) !== 0) {
    ctx.font = '500 14px ui-monospace, monospace';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'bottom';
    ctx.fillStyle = 'rgba(255, 200, 120, 0.5)';
    ctx.fillText('SLOW MODE — balanced for a polling opponent', C.FIELD_W - 12, C.FIELD_H - 10);
  }

  if (v.starved) {
    ctx.fillStyle = 'rgba(255, 120, 120, 0.55)';
    ctx.beginPath();
    ctx.arc(C.FIELD_W - 16, 16, 5, 0, Math.PI * 2);
    ctx.fill();
  }
}

function drawPaddle(ctx: CanvasRenderingContext2D, cx: number, cy: number, color: string, pal: Palette): void {
  const w = C.PADDLE_W;
  const h = C.PADDLE_H;
  ctx.shadowColor = pal.glow;
  ctx.shadowBlur = 16;
  ctx.fillStyle = color;
  const r = 4;
  const x = cx - w / 2;
  const y = cy - h / 2;
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
  ctx.fill();
  ctx.shadowBlur = 0;
}

/*
 * A ghost match, drawn in field coordinates behind the idle text.
 *
 * Ported from the handheld and desktop clients. The paddles follow the ball
 * LATE; that lag is what makes three moving shapes read as a rally rather than
 * three independent animations.
 */
function drawAttract(ctx: CanvasRenderingContext2D): void {
  // performance.now() rather than a frame counter: this renderer is called from
  // requestAnimationFrame and keeps no counter of its own, and wall time gives
  // the same motion whatever the refresh rate.
  const t = performance.now() * 0.06;

  const bx = C.FIELD_W / 2 + C.FIELD_W * 0.38 * Math.sin(t * 0.013);
  const by = C.FIELD_H / 2 + C.FIELD_H * 0.32 * Math.sin(t * 0.021);
  const ly = C.FIELD_H / 2 + C.FIELD_H * 0.28 * Math.sin(t * 0.021 - 0.6);
  const ry = C.FIELD_H / 2 + C.FIELD_H * 0.28 * Math.sin(t * 0.021 - 1.1);

  const ph = C.FIELD_H * 0.17;
  const pw = 9;

  ctx.fillStyle = GHOST_L;
  ctx.fillRect(C.FIELD_W * 0.05, ly - ph / 2, pw, ph);
  ctx.fillStyle = GHOST_R;
  ctx.fillRect(C.FIELD_W * 0.95 - pw, ry - ph / 2, pw, ph);

  for (let k = 7; k >= 1; k--) {
    const tt = t - k * 2.2;
    const tx = C.FIELD_W / 2 + C.FIELD_W * 0.38 * Math.sin(tt * 0.013);
    const ty = C.FIELD_H / 2 + C.FIELD_H * 0.32 * Math.sin(tt * 0.021);
    const sz = 5 - k * 0.45;
    ctx.fillStyle = `rgba(53,77,96,${(1 - k / 8).toFixed(3)})`;
    ctx.fillRect(tx - sz, ty - sz, sz * 2, sz * 2);
  }

  ctx.fillStyle = GHOST_BALL;
  ctx.beginPath();
  ctx.arc(bx, by, 6, 0, Math.PI * 2);
  ctx.fill();
}

function drawCentreText(ctx: CanvasRenderingContext2D, pal: Palette, text: string, size: number): void {
  ctx.font = `700 ${size}px ui-monospace, SFMono-Regular, Menlo, monospace`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillStyle = pal.text;
  ctx.fillText(text, C.FIELD_W / 2, C.FIELD_H / 2);
}
