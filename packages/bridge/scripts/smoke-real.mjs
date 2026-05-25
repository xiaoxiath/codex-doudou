// Drive real codex: connect as device, POST a prompt that should trigger
// an approval request, observe the question on our wire, accept it.
import { WebSocket } from 'ws';

const URL = 'ws://localhost:8788/device';
const DEVICE_ID = 'real-smoke';
const TOKEN = 'dev-token-change-me';

let outSeq = 1;
const next = () => outSeq++;
const seenTypes = new Set();
let questionId = null;

const ws = new WebSocket(URL);

ws.on('open', () => {
  console.log('OPEN → hello');
  ws.send(JSON.stringify({
    v: 1, type: 'hello', seq: next(), ts: 0,
    device_id: DEVICE_ID, fw_version: '0.0.1-real', pairing_token: TOKEN,
  }));
});

ws.on('message', (raw) => {
  const m = JSON.parse(String(raw));
  seenTypes.add(m.type);
  if (m.type === 'welcome') {
    console.log(`welcome session=${m.session_id}, kicking off prompt`);
    setTimeout(async () => {
      const r = await fetch('http://localhost:8788/api/prompt', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ text: 'Run `git status` in the current directory. Do not change anything.' }),
      });
      console.log('prompt POST →', r.status);
    }, 500);
  } else if (m.type === 'status') {
    console.log(`← status ${m.state} "${m.title}"`);
  } else if (m.type === 'question') {
    questionId = m.id;
    console.log(`← question ${m.id} [${m.risk}/${m.action_type}] "${m.title}" body="${m.body ?? ''}" confirm=${m.require_confirm}`);
    // accept the approval
    setTimeout(() => {
      ws.send(JSON.stringify({
        v: 1, type: 'reply', seq: next(), ts: 100,
        id: m.id, choice_id: 'accept', device_id: DEVICE_ID,
      }));
      console.log(`→ reply ${m.id} = accept`);
    }, 800);
  } else if (m.type === 'ping') {
    ws.send(JSON.stringify({ v: 1, type: 'pong', seq: next(), ts: 200, pong_for_seq: m.seq }));
  } else if (m.type === 'error') {
    console.log(`← error ${m.code} "${m.title}"`);
  } else if (m.type === 'ack') {
    console.log(`← ack seq=${m.ack_for_seq}`);
  }
});

ws.on('close', (code) => console.log('CLOSE', code));
ws.on('error', (e) => console.log('ERR', String(e)));

// hard timeout
setTimeout(() => {
  console.log('---');
  console.log('SUMMARY: types seen:', [...seenTypes].sort().join(','), 'first questionId:', questionId);
  ws.terminate();
  process.exit(0);
}, 60_000);
