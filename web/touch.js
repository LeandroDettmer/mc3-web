// Touch controls and the phone side of the page (web/index.html loads this before its own script).
// Ported in idea, not code, from nfsu2-web's touch controls and device helpers (th0mk, MIT;
// web/src/app/touch.ts, device.ts, display.ts): a virtual controller merged with a real one, a thumb stick
// where the thumb lands, pedals a thumb slides between, a press held long enough for the game's next read,
// no page scroll or zoom under the controls, phone vibration on a press, the Wake Lock kept while visible.
//
// The controller is pad 1 as a real one is: the page writes its state into the runtime's box
// (Module.mc3TouchPad, web/pre.js -> xboxrecomp/src/input/touch_pad_web.c), merged into port 0 with
// the keyboard and any real pad.
//   left half   thumb stick where the thumb lands: the left stick (steer, menus)
//   top left    the rare buttons in one row, apart from the page's Settings / Drift / Map (top right): Flash (White:
//               headlights, street race invite/join), Back, Start, Black (held 1 s: leave a street race); the d-pad
//               (I J K L: map, music, hydraulics) under them, small and high, clear of the race box over the minimap
//   right       the driving thumb's group (2026-11-13): Gas tall on the edge, Brake beside it, Handbrake (A) big
//               above Brake (a thumb rocks Gas, Handbrake, Brake), Look back (Y), Two wheels (B), Nitro (X) over
//               them, Special move (L3) over Nitro. After mobile racing HUDs (Asphalt, CarX Drift, Real Racing 3):
//               pedals and drift under the right thumb, big, >= 10 px apart; rare buttons out of its reach.
// A finger slides from one button to another without letting go (Gas to Handbrake, A to X).
(() => {
  'use strict';
  const HOLD_MS = 120;          // the game reads the pad once a frame: a tap stays down this long at least,
  const HOLD_FRAMES = 3;        // and for this many of the game's frames (Module.mc3Frames): the runtime samples the pad
                                // only when its USB thread gets the guest lock, which a slow device (a phone at a few fps)
                                // or a busy CPU holds longer than HOLD_MS -- a short tap (Start, d-pad) was never seen
  const HOLD_MAX_MS = 1500;     // no frames counted (a loading stall): let go anyway
  const STICK_PX = 56;          // thumb travel to full lock
  const DEAD = 0.12;            // stick dead zone
  const BIT = { up: 1, down: 2, left: 4, right: 8, start: 0x10, back: 0x20, ls: 0x40 };
  const AN = { A: 2, B: 3, X: 4, Y: 5, BLACK: 6, WHITE: 7, LT: 8, RT: 9 };   // touch_pad_web.c word numbers
  // id, label, [kind, value]: 'a' analog word (255), 'b' wButtons bit
  const CONTROLS = [
    ['gas', 'Gas', 'a', AN.RT], ['brake', 'Brake', 'a', AN.LT],
    ['fa', 'A', 'a', AN.A], ['fb', 'B', 'a', AN.B], ['fx', 'X', 'a', AN.X], ['fy', 'Y', 'a', AN.Y],
    ['white', 'Flash', 'a', AN.WHITE], ['black', 'Black', 'a', AN.BLACK],
    ['back', 'Back', 'b', BIT.back], ['start', 'Start', 'b', BIT.start],
    ['du', '▲', 'b', BIT.up], ['dd', '▼', 'b', BIT.down], ['dl', '◀', 'b', BIT.left], ['dr', '▶', 'b', BIT.right],
    ['ls', 'L3', 'b', BIT.ls],
  ];
  // a word under the letter: what the button does while driving
  const CAPTIONS = { fa: 'Handbrake', fb: '2 wheels', fx: 'Nitro', fy: 'Look back', ls: 'Special' };
  const TITLES = { fa: 'A: handbrake, select', fb: 'B: two wheels', fx: 'X: nitrous', fy: 'Y: look back',
                   white: 'Flash (White): headlights; near other players, invite them to a street race or join one',
                   black: 'Black: camera; held 1 s: cancel or give up a street race',ls: 'Left stick click: special move' };
  // the dashboard's high-beam symbol (a lamp, horizontal rays) on the White button
  const HIGH_BEAM = '<svg viewBox="0 0 24 16" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="2" '
    + 'stroke-linecap="round"><path d="M13 2c5 0 9 2.5 9 6s-4 6-9 6z" fill="currentColor"/>'
    + '<path d="M2 3h8M2 6.3h8M2 9.7h8M2 13h8"/></svg>';

  const CSS = `
  .tc { grid-row: -2 / -1; grid-column: 1; position: relative; pointer-events: none; touch-action: none;
        user-select: none; -webkit-user-select: none; -webkit-touch-callout: none; opacity: var(--tc-op, .45);
        --u: clamp(46px, 14vmin, 84px); --g: 10px;                 /* size unit; gap between buttons */
        --pw: calc(var(--u) * 1.45); --c: calc(var(--u) * .95);   /* a pedal column's width; a round button */
        --rh: max(44px, calc(var(--u) * .8)); --rw: calc(var(--u) * 1.25);   /* the top row's buttons */
        --d: calc(var(--u) * .7); --ds: calc(var(--u) * .74);     /* d-pad key, step */
        --l: max(12px, env(safe-area-inset-left)); --r: max(12px, env(safe-area-inset-right));
        --t: max(8px, env(safe-area-inset-top)); --b: max(12px, env(safe-area-inset-bottom));
        --dt: calc(var(--t) + var(--rh) + 12px); }
  /* upright: the top row goes under the page's Settings / Drift / Map, which then span the top */
  @media (max-aspect-ratio: 1/1) { .tc { --t: max(60px, calc(env(safe-area-inset-top) + 52px)); } }
  .tc > * { position: absolute; pointer-events: auto; touch-action: none; -webkit-tap-highlight-color: transparent; }
  .tc-zone { left: 0; top: 0; bottom: 0; width: 50%; }
  .tc-b { display: flex; align-items: center; justify-content: center; box-sizing: border-box; color: #fff;
          font: 700 italic calc(var(--u) * .28)/1 "Helvetica Neue", Helvetica, Arial, sans-serif;
          background: rgba(20, 20, 20, .7); border: 2px solid rgba(255, 255, 255, .8); border-radius: 12px; }
  .tc-b.on { background: rgba(240, 180, 64, .85); color: #000; border-color: #f0b440; }   /* the game's gold (index.html --gold) */
  .tc-b.cap { flex-direction: column; font-size: calc(var(--u) * .34); }
  .tc-b small { margin-top: 2px; font: 700 max(9px, calc(var(--u) * .17))/1.1 "Helvetica Neue", Helvetica, Arial, sans-serif; white-space: nowrap; }
  /* right thumb: Gas | Handbrake over Brake | Y B X over them | L3 over X */
  .tc-gas { right: var(--r); bottom: var(--b); width: var(--pw); height: calc(var(--u) * 2.35 + var(--g)); }
  .tc-brake { right: calc(var(--r) + var(--pw) + var(--g)); bottom: var(--b); width: var(--pw); height: calc(var(--u) * 1.2); }
  .tc-fa { right: calc(var(--r) + var(--pw) + var(--g)); bottom: calc(var(--b) + var(--u) * 1.2 + var(--g));
           width: var(--pw); height: calc(var(--u) * 1.15); }
  .tc-fx, .tc-fb, .tc-fy, .tc-ls { width: var(--c); height: var(--c); border-radius: 50%; bottom: calc(var(--b) + var(--u) * 2.35 + var(--g) * 2); }
  .tc-fx, .tc-ls { right: calc(var(--r) + (var(--pw) - var(--c)) / 2); }
  .tc-fb { right: calc(var(--r) + var(--pw) + var(--g) + (var(--pw) - var(--c)) / 2); }
  .tc-fy { right: calc(var(--r) + (var(--pw) + var(--g)) * 2 + (var(--pw) - var(--c)) / 2); }
  .tc-ls { bottom: calc(var(--b) + var(--u) * 2.35 + var(--g) * 3 + var(--c)); }
  /* top left: the rare buttons, then the d-pad */
  .tc-white, .tc-back, .tc-start, .tc-black { top: var(--t); width: var(--rw); height: var(--rh); font-size: calc(var(--u) * .24); }
  .tc-white { left: var(--l); gap: .3em; }
  .tc-white svg { width: 1.4em; height: .95em; flex: none; }
  .tc-back { left: calc(var(--l) + var(--rw) + var(--g)); }
  .tc-start { left: calc(var(--l) + (var(--rw) + var(--g)) * 2); }
  .tc-black { left: calc(var(--l) + (var(--rw) + var(--g)) * 3); }
  .tc-du, .tc-dd, .tc-dl, .tc-dr { width: var(--d); height: var(--d); font-size: calc(var(--u) * .24); }
  .tc-du { left: calc(var(--l) + var(--ds)); top: var(--dt); }
  .tc-dl { left: var(--l); top: calc(var(--dt) + var(--ds)); }
  .tc-dr { left: calc(var(--l) + var(--ds) * 2); top: calc(var(--dt) + var(--ds)); }
  .tc-dd { left: calc(var(--l) + var(--ds)); top: calc(var(--dt) + var(--ds) * 2); }
  .tc-base, .tc-knob { pointer-events: none; border-radius: 50%; transform: translate(-50%, -50%); }
  .tc-base { width: ${STICK_PX * 2}px; height: ${STICK_PX * 2}px; border: 2px solid rgba(255, 255, 255, .8); background: rgba(20, 20, 20, .4); }
  .tc-knob { width: 56px; height: 56px; background: rgba(255, 255, 255, .8); }
  `;

  /** a phone: the browser says it is a mobile one, or a touch screen with a phone's short side (nfsu2-web device.ts).
   * Never without a touch screen: Firefox on a TCL Google TV says 'Android 11; Mobile' (2026-11-09). */
  function isPhone() {
    const uad = navigator.userAgentData;
    return navigator.maxTouchPoints > 0 && (uad?.mobile === true || /Android.+Mobile|iPhone|iPod/.test(navigator.userAgent)
      || (navigator.maxTouchPoints > 1 && Math.min(screen.width, screen.height) < 600));
  }
  /** the primary pointer is a finger (phones, tablets), not a laptop whose screen also takes touch; a TV's remote
   * can report a coarse pointer, so a touch screen too (maxTouchPoints) */
  const touchFirst = () => navigator.maxTouchPoints > 0 && matchMedia('(pointer: coarse)').matches;
  const hasTouch = () => navigator.maxTouchPoints > 0;

  /** The on-screen controller over the game in `stage` (before `before`). mode: 'auto' (on a touch-first device,
   *  or from the first touch on the game; a key press hides it until the next touch), 'on', 'off'. */
  function attach(stage, before, { mode = 'auto', opacity = 0.45 } = {}) {
    const style = document.createElement('style');
    style.textContent = CSS;
    document.head.append(style);
    const root = document.createElement('div');
    root.className = 'tc';
    root.hidden = true;
    root.style.setProperty('--tc-op', opacity);
    root.setAttribute('aria-hidden', 'true');            // pointer controls only; the keyboard and pads do the same
    const zone = document.createElement('div');
    zone.className = 'tc-zone';
    const base = document.createElement('div'), knob = document.createElement('div');
    base.className = 'tc-base'; knob.className = 'tc-knob'; base.hidden = knob.hidden = true;
    root.append(zone, base, knob);
    const els = new Map();
    for (const [id, label, kind, value] of CONTROLS) {
      const el = document.createElement('div');
      el.className = 'tc-b tc-' + id;
      el.dataset.c = id;
      el.textContent = label;
      if (CAPTIONS[id]) { el.classList.add('cap'); el.append(Object.assign(document.createElement('small'), { textContent: CAPTIONS[id] })); }
      if (id === 'white') el.insertAdjacentHTML('afterbegin', HIGH_BEAM);
      if (TITLES[id]) el.title = TITLES[id];
      root.append(el);
      els.set(id, { el, kind, value });
    }
    stage.insertBefore(root, before);

    const words = new Int32Array(14);
    const fingers = new Map();       // pointerId -> control id, or 'stick'
    const downAt = new Map();        // control id -> its last press { t: time, f: game frame } (held HOLD_MS and HOLD_FRAMES)
    let on = false, stick = null, sent = '', timer = 0;
    const frames = () => window.Module?.mc3Frames?.();   // undefined until the runtime is up
    const held = (p, now, f) => !!p && (now - p.t < HOLD_MS
      || (p.f !== undefined && f !== undefined && f - p.f < HOLD_FRAMES && now - p.t < HOLD_MAX_MS));

    function compute() {
      words.fill(0);
      words[0] = on ? 1 : 0;
      const now = performance.now(), f = frames(), down = new Set(fingers.values());
      for (const [id, c] of els) {
        const pressed = down.has(id) || held(downAt.get(id), now, f);
        c.el.classList.toggle('on', pressed);
        if (!pressed) continue;
        if (c.kind === 'a') words[c.value] = 255; else words[1] |= c.value;
      }
      // api.tapMap (the page's Map button): d-pad up, held like a touch press, merged even with the controls off
      if (held(downAt.get('map'), now, f)) { words[0] = 1; words[1] |= BIT.up; }
      if (stick) {
        let x = stick.dx / STICK_PX, y = stick.dy / STICK_PX;
        const m = Math.hypot(x, y);
        if (m > 1) { x /= m; y /= m; }
        if (m < DEAD) x = y = 0;
        words[10] = Math.round(x * 32767);
        words[11] = Math.round(-y * 32767);              // the Xbox's Y points up
      }
    }
    function send() {
      compute();
      const key = words.join();
      // the runtime may not be up yet: send again until it took this state
      if (key !== sent && window.Module?.mc3TouchPad?.(words)) sent = key;
    }
    // re-sent every 50 ms while on: the HOLD_MS releases and a runtime that came up after the last change
    // (a throw in send must not leave `timer` set with no tick coming: the controls would stop for good)
    const tick = () => { try { send(); } finally { timer = on || sent !== words.join() ? setTimeout(tick, 50) : 0; } };
    const kick = () => { if (!timer) timer = setTimeout(tick, 50); send(); };

    function press(pid, id) {
      fingers.set(pid, id);
      downAt.set(id, { t: performance.now(), f: frames() });
      navigator.vibrate?.(8);                             // Android: a short tick; iOS has no vibration for pages
    }
    root.addEventListener('pointerdown', e => {
      e.preventDefault();                                 // no focus change, no mouse events, no zoom
      const c = e.target.closest('[data-c]');
      if (c) press(e.pointerId, c.dataset.c);
      else if (e.target === zone && !stick) {
        const r = root.getBoundingClientRect();
        stick = { pid: e.pointerId, x: e.clientX, y: e.clientY, dx: 0, dy: 0 };
        fingers.set(e.pointerId, 'stick');
        for (const el of [base, knob]) { el.hidden = false; el.style.left = (e.clientX - r.left) + 'px'; el.style.top = (e.clientY - r.top) + 'px'; }
      } else return;
      try { e.target.setPointerCapture(e.pointerId); } catch {}
      kick();
    });
    root.addEventListener('pointermove', e => {
      const f = fingers.get(e.pointerId);
      if (!f) return;
      if (f === 'stick') {
        stick.dx = e.clientX - stick.x; stick.dy = e.clientY - stick.y;
        const m = Math.hypot(stick.dx, stick.dy), k = m > STICK_PX ? STICK_PX / m : 1;
        const r = root.getBoundingClientRect();
        knob.style.left = (stick.x + stick.dx * k - r.left) + 'px'; knob.style.top = (stick.y + stick.dy * k - r.top) + 'px';
      } else {
        // a thumb slides onto another button: that one now (Gas to Brake, A to X)
        const c = document.elementFromPoint(e.clientX, e.clientY)?.closest?.('.tc [data-c]');
        if (c && c.dataset.c !== f) press(e.pointerId, c.dataset.c);
      }
      kick();
    });
    const lift = e => {
      const f = fingers.get(e.pointerId);
      if (!f) return;
      fingers.delete(e.pointerId);
      if (f === 'stick') { stick = null; base.hidden = knob.hidden = true; }
      kick();
    };
    root.addEventListener('pointerup', lift);
    root.addEventListener('pointercancel', lift);       // the phone took the touch (a system gesture)
    root.addEventListener('contextmenu', e => e.preventDefault());
    // iOS Safari: cancelling pointerdown does not stop its own touch handling (text-selection loupe on a held
    // button, page rubber-banding, a synthesized click); cancelling the touch events does. Pointer events still come.
    for (const t of ['touchstart', 'touchmove'])
      root.addEventListener(t, e => { if (e.cancelable) e.preventDefault(); }, { passive: false });

    function setOn(v) {
      if (v === on) return;
      on = v;
      root.hidden = !on;
      if (!on) { fingers.clear(); downAt.clear(); stick = null; base.hidden = knob.hidden = true; }
      kick();
      api.onchange?.(on);
    }
    // Auto: a finger on the game shows them, a key hides them until the next touch (nfsu2-web's rule)
    // (on the picture only: a tap on the bar's Touch button toggles them itself)
    stage.addEventListener('pointerdown', e => {
      if (mode === 'auto' && e.pointerType === 'touch' && e.target.tagName === 'CANVAS') setOn(true);
    }, true);
    addEventListener('keydown', e => {
      if (mode === 'auto' && !e.repeat && e.isTrusted && !e.target.closest?.('input, textarea, select')) setOn(false);
    }, true);
    // the in-game Settings change both live (web/index.html setSetting)
    function setMode(m) { mode = m; setOn(m === 'on' || (m === 'auto' && touchFirst())); }
    const setOpacity = v => root.style.setProperty('--tc-op', v);
    const tapMap = () => { downAt.set('map', { t: performance.now(), f: frames() }); kick(); };
    const api = { setOn, isOn: () => on, onchange: null, setMode, setOpacity, tapMap };
    setMode(mode);
    return api;
  }

  /** Element full screen where the browser has it: unprefixed, or WebKit's (iPad Safari before 16.4). An iPhone's
   *  Safari has neither (only video goes full screen there): false. Call from a click. */
  function fullscreen(el) {
    try {
      if (el.requestFullscreen) { el.requestFullscreen({ navigationUI: 'hide' })?.then(() => screen.orientation?.lock?.('landscape')).catch(() => {}); return true; }
      if (el.webkitRequestFullscreen) { el.webkitRequestFullscreen(); return true; }
    } catch {}
    return false;
  }
  const canFullscreen = () => !!(document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen);
  /** On Play: the whole screen, turned to landscape, where the browser allows (Android's Chrome; not an iPhone's
   *  Safari, whose pages never go full screen: there "Add to Home Screen" runs the page without Safari's bars). */
  const enterGame = el => { fullscreen(el); };

  /** The screen kept on while the game runs (Screen Wake Lock), taken again whenever the page shows; touch and
   *  controller input do not keep every system's screen awake (nfsu2-web display.ts keepScreenOn). */
  function keepScreenOn(log) {
    const wl = navigator.wakeLock;
    if (!wl) { log('[page] no Wake Lock in this browser: the screen may dim while you play'); return; }
    let lock = null, asking = false, said = false;
    const take = () => {
      if (asking || lock || document.hidden) return;
      asking = true;
      wl.request('screen').then(l => {
        lock = l;
        l.addEventListener('release', () => { if (lock === l) lock = null; });
        if (!said) { said = true; log('[page] the screen stays on while the game runs (Wake Lock)'); }
      }, e => { if (!said) { said = true; log('[page] Wake Lock refused: ' + e); } }).finally(() => { asking = false; });
    };
    document.addEventListener('visibilitychange', take);
    take();
  }

  /** no pinch zoom on an iPhone while playing: Safari ignores user-scalable and touch-action for a pinch, but
   *  cancelling its gesture events stops it */
  function noPinchZoom() {
    for (const t of ['gesturestart', 'gesturechange'])
      document.addEventListener(t, e => { if (document.body.classList.contains('playing')) e.preventDefault(); }, { passive: false });
  }

  window.MC3Touch = { attach, enterGame, fullscreen, canFullscreen, keepScreenOn, noPinchZoom, isPhone, touchFirst, hasTouch };
})();
