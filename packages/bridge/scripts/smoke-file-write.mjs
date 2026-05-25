import { WebSocket } from 'ws';
const ws = new WebSocket('ws://localhost:8788/device');
let seq = 1; const next = () => seq++;
ws.on('open', () => {
  ws.send(JSON.stringify({ v:1, type:'hello', seq:next(), ts:0, device_id:'fsmoke', fw_version:'0.0', pairing_token:'dev-token-change-me' }));
});
ws.on('message', async (raw) => {
  const m = JSON.parse(String(raw));
  if (m.type === 'welcome') {
    console.log('welcome → POST prompt');
    setTimeout(async () => {
      const r = await fetch('http://localhost:8788/api/prompt', {
        method:'POST', headers:{'content-type':'application/json'},
        body: JSON.stringify({ text: 'Create a file called /tmp/doudou-smoke-test.txt with content "hello"' })
      });
      console.log('POST →', r.status);
    }, 300);
  } else if (m.type === 'question') {
    console.log('★ QUESTION:', JSON.stringify({ id: m.id, risk: m.risk, action_type: m.action_type, title: m.title, body: m.body, confirm: m.require_confirm }));
    setTimeout(() => ws.send(JSON.stringify({ v:1, type:'reply', seq:next(), ts:0, id:m.id, choice_id:'accept', device_id:'fsmoke' })), 200);
  } else if (m.type === 'status') {
    console.log('status:', m.state, m.title);
  } else if (m.type === 'ping') {
    ws.send(JSON.stringify({ v:1, type:'pong', seq:next(), ts:0, pong_for_seq:m.seq }));
  } else if (m.type === 'error') {
    console.log('err:', m.code, m.title);
  }
});
ws.on('error', () => {});
setTimeout(() => { console.log('TIMEOUT'); process.exit(0); }, 60000);
