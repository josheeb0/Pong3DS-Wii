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

  // Playfield border.
  ctx.strokeStyle = pal.net;
  ctx.lineWidth = 2;
  ctx.strokeRect(1, 1, C.FIELD_W - 2, C.FIELD_H - 2);

  // Centre net.
  ctx.setLineDash([12, 14]);
  ctx.beginPath();
  ctx.moveTo(C.FIELD_W / 2, 0);
  ctx.lineTo(C.FIELD_W / 2, C.FIELD_H);
  ctx.stroke();
  ctx.setLineDash([]);

  const v = input.view;
  if (!v) {
    drawCentreText(ctx, pal, input.idleText ?? 'READY', 26);
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
    ctx.shadowColor = pal.glow;
    ctx.shadowBlur = 24;
    ctx.fillStyle = pal.ball;
    ctx.beginPath();
    ctx.arc(bx, by, C.BALL_R, 0, Math.PI * 2);
    ctx.fill();
    ctx.shadowBlur = 0;
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

function drawCentreText(ctx: CanvasRenderingContext2D, pal: Palette, text: string, size: number): void {
  ctx.font = `700 ${size}px ui-monospace, SFMono-Regular, Menlo, monospace`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillStyle = pal.text;
  ctx.fillText(text, C.FIELD_W / 2, C.FIELD_H / 2);
}
