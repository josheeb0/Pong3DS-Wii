/**
 * Headless request/response client.
 *
 * This runs the EXACT algorithm the 3DS will run over httpc: bootstrap a
 * session, then repeatedly POST one body containing the latest input and read a
 * body containing a batch of snapshots. Proving the loop here, in TypeScript,
 * means the C implementation is a transcription rather than a design exercise.
 *
 * It also reports the numbers that decide whether the HTTP path is playable:
 * observed round-trip time, snapshots received per response, and whether the
 * batching actually delivers intermediate sample points rather than one stale
 * position per poll.
 *
 *   npx tsx tools/rrclient.ts --url http://127.0.0.1:8788 --hz 10 --seconds 10
 */

import {
  C,
  MsgType,
  Platform,
  TransportKind,
  JoinMode,
  MatchStateName,
  EventKindName,
  encodeHELLO,
  encodeJOIN,
  encodeINPUT,
  encodePING,
  decodeWELCOME,
  decodeSNAPSHOT,
  decodeMATCH_START,
  decodeEVENT,
  decodePONG,
  parseFrames,
  concatFrames,
  padBytes,
} from '../../shared/gen/protocol.js';

interface Args {
  url: string;
  hz: number;
  seconds: number;
  mode: number;
  room: string;
  name: string;
  wait: number;
  quiet: boolean;
}

function parseArgs(): Args {
  const a = process.argv.slice(2);
  const get = (k: string, d: string): string => {
    const i = a.indexOf('--' + k);
    return i >= 0 && a[i + 1] !== undefined ? (a[i + 1] as string) : d;
  };
  return {
    url: get('url', 'http://127.0.0.1:8788').replace(/\/$/, ''),
    hz: Number(get('hz', '10')),
    seconds: Number(get('seconds', '10')),
    mode: get('mode', 'bot') === 'bot' ? JoinMode.VS_BOT
      : get('mode', 'bot') === 'room' ? JoinMode.ROOM_CODE : JoinMode.QUICKMATCH,
    room: get('room', ''),
    name: get('name', 'RRBOT'),
    wait: Number(get('wait', '0')),
    quiet: a.includes('--quiet'),
  };
}

const FIELD_H_Q = C.FIELD_H << C.Q4_SHIFT;
const PADDLE_HALF_Q = (C.PADDLE_H << C.Q4_SHIFT) >> 1;

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

