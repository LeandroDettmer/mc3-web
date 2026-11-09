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
//   l.players                        // who is online, pushed by the switch when it changes (web/lan-worker/src/switch.js
//                                    // {"t":"players"}: online, freeRoam, races, modes, list [{name, mode, city?, race?}], more);
//                                    // null while offline or from a switch without it (web/server): the page polls /lan/stats then
// Races (rooms, docs/races-protocol.md): the list is asked for on connecting and kept current.
//   l.rooms [{id, name, host, hostVip, city, mode, n, max, locked}], l.roomsSeen (the list has come),
//   l.room (the race we are in, or null), l.members [{vip, vipStr, name, host}], l.left {room, why} (last left),
//   l.error {op, code} (last refusal; cleared by the next request)
//   link.create({name, pass, city, mode, max}) / link.join(id, pass) / link.leave()
//   link.signal(to, kind, data) / link.onsignal = m => ...   // step 2 (WebRTC), relayed inside the race only
//   link.onMsg(m => ...) / link.sendMsg(m) // every JSON control message in / one out (docs/races-protocol.md)
//   link.rtc                         // set by mc3Rtc (web/rtc.js): net.js sends System Link's frames through it
window.mc3Lan = function (name, lanUrl) {
  var url = lanUrl || (location.protocol == 'https:' ? 'wss://' : 'ws://') + location.host + '/lan';
  var link = { url: url, state: 'connecting', vip: 0, vipStr: '', mac: '', peers: [], roam: [], sent: 0, recv: 0, onframe: null,
               players: null, rooms: [], roomsSeen: false, room: null, members: [], left: null, error: null, onsignal: null };
  var fns = [], msgFns = [], ws = null, beat = 0, want = true, tries = 0, retry = 0, heard = 0, pongs = false, beats = false, sub = 1, rejoin = null, pass = '', auto = false;
  // The keep-alive: the Cloudflare switch's runtime answers this exact text itself (setWebSocketAutoResponse,
  // web/races-worker/src/index.js BEAT), so an idle page never wakes its Durable Object. web/server ignores it: until
  // a beat comes back, the page sends {"t":"ping"} instead (answered with a pong by every switch).
  var BEAT = '{"t":"beat"}';
  function changed() { fns.forEach(function (f) { try { f(link); } catch (e) { console.error(e); } }); }
  function up() { return ws && ws.readyState == 1; }
  function hello() { ws.send(JSON.stringify({ t: 'hello', name: name || '', lan: 1 })); }
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
      if (typeof e.data != 'string') { link.recv++; if (link.onframe) link.onframe(new Uint8Array(e.data)); return; }
      if (e.data === BEAT) { beats = true; return; }
      var m;
      try { m = JSON.parse(e.data); } catch (_) { return; }
      if (m.t == 'welcome') { tries = 0; link.vip = m.vip >>> 0; link.vipStr = m.vipStr; link.mac = m.mac; link.state = 'online'; changed(); }
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
      else if (m.t == 'error') { if (auto && m.op == 'join') { auto = false; rejoin = null; } else link.error = m; changed(); }   // a rejoin refused (the room went meanwhile): quietly in none
      else if (m.t == 'signal') { if (link.onsignal) link.onsignal(m); }
      msgFns.forEach(function (f) { try { f(m); } catch (e) { console.error(e); } });
    };
    ws.onclose = function () { lost(me); };
  }
  function lost(me) {
    if (me !== ws) return;
    ws = null;
    clearInterval(beat);
    link.state = 'offline'; link.room = null; link.members = []; link.peers = []; link.players = null;
    changed();   // 'offline' first: web/rtc.js drops its channels, net.js tells the game the link is down
    if (!want) return;
    var d = Math.min(15000, 1000 << Math.min(tries++, 4)) * (0.75 + Math.random() * 0.5);
    link.state = 'connecting'; changed();
    retry = setTimeout(connect, d);
  }
  connect();
  link.on = function (f) { fns.push(f); f(link); };
  link.send = function (frame) {
    if (!up()) return false;
    ws.send(frame); link.sent++;
    return true;
  };
  link.rename = function (n) { name = n; if (up()) hello(); };
  link.askRoam = function () { if (up()) ws.send(JSON.stringify({ t: 'roam' })); };
  link.onMsg = function (f) { msgFns.push(f); };   // every control message (rooms, signal: web/rtc.js)
  link.sendMsg = function (m) { if (m && m.t == 'rooms') sub = m.sub ? 1 : 0; if (up()) ws.send(JSON.stringify(m)); };
  function ctl(m) { if (!up()) return false; link.error = null; ws.send(JSON.stringify(m)); return true; }
  link.create = function (o) { o = o || {}; pass = o.pass || ''; return ctl({ t: 'create', name: o.name || '', pass: o.pass || '', city: o.city || '', mode: o.mode || '', max: o.max || 8 }); };
  link.join = function (id, p) { pass = p || ''; auto = false; return ctl({ t: 'join', room: id, pass: pass }); };
  link.leave = function () { rejoin = null; return ctl({ t: 'leave' }); };
  link.signal = function (to, kind, data) { return ctl({ t: 'signal', to: to, kind: kind, data: data }); };
  link.close = function () { want = false; clearTimeout(retry); if (ws) ws.close(1000, 'closed'); else if (link.state != 'offline') { link.state = 'offline'; changed(); } };
  return link;
};
