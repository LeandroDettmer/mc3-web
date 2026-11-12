// The page's link to the server's System Link switch (web/server, ws://<host>/lan; after nfsu2-web's, MIT, th0mk):
// one WebSocket, the switch's control messages, and the game's datagram frames, which web/net.js moves to and from
// the game's mailbox (xboxrecomp/src/net/xnet_web.c).  Loaded by the start page; the game finds the link as
// Module.mc3Lan.
//
//   const link = mc3Lan(name[, url]); // connects at once; url: another server's switch (config.json lanUrl), else this host's /lan
//   link.on(l => ...)                // l.state 'connecting' | 'online' | 'offline', l.vip, l.vipStr, l.peers [{vip, vipStr, name}]
//                                    // a lost link goes 'offline', then 'connecting' again by itself until close() (see connect())
//   link.send(frame) / link.onframe  // binary frames (net.js); link.rename(name); link.close()
//   link.askRoam()                   // the server's free-roam players (its hub) into l.roam [{name, status}], then l.on's
//   link.where(w)                    // what the page is doing, for the players list: 'page' | 'loading' | 'menu' | 'career' | 'arcade'
//                                    // ({"t":"where","w"}, only on change, at most every 5 s, again after a reconnect's hello)
//   l.players                        // who is online, pushed by the switch when it changes (web/lan-worker/src/switch.js
//                                    // {"t":"players"}: online, freeRoam, races, modes, list [{name, mode, city?, race?}], more);
//                                    // null while offline or from a switch without it (web/server): the page polls /lan/stats then
// Races (rooms, docs/races-protocol.md): the list is asked for on connecting and kept current.
//   l.rooms [{id, name, host, hostVip, city, mode, n, max, locked}], l.roomsSeen (the list has come),
//   l.room (the race we are in, or null), l.members [{vip, vipStr, name, host}], l.left {room, why} (last left),
//   l.error {op, code} (last refusal; cleared by the next request)
//   l.refused 'tabs' when the switch closed this link for a third page of the browser (4002); the link stays offline
//   link.create({name, pass, city, mode, max}) / link.join(id, pass) / link.leave()
//   link.signal(to, kind, data) / link.onsignal = m => ...   // step 2 (WebRTC), relayed inside the race only
//   link.onMsg(m => ...) / link.sendMsg(m) // every JSON control message in / one out (docs/races-protocol.md)
//   link.rtc                         // set by mc3Rtc (web/rtc.js): net.js sends System Link's frames through it
// Free roam per city (web/lan-worker/src/city.js): when the switch's welcome says roamCity, free roam's frames and
// signals go over a second socket, straight to the city's own object:
//   link.roamSend(frame)             // a free-roam frame (net.js, rtc.js): the city socket when open, else the switch
//   link.roamMsg(m)                  // a free-roam signal (rtc.js): the same way
//   l.city                           // the city whose socket is open, or null (free roam through the switch)
//   l.roaming                        // true between the game's Free Roam HELLO and BYE (index.html's new-version notice)
//   l.career                         // that session is an online career's (index.html sets it; cleared with roaming): no forced reload
//   l.roamDown                    // ms timestamp the free-roam link has been lost since (3 s grace), 0 = fine (pre.js notice)
// The game's HELLO to the hub (10.77.0.253:47210) names its room (MC3: the city): a new one closes the old socket, asks
// the switch {"t":"ticket"} and opens wss .../lan/roam?city=N&vip&exp&sig; its BYE closes it. Until the socket is open,
// and whenever it is not (refused, a server without it), the frames go through the switch as before. A socket that never
// opened is tried again after 5 s, 10, 20, 40, then every 60 s, one that closed after opening after 1 s, at the next HELLO;
// a ticket unanswered for 10 s is asked again. Soon, because the switch's hub and the city's never share players: a
// page left on the switch meets nobody of its city who is on the city socket (2026-11-11: a San Diego player unseen).
window.mc3Lan = function (name, lanUrl) {
  var url = lanUrl || (location.protocol == 'https:' ? 'wss://' : 'ws://') + location.host + '/lan';
  var link = { url: url, state: 'connecting', vip: 0, vipStr: '', mac: '', peers: [], roam: [], sent: 0, recv: 0, onframe: null,
               players: null, rooms: [], roomsSeen: false, room: null, members: [], left: null, error: null, onsignal: null, refused: null, city: null, roamDown: 0 };
  var HUB = 0x0A4D00FD, ROAM_PORT = 47210, CITIES = 14, TICKET_WAIT = 10000, cityOn = false, city = null;   // city: {n, ws, asked, askedAt, open, fails, retryAt}
  var fns = [], msgFns = [], ws = null, beat = 0, want = true, tries = 0, retry = 0, heard = 0, pongs = false, beats = false, sub = 1, rejoin = null, pass = '', auto = false;
  // The keep-alive: the Cloudflare switch's runtime answers this exact text itself (setWebSocketAutoResponse,
  // web/races-worker/src/index.js BEAT), so an idle page never wakes its Durable Object. web/server ignores it: until
  // a beat comes back, the page sends {"t":"ping"} instead (answered with a pong by every switch).
  var BEAT = '{"t":"beat"}';
  function changed() { fns.forEach(function (f) { try { f(link); } catch (e) { console.error(e); } }); }
  function up() { return ws && ws.readyState == 1; }
  // A random id per browser: the switch lets the same browser take its own nick back from a ghost socket of an earlier visit.
  var cid = '';
  try { cid = localStorage.getItem('mc3.cid') || ''; if (!/^[\w-]{8,40}$/.test(cid)) { cid = (Math.random().toString(36).slice(2) + Math.random().toString(36).slice(2) + '00000000').slice(0, 20); localStorage.setItem('mc3.cid', cid); } } catch (_) {}
  function hello() { ws.send(JSON.stringify({ t: 'hello', name: name || '', lan: 1, cid: cid || undefined })); }
  // What this page is doing (link.where): sent only when it changed, at most once per WHERE_MS (an incoming message
  // is billed by the Worker), and again after every (re)connect's hello: a new socket starts as 'menu' on the switch.
  var WHERE_MS = 5000, now = '', told = '', toldAt = 0, whereT = 0;
  function tellWhere() {
    whereT = 0;
    if (!up() || !now || now == told) return;
    told = now; toldAt = Date.now();
    ws.send(JSON.stringify({ t: 'where', w: now }));
  }
  // The link stays wanted until close(): a drop (network, proxy idle, the server's or Cloud Run's request timeout,
  // a redeploy) reconnects after 1, 2, 4, 8, 15 s (+-25%), asking for the same address back (?vip=, the switch gives
  // it if free), then says hello, subscribes and rejoins the race room again.  The same link object throughout, so
  // web/net.js's mailboxes and web/rtc.js stay on it.
  function connect() {
    retry = 0;
    var u = url;
    if (link.vip) u += (u.indexOf('?') < 0 ? '?' : '&') + 'vip=' + link.vip;
    try { ws = new WebSocket(u); } catch (e) { ws = null; link.state = 'offline'; want = false; changed(); return; }   // a bad URL: no retry
    var me = ws;
    ws.binaryType = 'arraybuffer';
    ws.onopen = function () {
      heard = Date.now();
      hello();
      told = ''; tellWhere();
      if (sub) ws.send(JSON.stringify({ t: 'rooms', sub: 1 }));
      ws.send(JSON.stringify({ t: 'players', sub: 1 }));
      if (rejoin) { auto = true; ws.send(JSON.stringify({ t: 'join', room: rejoin.id, pass: rejoin.pass })); }
      ws.send(BEAT);   // does this switch answer beats?
      // keeps an idle page joined (the server's IDLE, proxies); answered, so a dead link shows as silence
      beat = setInterval(function () {
        if (me.readyState != 1) return;
        if ((pongs || beats) && Date.now() - heard > 50000) { try { me.close(4000, 'no answer'); } catch (_) {} lost(me); return; }   // a dead TCP closes late
        me.send(beats ? BEAT : JSON.stringify({ t: 'ping', id: Date.now() }));
      }, 20000);
    };
    ws.onmessage = function (e) {
      heard = Date.now();
      if (typeof e.data != 'string') {
        var f = new Uint8Array(e.data);
        // the switch's own hub keeps listing us ~10 s after the city socket opens (its gone_ms): two hubs, two lists
        // (other players, stale FAR) into one core; with the city socket open only the city's hub counts
        if (f.length >= 8 && f[4] == 10 && f[5] == 77 && f[6] == 0 && f[7] == 253 && cityUp()) return;
        link.recv++; if (link.onframe) link.onframe(f); return;
      }
      if (e.data === BEAT) { beats = true; return; }
      var m;
      try { m = JSON.parse(e.data); } catch (_) { return; }
      if (m.t == 'welcome') { tries = 0; link.vip = m.vip >>> 0; link.vipStr = m.vipStr; link.mac = m.mac; link.state = 'online'; cityOn = m.roamCity == 1; changed(); }
      else if (m.t == 'ticket') cityOpen(m);
      else if (m.t == 'pong') pongs = true;
      else if (m.t == 'peers') { link.peers = m.peers || []; changed(); }
      else if (m.t == 'roam') { link.roam = m.players || []; changed(); }
      else if (m.t == 'players') { link.players = m; changed(); }
      else if (m.t == 'rooms') { link.rooms = m.rooms || []; link.roomsSeen = true; changed(); }
      else if (m.t == 'room') {
        link.room = m.room; link.members = m.members || []; auto = false;
        // a member comes back to its room; a host cannot (its leaving closed the room: no host migration)
        rejoin = link.room && (link.room.hostVip >>> 0) != link.vip ? { id: link.room.id, pass: pass } : null;
        changed();
      }
      else if (m.t == 'left') { if (link.room && link.room.id == m.room) { link.room = null; link.members = []; } if (rejoin && rejoin.id == m.room) rejoin = null; link.left = m; changed(); }
      else if (m.t == 'error' && m.op == 'ticket') { if (city) cityFail(city, false); }
      else if (m.t == 'error') { if (auto && m.op == 'join') { auto = false; rejoin = null; } else link.error = m; changed(); }   // a rejoin refused (the room went meanwhile): quietly in none
      else if (m.t == 'signal') { if (link.onsignal) link.onsignal(m); }
      msgFns.forEach(function (f) { try { f(m); } catch (e) { console.error(e); } });
    };
    // 4001: this browser's newer page took our place; 4002: a third page of this browser did (link.refused = 'tabs'): both
    // stay offline until the page is opened again, else the tabs would evict one another in a loop. 4004: reconnecting
    // too fast, so wait the longest backoff. Anything else (4003 no hello, 1006 a refused upgrade, ...): back off and retry.
    ws.onclose = function (e) {
      var code = e && e.code;
      if (code == 4001 || code == 4002) { want = false; link.refused = code == 4002 ? 'tabs' : null; }
      else if (code == 4004) tries = 4;
      lost(me);
    };
  }
  function lost(me) {
    if (me !== ws) return;
    ws = null;
    clearInterval(beat);
    cityClose();   // the next vip may differ: a new ticket at the next HELLO
    link.state = 'offline'; link.room = null; link.members = []; link.peers = []; link.players = null;
    changed();   // 'offline' first: web/rtc.js drops its channels, net.js tells the game the link is down
    if (!want) return;
    var d = Math.min(15000, 1000 << Math.min(tries++, 4)) * (0.75 + Math.random() * 0.5);
    link.state = 'connecting'; changed();
    retry = setTimeout(connect, d);
  }
  // --- free roam's city socket (see the head) ---
  function cityUp() { return !!(city && city.open && city.ws && city.ws.readyState == 1); }
  function cityClose() {
    var c = city;
    city = null;
    if (c && c.ws) { c.ws.onclose = c.ws.onmessage = null; try { c.ws.close(1000, 'left'); } catch (_) {} }
    if (link.city != null) { link.city = null; changed(); }
  }
  function cityFail(c, wasOpen) {
    if (c.ws) { c.ws.onclose = c.ws.onmessage = null; try { c.ws.close(); } catch (_) {} }
    c.ws = null; c.asked = false; c.open = false;
    c.retryAt = Date.now() + (wasOpen ? 1000 : Math.min(60000, 5000 << Math.min(c.fails++, 4)));
    if (link.city != null) { link.city = null; changed(); }
  }
  function cityWant(n) {
    if (!cityOn || !up() || !(n < CITIES)) return;
    var t = Date.now();
    if (city && city.n == n) { if (city.ws || (city.asked && t - city.askedAt < TICKET_WAIT) || t < city.retryAt) return; }
    else { cityClose(); city = { n: n, ws: null, asked: false, askedAt: 0, open: false, fails: 0, retryAt: 0 }; }
    city.asked = true; city.askedAt = t;
    ws.send(JSON.stringify({ t: 'ticket' }));
  }
  function cityOpen(t) {
    var c = city;
    if (!c || !c.asked || c.ws) return;
    c.asked = false;
    try {
      var u = new URL(url, location.href);
      u.pathname = u.pathname.replace(/\/$/, '') + '/roam';
      u.search = '?city=' + c.n + '&vip=' + (t.vip >>> 0) + '&exp=' + Number(t.exp) + '&sig=' + encodeURIComponent(t.sig);
      c.ws = new WebSocket(u.href);
    } catch (e) { cityFail(c, false); return; }
    var s = c.ws;
    s.binaryType = 'arraybuffer';
    s.onopen = function () { if (city !== c) return; c.open = true; c.fails = 0; link.city = c.n; changed(); };
    s.onmessage = function (e) {
      if (typeof e.data != 'string') { link.recv++; if (link.onframe) link.onframe(new Uint8Array(e.data)); return; }
      var m;
      try { m = JSON.parse(e.data); } catch (_) { return; }
      msgFns.forEach(function (f) { try { f(m); } catch (err) { console.error(err); } });   // free-roam signals: web/rtc.js
    };
    s.onclose = function () { if (city === c) cityFail(c, c.open); };
  }
  // A free-roam frame (16-byte header, net.js): its HELLO / BYE to the hub steer the city socket.
  link.roamSend = function (f) {
    if (f.length >= 22 && f[8] == 10 && f[9] == 77 && f[10] == 0 && f[11] == 253 && ((f[14] << 8) | f[15]) == ROAM_PORT &&
        f[16] == 0x4D && f[17] == 0x43 && f[18] == 0x33 && f[19] == 0x52) {
      if (f[21] == 1 && f.length >= 44) { roaming = link.roaming = true; cityWant((f[40] | f[41] << 8 | f[42] << 16 | f[43] << 24) >>> 0); }   // HELLO: room u32 LE at datagram +24
      else if (f[21] == 8) { roaming = link.roaming = link.career = false; var ok =roamOut(f); cityClose(); watch(); return ok; }   // BYE: the game left free roam
    }
    return roamOut(f);
  };
  // link.roamDown: ms timestamp since when Free Roam (a HELLO sent, no BYE yet) has lost its link for 3 s running, else 0:
  // the switch socket is not up, or city objects are on and the city socket is not open (so also a page left on the
  // switch's fallback hub). web/pre.js shows the notice from it, web/net.js logs it; it clears as soon as both are open.
  var roaming = false, badSince = 0, WATCH_GRACE = 3000;
  function watch() {
    var bad = roaming && (!up() || (cityOn && !cityUp())), t = Date.now();
    if (!bad) badSince = 0; else if (!badSince) badSince = t;
    var d = bad && t - badSince >= WATCH_GRACE ? badSince : 0;
    if (d != link.roamDown) { link.roamDown = d; changed(); }
  }
  var watchT = setInterval(watch, 1000);
  function roamOut(f) {
    if (cityUp()) { city.ws.send(f); link.sent++; return true; }
    return link.send(f);
  }
  link.roamMsg = function (m) {
    if (cityUp()) { city.ws.send(JSON.stringify(m)); return true; }
    return link.sendMsg(m);
  };

  connect();
  link.on = function (f) { fns.push(f); f(link); };
  link.send = function (frame) {
    if (!up()) return false;
    ws.send(frame); link.sent++;
    return true;
  };
  link.rename = function (n) { name = n; if (up()) hello(); };
  // Subscribes to the players push again (the switch answers at once); the page calls it when it wants the list and has none.
  link.askPlayers = function () { if (up()) ws.send(JSON.stringify({ t: 'players', sub: 1 })); };
  // w: 'page' | 'loading' | 'menu' | 'career' | 'arcade' (the switch ignores any other); call as often as you like
  link.where = function (w) {
    now = w;
    if (!up() || w == told || whereT) return;
    var wait = toldAt + WHERE_MS - Date.now();
    if (wait <= 0) tellWhere(); else whereT = setTimeout(tellWhere, wait);
  };
  link.askRoam = function () { if (up()) ws.send(JSON.stringify({ t: 'roam' })); };
  link.onMsg = function (f) { msgFns.push(f); };   // every control message (rooms, signal: web/rtc.js)
  link.sendMsg = function (m) { if (m && m.t == 'rooms') sub = m.sub ? 1 : 0; if (up()) ws.send(JSON.stringify(m)); };
  function ctl(m) { if (!up()) return false; link.error = null; ws.send(JSON.stringify(m)); return true; }
  link.create = function (o) { o = o || {}; pass = o.pass || ''; return ctl({ t: 'create', name: o.name || '', pass: o.pass || '', city: o.city || '', mode: o.mode || '', max: o.max || 8 }); };
  link.join = function (id, p) { pass = p || ''; auto = false; return ctl({ t: 'join', room: id, pass: pass }); };
  link.leave = function () { rejoin = null; return ctl({ t: 'leave' }); };
  link.signal = function (to, kind, data) { return ctl({ t: 'signal', to: to, kind: kind, data: data }); };
  link.close = function () { want = false; clearTimeout(retry); clearInterval(watchT); roaming = link.roaming = link.career = false; watch(); cityClose(); if (ws) ws.close(1000, 'closed'); else if (link.state != 'offline') { link.state = 'offline'; changed(); } };
  return link;
};
