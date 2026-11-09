// WebRTC inside a race room (docs/races-protocol.md, "Step 2"): one unordered, unreliable data channel to each other
// member of the page's room, signalled through the /lan link (web/lan.js), the switch as fallback relay.  Loaded by
// the start page after lan.js; web/net.js sends System Link's frames through link.rtc when it is there.
//
//   const rtc = mc3Rtc(link[, iceServers]); // sets link.rtc; iceServers: config.json's, else public STUN only
//   rtc.send(frame)                         // a System Link frame (16-byte header, net.js): data channel or switch
//   rtc.members                             // the room's members [{vip, vipStr, name, host}], [] in no room
//   rtc.mode(vip)                           // 'direct' (data channel open) | 'relay' (switch)
//   rtc.on(r => ...)                        // after every member or channel change; rtc.close()
//
// Routing (plan() below, documented in races-protocol.md): a unicast to a member with an open channel goes over it,
// anything else to the switch as before.  A broadcast in a room with at least one open channel is split by the page:
// over each open channel, and as a unicast through the switch (destination rewritten to that member) to each member
// without one; the switch never sees it as a broadcast, so it cannot deliver it twice.  The game's receive path does
// not see the destination (xnet_web.c's RX slot holds the source only).  Free roam (port 47210, the hub) never comes
// here: net.js sends it on the switch.
(function (root) {
  var BCAST = 0xFFFFFFFF, KIND = 3, HDR = 16, MAX = 1472, ROAM_PORT = 47210, RETRIES = 3;
  var STUN = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun.cloudflare.com:3478' }];

  // Which path a frame to dst takes: [[vip, 'dc' | 'ws'], ...]; [[BCAST, 'ws']] is the switch's own broadcast.
  function plan(dst, self, members, isOpen) {
    if (dst != BCAST) return [[dst, isOpen(dst) ? 'dc' : 'ws']];
    var others = members.filter(function (v) { return v != self; });
    if (!others.some(isOpen)) return [[BCAST, 'ws']];
    return others.map(function (v) { return [v, isOpen(v) ? 'dc' : 'ws']; });
  }

  function mc3Rtc(link, iceServers) {
    var cfg = { iceServers: iceServers && iceServers.length ? iceServers : STUN };
    var rtc = { members: [], sent: 0, recv: 0 }, peers = {}, tries = {}, timers = {}, fns = [], vips = [];
    function changed() { fns.forEach(function (f) { try { f(rtc); } catch (e) { console.error(e); } }); }
    function isOpen(v) { var p = peers[v]; return !!p && p.dc.readyState == 'open'; }
    function signal(to, kind, data) { link.sendMsg({ t: 'signal', to: to, kind: kind, data: data }); }

    function drop(v) {
      var p = peers[v];
      if (!p) return;
      delete peers[v];
      p.dc.onclose = p.pc.onconnectionstatechange = null;
      try { p.pc.close(); } catch (_) {}
      changed();
    }
    // The lower vip makes the offer; on a failure it tries again (5, 10, 15 s), then the pair stays on the relay.
    function retry(v) {
      if (link.vip >= v || vips.indexOf(v) < 0 || timers[v] || (tries[v] | 0) >= RETRIES) return;
      tries[v] = (tries[v] | 0) + 1;
      timers[v] = setTimeout(function () { delete timers[v]; if (vips.indexOf(v) >= 0 && !peers[v]) offer(v); }, 5000 * tries[v]);
    }
    function make(v) {
      drop(v);
      var pc = new RTCPeerConnection(cfg);
      // negotiated: both ends make the same channel, no ondatachannel round
      var dc = pc.createDataChannel('mc3', { ordered: false, maxRetransmits: 0, negotiated: true, id: 0 });
      var p = peers[v] = { pc: pc, dc: dc, ice: [], remote: false };
      dc.binaryType = 'arraybuffer';
      dc.onopen = function () { tries[v] = 0; changed(); };
      dc.onclose = function () { if (peers[v] === p) { drop(v); retry(v); } };
      dc.onmessage = function (e) { rx(v, e.data); };
      pc.onicecandidate = function (e) { signal(v, 'ice', e.candidate ? e.candidate.toJSON() : null); };
      pc.onconnectionstatechange = function () { if (pc.connectionState == 'failed' && peers[v] === p) { drop(v); retry(v); } };
      return p;
    }
    function remote(p, kind, sdp) {
      return p.pc.setRemoteDescription({ type: kind, sdp: sdp }).then(function () {
        p.remote = true;
        p.ice.splice(0).forEach(function (c) { p.pc.addIceCandidate(c).catch(function () {}); });
      });
    }
    function fail(v) { return function (e) { console.warn('[rtc] ' + v + ': ' + e); drop(v); retry(v); }; }
    function offer(v) {
      var p = make(v);
      p.pc.createOffer().then(function (o) { return p.pc.setLocalDescription(o); })
        .then(function () { signal(v, 'offer', { sdp: p.pc.localDescription.sdp }); }).catch(fail(v));
    }
    function onSignal(m) {
      var v = m.from >>> 0, d = m.data, p = peers[v];
      if (m.kind == 'offer' && v < link.vip && d && typeof d.sdp == 'string') {
        p = make(v);
        remote(p, 'offer', d.sdp).then(function () { return p.pc.createAnswer(); })
          .then(function (a) { return p.pc.setLocalDescription(a); })
          .then(function () { signal(v, 'answer', { sdp: p.pc.localDescription.sdp }); }).catch(fail(v));
      } else if (m.kind == 'answer' && p && p.pc.signalingState == 'have-local-offer' && d && typeof d.sdp == 'string') {
        remote(p, 'answer', d.sdp).catch(fail(v));
      } else if (m.kind == 'ice' && p && d) {
        if (p.remote) p.pc.addIceCandidate(d).catch(function () {}); else p.ice.push(d);
      }
    }
    function setMembers(list) {
      rtc.members = list;
      vips = list.map(function (x) { return x.vip >>> 0; }).filter(function (v) { return v != link.vip; });
      Object.keys(peers).forEach(function (k) { if (vips.indexOf(+k) < 0) drop(+k); });
      Object.keys(timers).forEach(function (k) { if (vips.indexOf(+k) < 0) { clearTimeout(timers[k]); delete timers[k]; } });
      tries = {};
      vips.forEach(function (v) { if (link.vip < v && !peers[v]) offer(v); });
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

    rtc.send = function (f) {
      var fv = new DataView(f.buffer, f.byteOffset, f.length), dst = fv.getUint32(8), ok = false;
      if (fv.getUint16(14) == ROAM_PORT || fv.getUint16(12) == ROAM_PORT) return link.send(f);
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
    rtc.mode = function (v) { return isOpen(v >>> 0) ? 'direct' : 'relay'; };
    rtc.on = function (f) { fns.push(f); f(rtc); };
    rtc.close = function () { setMembers([]); fns = []; };

    link.onMsg(function (m) {
      if (m.t == 'room') setMembers(m.members || []);
      else if (m.t == 'left') setMembers([]);
      else if (m.t == 'signal') onSignal(m);
    });
    link.on(function (l) { if (l.state == 'offline' && rtc.members.length) setMembers([]); });
    link.rtc = rtc;
    return rtc;
  }

  mc3Rtc.plan = plan;
  root.mc3Rtc = mc3Rtc;
  if (typeof module == 'object' && module.exports) module.exports = mc3Rtc;
})(typeof window == 'object' ? window : globalThis);
