// GET /lan/ice: origin check, 10-min cache, STUN fallback. fetch and caches are faked. node --test test/
import test from 'node:test';
import assert from 'node:assert/strict';
import worker from '../src/index.js';

const ORIGIN = 'https://play.example';
const env = { ALLOW_ORIGINS: ORIGIN, TURN_KEY_ID: 'kid-secret', TURN_KEY_API_TOKEN: 'tok-secret' };
const SET = { iceServers: [{ urls: ['stun:stun.cloudflare.com:3478'] }, { urls: ['turn:turn.cloudflare.com:3478?transport=udp'], username: 'u', credential: 'c' }] };
const req = origin => new Request('https://w.example/lan/ice', { headers: origin ? { Origin: origin } : {} });

function fake(apiRes) {
  const store = new Map(), calls = [];
  globalThis.caches = { default: { match: async k => store.get(k.url)?.clone(), put: async (k, r) => { store.set(k.url, r); } } };
  globalThis.fetch = async (u, init) => { calls.push({ u, init }); return apiRes(); };
  const pending = [], ctx = { waitUntil: p => pending.push(p) };
  return { calls, ctx, flush: () => Promise.all(pending) };
}

test('no origin or a foreign one: 403, no API call', async () => {
  const f = fake(() => Response.json(SET));
  for (const o of [null, 'https://evil.example']) assert.equal((await worker.fetch(req(o), env, f.ctx)).status, 403);
  assert.equal(f.calls.length, 0);
});

test('credentials generated once, then served from the cache; headers and body', async () => {
  const f = fake(() => Response.json(SET));
  const a = await worker.fetch(req(ORIGIN), env, f.ctx);
  await f.flush();
  assert.equal(a.headers.get('Access-Control-Allow-Origin'), ORIGIN);
  assert.equal(a.headers.get('Cache-Control'), 'private, no-store');
  const text = await a.text();
  assert.deepEqual(JSON.parse(text), SET);
  assert.ok(!text.includes('kid-secret') && !text.includes('tok-secret'));
  assert.deepEqual(JSON.parse(f.calls[0].init.body), { ttl: 21600 });
  assert.equal(f.calls[0].init.headers.Authorization, 'Bearer tok-secret');
  const b = await worker.fetch(req(ORIGIN), env, f.ctx);
  assert.deepEqual(await b.json(), SET);
  assert.equal(f.calls.length, 1);
});

test('API failure, throw or missing secrets: STUN only, 200, not cached', async () => {
  const stun = { iceServers: [{ urls: 'stun:stun.cloudflare.com:3478' }] };
  for (const [e, api] of [[env, () => new Response('no', { status: 500 })], [env, () => { throw new Error('down'); }], [{ ALLOW_ORIGINS: ORIGIN }, () => Response.json(SET)]]) {
    const f = fake(api);
    const r = await worker.fetch(req(ORIGIN), e, f.ctx);
    await f.flush();
    assert.equal(r.status, 200);
    assert.deepEqual(await r.json(), stun);
    assert.equal(await globalThis.caches.default.match(new Request('https://w.example/lan/ice')), undefined);
  }
});
