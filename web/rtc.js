// WebRTC between pages (docs/races-protocol.md, "Step 2"): unordered, unreliable data channels signalled through the
// /lan link (web/lan.js), the switch as fallback relay. Loaded by the start page after lan.js; web/net.js sends the
// game's frames through link.rtc when it is there. Two kinds of pairs, each its own RTCPeerConnection:
//  - race room: one channel to each other member of the page's room (System Link's frames);
//  - free roam: channels to the players the game actually sends free-roam frames to (port 47210), busiest first
//    (roam_core.c thins snapshots with distance: the busiest are the nearest), at most rtc.roamMax.
//
//   const rtc = mc3Rtc(link[, iceServers]); // sets link.rtc; ICE: the switch's GET .../lan/ice (STUN + TURN), else
//                                           // iceServers (config.json's), else public STUN only (iceFetch below)
//   rtc.send(frame)                         // a System Link frame (16-byte header, net.js): data channel or switch
//   rtc.roamSend(frame)                     // a free-roam frame (net.js): its peer's channel when open, else the switch
//   rtc.roamP2P = false                     // the page's 'Direct connections (P2P) in Free Roam' (default on, live)
//   rtc.roamMax = 6                         // free-roam channels at most (default 12; the page: 6 in low-memory mode)
//   rtc.log = s => ...                      // where the one-line outcomes go (the page's log; default console)
//   rtc.members                             // the room's members [{vip, vipStr, name, host}], [] in no room
//   rtc.mode(vip)                           // 'direct' (room data channel open) | 'relay' (switch)
//   rtc.on(r => ...)                        // after every member or channel change; rtc.close()
//
// Room routing (plan() below, documented in races-protocol.md): a unicast to a member with an open channel goes over
// it, anything else to the switch as before. A broadcast in a room with at least one open channel is split by the
// page: over each open channel, and as a unicast through the switch (destination rewritten to that member) to each
// member without one; the switch never sees it as a broadcast, so it cannot deliver it twice. The game's receive path
// does not see the destination (xnet_web.c's RX slot holds the source only).
//
// Free-roam routing: a frame to the hub (10.77.0.253) or a broadcast always goes to the switch; a unicast to a peer
// with an open free-roam channel goes over it, anything else to the switch. Once a second (tick) the page closes
// channels idle for ROAM.idleMs and offers up to ROAM.perTick new ones to the busiest destinations of the last second
// (at least ROAM.minFrames: a peer that gets only HELLOs stays on the switch) while it holds fewer than roamMax.
// Signals carry "roam":1 (the switch relays them between two players of the same free-roam city; with a city socket
// open, web/lan.js roamMsg, the city's own object does). Relayed frames take link.roamSend (the city socket or the switch). A pair that does
// not open in ROAM.openMs (ICE failed even through TURN, or no TURN) stays on the switch; the offerer tries again after 30, 60, 120 ... s (at most 5 min).
// Cost caps: at most ROAM.relayMax channels may run through a TURN relay (selected ICE pair 'relay', checked when the
// channel opens and when ICE reconnects); a relayed channel beyond that, the least busy, closes and its peer goes
// through the server, retried with the same backoff (both ends refuse it meanwhile). Through the server, a SNAP to a
// peer outside the max(ROAM.near, roamMax) busiest (= nearest; roamMax = the game's cars) is thinned to 1 in ROAM.thin
// (20 Hz -> 5 Hz), unless the core already thinned it by distance (header flags & RC_HF_EVERY = 4 or 10, roam_core.c
// snap_every: 300 m / 1000 m): thinning those again left ~0.5 Hz against the core's 3 s lost timer. HELLO, BYE, FAR and
// anything to the hub never are. Every 30 s in Free Roam, on change: '[rtc] free roam load: N peers, P2P x, relayed y,
// thinned z/s' (frames a second of each kind over those 30 s).
(function (root) {
  var BCAST = 0xFFFFFFFF, KIND = 3, HDR = 16, MAX = 1472, ROAM_PORT = 47210, RETRIES = 3, HUB = 0x0A4D00FD;
  var ROAM = { max: 12, idleMs: 10000, openMs: 15000, perTick: 2, tickMs: 1000, minFrames: 2, backoffMs: 30000, backoffMaxMs: 300000, statsMs: 60000, queue: 65536,
    relayMax: 8, near: 8, thin: 4 };
  var STUN = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun.cloudflare.com:3478' }];
  function vs(v) { return [v >>> 24, v >>> 16 & 255, v >>> 8 & 255, v & 255].join('.'); }

  // Which path a frame to dst takes: [[vip, 'dc' | 'ws'], ...]; [[BCAST, 'ws']] is the switch's own broadcast.
  function plan(dst, self, members, isOpen) {
    if (dst != BCAST) return [[dst, isOpen(dst) ? 'dc' : 'ws']];
    var others = members.filter(function (v) { return v != self; });
    if (!others.some(isOpen)) return [[BCAST, 'ws']];
    return others.map(function (v) { return [v, isOpen(v) ? 'dc' : 'ws']; });
  }

  // Free roam: the vips to offer a channel now. seen {vip: frames sent to it in the last second}; have: vips holding a
  // channel (open or opening); busiest first, ties by vip, at least minFrames, up to max channels in all and n new.
  function pick(seen, have, max, n, skip) {
    var room = Math.min(max - have.length, n);
    if (room <= 0) return [];
    return Object.keys(seen).map(Number)
      .filter(function (v) { return seen[v] >= ROAM.minFrames && have.indexOf(v) < 0 && !skip(v); })
      .sort(function (a, b) { return seen[b] - seen[a] || a - b; }).slice(0, room);
  }

  function mc3Rtc(link, iceServers) {
    var cfg = { iceServers: iceServers && iceServers.length ? iceServers : STUN };
    var rtc = { members: [], sent: 0, recv: 0, roamP2P: true, roamMax: ROAM.max, roamSent: 0, roamRecv: 0, roamRelay: 0, roamThinned: 0, log: null };
    var room = { peers: {}, roam: false }, fr = { peers: {}, roam: true }, peers = room.peers;
    var tries = {}, timers = {}, fns = [], vips = [];
    var act = {}, bad = {}, near = {}, nearN = 0, refused = false, statsAt = Date.now(), statsLast = '';
    var loadAt = statsAt, load = [0, 0, 0], loadLast = '';
    function nearMax() { return Math.max(ROAM.near, rtc.roamMax); }
    function changed() { fns.forEach(function (f) { try { f(rtc); } catch (e) { console.error(e); } }); }
    function say(s) { try { (rtc.log || console.log)(s); } catch (_) {} }
    function openP(p) { return !!p && p.dc.readyState == 'open' && p.pc.connectionState != 'disconnected'; }
    function isOpen(v) { return openP(peers[v]); }
    // ICE servers from the switch (GET <lan as http(s)>/ice, web/lan-worker: STUN + Cloudflare TURN, credentials for
    // 6 h): asked once per session and again when older than ICE.ageMs; until then, or if it fails (no such route,
    // web/server, offline), config.json's iceServers or public STUN. New pairs take the newest list.
    var ICE = { ageMs: 5 * 3600e3, retryMs: 600e3 }, iceAt = 0, iceTry = -Infinity, iceSaid = false;
    function iceUrl() {
      try {
        var u = new URL(link.url, typeof location == 'object' ? location.href : undefined);
        u.protocol = u.protocol == 'wss:' ? 'https:' : 'http:';
        u.pathname = u.pathname.replace(/\/$/, '') + '/ice';
        return u.href;
      } catch (_) { return null; }
    }
    function iceFetch() {
      var now = Date.now(), u = link.url && typeof fetch == 'function' && iceUrl();
      if (!u || (iceAt && now - iceAt < ICE.ageMs) || now - iceTry < ICE.retryMs) return;
      iceTry = now;
      fetch(u).then(function (r) { if (!r.ok) throw 'HTTP ' + r.status; return r.json(); }).then(function (j) {
        var l = (j && Array.isArray(j.iceServers) ? j.iceServers : []).filter(function (s) { return s && (typeof s.urls == 'string' || Array.isArray(s.urls)); });
        if (!l.length) throw 'no servers';
        var turn = l.some(function (s) { return [].concat(s.urls).some(function (x) { return /^turns?:/.test(x); }); });
        if (!iceAt) say('[rtc] ICE servers from the switch: ' + (turn ? 'STUN + TURN' : 'STUN only'));
        cfg.iceServers = l; iceAt = Date.now();
      }).catch(function (e) {
        if (!iceSaid) say('[rtc] ICE servers: the switch gave none (' + e + '); ' + (iceServers && iceServers.length ? "config.json's" : 'public STUN only, no TURN'));
        iceSaid = true;
      });
    }
    // Whether an open pair goes through a TURN relay (the selected candidate pair); false when unknown.
    function via(pc) {
      var p = pc.getStats ? pc.getStats() : Promise.reject();
      return p.then(function (st) {
        var pair = null;
        st.forEach(function (s) {
          if (s.type == 'transport' && s.selectedCandidatePairId) pair = st.get(s.selectedCandidatePairId) || pair;
          else if (!pair && s.type == 'candidate-pair' && (s.selected || (s.nominated && s.state == 'succeeded'))) pair = s;
        });
        var l = pair && st.get(pair.localCandidateId), r = pair && st.get(pair.remoteCandidateId);
        return !!((l && l.candidateType == 'relay') || (r && r.candidateType == 'relay'));
      }).catch(function () { return false; });
    }
    function signal(to, kind, data, roam) {
      var m = { t: 'signal', to: to, kind: kind, data: data };
      if (roam) { m.roam = 1; if (link.roamMsg) return link.roamMsg(m); }   // the city's own socket when open (web/lan.js)
      link.sendMsg(m);
    }

    function drop(v, T) {
      var p = T.peers[v];
      if (!p) return;
      delete T.peers[v];
      clearTimeout(p.deadline);
      p.dc.onclose = p.pc.onconnectionstatechange = null;
      try { p.pc.close(); } catch (_) {}
      if (!T.roam) changed();
    }
    // Room: the lower vip makes the offer; on a failure it tries again (5, 10, 15 s), then the pair stays on the relay.
    function retry(v) {
      if (link.vip >= v || vips.indexOf(v) < 0 || timers[v] || (tries[v] | 0) >= RETRIES) return;
      tries[v] = (tries[v] | 0) + 1;
      timers[v] = setTimeout(function () { delete timers[v]; if (vips.indexOf(v) >= 0 && !peers[v]) offer(v, room); }, 5000 * tries[v]);
    }
    // Free roam: one line per pair outcome; the offerer waits longer after each failure.
    function backoff(v) {
      var b = bad[v] = bad[v] || { n: 0, until: 0 };
      b.n++;
      var wait = Math.min(ROAM.backoffMaxMs, ROAM.backoffMs * Math.pow(2, Math.min(b.n - 1, 4)));
      b.until = Date.now() + wait;
      return Math.round(wait / 1000);
    }
    function roamFail(v, why) {
      var p = fr.peers[v];
      if (!p) return;
      drop(v, fr);
      if (!p.offerer) { say('[rtc] free roam ' + vs(v) + ': no direct path (' + why + '), via the server'); return; }
      say('[rtc] free roam ' + vs(v) + ': no direct path (' + why + '), via the server; next try in ' + backoff(v) + ' s');
    }
    function rate(v) { return act[v] ? act[v].last | 0 : 0; }
    function relayed() { return Object.keys(fr.peers).map(Number).filter(function (v) { return fr.peers[v].relay && openP(fr.peers[v]); }); }
    // At most ROAM.relayMax relayed channels: the busiest stay, the rest close (both ends back off); false if v closed.
    function capRelay(v) {
      var r = relayed().sort(function (a, b) { return rate(b) - rate(a) || a - b; });
      r.slice(ROAM.relayMax).forEach(function (w) {
        drop(w, fr);
        say('[rtc] free roam ' + vs(w) + ': via TURN beyond the ' + ROAM.relayMax + ' allowed, closed; via the server, next try in ' + backoff(w) + ' s');
      });
      var i = r.indexOf(v);
      return i >= 0 && i < ROAM.relayMax;
    }
    // Direct or TURN, logged once and again only on a change (ICE reconnected on another pair).
    function roamVia(v, p) {
      via(p.pc).then(function (relay) {
        if (fr.peers[v] !== p || (p.checked && p.relay == relay)) return;
        p.checked = true; p.relay = relay;
        if (relay && !capRelay(v)) return;
        delete bad[v];
        say('[rtc] free roam ' + vs(v) + ': ' + (relay ? 'via TURN (' + relayed().length + ' of ' + ROAM.relayMax + ')' : 'direct (P2P)'));
      });
    }
    function lost(v, T, why) { if (T.roam) roamFail(v, why); else { drop(v, T); retry(v); } }
    function make(v, T) {
      drop(v, T);
      var pc = new RTCPeerConnection(cfg);
      // negotiated: both ends make the same channel, no ondatachannel round
      var dc = pc.createDataChannel('mc3', { ordered: false, maxRetransmits: 0, negotiated: true, id: 0 });
      var p = T.peers[v] = { pc: pc, dc: dc, ice: [], remote: false, offerer: false, deadline: 0 };
      dc.binaryType = 'arraybuffer';
      dc.onopen = function () {
        clearTimeout(p.deadline);
        p.open = true;
        if (T.roam) { touch(v); roamVia(v, p); }
        else { tries[v] = 0; changed(); via(pc).then(function (w) { say('[rtc] race room ' + vs(v) + ': ' + (w ? 'P2P through a TURN relay' : 'direct (P2P)')); }); }
      };
      dc.onclose = function () {
        if (T.peers[v] !== p) return;
        if (T.roam && p.open) { drop(v, fr); say('[rtc] free roam ' + vs(v) + ': channel closed by the other side, via the server'); }
        else lost(v, T, 'closed');
      };
      dc.onmessage = function (e) { if (T.roam) rxRoam(v, e.data); else rx(v, e.data); };
      pc.onicecandidate = function (e) { signal(v, 'ice', e.candidate ? e.candidate.toJSON() : null, T.roam); };
      pc.onconnectionstatechange = function () {
        if (T.peers[v] !== p) return;
        if (pc.connectionState == 'failed') lost(v, T, 'ICE failed');
        else if (pc.connectionState == 'connected' && T.roam && p.checked) roamVia(v, p);   // back after a drop: maybe another pair
      };
      if (T.roam) p.deadline = setTimeout(function () { if (fr.peers[v] === p && dc.readyState != 'open') roamFail(v, 'not open in ' + ROAM.openMs / 1000 + ' s'); }, ROAM.openMs);
      return p;
    }
    function remote(p, kind, sdp) {
      return p.pc.setRemoteDescription({ type: kind, sdp: sdp }).then(function () {
        p.remote = true;
        p.ice.splice(0).forEach(function (c) { p.pc.addIceCandidate(c).catch(function () {}); });
      });
    }
    function fail(v, T) { return function (e) { console.warn('[rtc] ' + vs(v) + ': ' + e); if (T.peers[v]) lost(v, T, String(e && e.name || e)); }; }
    function offer(v, T) {
      var p = make(v, T);
      p.offerer = true;
      p.pc.createOffer().then(function (o) { return p.pc.setLocalDescription(o); })
        .then(function () { signal(v, 'offer', { sdp: p.pc.localDescription.sdp }, T.roam); }).catch(fail(v, T));
    }
    function onSignal(m) {
      var v = m.from >>> 0, d = m.data, T = m.roam ? fr : room, p = T.peers[v];
      if (m.kind == 'offer' && v < link.vip && d && typeof d.sdp == 'string') {
        // free roam: only while on, and within roamMax (a full page leaves the offerer on the switch)
        // or one closed here as a TURN channel beyond the cap, until its backoff ends
        if (T.roam && (!rtc.roamP2P || refused || (bad[v] && Date.now() < bad[v].until) || (!p && Object.keys(fr.peers).length >= rtc.roamMax))) return;
        p = make(v, T);
        if (T.roam) touch(v);
        remote(p, 'offer', d.sdp).then(function () { return p.pc.createAnswer(); })
          .then(function (a) { return p.pc.setLocalDescription(a); })
          .then(function () { signal(v, 'answer', { sdp: p.pc.localDescription.sdp }, T.roam); }).catch(fail(v, T));
      } else if (m.kind == 'answer' && p && p.pc.signalingState == 'have-local-offer' && d && typeof d.sdp == 'string') {
        remote(p, 'answer', d.sdp).catch(fail(v, T));
      } else if (m.kind == 'ice' && p && d) {
        if (p.remote) p.pc.addIceCandidate(d).catch(function () {}); else p.ice.push(d);
      }
    }
    function setMembers(list) {
      rtc.members = list;
      vips = list.map(function (x) { return x.vip >>> 0; }).filter(function (v) { return v != link.vip; });
      Object.keys(peers).forEach(function (k) { if (vips.indexOf(+k) < 0) drop(+k, room); });
      Object.keys(timers).forEach(function (k) { if (vips.indexOf(+k) < 0) { clearTimeout(timers[k]); delete timers[k]; } });
      tries = {};
      vips.forEach(function (v) { if (link.vip < v && !peers[v]) offer(v, room); });
      changed();
    }
    // A frame from a member's channel: the same frame as the switch's, its source taken from the channel, not the frame.
    function rx(v, data) {
      if (!(data instanceof ArrayBuffer) || data.byteLength < HDR || data.byteLength - HDR > MAX) return;
      var f = new Uint8Array(data), fv = new DataView(data);
      if (f[0] != KIND || fv.getUint16(14) == ROAM_PORT || fv.getUint16(12) == ROAM_PORT) return;
      fv.setUint32(4, v);
      rtc.recv++;
      if (link.onframe) link.onframe(f);
    }
    // A free-roam peer's frame: only to the free-roam port, written as the switch writes it (source = the channel's
    // peer, never the frame's own; destination = us), into the same receive path as the switch's frames.
    function rxRoam(v, data) {
      if (!(data instanceof ArrayBuffer) || data.byteLength < HDR || data.byteLength - HDR > MAX) return;
      var f = new Uint8Array(data), fv = new DataView(data);
      if (f[0] != KIND || fv.getUint16(14) != ROAM_PORT) return;
      f[1] = f[2] = f[3] = 0;
      fv.setUint32(4, v);
      fv.setUint32(8, link.vip);
      rtc.roamRecv++;
      touch(v);
      if (link.onframe) link.onframe(f);
    }
    function touch(v) { var a = act[v] || (act[v] = { tx: 0, last: 0, skip: 0, at: 0 }); a.at = Date.now(); return a; }

    rtc.send = function (f) {
      var fv = new DataView(f.buffer, f.byteOffset, f.length), dst = fv.getUint32(8), ok = false;
      if (fv.getUint16(14) == ROAM_PORT || fv.getUint16(12) == ROAM_PORT) return rtc.roamSend(f);
      plan(dst, link.vip, vips, isOpen).forEach(function (r) {
        if (r[1] == 'dc') {
          try { peers[r[0]].dc.send(f); rtc.sent++; ok = true; return; } catch (_) {}   // queue full or just closed: relay
        }
        var g = f;
        if (r[0] != dst) { g = f.slice(); new DataView(g.buffer).setUint32(8, r[0]); }
        ok = link.send(g) || ok;
      });
      return ok;
    };
    rtc.roamSend = function (f) {
      var dst = new DataView(f.buffer, f.byteOffset, f.length).getUint32(8);
      var out = link.roamSend || link.send;   // the city's own socket when open (web/lan.js), else the switch
      if (dst == HUB || dst == BCAST) return out(f);
      touch(dst).tx++;
      var p = rtc.roamP2P && fr.peers[dst];
      if (openP(p) && p.dc.bufferedAmount < ROAM.queue) {
        try { p.dc.send(f); rtc.roamSent++; return true; } catch (_) {}   // just closed: the switch
      }
      if (thin(dst, f)) { rtc.roamThinned++; return true; }
      rtc.roamRelay++;
      return out(f);
    };
    // Through the server: a SNAP (roam datagram 'MC3R', type 3 at +5) to a peer outside the nearMax() busiest goes
    // 1 in ROAM.thin, unless the core thinned it already (flags u16 little-endian at +14, every-n nibble 4 or 10).
    // Until nearMax() peers are ranked, every peer is near.
    function thin(dst, f) {
      if (f.length < HDR + 6 || f[HDR] != 0x4D || f[HDR + 1] != 0x43 || f[HDR + 2] != 0x33 || f[HDR + 3] != 0x52 || f[HDR + 5] != 3) return false;
      if ((f[HDR + 14] & 0x0F) > 1) return false;
      if (nearN < nearMax() || near[dst]) return false;
      return act[dst].skip++ % ROAM.thin != 0;
    }
    // Once a second: idle channels go, the busiest new destinations get an offer, a stats line each minute on change.
    function tick() {
      var now = Date.now(), on = rtc.roamP2P && !refused && link.state == 'online' && link.vip > 0;
      iceFetch();
      Object.keys(fr.peers).forEach(function (k) {
        var a = act[k], p = fr.peers[k];
        if (on && a && now - a.at <= ROAM.idleMs) return;
        drop(+k, fr);
        if (p.open) say('[rtc] free roam ' + vs(+k) + ': channel closed (' + (on ? 'idle' : 'P2P off or offline') + ')');
      });
      Object.keys(act).forEach(function (k) { if (now - act[k].at > 3 * ROAM.idleMs && !fr.peers[k]) delete act[k]; });
      Object.keys(bad).forEach(function (k) { if (now - bad[k].until > ROAM.backoffMaxMs) delete bad[k]; });
      if (on) {
        var seen = {};
        Object.keys(act).forEach(function (k) { seen[k] = act[k].tx; });
        pick(seen, Object.keys(fr.peers).map(Number), rtc.roamMax, ROAM.perTick, function (v) {
          return v <= link.vip || (bad[v] && now < bad[v].until);   // the lower vip offers; a failed pair waits
        }).forEach(function (v) { offer(v, fr); });
      }
      // the last second's frames rank the peers: the nearMax() busiest are the near ones (not thinned)
      Object.keys(act).forEach(function (k) { act[k].last = act[k].tx; act[k].tx = 0; });
      var live = Object.keys(act).filter(function (k) { return act[k].last > 0; });
      var ranked = live.slice().sort(function (a, b) { return act[b].last - act[a].last || a - b; }).slice(0, nearMax());
      near = {}; nearN = ranked.length;
      ranked.forEach(function (k) { near[k] = 1; });
      if (now - loadAt >= 30000) {
        var s = (now - loadAt) / 1000, d = [rtc.roamSent - load[0], rtc.roamRelay - load[1], rtc.roamThinned - load[2]];
        loadAt = now; load = [rtc.roamSent, rtc.roamRelay, rtc.roamThinned];
        var ll = '[rtc] free roam load: ' + live.length + ' peers, P2P ' + Math.round(d[0] / s) + ', relayed ' + Math.round(d[1] / s) +
          ', thinned ' + Math.round(d[2] / s) + '/s';
        if ((d[0] || d[1]) && ll != loadLast) { loadLast = ll; say(ll); }   // only while frames flow (in Free Roam)
      }
      if (now - statsAt >= ROAM.statsMs) {
        statsAt = now;
        var open = Object.keys(fr.peers).filter(function (k) { return openP(fr.peers[k]); });
        var m = relayed().length, srv = Object.keys(act).filter(function (k) { return act[k].last > 0 && open.indexOf(k) < 0; });
        var t = nearN < nearMax() ? 0 : srv.filter(function (k) { return !near[k]; }).length;
        var line = '[rtc] free roam: ' + (open.length - m) + ' direct, ' + m + ' via TURN, ' + srv.length + ' via the server (thinned ' + t +
          '); frames P2P sent ' + rtc.roamSent + ', received ' + rtc.roamRecv + ', via the server ' + rtc.roamRelay + ', thinned out ' + rtc.roamThinned;
        if (line != statsLast && (rtc.roamSent || rtc.roamRelay)) { statsLast = line; say(line); }
      }
    }
    var ticker = setInterval(tick, ROAM.tickMs);
    rtc.tick = tick;   // tests
    iceFetch();
    rtc.mode = function (v) { return isOpen(v >>> 0) ? 'direct' : 'relay'; };
    rtc.roamMode = function (v) { return openP(fr.peers[v >>> 0]) ? 'direct' : 'relay'; };
    rtc.on = function (f) { fns.push(f); f(rtc); };
    rtc.close = function () {
      clearInterval(ticker);
      Object.keys(fr.peers).forEach(function (k) { drop(+k, fr); });
      setMembers([]); fns = [];
    };

    link.onMsg(function (m) {
      if (m.t == 'room') setMembers(m.members || []);
      else if (m.t == 'left') setMembers([]);
      else if (m.t == 'signal') onSignal(m);
      // a switch without free-roam signalling (web/server) refuses ours as 'room' while we are in none: switch only
      else if (m.t == 'error' && m.op == 'signal' && m.code == 'room' && !rtc.members.length && !refused && Object.keys(fr.peers).length) {
        refused = true;
        say('[rtc] free roam: this server relays no free-roam signalling; every frame goes through it');
        Object.keys(fr.peers).forEach(function (k) { drop(+k, fr); });
      }
    });
    link.on(function (l) {
      if (l.state != 'offline') return;
      if (rtc.members.length) setMembers([]);
      Object.keys(fr.peers).forEach(function (k) { drop(+k, fr); });
    });
    link.rtc = rtc;
    return rtc;
  }

  mc3Rtc.plan = plan;
  mc3Rtc.pick = pick;
  mc3Rtc.ROAM = ROAM;
  root.mc3Rtc = mc3Rtc;
  if (typeof module == 'object' && module.exports) module.exports = mc3Rtc;
})(typeof window == 'object' ? window : globalThis);
