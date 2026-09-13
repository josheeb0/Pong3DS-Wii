import { useCallback, useMemo, useRef, useState, useSyncExternalStore } from 'react';
import {
  AppBar, Box, Button, Chip, Dialog, DialogContent, DialogTitle, Divider,
  Drawer, IconButton, LinearProgress, Stack, TextField, Toolbar, Tooltip, Typography,
} from '@mui/material';
import BugReportIcon from '@mui/icons-material/BugReport';
import SportsEsportsIcon from '@mui/icons-material/SportsEsports';
import SmartToyIcon from '@mui/icons-material/SmartToy';
import GroupsIcon from '@mui/icons-material/Groups';
import CloseIcon from '@mui/icons-material/Close';

import { JoinMode, Platform, MatchStateName } from '../../shared/gen/protocol';
import { GameClient } from './game/client';
import PongCanvas from './components/PongCanvas';
import type { RungName } from './net/ladder';

const PLATFORM_LABEL: Record<number, string> = {
  [Platform.UNKNOWN]: '?',
  [Platform.WEB]: 'BROWSER',
  [Platform.N3DS]: '3DS',
  [Platform.WII]: 'WII',
  // Added when the desktop and Vita clients arrived. Without them a real
  // opponent was labelled '?', which reads as a fault rather than a platform
  // this build had not heard of.
  [Platform.PC]: 'PC',
  [Platform.VITA]: 'VITA',
};

/** `?transport=ws|sse|poll` forces a rung, for testing the ladder. */
function forcedRung(): RungName | null {
  const v = new URLSearchParams(location.search).get('transport');
  return v === 'ws' || v === 'sse' || v === 'poll' ? v : null;
}

