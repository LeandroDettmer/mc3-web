// Two headless Chromes on the broadcast page (index.html, bcast.c), both on the server's switch: each must get a
// virtual address and hear the other's broadcasts through its game-side socket.
//   node check.mjs <url> [chrome path]   (Node 22+: global WebSocket; CDP driver after the start page's cdp_run.mjs)
import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2] ?? 'http://127.0.0.1:3002/';
const chrome = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function browser(port, name) {
  const proc = spawn(chrome, ['--headless=new', `--remote-debugging-port=${port}`, '--remote-allow-origins=*',
    `--user-data-dir=${mkdtempSync(join(tmpdir(), 'xnetweb-'))}`, '--no-first-run', 'about:blank'], { stdio: 'ignore' });
  let ws;
  for (let i = 0; i < 50 && !ws; i++) {
    await sleep(200);
    try {
      const page = (await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()).find(t => t.type === 'page');
      if (page) ws = new WebSocket(page.webSocketDebuggerUrl);
    } catch { /* not up yet */ }
  }
  await new Promise(r => ws.addEventListener('open', r));
  let id = 0;
  const pending = new Map(), lines = [];
  ws.addEventListener('message', e => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
    if (m.method === 'Runtime.consoleAPICalled') lines.push(m.params.args.map(a => a.value ?? a.description ?? '').join(' '));
    if (m.method === 'Runtime.exceptionThrown') lines.push('[exception] ' + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text));
  });
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  await send('Runtime.enable');
  await send('Page.navigate', { url: url + '?name=' + name });
  const evaluate = async expr => (await send('Runtime.evaluate', { expression: expr, returnByValue: true })).result?.result?.value;
  // Browser.close ends every Chrome process; killing the launcher alone left them listening on the port
  return { name, lines, evaluate, close: async () => { await Promise.race([send('Browser.close'), sleep(2000)]); proc.kill(); } };
}

const a = await browser(9777, 'A'), b = await browser(9778, 'B');
let ok = false, sa, sb;
for (let t = 0; t < 40 && !ok; t++) {
  await sleep(500);
  [sa, sb] = [await a.evaluate('Module.mc3Net && Module.mc3Net()'), await b.evaluate('Module.mc3Net && Module.mc3Net()')];
  const heard = (x, y) => x.lines.some(l => l.startsWith('[RX] ' + y + ':1000 hello from ' + y));
  ok = !!(sa?.vip && sb?.vip && sa.vip !== sb.vip && heard(a, sb.vip) && heard(b, sa.vip));
}
for (const x of [a, b]) {
  console.log(`--- ${x.name}`);
  console.log(x.lines.filter(l => /^\[(RX|XNET|net)\]|^bcast|exception/.test(l)).slice(0, 12).join('\n'));
}
console.log('A', JSON.stringify(sa));
console.log('B', JSON.stringify(sb));
console.log(ok ? `PASS: ${sa.vip} and ${sb.vip} hear each other's broadcasts through the switch` : 'FAIL');
await Promise.all([a.close(), b.close()]);
process.exit(ok ? 0 : 1);
