import { useEffect, useRef } from 'react';
import Box from '@mui/material/Box';
import { C } from '../../../shared/gen/protocol';
import { FIELD_H_Q4 } from '../../../shared/sim/paddle';
import { render, resizeCanvas } from '../game/render';
import type { GameClient } from '../game/client';

/** Says what the client is actually doing, rather than always "CONNECTING". */
function idleTextFor(status: string): string {
  switch (status) {
    case 'idle': return 'PICK A MODE BELOW';
    case 'connecting': return 'CONNECTING…';
    case 'lobby': return 'PICK A MODE BELOW';
    case 'queued': return 'WAITING FOR AN OPPONENT…';
    default: return 'READY';
  }
}

/**
 * The only 60fps surface in the app.
 *
 * React mounts this once and then stays out of the way entirely: the render
 * loop reads game state straight off the GameClient instance and paints. No
 * props change per frame, no state updates, no reconciliation.
 */
export default function PongCanvas({ client }: { client: GameClient }) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const wrapRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d', { alpha: false });
    if (!ctx) return;

    let raf = 0;
    const draw = () => {
      raf = requestAnimationFrame(draw);
      resizeCanvas(canvas);
      const hud = client.getHud();
      render(ctx, canvas, {
        view: client.getView(),
        myYQ4: client.getMyY(),
        mySide: hud.side,
        countdownSecs: hud.countdown,
        idleText: idleTextFor(hud.status),
      });
    };
    draw();
    return () => cancelAnimationFrame(raf);
  }, [client]);

  /* ---- input: pointer is absolute, which matches the protocol exactly ---- */
  useEffect(() => {
    const el = wrapRef.current;
    if (!el) return;

    const fromPointer = (clientY: number) => {
      const r = el.getBoundingClientRect();
      // The field is letterboxed inside the element; map through the same
      // transform the renderer uses so the paddle lands under the cursor.
      const scale = Math.min(r.width / C.FIELD_W, r.height / C.FIELD_H);
      const oy = (r.height - C.FIELD_H * scale) / 2;
      const fieldY = (clientY - r.top - oy) / scale;
      client.setTargetNormalized(fieldY / C.FIELD_H);
    };

    const onMove = (e: PointerEvent) => {
      e.preventDefault();
      fromPointer(e.clientY);
    };

    el.addEventListener('pointermove', onMove, { passive: false });
    el.addEventListener('pointerdown', onMove, { passive: false });

    // Keyboard: a nudge per frame at the same speed cap the server enforces,
    // so holding a key produces exactly the motion a mouse would.
    const held = new Set<string>();
    const onKeyDown = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement | null;
      if (target?.isContentEditable || target?.tagName === 'INPUT' || target?.tagName === 'TEXTAREA') return;
      if (['ArrowUp', 'ArrowDown', 'w', 's', 'W', 'S'].includes(e.key)) {
        held.add(e.key);
        e.preventDefault();
      }
    };
    const onKeyUp = (e: KeyboardEvent) => held.delete(e.key);
    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('keyup', onKeyUp);

    let raf = 0;
    const pump = () => {
      raf = requestAnimationFrame(pump);

      /*
       * With two people at one keyboard the keys split: W/S on the left of the
       * board drives the left paddle, the arrow cluster on the right drives the
       * right one. Everywhere else both sets drive the only paddle there is --
       * taking half of them away from a solo player for the sake of uniformity
       * would be worse.
       */
      const twoUp = client.inLocal && client.localMode === 'two-player';

      let dir = 0;
      if (held.has('w') || held.has('W')) dir -= 1;
      if (held.has('s') || held.has('S')) dir += 1;
      if (!twoUp && held.has('ArrowUp')) dir -= 1;
      if (!twoUp && held.has('ArrowDown')) dir += 1;
      if (dir !== 0) client.nudgeTarget(dir * C.MAX_PADDLE_SPEED_Q4);

      if (twoUp) {
        let d2 = 0;
        if (held.has('ArrowUp')) d2 -= 1;
        if (held.has('ArrowDown')) d2 += 1;
        if (d2 !== 0) client.nudgeP2(d2 * C.MAX_PADDLE_SPEED_Q4);
      }
    };
    pump();

    return () => {
      el.removeEventListener('pointermove', onMove);
      el.removeEventListener('pointerdown', onMove);
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('keyup', onKeyUp);
      cancelAnimationFrame(raf);
    };
  }, [client]);

  return (
    <Box
      ref={wrapRef}
      sx={{
        position: 'relative',
        width: '100%',
        flex: 1,
        minHeight: 0,
        cursor: 'none',
        touchAction: 'none',
        borderRadius: 2,
        overflow: 'hidden',
        border: '1px solid rgba(126,231,255,0.14)',
        boxShadow: '0 0 60px rgba(126,231,255,0.06) inset',
      }}
    >
      <canvas ref={canvasRef} style={{ width: '100%', height: '100%', display: 'block' }} />
    </Box>
  );
}

export { FIELD_H_Q4 };