export default function App() {
  // One client for the lifetime of the page. Everything that moves at frame
  // rate lives inside it, deliberately outside React.
  const clientRef = useRef<GameClient | null>(null);
  if (!clientRef.current) clientRef.current = new GameClient();
  const client = clientRef.current;

  // React re-renders only when this coarse projection changes -- a few times a
  // second, never at frame rate.
  const hud = useSyncExternalStore(client.subscribe, client.getHud);

  const [name, setName] = useState(() => localStorage.getItem('pong.name') ?? 'PLAYER');
  const [room, setRoom] = useState('');
  const [debugOpen, setDebugOpen] = useState(false);
  const [busy, setBusy] = useState(false);

  const base = useMemo(() => {
    // In dev, Vite proxies /api to the server; in production the server serves
    // this bundle itself. Either way a relative base is correct.
    return '';
  }, []);

  const ensureConnected = useCallback(async () => {
    if (hud.status !== 'idle') return;
    setBusy(true);
    try {
      localStorage.setItem('pong.name', name);
      await client.start({ base, name, force: forcedRung() });
    } catch {
      /* the error surfaces through the HUD */
    } finally {
      setBusy(false);
    }
  }, [client, base, name, hud.status]);

  const play = useCallback(async (mode: number, code = '') => {
    await ensureConnected();
    client.join(mode, code);
  }, [client, ensureConnected]);

  const connected = hud.status !== 'idle' && hud.status !== 'connecting';
  const inMatch = hud.status === 'playing' || hud.status === 'over';

  return (
    <Box sx={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
      <AppBar position="static" color="transparent" elevation={0}
        sx={{ borderBottom: '1px solid rgba(126,231,255,0.12)' }}>
        <Toolbar variant="dense" sx={{ gap: 1 }}>
          <Typography variant="h1" sx={{ fontSize: '1rem', flexGrow: 1 }}>
            PONG MULTIPLAYER!
            <Box component="span" sx={{ opacity: 0.55, fontSize: '0.78rem', ml: 1.5 }}>
              3DS · Vita · Windows · macOS · Linux · browser
            </Box>
          </Typography>

          {connected && (
            <>
              <Tooltip title="Negotiated transport. WebSocket is fastest; SSE and long-poll are the fallbacks that survive a hostile proxy.">
                <Chip size="small" label={hud.transport} color="primary" variant="outlined" />
              </Tooltip>
              <Tooltip title="Minimum observed round-trip. Min rather than mean, because the least-queued sample is the honest one.">
                <Chip size="small" label={`${hud.rttMs}ms`} variant="outlined" />
              </Tooltip>
              <Tooltip title="How far behind live we render, sized from the arrival cadence we actually see.">
                <Chip size="small" label={`+${hud.renderDelayMs}ms`} variant="outlined" />
              </Tooltip>
              {hud.slowMode && (
                <Tooltip title="One player is on a polling transport, so the ball speed ceiling is lowered for both. Fairness lever, not a netcode one.">
                  <Chip size="small" label="SLOW" color="warning" variant="outlined" />
                </Tooltip>
              )}
            </>
          )}

          <IconButton size="small" onClick={() => setDebugOpen(true)} aria-label="diagnostics">
            <BugReportIcon fontSize="small" />
          </IconButton>
        </Toolbar>
        {(busy || hud.status === 'connecting' || hud.status === 'queued') && <LinearProgress />}
      </AppBar>

      <Box sx={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column', p: 1.5, gap: 1.5 }}>
        {inMatch && (
          <Stack direction="row" alignItems="center" justifyContent="center" spacing={3}>
            <Typography sx={{ fontSize: '2rem', fontWeight: 700, color: hud.side === 0 ? 'primary.main' : 'text.secondary' }}>
              {hud.scoreL}
            </Typography>
            <Stack alignItems="center" spacing={0.25}>
              <Typography variant="caption" color="text.secondary">
                {MatchStateName[hud.phase] ?? hud.phase} · first to {hud.winScore}
              </Typography>
              <Typography variant="caption" color="text.secondary">
                you are {hud.side === 0 ? 'LEFT' : 'RIGHT'} · vs {hud.oppName}
                {hud.oppPlatform ? ` (${PLATFORM_LABEL[hud.oppPlatform] ?? '?'})` : ''}
              </Typography>
            </Stack>
            <Typography sx={{ fontSize: '2rem', fontWeight: 700, color: hud.side === 1 ? 'primary.main' : 'text.secondary' }}>
              {hud.scoreR}
            </Typography>
          </Stack>
        )}

        <PongCanvas client={client} />

        <Stack direction="row" spacing={1} justifyContent="center" flexWrap="wrap" useFlexGap>
          {!inMatch && (
            <>
              <TextField
                size="small" label="name" value={name}
                onChange={(e) => setName(e.target.value.slice(0, 16))}
                sx={{ width: 150 }}
              />
              <Button variant="contained" startIcon={<GroupsIcon />}
                onClick={() => void play(JoinMode.QUICKMATCH)} disabled={busy}>
                Quick match
              </Button>
              <Button variant="outlined" startIcon={<SmartToyIcon />}
                onClick={() => void play(JoinMode.VS_BOT)} disabled={busy}>
                Vs CPU
              </Button>
              <TextField
                size="small" label="room code" value={room}
                onChange={(e) => setRoom(e.target.value.toUpperCase().slice(0, 6))}
                sx={{ width: 130 }}
              />
              <Button variant="outlined" startIcon={<SportsEsportsIcon />}
                disabled={room.length === 0 || busy}
                onClick={() => void play(JoinMode.ROOM_CODE, room)}>
                Join room
              </Button>
            </>
          )}
          {inMatch && (
            <Button variant="outlined" onClick={() => client.leave()}>Leave match</Button>
          )}
        </Stack>

        <Typography variant="caption" color="text.secondary" align="center">
          {hud.status === 'queued'
            // No longer true that a 3DS gets priority: the pairing delay that
            // implemented it made two same-platform players wait eight seconds
            // every time, and was removed.
            ? 'Waiting for an opponent — 3DS, Vita, desktop or another browser. A CPU opponent is offered after 15s.'
            : 'Move with the mouse, or ↑/↓ (W/S).'}
        </Typography>

        {hud.error && (
          <Typography variant="caption" color="error" align="center">{hud.error}</Typography>
        )}
      </Box>

      <Drawer anchor="right" open={debugOpen} onClose={() => setDebugOpen(false)}>
        <Box sx={{ width: 380, p: 2 }}>
          <Stack direction="row" alignItems="center" justifyContent="space-between">
            <Typography variant="h6">Diagnostics</Typography>
            <IconButton size="small" onClick={() => setDebugOpen(false)}><CloseIcon fontSize="small" /></IconButton>
          </Stack>
          <Divider sx={{ my: 1.5 }} />

          <Typography variant="overline" color="text.secondary">Transport ladder</Typography>
          <Stack spacing={0.5} sx={{ mb: 2 }}>
            {hud.ladder.length === 0 && <Typography variant="body2" color="text.secondary">not connected</Typography>}
            {hud.ladder.map((e, i) => (
              <Typography key={i} variant="body2" sx={{ color: e.ok ? 'success.main' : 'error.main' }}>
                {e.ok ? '✓' : '✗'} {e.rung} — {e.detail} ({e.ms}ms)
              </Typography>
            ))}
          </Stack>

          <Typography variant="overline" color="text.secondary">Netcode</Typography>
          <Stack spacing={0.25} sx={{ mb: 2 }}>
            <Row k="transport" v={hud.transport} />
            <Row k="min RTT" v={`${hud.rttMs} ms`} />
            <Row k="arrival gap" v={`${hud.arrivalGapMs} ms (median)`} />
            <Row k="render delay" v={`${hud.renderDelayMs} ms`} />
            <Row k="snapshot ring" v={String(client.stats().ringSize)} />
            <Row k="newest tick" v={String(client.stats().newestTick)} />
            <Row k="slow mode" v={hud.slowMode ? 'on' : 'off'} />
          </Stack>

          <Typography variant="caption" color="text.secondary">
            Force a rung by reloading with <code>?transport=ws</code>, <code>sse</code> or <code>poll</code>.
            The working rung is cached for 10 minutes so a blocked WebSocket is not re-probed every load.
          </Typography>
        </Box>
      </Drawer>

      <Dialog open={hud.status === 'over'} onClose={() => client.leave()}>
        <DialogTitle>
          {(hud.side === 0 ? hud.scoreL > hud.scoreR : hud.scoreR > hud.scoreL) ? 'You win!' : 'You lose'}
        </DialogTitle>
        <DialogContent>
          <Typography sx={{ mb: 2 }}>Final score {hud.scoreL} — {hud.scoreR}</Typography>
          <Button variant="contained" onClick={() => client.join(JoinMode.QUICKMATCH)}>Play again</Button>
        </DialogContent>
      </Dialog>
    </Box>
  );
}

function Row({ k, v }: { k: string; v: string }) {
  return (
    <Stack direction="row" justifyContent="space-between">
      <Typography variant="body2" color="text.secondary">{k}</Typography>
      <Typography variant="body2">{v}</Typography>
    </Stack>
  );
}
