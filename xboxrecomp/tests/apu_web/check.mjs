// Headless check of the sine page (index.html, sine.c): Chrome plays it, the
// worklet must consume the ring, hear something, and add no underruns once
// running.  node check.mjs <url> [chrome path]   (Node 22+: global WebSocket)
import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2] ?? 'http://127.0.0.1:3001/';
const chrome = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const port = 9471;
const sleep = ms => new Promise(r => setTimeout(r, ms));
const proc = spawn(chrome, ['--headless=new', `--remote-debugging-port=${port}`, '--autoplay-policy=no-user-gesture-required',
  `--user-data-dir=${mkdtempSync(join(tmpdir(), 'apuweb-'))}`, '--no-first-run', url], { stdio: 'ignore' });

let target;
for (let i = 0; i < 50 && !target; i++) {
  await sleep(200);
  try { target = (await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()).find(t => t.type === 'page' && t.url.startsWith(url)); } catch { /* not up yet */ }
}
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise(r => ws.addEventListener('open', r));
let id = 0;
const evaluate = expr => new Promise(res => {
  const my = ++id;
  const on = e => { const m = JSON.parse(e.data); if (m.id === my) { ws.removeEventListener('message', on); res(m.result?.result?.value); } };
  ws.addEventListener('message', on);
  ws.send(JSON.stringify({ id: my, method: 'Runtime.evaluate', params: { expression: expr, returnByValue: true } }));
});
const stats = () => evaluate('Module.mc3Audio ? Module.mc3Audio() : null');

await sleep(3000);
const a = await stats();
await sleep(5000);
const b = await stats();
console.log('t=3s', JSON.stringify(a));
console.log('t=8s', JSON.stringify(b));
const ok = a && b && b.attached && b.state === 'running' && b.read - a.read > 5 * 0.9 * 48000 &&
  b.quanta > a.quanta && b.loud > a.loud && b.underruns === a.underruns;
console.log(ok ? 'PASS: ' + ((b.read - a.read) / 5 / 1000).toFixed(1) + ' kframes/s consumed, underruns ' + b.underruns
               : 'FAIL ' + JSON.stringify(await evaluate(
                 '({ isolated: crossOriginIsolated, ac: typeof AudioContext, mod: typeof Module, keys: Object.keys(Module).join(), out: document.body.innerText.slice(0, 300) })')));
ws.close(); proc.kill();
process.exit(ok ? 0 : 1);
