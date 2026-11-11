// mc3-lan: the /lan switch on a Cloudflare Worker with one Durable Object (src/switch.js, which says how it works).
// Only classes and the handler are exported here: workerd takes every named export of the entry for an entrypoint.
//
//   GET /lan/stats -> {online, freeRoam: {total, rooms: {<room id>: n}}, races: {rooms}}, CORS for ALLOW_ORIGINS; no origin = same-origin, allowed
//   GET /lan/ice -> {iceServers} Cloudflare Realtime TURN credentials (6 h), cached 10 min; Origin required; STUN-only on failure. Secrets TURN_KEY_ID, TURN_KEY_API_TOKEN
//   GET /lan/activity -> what each player does (private: Authorization: Bearer <ADMIN_KEY>, or ?key=; 404 without it; no CORS, never cached)
//   GET /lan[?vip=n] (WebSocket upgrade, Origin in ALLOW_ORIGINS) -> the Durable Object "Switch" (one for the server)
//   GET /lan/roam?city=N&vip&exp&sig (WebSocket upgrade, Origin, a ticket from the Switch) -> the Durable Object
//       "RoamCity" named city-N (src/city.js: free roam, one object per city); 404 without the ROAM binding or ROAM_SECRET
//
// secret: ADMIN_KEY (wrangler secret put ADMIN_KEY); unset = /lan/activity is always 404.
// secret: ROAM_SECRET (a long random string): signs the city-socket tickets; unset = no city objects, free roam stays on
// the Switch alone as before.
// vars: ALLOW_ORIGINS "https://a.example,https://b.example" (scheme://host, comma-separated, any case); empty = any.
import { originOk, SWITCH_NAME, cityOk, cityName, ticketOk } from './switch.js';

export { Switch } from './switch.js';
export { RoamCity } from './city.js';

// Constant-time compare: both sides hashed to 32 bytes first, so neither the length nor the first wrong byte shows in the time.
async function keyOk(given, want) {
  if (!want || !given) return false;
  const h = async v => new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(v)));
  const [a, b] = await Promise.all([h(given), h(want)]);
  let d = 0;
  for (let i = 0; i < 32; i++) d |= a[i] ^ b[i];
  return d === 0;
}

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    const allowed = (env.ALLOW_ORIGINS || '').split(',').map(s => s.trim()).filter(Boolean);
    if (url.pathname === '/lan/stats' && req.method === 'GET') {
      const origin = req.headers.get('Origin');
      if (origin && !originOk(allowed, origin)) return new Response('origin not allowed', { status: 403 });
      // Edge cache (15 s, per data center): polls are answered here, so they never wake the Durable Object.
      const cache = typeof caches !== 'undefined' ? caches.default : null, key = new Request(url.origin + url.pathname);
      let res = cache && await cache.match(key);
      if (!res) {
        res = await env.SWITCH.get(env.SWITCH.idFromName(SWITCH_NAME)).fetch(req);
        if (cache && res.ok) {
          const body = await res.text();
          ctx?.waitUntil(cache.put(key, new Response(body, { headers: { 'Content-Type': 'application/json', 'Cache-Control': 'public, max-age=15' } })));
          res = new Response(body, { status: res.status });
        }
      }
      return new Response(res.body, { status: res.status, headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Access-Control-Allow-Origin': allowed.length && origin ? origin : '*', Vary: 'Origin' } });
    }
    if (url.pathname === '/lan/ice' && req.method === 'GET') {
      const origin = req.headers.get('Origin');
      if (!origin || !originOk(allowed, origin)) return new Response('origin not allowed', { status: 403 });
      const head = { 'Content-Type': 'application/json', 'Cache-Control': 'private, no-store', 'Access-Control-Allow-Origin': origin, Vary: 'Origin' };
      // One generated set serves everybody for 10 min (the credentials live 6 h): Cloudflare is called once per data center per window.
      const cache = typeof caches !== 'undefined' ? caches.default : null, key = new Request(url.origin + url.pathname);
      let body = null;
      const hit = cache && await cache.match(key);
      if (hit) body = await hit.text();
      else if (env.TURN_KEY_ID && env.TURN_KEY_API_TOKEN) {
        try {
          const r = await fetch(`https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(env.TURN_KEY_ID)}/credentials/generate-ice-servers`, {
            method: 'POST', headers: { Authorization: `Bearer ${env.TURN_KEY_API_TOKEN}`, 'Content-Type': 'application/json' }, body: JSON.stringify({ ttl: 21600 }),
          });
          const j = r.ok ? await r.json() : null;
          if (Array.isArray(j?.iceServers) && j.iceServers.length) {
            body = JSON.stringify({ iceServers: j.iceServers });
            ctx?.waitUntil(cache?.put(key, new Response(body, { headers: { 'Cache-Control': 'public, max-age=600' } })));
          }
        } catch {}
      }
      return new Response(body ?? JSON.stringify({ iceServers: [{ urls: 'stun:stun.cloudflare.com:3478' }] }), { headers: head });
    }
    if (url.pathname === '/lan/activity' && req.method === 'GET') {
      const bearer = /^Bearer (.+)$/.exec(req.headers.get('Authorization') || '')?.[1];
      if (!await keyOk(bearer ?? url.searchParams.get('key'), env.ADMIN_KEY)) return new Response('not found', { status: 404 });
      const res = await env.SWITCH.get(env.SWITCH.idFromName(SWITCH_NAME)).fetch(req);
      return new Response(res.body, { status: res.status, headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' } });
    }
    if (url.pathname === '/lan/roam') {
      if (!originOk(allowed, req.headers.get('Origin'))) return new Response('origin not allowed', { status: 403 });
      if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket only', { status: 426 });
      if (!env.ROAM || !env.ROAM_SECRET) return new Response('not found', { status: 404 });
      const q = url.searchParams, city = Number(q.get('city'));
      if (!cityOk(city) || !await ticketOk(env.ROAM_SECRET, Number(q.get('vip')) >>> 0, Number(q.get('exp')), q.get('sig'))) return new Response('bad ticket', { status: 403 });
      return env.ROAM.get(env.ROAM.idFromName(cityName(city))).fetch(req);
    }
    if (url.pathname !== '/lan') return new Response('not found', { status: 404 });
    if (!originOk(allowed, req.headers.get('Origin'))) return new Response('origin not allowed', { status: 403 });
    if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket only', { status: 426 });
    return env.SWITCH.get(env.SWITCH.idFromName(SWITCH_NAME)).fetch(req);
  },
};