async function main(): Promise<void> {
  const args = parseArgs();
  const log = (...m: unknown[]) => { if (!args.quiet) console.log(...m); };

  /* -- 1. bootstrap over a plain POST, before choosing a transport --------- */
  const hello = encodeHELLO({
    platform: Platform.N3DS, // pretend to be the console we are modelling
    transport: TransportKind.RR,
    caps: 0,
    name: padBytes(args.name, C.NAME_BYTES),
    buildId: 1,
  }, 1);

  const sres = await fetch(args.url + '/api/session', {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: hello,
  });
  if (!sres.ok) throw new Error(`session failed: ${sres.status}`);

  const sessionId = sres.headers.get('x-pong-session');
  if (!sessionId) throw new Error('no X-Pong-Session header');
  const welcomeBuf = new Uint8Array(await sres.arrayBuffer());
  const wf = parseFrames(welcomeBuf).frames[0];
  if (!wf || wf.type !== MsgType.WELCOME) throw new Error('no WELCOME frame');
  const welcome = decodeWELCOME(wf.buf, wf.payloadOff);
  log(`session ${sessionId.slice(0, 12)}...  field ${welcome.fieldW}x${welcome.fieldH}` +
      `  tick ${welcome.tickRate}Hz  snap ${welcome.snapshotRate}Hz`);

  /* -- 2. state the client maintains -------------------------------------- */
  let lastTickSeen = 0;
  let inputSeq = 0;
  let mySide = 0;
  let targetY = FIELD_H_Q >> 1;
  let ballY = FIELD_H_Q >> 1;
  let ballX = 0;
  let scoreL = 0;
  let scoreR = 0;
  let phase = 0;
  let clockOffset = 0;
  let minRtt = Number.POSITIVE_INFINITY;

  const rtts: number[] = [];
  const batchSizes: number[] = [];
  const tickGaps: number[] = [];
  let snapshots = 0;
  let responses = 0;
  let matched = false;

  const post = async (frames: Uint8Array[], waitMs: number): Promise<Uint8Array> => {
    const t0 = Date.now();
    const q = waitMs > 0 ? `?wait=${waitMs}` : '';
    const res = await fetch(args.url + '/api/rpc' + q, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/octet-stream',
        'X-Pong-Session': sessionId,
      },
      body: frames.length ? concatFrames(frames) : new Uint8Array(0),
    });
    const rtt = Date.now() - t0;
    rtts.push(rtt);
    if (rtt < minRtt) minRtt = rtt;
    responses++;
    if (res.status === 204) return new Uint8Array(0);
    if (!res.ok) throw new Error(`rpc ${res.status}`);
    return new Uint8Array(await res.arrayBuffer());
  };

  const consume = (buf: Uint8Array): void => {
    if (buf.length === 0) return;
    const { frames, error } = parseFrames(buf);
    if (error !== 0) throw new Error(`parse error ${error}`);
    let prevTick = -1;

    for (const f of frames) {
      switch (f.type) {
        case MsgType.SNAPSHOT: {
          const s = decodeSNAPSHOT(f.buf, f.payloadOff);
          snapshots++;
          if (prevTick >= 0) tickGaps.push(s.tick - prevTick);
          prevTick = s.tick;
          if (s.tick > lastTickSeen) lastTickSeen = s.tick;
          ballX = s.ballXQ4;
          ballY = s.ballYQ4;
          scoreL = s.scoreL;
          scoreR = s.scoreR;
          phase = s.state;
          break;
        }
        case MsgType.MATCH_START: {
          const m = decodeMATCH_START(f.buf, f.payloadOff);
          mySide = m.yourSide;
          matched = true;
          const slow = (m.matchFlags & 1) !== 0;
          log(`match ${m.matchId}: seat ${mySide === 0 ? 'LEFT' : 'RIGHT'}` +
              `  first to ${m.winScore}${slow ? '  [SLOW MODE]' : ''}`);
          break;
        }
        case MsgType.EVENT: {
          const e = decodeEVENT(f.buf, f.payloadOff);
          log(`  event ${EventKindName[e.kind] ?? e.kind} (${e.b}-${e.c})`);
          break;
        }
        case MsgType.PONG: {
          const p = decodePONG(f.buf, f.payloadOff);
          // Min-RTT clock sync: the least-queued sample is the best estimate.
          const rtt = Date.now() - p.clientTimeMs;
          if (rtt <= minRtt) clockOffset = p.serverTimeMs + rtt / 2 - Date.now();
          break;
        }
        default:
          break;
      }
    }
    if (frames.length > 1) batchSizes.push(frames.filter((f) => f.type === MsgType.SNAPSHOT).length);
  };

  /* -- 3. join ------------------------------------------------------------ */
  consume(await post([encodeJOIN({
    mode: args.mode,
    flags: 0,
    roomCode: padBytes(args.room, C.ROOM_CODE_BYTES),
  }, ++inputSeq)], 0));

  /* -- 4. the loop -------------------------------------------------------- */
  const periodMs = 1000 / args.hz;
  const until = Date.now() + args.seconds * 1000;
  let nextPing = 0;

  while (Date.now() < until) {
    const started = Date.now();

    // Track the ball, exactly as the 3DS's input code will.
    targetY = clamp(ballY, PADDLE_HALF_Q, FIELD_H_Q - PADDLE_HALF_Q);

    const out: Uint8Array[] = [encodeINPUT({
      inputSeq: ++inputSeq,
      lastTickSeen,
      desiredYQ4: targetY,
      buttons: 0,
      flags: 0,
    }, inputSeq & 0xffff)];

    if (Date.now() >= nextPing) {
      out.push(encodePING({ clientTimeMs: Date.now(), echoServerTimeMs: 0 }, 0));
      nextPing = Date.now() + 2000;
    }

    consume(await post(out, args.wait));

    // Self-pace: hold the target rate on a fast link, degrade gracefully on a
    // slow one rather than queueing requests. This is the 3DS worker's policy.
    const elapsed = Date.now() - started;
    const sleep = Math.max(0, periodMs - elapsed);
    if (sleep > 0) await new Promise((r) => setTimeout(r, sleep));
  }

  /* -- 5. report ---------------------------------------------------------- */
  const pct = (a: number[], p: number): number => {
    if (!a.length) return 0;
    const s = [...a].sort((x, y) => x - y);
    return s[Math.min(s.length - 1, Math.floor((p / 100) * s.length))] as number;
  };
  const avg = (a: number[]): number => (a.length ? a.reduce((x, y) => x + y, 0) / a.length : 0);

  console.log('');
  console.log('=== RR transport report ===');
  console.log(`matched         : ${matched}`);
  console.log(`phase           : ${MatchStateName[phase] ?? phase}`);
  console.log(`score           : ${scoreL} - ${scoreR}`);
  console.log(`responses       : ${responses} in ${args.seconds}s (${(responses / args.seconds).toFixed(1)}/s, asked ${args.hz})`);
  console.log(`snapshots       : ${snapshots} (${(snapshots / args.seconds).toFixed(1)}/s effective)`);
  console.log(`snapshots/resp  : avg ${avg(batchSizes).toFixed(2)}  max ${Math.max(0, ...batchSizes)}`);
  console.log(`tick gap in batch: avg ${avg(tickGaps).toFixed(2)} ticks (2 == true 30Hz sampling)`);
  console.log(`rtt ms          : min ${minRtt}  p50 ${pct(rtts, 50)}  p95 ${pct(rtts, 95)}`);
  console.log(`clock offset ms : ${clockOffset.toFixed(1)}`);
  console.log(`last tick seen  : ${lastTickSeen}`);

  const ok = matched && snapshots > args.seconds * args.hz * 0.5;
  console.log(ok ? '\nPASS: request/response transport is carrying a live match' : '\nFAIL: not receiving state');
  process.exit(ok ? 0 : 1);
}

main().catch((e) => {
  console.error('rrclient failed:', e);
  process.exit(1);
});
