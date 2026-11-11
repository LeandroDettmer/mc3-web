// Linked in front of the Emscripten runtime (--pre-js, CMakeLists.txt): datagrams between the game's mailboxes in
// shared wasm memory and the page's link to the server's switch (Module.mc3Lan, web/lan.js).  Two mailboxes, one
// layout (described in xboxrecomp/src/net/xnet_web.c): System Link's (xnet_web.c, published at the game's
// XNetStartup, MC3_NETPLAY=1) and free roam's (xboxrecomp/src/net/roam/roam_web.c, published at roam_start; after
// nfsu2-web's, MIT, th0mk), which takes the frames for UDP port 47210.  Main thread only.  Without a link a mailbox
// is told "offline" at once.
//
// C -> switch: the game rings tx_bell; we wait on it with Atomics.waitAsync and send each TX slot as a kind 3 frame
// [3][0 0 0][src vip][dst vip][src port][dst port][data], big-endian.  Switch -> C: each kind 3 frame into an RX
// slot of the mailbox its port belongs to (ring full: dropped, as a LAN drops), then rx_bell is bumped and
// notified.  Module.mc3Net() returns System Link's counters, Module.mc3RoamNet() free roam's.
(function () {
  if (typeof document != 'object') return;
  var KIND = 3, SLOT_HDR = 16, MAX = 1472, ROAM_PORT = 47210;
  var W_MAGIC = 0, W_TX_SLOTS = 2, W_RX_SLOTS = 3, W_SLOT_BYTES = 4, W_TX_OFF = 5, W_RX_OFF = 6,
      W_TX_HEAD = 8, W_TX_TAIL = 9, W_RX_HEAD = 10, W_RX_TAIL = 11, W_TX_BELL = 12, W_STATE = 13, W_VIP = 14,
      W_PEERS = 15, W_TX_DROP = 16, W_RX_DROP = 17, W_C_TX_FULL = 18, W_C_RX_LOST = 19, W_RX_BELL = 20;
  var STATES = { connecting: 1, online: 2, offline: 3 };
  var link = null, boxes = [], said = '', saidVip = '', saidCity = null, saidDown = 0;

  // The link reconnects by itself (web/lan.js): the same object, so the mailboxes stay attached; said once for both.
  function say(l) {
    if ((l.city == null ? null : l.city) !== saidCity) {
      err('[net] free roam: ' + (l.city != null ? 'own socket to city ' + l.city + ' open' : 'through the switch') + (saidCity != null ? ' (was city ' + saidCity + ')' : ''));
      saidCity = l.city == null ? null : l.city;
    }
    var rd = l.roamDown || 0;
    if (rd != saidDown) {
      err(rd ? '[net] free roam: link lost' : '[net] free roam: link back after ' + Math.round((Date.now() - saidDown) / 1000) + ' s');
      saidDown = rd;
    }
    var s = l.state == 'online' ? 'up' : l.state == 'offline' ? 'down' : '';
    if (!s || s == said) return;
    if (said) err('[net] switch link ' + s + ' (vip ' + (l.vipStr || 'none') + (s == 'up' && saidVip && saidVip != l.vipStr ? ', was ' + saidVip : '') + ')');
    said = s;
    if (s == 'up') saidVip = l.vipStr;
  }

  // a frame from the switch to the mailbox of its destination port
  function route(f) {
    if (f.length < SLOT_HDR || f[0] != KIND || f.length - SLOT_HDR > MAX) return;
    var roam = ((f[14] << 8) | f[15]) == ROAM_PORT;
    for (var i = 0; i < boxes.length; i++) if (boxes[i].roam == roam) { boxes[i].rx(f); return; }
  }

  function attach(name, fn, magic, roam, counters) {
    var h = null, u8, dv, base, txSlots, rxSlots, slot, txOff, rxOff;
    function status() {
      Atomics.store(h, W_VIP, link ? link.vip | 0 : 0);
      Atomics.store(h, W_PEERS, link ? link.peers.length : 0);
      Atomics.store(h, W_STATE, link ? STATES[link.state] || 3 : 3);   // the address first: C reads it on "online"
    }
    function drainTx() {
      var tail = Atomics.load(h, W_TX_TAIL), head = Atomics.load(h, W_TX_HEAD);
      while (tail != head) {
        var o = base + txOff + ((tail >>> 0) % txSlots) * slot, len = dv.getUint32(o, true);
        if (len <= MAX) {
          var f = new Uint8Array(SLOT_HDR + len), fv = new DataView(f.buffer), ports = dv.getUint32(o + 8, true);
          f[0] = KIND;
          fv.setUint32(4, link ? link.vip : 0);
          fv.setUint32(8, dv.getUint32(o + 4, true));
          fv.setUint16(12, ports & 0xFFFF);
          fv.setUint16(14, ports >>> 16);
          f.set(u8.subarray(o + SLOT_HDR, o + SLOT_HDR + len), SLOT_HDR);
          // peer to peer where it can (web/rtc.js): System Link inside a race room, free roam to the nearest players;
          // free roam's other frames over the city's own socket when there is one (web/lan.js roamSend)
          var rtc = link && link.rtc;
          if (!link || !(rtc ? (roam ? rtc.roamSend(f) : rtc.send(f)) : roam && link.roamSend ? link.roamSend(f) : link.send(f))) Atomics.add(h, W_TX_DROP, 1);
        }
        tail = (tail + 1) | 0;
        Atomics.store(h, W_TX_TAIL, tail);
      }
    }
    function txLoop() {
      var bell = Atomics.load(h, W_TX_BELL);
      drainTx();
      if (typeof Atomics.waitAsync == 'function') {
        var r = Atomics.waitAsync(h, W_TX_BELL, bell, 50);   // the timeout only in case a notify is missed
        if (r.async) r.value.then(txLoop); else setTimeout(txLoop, 0);
      } else setTimeout(txLoop, 4);
    }
    function rx(f) {
      var fv = new DataView(f.buffer, f.byteOffset, f.length);
      var head = Atomics.load(h, W_RX_HEAD), tail = Atomics.load(h, W_RX_TAIL);
      if (((head - tail) >>> 0) >= rxSlots || f.length - SLOT_HDR > slot - SLOT_HDR) { Atomics.add(h, W_RX_DROP, 1); return; }
      var o = base + rxOff + ((head >>> 0) % rxSlots) * slot;
      dv.setUint32(o, f.length - SLOT_HDR, true);
      dv.setUint32(o + 4, fv.getUint32(4), true);
      dv.setUint32(o + 8, (fv.getUint16(12) | fv.getUint16(14) << 16) >>> 0, true);
      dv.setUint32(o + 12, 0, true);
      u8.set(f.subarray(SLOT_HDR), o + SLOT_HDR);
      Atomics.store(h, W_RX_HEAD, (head + 1) | 0);
      Atomics.add(h, W_RX_BELL, 1);
      Atomics.notify(h, W_RX_BELL);
    }
    // The mailbox is a static in the wasm image; its magic is set when the game starts using it.
    var poll = setInterval(function () {
      if (typeof fn() != 'function' || typeof HEAPU8 == 'undefined' || !HEAPU8) return;
      var ptr = fn()() >>> 0, buf = HEAPU8.buffer, v = new Int32Array(buf, ptr, 32);
      if ((v[W_MAGIC] >>> 0) != magic) return;
      clearInterval(poll);
      h = v; base = ptr; u8 = new Uint8Array(buf); dv = new DataView(buf);
      txSlots = v[W_TX_SLOTS]; rxSlots = v[W_RX_SLOTS]; slot = v[W_SLOT_BYTES]; txOff = v[W_TX_OFF]; rxOff = v[W_RX_OFF];
      link = Module['mc3Lan'] || null;
      boxes.push({ roam: roam, rx: rx });
      if (link) { link.onframe = route; link.on(status); if (boxes.length == 1) link.on(say); } else status();
      err('[net] ' + name + ' mailbox attached: ' + (link ? 'switch ' + link.state + (link.vipStr ? ' as ' + link.vipStr : '') : 'no link (Online off on the page)'));
      txLoop();
    }, 50);
    Module[counters] = function () {
      var z = function (i) { return h ? Atomics.load(h, i) >>> 0 : 0; };
      return { attached: !!h, state: link ? link.state : 'none', vip: link ? link.vipStr : '', peers: link ? link.peers.length : 0,
               sent: link ? link.sent : 0, recv: link ? link.recv : 0,
               p2pSent: link && link.rtc ? (roam ? link.rtc.roamSent : link.rtc.sent) : 0,
               p2pRecv: link && link.rtc ? (roam ? link.rtc.roamRecv : link.rtc.recv) : 0, txDrop: z(W_TX_DROP), rxDrop: z(W_RX_DROP),
               cTxFull: z(W_C_TX_FULL), cRxLost: z(W_C_RX_LOST) };
    };
  }

  attach('System Link', function () { return typeof _xnet_web_mailbox == 'function' ? _xnet_web_mailbox : null; }, 0x42574E58, false, 'mc3Net');
  attach('free roam', function () { return typeof _roam_web_mailbox == 'function' ? _roam_web_mailbox : null; }, 0x52574D52, true, 'mc3RoamNet');
})();
