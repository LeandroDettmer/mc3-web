// In-game chat, whispers and friends (docs/chat-plan.md section 4; after nfsu2-web's messages.ts MessageStore and its
// in-game card, MIT, th0mk). Loaded by the start page like cloud.js: window.MC3Chat; a missing file = no chat.
//
//   MC3Chat.attach({ lan, stage, canvas, playing(), keys(), keyUsed(code), log(line), settings() })
//       lan: web/lan.js's link (attach again with each new link); keys(): the codes that open the panel (default U);
//       keyUsed(code): a game input has that key too (then it does not open chat); settings(): {on, city}
//   MC3Chat.open() / .close() / .sync()   the panel (Settings' Chat button, the pad route) / settings changed
//   MC3Chat.ChatStore                     the store alone (tools/test_chat.mjs)
//
// Wire (docs/chat-plan.md section 2): JSON on the Switch socket only (link.chat), never the city socket or game frames:
//   out {"t":"chat","op":"say","ch":"c"|"w","to"?,"name"?,"text","id"} | op "ask"/"accept" {to,name,id} | "seen" {f,id}
//   in  op "say" {ch "c"|"r"|"w", from, name, text, at} | "done" {id, done, n} | "ask" {from, name, at}
//       | "friend" {vip, name, fid} | "seen" {id, on [{f, vip, name, w, city}]}
// A Switch without chat answers nothing: no done within 5 s, first time on this link -> "Chat is not available on this
// server" and the input greyed for the link's life.
// Nothing is kept on the server: the city/room lines live in memory (CITY_KEEP, this page), whisper threads in
// localStorage mc3.chat.w, friends in mc3.friends, blocks in mc3.chat.block (every access in try/catch: without
// storage it all works for the session). Text is put on the page with textContent only. The page log gets counts and
// outcomes, never anyone's text.
// Look: the closed state is a few faded lines at the picture's top left (inside its middle 16:9, above the drift score at
// 27-36% height, clear of the minimap's prompt box, the players chip on the right and the link notice at the top
// centre), right of any touch button there, plus a small chip (speech balloon + unread count) that opens the panel. The
// panel is a non-modal <dialog id="chat-dlg"> in the stage at the same spot: tabs Chat / Whispers / Friends, the input
// (U opens it focused; Enter sends, Esc closes), a whisper pill, /w name text, /r text, and quick phrases for pads and
// phones. While it has the focus its keys never reach the game (keydown/keyup/keypress stopped, every held key let go).
(function () {
  'use strict';
  var root = typeof window == 'object' ? window : globalThis;
  var CHAT_LEN = 140, CITY_KEEP = 200, THREAD_KEEP = 40, THREADS_KEEP = 30, FRIENDS_KEEP = 60, BLOCK_KEEP = 200, ASKS_KEEP = 20;
  var DONE_MS = 5000, LINE_MS = 12000, WHISPER_MS = 20000, FADE_MS = 1200, LOG_LINES = 5, SEEN_MS = 30000;
  var PHRASES = ['gg', 'race?', 'follow me', 'nice drift', 'wait', 'thanks', 'sorry', 'bye'];
  var CITIES = ['San Diego', 'Atlanta', 'Detroit', 'Tokyo'];   // free-roam room ids (index.html CITIES)
  var FID = /^[0-9a-f]{16}$/;
  var WHY = { away: 'is not online.', alone: 'Chat works in Free Roam, Online career and race rooms.', rate: 'Wait a moment.',
    nofid: 'Friends need this browser to keep its data (storage is blocked).', noask: 'That friend request is gone: ask again.',
    hello: 'Not connected yet.', offline: 'Not connected.', timeout: 'Not sent.', off: 'Chat is not available on this server.' };

  function low(s) { return String(s == null ? '' : s).toLowerCase(); }
  function lsGet(k, d) { try { var v = JSON.parse(localStorage.getItem(k)); return v == null ? d : v; } catch (e) { return d; } }
  function lsSet(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) {} }
  function clip(s, n) { return String(s == null ? '' : s).replace(/\s+/g, ' ').trim().slice(0, n); }

  // ------------------------------------------------------------------------------------------------------- store --
  function ChatStore() {
    this.ring = []; this.asks = []; this.cityUnread = 0; this.seq = 0;
    var w = lsGet('mc3.chat.w', {}), f = lsGet('mc3.friends', []), b = lsGet('mc3.chat.block', []);
    this.threads = w && typeof w == 'object' && !Array.isArray(w) ? w : {};
    this.friends = Array.isArray(f) ? f.filter(function (x) { return x && FID.test(x.fid); }) : [];
    this.blocks = Array.isArray(b) ? b.filter(function (x) { return x && x.name; }) : [];
  }
  var S = ChatStore.prototype;
  S.save = function () { lsSet('mc3.chat.w', this.threads); lsSet('mc3.friends', this.friends); lsSet('mc3.chat.block', this.blocks); };
  // a line of the session's log (city, room, whisper, notice): memory only, CITY_KEEP at most
  S.line = function (l) {
    l.id = ++this.seq; l.at = l.at || Date.now();
    this.ring.push(l);
    if (this.ring.length > CITY_KEEP) this.ring.splice(0, this.ring.length - CITY_KEEP);
    return l;
  };
  S.friendBy = function (name, fid) {
    for (var i = 0; i < this.friends.length; i++) { var f = this.friends[i]; if (fid ? f.fid === fid : low(f.name) === low(name)) return f; }
    return null;
  };
  // a whisper thread's key: the friend's fid (survives renames), else the lower-case nick
  S.keyOf = function (name) { var f = this.friendBy(name); return f ? f.fid : 'name:' + low(name); };
  S.whisper = function (name, dir, text, at, st) {
    var k = this.keyOf(name), t = this.threads[k] || (this.threads[k] = { name: name, msgs: [], unread: 0 });
    var m = { dir: dir, text: clip(text, CHAT_LEN), at: at || Date.now() };
    if (st) m.st = st;
    t.name = name; t.msgs.push(m);
    if (t.msgs.length > THREAD_KEEP) t.msgs.splice(0, t.msgs.length - THREAD_KEEP);
    if (dir === 'in') t.unread = (t.unread | 0) + 1;
    this.trim(); this.save();
    return m;
  };
  S.last = function (t) { return t && t.msgs && t.msgs.length ? t.msgs[t.msgs.length - 1].at : 0; };
  S.trim = function () {   // the threads talked in last stay
    var self = this, ks = Object.keys(this.threads);
    if (ks.length <= THREADS_KEEP) return;
    ks.sort(function (a, b) { return self.last(self.threads[a]) - self.last(self.threads[b]); });
    ks.slice(0, ks.length - THREADS_KEEP).forEach(function (k) { delete self.threads[k]; });
  };
  S.list = function () {   // unread first, then the newest
    var self = this;
    return Object.keys(this.threads).map(function (k) { var t = self.threads[k]; return { key: k, name: t.name, unread: t.unread | 0, at: self.last(t), t: t }; })
      .sort(function (a, b) { return (b.unread > 0) - (a.unread > 0) || b.at - a.at; });
  };
  S.read = function (k) { var t = this.threads[k]; if (t && t.unread) { t.unread = 0; this.save(); } };
  S.unread = function () {
    var n = this.cityUnread + this.asks.length, k;
    for (k in this.threads) n += this.threads[k].unread | 0;
    return n;
  };
  S.blocked = function (name, fid) {
    return this.blocks.some(function (b) { return (fid && b.fid === fid) || low(b.name) === low(name); });
  };
  S.block = function (name) {
    if (!name || this.blocked(name)) return;
    var f = this.friendBy(name), b = { name: String(name) };
    if (f) b.fid = f.fid;
    this.blocks.push(b);
    if (this.blocks.length > BLOCK_KEEP) this.blocks.splice(0, this.blocks.length - BLOCK_KEEP);
    this.dropAsk(name); this.save();
  };
  S.unblock = function (name) { this.blocks = this.blocks.filter(function (b) { return low(b.name) !== low(name); }); this.save(); };
  S.addFriend = function (fid, name) {
    if (!FID.test(fid || '') || !name) return null;
    var old = this.friendBy(null, fid), f = { fid: fid, name: String(name), since: old ? old.since : Date.now() };
    this.friends = this.friends.filter(function (x) { return x.fid !== fid; });
    this.friends.push(f);
    if (this.friends.length > FRIENDS_KEEP) this.friends.splice(0, this.friends.length - FRIENDS_KEEP);   // the oldest go
    var nk = 'name:' + low(name), t = this.threads[nk];   // the stranger thread becomes the friend's
    if (t) {
      var to = this.threads[fid] || (this.threads[fid] = { name: f.name, msgs: [], unread: 0 });
      to.msgs = to.msgs.concat(t.msgs).sort(function (a, b) { return a.at - b.at; }).slice(-THREAD_KEEP);
      to.unread = (to.unread | 0) + (t.unread | 0); to.name = f.name;
      delete this.threads[nk];
    }
    this.dropAsk(name); this.save();
    return f;
  };
  S.removeFriend = function (fid) { this.friends = this.friends.filter(function (x) { return x.fid !== fid; }); this.save(); };
  S.rename = function (fid, name) {
    var f = this.friendBy(null, fid);
    if (!f || !name || f.name === name) return false;
    f.name = String(name);
    if (this.threads[fid]) this.threads[fid].name = f.name;
    this.save();
    return true;
  };
  S.addAsk = function (vip, name) {
    this.dropAsk(name);
    this.asks.push({ vip: vip >>> 0, name: String(name), at: Date.now() });
    if (this.asks.length > ASKS_KEEP) this.asks.shift();
  };
  S.dropAsk = function (name) { this.asks = this.asks.filter(function (a) { return low(a.name) !== low(name); }); };

  // ---------------------------------------------------------------------------------------------------- the page --
  var store = null, o = null, link = null, ui = null, open = false, tab = 'chat', thread = null, to = null, last = null;
  var pend = {}, idn = 0, gotDone = false, off = false, seenAt = 0, seen = {}, told = {}, senders = {}, acts = null;
  var dirty = true, placed = '', msgT = 0;

  function cfg() { var s = (o && o.settings && o.settings()) || {}; return { on: s.on !== false, city: s.city !== false }; }
  function peers() { return (link && Array.isArray(link.peers)) ? link.peers : []; }
  function peerBy(name) { var n = low(name); return peers().filter(function (p) { return low(p.name) === n; })[0] || null; }
  function vipOf(name) { var p = peerBy(name); return p ? p.vip >>> 0 : 0; }
  function online() { return !!link && link.state === 'online'; }
  function changed() { dirty = true; }
  function logLine(s) { try { if (o && o.log) o.log('[chat] ' + s); } catch (e) {} }
  function notice(text) { store.line({ kind: 'n', text: text }); changed(); }

  function send(m, cb) {
    if (off) return cb('off');
    if (!link || !link.chat) return cb('offline');
    m.t = 'chat'; m.id = ++idn;
    if (!link.chat(m)) return cb('offline');
    var me = link;
    pend[m.id] = { cb: cb, t: setTimeout(function () {
      delete pend[m.id];
      if (!gotDone && me === link && !off) { off = true; logLine('no answer from the server: chat not available here'); }
      cb(off ? 'off' : 'timeout'); changed();
    }, DONE_MS) };
  }
  function answered(id, done, n) {
    var p = pend[id];
    gotDone = true;
    if (!p) return;
    clearTimeout(p.t); delete pend[id];
    p.cb(done, n); changed();
  }
  function why(d, name) { return d === 'away' ? (name || 'That player') + ' ' + WHY.away : WHY[d] || (d === 'empty' ? '' : 'Not sent (' + d + ').'); }
  function say(text, bad) {
    if (!ui) return;
    ui.msg.textContent = text || ''; ui.msg.classList.toggle('bad', !!bad);
    clearTimeout(msgT);
    if (text) msgT = setTimeout(function () { ui.msg.textContent = ''; }, 6000);
  }

  function onChat(m) {
    if (m.op === 'done') return answered(m.id, String(m.done || ''), m.n | 0);
    if (m.op === 'seen') {
      gotDone = true;
      seen = {}; seenAt = Date.now();
      (Array.isArray(m.on) ? m.on : []).forEach(function (e) {
        if (!e || !FID.test(e.f || '')) return;
        seen[e.f] = e;
        if (e.name) store.rename(e.f, clip(e.name, 40));
      });
      if (pend[m.id]) { clearTimeout(pend[m.id].t); delete pend[m.id]; }
      return changed();
    }
    if (!cfg().on) return;
    var name = clip(m.name, 40) || '?', vip = (m.from >>> 0) || (m.vip >>> 0);
    if (m.op === 'friend') {
      if (store.addFriend(String(m.fid || ''), name)) { told[low(name)] = true; notice(name + ' is now your friend.'); logLine('friend added'); }
      return;
    }
    if (store.blocked(name)) return;
    if (m.op === 'ask') { store.addAsk(vip, name); notice(name + ' wants to be your friend (Friends tab).'); return; }
    if (m.op !== 'say') return;
    var text = clip(m.text, CHAT_LEN);
    if (!text) return;
    senders[low(name)] = { name: name, vip: vip, at: Date.now() };
    if (m.ch === 'w') {
      store.whisper(name, 'in', text);
      store.line({ kind: 'w', name: name, text: text });
      last = { name: name, vip: vip };
      if (open && tab === 'w' && thread === store.keyOf(name)) store.read(thread);
    } else {
      if (!cfg().city) return;
      store.line({ kind: m.ch === 'r' ? 'r' : 'c', name: name, text: text });
      if (!(open && tab === 'chat')) store.cityUnread++;
    }
    changed();
  }
  // a friend whose stored name shows in the peers list: "is online", once a session per friend
  function presence() {
    if (!online() || !cfg().on) return;
    store.friends.forEach(function (f) {
      var k = low(f.name);
      if (!told[k] && peerBy(f.name)) { told[k] = true; notice(f.name + ' is online.'); }
    });
    changed();
  }

  // ------------------------------------------------------------------------------------------------- sending --
  function setTo(t) { to = t; changed(); }
  function submit(raw) {
    var text = clip(raw, 400), t = to, mm = /^\/(w|r)(?:\s+|$)(.*)$/i.exec(text);
    if (mm) {
      if (low(mm[1]) === 'r') {
        if (!last) return say('No whisper to answer yet.', true), false;
        t = last; text = mm[2];
      } else {   // /w name text: the longest online nick that starts the rest (nicks may hold spaces)
        var rest = mm[2], best = null, me = link ? link.vip : 0;
        peers().forEach(function (p) {
          var n = low(p.name);
          if ((p.vip >>> 0) !== me && n && (low(rest) === n || low(rest).indexOf(n + ' ') === 0) && (!best || n.length > best.name.length)) best = p;
        });
        if (!best) return say((rest.split(' ')[0] || 'That player') + ' ' + WHY.away, true), false;
        t = { name: best.name, vip: best.vip >>> 0 }; text = rest.slice(best.name.length).trim();
      }
      setTo(t);
      if (!text) return true;
    }
    text = text.slice(0, CHAT_LEN);
    if (!text) return false;
    if (!t && !cfg().city) return say('City chat is off (Settings). Whisper with /w name text.', true), false;
    if (t) whisper(t, text); else city(text);
    return true;
  }
  function city(text) {
    var l = store.line({ kind: 'c', own: true, name: 'You', text: text, st: 'wait' });
    changed();
    send({ op: 'say', ch: 'c', text: text }, function (d, n) {
      l.st = d === 'sent' ? '' : 'failed';
      if (d !== 'sent') { l.why = why(d); say(l.why, true); }
      logLine('sent city (' + text.length + ' chars, done ' + d + (d === 'sent' ? ', ' + n + ' got it' : '') + ')');
    });
  }
  function whisper(t, text) {
    var vip = vipOf(t.name) || t.vip, m = store.whisper(t.name, 'out', text, 0, 'wait');
    var l = store.line({ kind: 'w', own: true, to: t.name, text: text, st: 'wait' });
    changed();
    send({ op: 'say', ch: 'w', to: vip, name: t.name, text: text }, function (d) {
      m.st = l.st = d === 'sent' ? 'sent' : 'failed';
      if (d !== 'sent') { l.why = why(d, t.name); say(l.why, true); }
      store.save();
      logLine('whisper to ' + t.name + ': ' + d);
    });
  }
  function askFriend(name) {
    var vip = vipOf(name);
    if (!vip) return say(name + ' ' + WHY.away, true);
    send({ op: 'ask', to: vip, name: name }, function (d) {
      say(d === 'sent' ? 'Friend request sent to ' + name + '.' : why(d, name), d !== 'sent');
      logLine('friend request: ' + d);
    });
  }
  function accept(a) {
    var vip = vipOf(a.name) || a.vip;
    send({ op: 'accept', to: vip, name: a.name }, function (d) {
      if (d === 'sent' || d === 'noask') store.dropAsk(a.name);
      if (d !== 'sent') say(why(d, a.name), true);
      logLine('accept: ' + d);
    });
  }
  function askSeen() {
    if (!store.friends.length || Date.now() - seenAt < SEEN_MS || !online()) return;
    seenAt = Date.now();
    send({ op: 'seen', f: store.friends.slice(-60).map(function (f) { return f.fid; }) }, function () {});
  }

  // ------------------------------------------------------------------------------------------------------- DOM --
  function el(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }
  function btn(text, fn, cls) {
    var b = el('button', 'ch-btn' + (cls ? ' ' + cls : ''), text);
    b.type = 'button';
    b.addEventListener('click', function (e) { e.stopPropagation(); fn(); });
    return b;
  }
  var BALLOON = '<svg viewBox="0 0 16 16" width="1.15em" height="1.15em" aria-hidden="true"><path d="M2 2.8h12v8.4H7.2L4 14v-2.8H2z" ' +
    'fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/></svg>';
  function build() {
    if (ui || typeof document != 'object' || !o.stage) return;
    ui = {};
    ui.log = el('div'); ui.log.id = 'chat-log';
    ui.chip = el('div'); ui.chip.id = 'chat-chip'; ui.chip.title = 'Chat';
    ui.chip.setAttribute('role', 'button'); ui.chip.setAttribute('aria-label', 'Chat');
    ui.chip.addEventListener('pointerdown', function (e) {   // not a click: the canvas keeps the focus (keys)
      e.preventDefault(); e.stopPropagation();
      if (open) close(); else openPanel(false);
    });
    ui.lines = el('div', 'cl-lines');
    ui.log.append(ui.chip, ui.lines);

    var d = ui.dlg = el('dialog'); d.id = 'chat-dlg'; d.setAttribute('aria-label', 'Chat');
    var head = el('div', 'ch-head');
    ui.tabs = {};
    [['chat', 'Chat'], ['w', 'Whispers'], ['f', 'Friends']].forEach(function (t) {
      var b = el('button', 'ch-tab'); b.type = 'button'; b.setAttribute('role', 'tab');
      b.addEventListener('click', function () { show(t[0]); });
      ui.tabs[t[0]] = { b: b, label: t[1] };
      head.append(b);
    });
    head.append(btn('×', close, 'ch-x'));
    head.lastChild.setAttribute('aria-label', 'Close chat');
    ui.acts = el('div', 'ch-acts'); ui.acts.hidden = true;
    ui.body = el('div', 'ch-body');
    ui.body.addEventListener('click', function (e) {
      var n = e.target.closest && e.target.closest('[data-name]');
      if (n) { acts = n.getAttribute('data-name'); changed(); }
    });
    ui.body.addEventListener('scroll', function () {
      if (tab === 'chat' || (tab === 'w' && thread)) ui.stick = ui.body.scrollHeight - ui.body.scrollTop - ui.body.clientHeight < 24;
    });
    var foot = el('div', 'ch-foot'), row = el('div', 'ch-send');
    ui.pill = el('span', 'ch-pill'); ui.pill.hidden = true;
    ui.in = el('input', 'ch-in'); ui.in.type = 'text'; ui.in.maxLength = CHAT_LEN; ui.in.autocomplete = 'off';
    ui.in.setAttribute('enterkeyhint', 'send'); ui.in.setAttribute('aria-label', 'Message');
    ui.in.addEventListener('input', function () { ui.count.textContent = ui.in.value.length + '/' + CHAT_LEN; });
    ui.in.addEventListener('keydown', function (e) {
      if (e.key === 'Enter' && !e.isComposing) { e.preventDefault(); if (submit(ui.in.value)) { ui.in.value = ''; ui.count.textContent = ''; } }
      else if (e.key === 'Backspace' && !ui.in.value && to) setTo(null);   // Backspace on an empty input drops the whisper pill
    });
    ui.send = btn('Send', function () { if (submit(ui.in.value)) { ui.in.value = ''; ui.count.textContent = ''; } ui.in.focus(); }, 'ch-go');
    ui.count = el('span', 'ch-count');
    row.append(ui.pill, ui.in, ui.send);
    ui.quick = el('div', 'ch-quick');
    PHRASES.forEach(function (p) { ui.quick.append(btn(p, function () { submit(p); })); });
    ui.msg = el('div', 'ch-msg'); ui.msg.setAttribute('role', 'status');
    var info = el('div', 'ch-info'); info.append(ui.msg, ui.count);
    foot.append(row, ui.quick, info);
    d.append(head, ui.acts, ui.body, foot);
    // the panel's keys are the page's: they must not reach the game (SDL listens on the window); Esc closes
    ['keydown', 'keyup', 'keypress'].forEach(function (k) { d.addEventListener(k, function (e) { e.stopPropagation(); }); });
    d.addEventListener('keydown', function (e) { if (e.key === 'Escape') { e.preventDefault(); close(); } });
    d.addEventListener('close', function () {
      open = false; acts = null;
      if (o.playing && o.playing() && o.canvas) o.canvas.focus();
      changed();
    });
    o.stage.append(ui.log, d);
  }

  function lineEl(l, panel) {
    var d = el('div', 'cl-line k-' + l.kind + (l.own ? ' own' : '') + (l.st ? ' st-' + l.st : ''));
    if (l.kind === 'n') { d.textContent = l.text; return d; }
    if (l.kind === 'w') d.append(el('span', 'cl-tag', l.own ? '✉ to' : '✉'));
    var who = l.own ? (l.kind === 'w' ? l.to : 'You') : l.name, n = el('span', 'cn', who);
    if (panel && (l.kind === 'w' || !l.own)) n.setAttribute('data-name', who);
    d.append(n, ': ' + l.text);
    if (l.st === 'failed') d.append(el('span', 'cl-why', ' ' + (l.why || WHY.timeout)));
    return d;
  }
  function row(text, sub, buttons, dot) {
    var r = el('div', 'ch-row');
    if (dot != null) r.append(el('i', 'ch-dot' + (dot ? ' on' : '')));
    var s = el('span');
    if (typeof text == 'string') s.textContent = text; else s.append(text);
    if (sub) s.append(el('small', '', ' ' + sub));
    r.append(s);
    buttons.forEach(function (b) { r.append(b); });
    return r;
  }
  function nameEl(name) { var n = el('b', 'cn', name); n.setAttribute('data-name', name); return n; }
  function status(f) {
    var e = seen[f.fid];
    if (e && Date.now() - seenAt < 120000)
      return e.w === 'race' ? 'racing' : e.w === 'roam' ? (CITIES[e.city | 0] || 'Free Roam') : 'in the menus';
    return peerBy(f.name) ? 'online' : 'offline';
  }
  function here() {   // names worth an action: this session's senders still online, the players push's same-city ones
    var out = {}, me = link ? link.vip : 0, pl = link && link.players && Array.isArray(link.players.list) ? link.players.list : null;
    Object.keys(senders).forEach(function (k) { var p = peerBy(senders[k].name); if (p && (p.vip >>> 0) !== me) out[k] = p.name; });
    if (pl) {
      var mine = null;
      peers().forEach(function (p) { if ((p.vip >>> 0) === me) mine = pl.filter(function (q) { return low(q.name) === low(p.name); })[0]; });
      if (mine && mine.city != null) pl.forEach(function (q) { if (q.city === mine.city && q !== mine && peerBy(q.name)) out[low(q.name)] = q.name; });
    }
    return Object.keys(out).slice(0, 30).map(function (k) { return out[k]; });
  }
  function whisperTo(name) {
    setTo({ name: name, vip: vipOf(name) });
    if (tab !== 'w') show('w', store.keyOf(name)); else { thread = store.keyOf(name); store.read(thread); }
    acts = null;
    if (ui) ui.in.focus();
  }

  function renderBody() {
    var b = ui.body, keep = b.scrollTop, frag = document.createDocumentFragment();
    if (tab === 'chat') {
      store.ring.forEach(function (l) { frag.append(lineEl(l, true)); });
      if (!store.ring.length) frag.append(el('p', 'ch-hint', cfg().city ? 'Say hi to the players around you. Enter sends, Esc closes; /w name text whispers.' :
        'City chat is off (Settings). Whispers and friends still work.'));
    } else if (tab === 'w') {
      var t = thread && store.threads[thread];
      if (t) {
        frag.append(row(nameEl(t.name), peerBy(t.name) ? 'online' : 'offline', [btn('‹ All', function () { thread = null; changed(); })], !!peerBy(t.name)));
        t.msgs.forEach(function (m) {
          frag.append(lineEl({ kind: 'w', own: m.dir === 'out', name: t.name, to: t.name, text: m.text, st: m.st === 'failed' ? 'failed' : m.st === 'wait' ? 'wait' : '' }, true));
        });
      } else {
        var ls = store.list();
        ls.forEach(function (x) {
          var go = function () { thread = x.key; store.read(x.key); ui.stick = true; setTo({ name: x.name, vip: vipOf(x.name) }); };
          var lm = x.t.msgs[x.t.msgs.length - 1], r = row(x.name, lm ? (lm.dir === 'out' ? 'you: ' : '') + lm.text : '', [btn('Open', go)], !!peerBy(x.name));
          if (x.unread) r.insertBefore(el('span', 'cl-badge', String(x.unread)), r.lastChild);
          r.classList.add('ch-pick');
          r.addEventListener('click', go);
          frag.append(r);
        });
        if (!ls.length) frag.append(el('p', 'ch-hint', 'No whispers yet. Click a name, or type /w name text. Kept in this browser only.'));
      }
    } else {
      askSeen();
      if (store.asks.length) {
        frag.append(el('h4', 'ch-h', 'Friend requests'));
        store.asks.forEach(function (a) {
          frag.append(row(nameEl(a.name), 'wants to be friends', [btn('Accept', function () { accept(a); }),
            btn('Ignore', function () { store.dropAsk(a.name); changed(); })]));
        });
      }
      frag.append(el('h4', 'ch-h', 'Friends'));
      store.friends.slice().sort(function (x, y) { return ((status(y) !== 'offline') - (status(x) !== 'offline')) || (low(x.name) < low(y.name) ? -1 : 1); })
        .forEach(function (f) {
          var st = status(f);
          frag.append(row(nameEl(f.name), st, [btn('Whisper', function () { whisperTo(f.name); }),
            btn('Remove', function () { store.removeFriend(f.fid); say(f.name + ' removed from your friends.'); changed(); })], st !== 'offline'));
        });
      if (!store.friends.length) frag.append(el('p', 'ch-hint', 'No friends yet: Add friend on a player below or on a name in the chat. Kept in this browser only.'));
      var h = here();
      if (h.length) {
        frag.append(el('h4', 'ch-h', 'Players here'));
        h.forEach(function (n) {
          var bs = [btn('Whisper', function () { whisperTo(n); })];
          if (!store.friendBy(n)) bs.push(btn('Add friend', function () { askFriend(n); }));
          bs.push(btn('Block', function () { store.block(n); say(n + ' blocked.'); changed(); }));
          frag.append(row(nameEl(n), '', bs, true));
        });
      }
      if (store.blocks.length) {
        frag.append(el('h4', 'ch-h', 'Blocked'));
        store.blocks.forEach(function (x) { frag.append(row(x.name, '', [btn('Unblock', function () { store.unblock(x.name); changed(); })])); });
      }
    }
    b.replaceChildren(frag);
    if (tab === 'chat' || (tab === 'w' && thread)) { if (ui.stick !== false) b.scrollTop = b.scrollHeight; }
    else b.scrollTop = keep;
  }
  function renderPanel() {
    var t = store.threads, wu = 0, k;
    for (k in t) wu += t[k].unread | 0;
    var counts = { chat: store.cityUnread, w: wu, f: store.asks.length };
    for (k in ui.tabs) {
      var x = ui.tabs[k];
      x.b.textContent = x.label;
      if (counts[k]) x.b.append(el('span', 'cl-badge', counts[k] > 9 ? '9+' : String(counts[k])));
      x.b.setAttribute('aria-selected', k === tab);
    }
    ui.acts.hidden = !acts;
    if (acts) {
      var n = acts, bs = [el('b', 'cn', n), btn('Whisper', function () { whisperTo(n); })];
      if (!store.friendBy(n)) bs.push(btn('Add friend', function () { askFriend(n); acts = null; changed(); }));
      bs.push(btn('Block', function () { store.block(n); if (to && low(to.name) === low(n)) to = null; acts = null; say(n + ' blocked.'); changed(); }));
      bs.push(btn('×', function () { acts = null; changed(); }, 'ch-x'));
      ui.acts.replaceChildren.apply(ui.acts, bs);
    }
    ui.pill.hidden = !to;
    if (to) { ui.pill.textContent = 'To ' + to.name; ui.pill.append(btn('×', function () { setTo(null); ui.in.focus(); })); }
    var c = cfg(), dis = off || !online();
    ui.in.disabled = ui.send.disabled = dis;
    ui.quick.querySelectorAll('button').forEach(function (b) { b.disabled = dis || (!to && !c.city); });
    ui.in.placeholder = off ? WHY.off : !online() ? 'Not connected' : to ? 'Whisper to ' + to.name : c.city ? 'Say something (Enter)' : '/w name text';
    renderBody();
  }
  function renderLog(now) {
    var vis = [], i, l;
    for (i = store.ring.length - 1; i >= 0 && vis.length < LOG_LINES; i--) {
      l = store.ring[i];
      if (now - l.at < (l.kind === 'w' ? WHISPER_MS : LINE_MS)) vis.unshift(l); else break;
    }
    var key = vis.map(function (x) { return x.id + x.st + (now - x.at > (x.kind === 'w' ? WHISPER_MS : LINE_MS) - FADE_MS ? 'f' : ''); }).join(',') + '|' + store.unread();
    if (key === ui.logKey) return;
    ui.logKey = key;
    ui.lines.replaceChildren.apply(ui.lines, vis.map(function (x) {
      var e = lineEl(x, false);
      if (now - x.at > (x.kind === 'w' ? WHISPER_MS : LINE_MS) - FADE_MS) e.classList.add('fade');
      return e;
    }));
    var n = store.unread();
    ui.chip.innerHTML = BALLOON;
    if (n) ui.chip.append(el('span', 'cl-badge', n > 9 ? '9+' : String(n)));
  }
  // the picture's middle 16:9 inside the canvas (object-fit contain unless Stretch), as pre.js's overlays place theirs
  function place(now) {
    var cv = o.canvas, P = root.Module && root.Module.mc3Pic, pc = P ? P(now) : null;
    var r = (pc && pc.r) || cv.getBoundingClientRect(), L = (pc && pc.L) || o.stage.getBoundingClientRect();
    var x = r.left - L.left, y = r.top - L.top, w = r.width, h = r.height, a = cv.width / cv.height;
    if (!(a > 0.5 && a < 5)) a = 16 / 9;
    if (!o.stage.classList.contains('stretch')) { if (w / h > a) { x += (w - h * a) / 2; w = h * a; } else { y += (h - w / a) / 2; h = w / a; } }
    if (!(w > 0 && h > 0)) return null;
    var hw = Math.min(w, h * 16 / 9), left = x + (w - hw) / 2 + Math.max(8, h * 0.02), top = y + h * 0.08;
    var lw = Math.min(420, Math.max(220, hw * 0.34), w - 16), bottom = y + h * 0.25;
    document.querySelectorAll('.tc:not([hidden]) .tc-b').forEach(function (c) {   // right of touch buttons there
      var q = c.getBoundingClientRect(), ql = q.left - L.left, qr = q.right - L.left;
      if (q.width && ql < left + lw && qr > left && q.bottom - L.top > top && q.top - L.top < bottom) left = qr + 6;
    });
    return { left: left, top: top, lw: lw, logH: bottom - top, pw: Math.min(440, Math.max(280, hw * 0.4), w - 16),
      ph: Math.max(220, Math.min(h * 0.55, h - (top - y) - 8)), fs: Math.min(18, Math.max(11, h * 0.022)) };
  }
  function tick() {
    if (!o || !ui) return;
    var now = performance.now(), wall = Date.now(), c = cfg(), shown = c.on && o.playing() && (online() || open);
    if (!c.on && open) close();
    if (!shown) { if (ui.log.style.display !== 'none') { ui.log.style.display = 'none'; placed = ''; } return; }
    var p = place(now);
    if (!p) return;
    var pk = [p.left, p.top, p.lw, p.logH, p.pw, p.ph, p.fs, open].map(function (v) { return typeof v == 'number' ? v.toFixed(0) : v; }).join('|');
    if (pk !== placed) {
      placed = pk;
      var s = ui.log.style;
      s.transform = 'translate(' + p.left.toFixed(1) + 'px,' + p.top.toFixed(1) + 'px)';
      s.width = p.lw.toFixed(0) + 'px'; s.maxHeight = p.logH.toFixed(0) + 'px'; s.fontSize = p.fs.toFixed(1) + 'px';
      var ds = ui.dlg.style;
      ds.left = p.left.toFixed(1) + 'px'; ds.top = p.top.toFixed(1) + 'px';
      ds.width = p.pw.toFixed(0) + 'px'; ds.maxHeight = p.ph.toFixed(0) + 'px';
    }
    ui.log.style.display = open ? 'none' : '';
    if (!open) renderLog(wall);
    if (dirty && open) { dirty = false; renderPanel(); }
    else if (dirty) dirty = false;
  }

  function show(t, th) {
    tab = t; acts = null;
    if (t === 'chat') { store.cityUnread = 0; ui && (ui.stick = true); }
    if (t === 'w') { thread = th || thread; if (thread) store.read(thread); ui && (ui.stick = true); }
    changed(); tick();
  }
  function openPanel(focus) {
    if (!o || !store) return;
    build();
    if (!ui) return;
    if (!open) { ui.dlg.show(); open = true; ui.stick = true; }
    try { root.Module && root.Module.mc3ReleaseKeys && root.Module.mc3ReleaseKeys(); } catch (e) {}
    if (tab === 'chat') store.cityUnread = 0;
    if (tab === 'f') askSeen();
    changed(); placed = ''; tick();
    if (focus && !ui.in.disabled) ui.in.focus();
  }
  function close() { if (ui && ui.dlg.open) ui.dlg.close(); }

  // U (or the player's key) opens the panel with the input focused: only while playing, online, chat on, no other
  // dialog open and no field focused; the game never sees that key press
  var keysOn = false;
  function onKey(e) {
    if (!o || open || e.repeat || e.ctrlKey || e.altKey || e.metaKey) return;
    if (!o.playing() || !cfg().on || !online()) return;
    var ks = (o.keys && o.keys()) || ['KeyU'];
    if (ks.indexOf(e.code) < 0 || (o.keyUsed && o.keyUsed(e.code))) return;
    var t = e.target;
    if (t && t.closest && t.closest('input, textarea, select, [contenteditable], dialog')) return;
    if (document.querySelector('dialog[open]:not(.docked):not(#chat-dlg)')) return;
    e.preventDefault(); e.stopImmediatePropagation();
    openPanel(true);
  }

  root.MC3Chat = {
    ChatStore: ChatStore,
    attach: function (opts) {
      o = opts || {};
      if (!store) store = new ChatStore();
      var l = o.lan;
      if (l && l !== link) {
        link = l; gotDone = false; off = false; seenAt = 0;
        Object.keys(pend).forEach(function (id) { clearTimeout(pend[id].t); delete pend[id]; });
        if (l.onMsg) l.onMsg(function (m) { if (l === link && m && m.t === 'chat') onChat(m); });
        if (l.on) l.on(function (x) { if (x === link) presence(); });
      }
      build();
      if (!keysOn && typeof addEventListener == 'function') {
        keysOn = true;
        addEventListener('keydown', onKey, true);
        setInterval(tick, 250);
      }
      changed();
    },
    open: function () { openPanel(false); },
    close: close,
    sync: function () { changed(); placed = ''; tick(); },
  };
})();
