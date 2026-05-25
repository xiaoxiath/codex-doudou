// Smoke test for reconnect resume: connect, wait for a question, disconnect
// mid-flight, reconnect, and verify the unreplied question gets replayed.
import { WebSocket } from 'ws';

const URL = 'ws://localhost:8788/device';
const DEVICE_ID = 'reconnect-smoke';
const TOKEN = 'dev-token-change-me';

const questionsSeen = new Map(); // id → { count, status: 'open' | 'replied' }
let phase = 1;

function makeClient() {
  let outSeq = 1;
  const next = () => outSeq++;
  const ws = new WebSocket(URL);

  ws.on('open', () => {
    console.log(`[P${phase}] open → hello`);
    ws.send(JSON.stringify({
      v: 1, type: 'hello', seq: next(), ts: 0,
      device_id: DEVICE_ID, fw_version: '0.0.1', pairing_token: TOKEN,
      resume_after_seq: 0,
    }));
  });

  ws.on('message', (raw) => {
    const m = JSON.parse(String(raw));
    if (m.type === 'welcome') {
      console.log(`[P${phase}] welcome session=${m.session_id}`);
    } else if (m.type === 'question') {
      const prev = questionsSeen.get(m.id) ?? { count: 0, status: 'open' };
      prev.count++;
      questionsSeen.set(m.id, prev);
      console.log(`[P${phase}] ← question ${m.id} (seen ${prev.count}x, status=${prev.status})`);
      if (phase === 1) {
        // do NOT reply in phase 1 — we want to test resume
        return;
      }
      if (prev.status === 'open') {
        prev.status = 'replied';
        // reply
        setTimeout(() => {
          ws.send(JSON.stringify({
            v: 1, type: 'reply', seq: next(), ts: 100,
            id: m.id, choice_id: 'accept', device_id: DEVICE_ID,
          }));
          console.log(`[P${phase}] → reply ${m.id}`);
        }, 100);
      }
    } else if (m.type === 'status') {
      console.log(`[P${phase}] ← status ${m.state} "${m.title}"`);
    } else if (m.type === 'ping') {
      ws.send(JSON.stringify({ v: 1, type: 'pong', seq: next(), ts: 200, pong_for_seq: m.seq }));
    } else if (m.type === 'error') {
      console.log(`[P${phase}] ← error ${m.code} "${m.title}"`);
    } else if (m.type === 'ack') {
      console.log(`[P${phase}] ← ack for seq=${m.ack_for_seq}`);
    }
  });

  ws.on('close', (code) => console.log(`[P${phase}] CLOSE ${code}`));
  ws.on('error', () => {});

  return ws;
}

// Phase 1: connect, wait 10s to receive at least one question, drop
const ws1 = makeClient();
setTimeout(() => {
  console.log('--- DROPPING phase 1 connection ---');
  ws1.terminate();
}, 12_000);

// Phase 2: reconnect 2s later, should see replayed question(s)
setTimeout(() => {
  phase = 2;
  console.log('--- RECONNECTING (phase 2) ---');
  const ws2 = makeClient();
  setTimeout(() => {
    console.log('---');
    console.log('SUMMARY:');
    for (const [id, info] of questionsSeen) {
      console.log(`  ${id}: seen ${info.count}x, ${info.status}`);
    }
    ws2.terminate();
    process.exit(0);
  }, 10_000);
}, 14_000);
