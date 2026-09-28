/* Touchscreen controls overlay (GBA-style pad for phones and tablets).
 *
 * Why: a guest opening a magic link on their phone has no keyboard or
 * gamepad — without this they can watch but never play. The overlay is a
 * transparent layer with a D-pad, A/B, L/R shoulders, START/SELECT, and a
 * small pause button; it feeds the SAME per-player key-mask pipeline the
 * keyboard and gamepads use (main.js merges `mgbaTouch.maskFor(p)` into
 * sendKeys), so host/local play and guest input forwarding both work
 * unchanged.
 *
 * Multi-touch: every pointer is tracked independently (press A while holding
 * a direction), and the D-pad supports sliding between directions without
 * lifting the thumb — the natural way people play GBA games.
 *
 * Visibility modes (persisted): 'auto' (default) shows on the first real
 * touch and when a magic-link guest connects; 'on'/'off' force it. Desktops
 * never see it in auto mode.
 */
(() => {
  // Must match GBA_BUTTONS in main.js (mGBA's key mask layout).
  const BUTTON_BITS = {
    A: 1 << 0, B: 1 << 1, SELECT: 1 << 2, START: 1 << 3,
    RIGHT: 1 << 4, LEFT: 1 << 5, UP: 1 << 6, DOWN: 1 << 7,
    R: 1 << 8, L: 1 << 9,
  };

  const MODE_KEY = 'mgba-splitscreen_touch_v1';
  const MAX_PLAYERS = 4;

  const state = {
    root: null,
    note: null,
    picker: null,
    callbacks: {},
    masks: new Array(MAX_PLAYERS).fill(0),
    activePlayer: 0,
    playerCount: 2,
    pointers: new Map(), // pointerId -> button name currently held
    mode: 'auto',        // 'auto' | 'on' | 'off'
    touchSeen: false,    // a real touch input happened at least once
    guestSeat: null,     // set when the magic-link guest is welcomed
    awaitingGuest: false, // ?online=join seen but welcome not yet received
  };

  function loadMode() {
    try {
      const raw = localStorage.getItem(MODE_KEY);
      if (raw === 'on' || raw === 'off' || raw === 'auto') state.mode = raw;
    } catch (_) {}
  }

  function saveMode() {
    try { localStorage.setItem(MODE_KEY, state.mode); } catch (_) {}
  }

  function query(name) {
    try { return new URLSearchParams(window.location.search).get(name); } catch (_) { return null; }
  }

  function shouldShow() {
    if (state.mode === 'on') return true;
    if (state.mode === 'off') return false;
    return state.touchSeen || state.guestSeat !== null
      || (state.awaitingGuest && state.touchSeen);
  }

  function updateVisibility() {
    if (!state.root) return;
    state.root.hidden = !shouldShow();
  }

  function updatePicker() {
    if (!state.picker) return;
    if (state.guestSeat !== null) {
      // Guests drive exactly one seat; the picker is meaningless to them.
      state.picker.hidden = true;
      if (state.note) state.note.textContent = `You are Player ${state.guestSeat + 1}`;
      return;
    }
    state.picker.hidden = state.playerCount <= 1;
    state.picker.querySelectorAll('button').forEach((btn) => {
      const p = parseInt(btn.dataset.player, 10);
      btn.classList.toggle('active', p === state.activePlayer);
      btn.style.display = p < state.playerCount ? '' : 'none';
    });
    if (state.note) state.note.textContent = '';
  }

  function sendMask() {
    // Route through main.js's union sender so keyboard/gamepad masks survive.
    state.callbacks.onInput?.(state.activePlayer);
  }

  function pressedVisual(btn, on) {
    const el = state.root && state.root.querySelector(`[data-btn="${btn}"]`);
    if (el) el.classList.toggle('pressed', on);
  }

  function press(btn) {
    const bit = BUTTON_BITS[btn];
    if (!bit || (state.masks[state.activePlayer] & bit)) return;
    // Guests must not inject input before the host assigns their seat —
    // seat 0 is Player 1 on the host, and an unassigned guest is nobody.
    if (state.awaitingGuest && state.guestSeat === null) return;
    state.masks[state.activePlayer] |= bit;
    pressedVisual(btn, true);
    try { navigator.vibrate && navigator.vibrate(8); } catch (_) {}
    sendMask();
  }

  function release(btn) {
    const bit = BUTTON_BITS[btn];
    if (!bit || !(state.masks[state.activePlayer] & bit)) { pressedVisual(btn, false); return; }
    state.masks[state.activePlayer] &= ~bit;
    pressedVisual(btn, false);
    sendMask();
  }

  function buttonAt(x, y) {
    const el = document.elementFromPoint(x, y);
    return el && el.closest ? el.closest('[data-btn]') : null;
  }

  function onPointerDown(e) {
    if (!state.root || state.root.hidden) return;
    const target = e.target.closest ? e.target.closest('[data-btn]') : null;
    if (!target) return;
    e.preventDefault();
    try { state.root.setPointerCapture(e.pointerId); } catch (_) {}
    state.pointers.set(e.pointerId, target.dataset.btn);
    press(target.dataset.btn);
  }

  function onPointerMove(e) {
    if (!state.pointers.has(e.pointerId)) return;
    e.preventDefault();
    const hit = buttonAt(e.clientX, e.clientY);
    const btn = hit ? hit.dataset.btn : null;
    const current = state.pointers.get(e.pointerId);
    if (btn === current) return;
    // Slide support: release the old button, press the new one under the
    // finger (D-pad rolls between directions; lifting off releases all).
    if (current) release(current);
    if (btn) press(btn);
    state.pointers.set(e.pointerId, btn);
  }

  function onPointerUp(e) {
    if (!state.pointers.has(e.pointerId)) return;
    e.preventDefault();
    const btn = state.pointers.get(e.pointerId);
    state.pointers.delete(e.pointerId);
    if (btn) release(btn);
  }

  function bindEvents() {
    state.root.addEventListener('pointerdown', onPointerDown);
    state.root.addEventListener('pointermove', onPointerMove);
    state.root.addEventListener('pointerup', onPointerUp);
    state.root.addEventListener('pointercancel', onPointerUp);
    // A long-press must never open the text-selection/callout UI mid-game.
    state.root.addEventListener('contextmenu', (e) => e.preventDefault());
    // Pause button (tiny, top-center): same hotkey path as the menubar.
    const pauseBtn = state.root.querySelector('[data-action="pause"]');
    if (pauseBtn) {
      pauseBtn.addEventListener('pointerdown', (e) => e.preventDefault());
      pauseBtn.addEventListener('click', () => state.callbacks.onPause?.());
    }
    if (state.picker) {
      state.picker.addEventListener('click', (e) => {
        const btn = e.target.closest('button[data-player]');
        if (!btn) return;
        const p = parseInt(btn.dataset.player, 10);
        if (p >= 0 && p < state.playerCount && p !== state.activePlayer) {
          // Don't let a held button stick on the old player.
          for (const [id, b] of state.pointers) { if (b) release(b); }
          state.masks.fill(0);
          state.activePlayer = p;
          updatePicker();
          sendMask();
        }
      });
    }
    // Auto-show on the first REAL touch (desktop mice never trigger this).
    window.addEventListener('pointerdown', (e) => {
      if (e.pointerType === 'touch' || e.pointerType === 'pen') {
        if (!state.touchSeen) {
          state.touchSeen = true;
          updateVisibility();
        }
      }
    }, { capture: true, passive: true });
  }

  function setModeLabel() {
    const btn = document.getElementById('toggle-touch');
    if (btn) {
      const label = state.mode === 'auto' ? 'Auto' : state.mode === 'on' ? 'On' : 'Off';
      btn.textContent = `Touch Controls: ${label}`;
      btn.classList.toggle('active', state.mode !== 'off');
    }
  }

  window.mgbaTouch = {
    init(callbacks) {
      state.callbacks = callbacks || {};
      state.root = document.getElementById('touch-controls');
      state.note = document.getElementById('touch-note');
      state.picker = document.getElementById('touch-player-picker');
      if (!state.root) return;
      loadMode();
      state.awaitingGuest = query('online') === 'join';
      // Build the P1-P4 picker once (local play only; guests never see it).
      if (state.picker && !state.picker.children.length) {
        for (let p = 0; p < MAX_PLAYERS; p++) {
          const b = document.createElement('button');
          b.type = 'button';
          b.dataset.player = String(p);
          b.textContent = `P${p + 1}`;
          state.picker.appendChild(b);
        }
      }
      bindEvents();
      setModeLabel();
      updatePicker();
      updateVisibility();
    },
    // main.js calls this when the player count changes (screens rebuilt).
    setPlayerCount(count) {
      const n = Math.max(1, Math.min(MAX_PLAYERS, count | 0));
      if (n === state.playerCount) return;
      state.playerCount = n;
      state.masks.fill(0);
      // Never clamp an assigned guest down to the local count: guests build a
      // 1-screen view but drive a seat the host picked (0..3). Only local
      // players follow the local grid size.
      if (state.guestSeat === null) {
        state.activePlayer = Math.min(state.activePlayer, n - 1);
      }
      updatePicker();
    },
    // main.js calls this when an online session role resolves:
    // seat = 0-based guest slot, or null for hosts/local play.
    setGuest(seat) {
      state.guestSeat = (Number.isInteger(seat) && seat >= 0) ? seat : null;
      if (state.guestSeat !== null) {
        state.activePlayer = state.guestSeat;
        state.masks.fill(0);
      }
      updatePicker();
      updateVisibility(); // magic-link arrival (or host fallback) re-evaluates
    },
    cycleMode() {
      state.mode = state.mode === 'auto' ? 'on' : state.mode === 'on' ? 'off' : 'auto';
      saveMode();
      setModeLabel();
      updateVisibility();
      state.callbacks.onStatus?.(
        state.mode === 'auto' ? 'Touch controls: auto (shows on touch devices and guests)'
          : state.mode === 'on' ? 'Touch controls: always shown'
          : 'Touch controls: hidden');
    },
    maskFor(player) {
      return state.masks[player] || 0;
    },
    releaseAll() {
      state.masks.fill(0);
      state.pointers.clear();
      if (!state.root) return;
      state.root.querySelectorAll('.touch-btn.pressed').forEach((el) => el.classList.remove('pressed'));
    },
  };
})();
