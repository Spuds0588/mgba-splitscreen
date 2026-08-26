const IS_TAURI = typeof window.__TAURI__ !== 'undefined';
// Tauri v2 exposes commands as window.__TAURI__.core.invoke; grab it once so the
// keyboard path doesn't rely on an undefined bare `invoke`.
const invoke = IS_TAURI ? window.__TAURI__.core.invoke : null;

const GBA_WIDTH = 240;
const GBA_HEIGHT = 160;
// RGBA8888: the backend sends frames already in the format putImageData wants,
// so the frontend copies each frame straight into a preallocated ImageData with
// no per-pixel decode work.
const FRAME_SIZE = GBA_WIDTH * GBA_HEIGHT * 4;

const GBA_BUTTONS = {
  A: 1 << 0,
  B: 1 << 1,
  SELECT: 1 << 2,
  START: 1 << 3,
  RIGHT: 1 << 4,
  LEFT: 1 << 5,
  UP: 1 << 6,
  DOWN: 1 << 7,
  R: 1 << 8,
  L: 1 << 9,
};

const P1_MAP = {
  'KeyW': GBA_BUTTONS.UP,
  'KeyS': GBA_BUTTONS.DOWN,
  'KeyA': GBA_BUTTONS.LEFT,
  'KeyD': GBA_BUTTONS.RIGHT,
  'KeyK': GBA_BUTTONS.A,
  'KeyJ': GBA_BUTTONS.B,
  'KeyL': GBA_BUTTONS.R,
  'KeyH': GBA_BUTTONS.L,
  'Space': GBA_BUTTONS.START,
  'Backspace': GBA_BUTTONS.SELECT,
};

const P2_MAP = {
  'ArrowUp': GBA_BUTTONS.UP,
  'ArrowDown': GBA_BUTTONS.DOWN,
  'ArrowLeft': GBA_BUTTONS.LEFT,
  'ArrowRight': GBA_BUTTONS.RIGHT,
  'KeyM': GBA_BUTTONS.A,
  'KeyN': GBA_BUTTONS.B,
  'KeyV': GBA_BUTTONS.L,
  'KeyB': GBA_BUTTONS.R,
  'Enter': GBA_BUTTONS.START,
  'KeyO': GBA_BUTTONS.SELECT,
};

// P3/P4 maps: one keyboard, four players, no overlap with P1/P2 bindings.
// P1 uses W/S/A/D + K/J/L/H; P2 uses arrows + M/N/V/B + Enter/O; P3/P4 use the
// T/G/F/R + Y/U + digits and I/Q/C/E + P/brackets/comma/period clusters.
const P3_MAP = {
  'KeyT': GBA_BUTTONS.UP,
  'KeyG': GBA_BUTTONS.DOWN,
  'KeyF': GBA_BUTTONS.LEFT,
  'KeyR': GBA_BUTTONS.RIGHT,
  'KeyY': GBA_BUTTONS.A,
  'KeyU': GBA_BUTTONS.B,
  'KeyZ': GBA_BUTTONS.L,
  'KeyX': GBA_BUTTONS.R,
  'Digit1': GBA_BUTTONS.START,
  'Digit2': GBA_BUTTONS.SELECT,
};

const P4_MAP = {
  'KeyI': GBA_BUTTONS.UP,
  'KeyQ': GBA_BUTTONS.DOWN,
  'KeyC': GBA_BUTTONS.LEFT,
  'KeyE': GBA_BUTTONS.RIGHT,
  'KeyP': GBA_BUTTONS.A,
  'BracketLeft': GBA_BUTTONS.B,
  'Comma': GBA_BUTTONS.L,
  'Period': GBA_BUTTONS.R,
  'Digit3': GBA_BUTTONS.START,
  'Digit4': GBA_BUTTONS.SELECT,
};

// ---- Controls (per-player, persisted to localStorage) ----
// Keyboard defaults come from the P1-P4 maps above. Gamepads are assigned by
// controller slot (controller #1 -> P1, #2 -> P2, ...) using the standard
// mapping: face buttons, LB/RB, Select/Start, and the D-pad; the left stick
// maps to movement through the axes (not remappable, kept as a bonus).
const DEFAULT_KEYBOARD_MAPS = [P1_MAP, P2_MAP, P3_MAP, P4_MAP];

const DEFAULT_GAMEPAD_BUTTONS = {
  0: GBA_BUTTONS.A,
  1: GBA_BUTTONS.B,
  // L/R default to the shoulder controls: both bumpers (LB/RB) and analog
  // triggers (LT/RT) map to L/R so the layout works on any controller. X/Y
  // (buttons 2/3) are left unbound by default so they're free for remapping.
  4: GBA_BUTTONS.L, // LB bumper -> L
  5: GBA_BUTTONS.R, // RB bumper -> R
  6: GBA_BUTTONS.L, // LT trigger -> L (alternate)
  7: GBA_BUTTONS.R, // RT trigger -> R (alternate)
  8: GBA_BUTTONS.SELECT,
  9: GBA_BUTTONS.START,
  12: GBA_BUTTONS.UP,
  13: GBA_BUTTONS.DOWN,
  14: GBA_BUTTONS.LEFT,
  15: GBA_BUTTONS.RIGHT,
};

const DEFAULT_GAMEPAD_AXES = {
  0: { neg: GBA_BUTTONS.LEFT, pos: GBA_BUTTONS.RIGHT }, // left stick X
  1: { neg: GBA_BUTTONS.UP, pos: GBA_BUTTONS.DOWN }, // left stick Y (up = -1)
};

const CONTROLS_KEY = 'dualboy_controls_v2'; // v2: L/R default to bumpers/triggers
let controls = []; // 4 entries: { keyboard, gamepadButtons, gamepadAxes }

function defaultControlFor(p) {
  return {
    keyboard: { ...DEFAULT_KEYBOARD_MAPS[p] },
    gamepadButtons: { ...DEFAULT_GAMEPAD_BUTTONS },
    gamepadAxes: { ...DEFAULT_GAMEPAD_AXES },
  };
}

function loadControls() {
  controls = [];
  for (let p = 0; p < 4; p++) controls.push(defaultControlFor(p));
  try {
    const raw = localStorage.getItem(CONTROLS_KEY);
    if (!raw) return;
    const saved = JSON.parse(raw);
    for (let p = 0; p < 4; p++) {
      const s = saved[p];
      if (!s) continue;
      controls[p] = {
        keyboard: { ...controls[p].keyboard, ...(s.keyboard || {}) },
        gamepadButtons: { ...controls[p].gamepadButtons, ...(s.gamepadButtons || {}) },
        gamepadAxes: { ...controls[p].gamepadAxes, ...(s.gamepadAxes || {}) },
      };
    }
  } catch (e) {
    // Corrupt storage: keep the defaults.
  }
}

function saveControls() {
  try {
    localStorage.setItem(CONTROLS_KEY, JSON.stringify(controls));
  } catch (e) {
    // localStorage unavailable (private mode): remaps just won't persist.
  }
}

// Pretty names + helpers for the remap UI.
const REMAP_ACTIONS = [
  { name: 'A', bit: GBA_BUTTONS.A },
  { name: 'B', bit: GBA_BUTTONS.B },
  { name: 'L', bit: GBA_BUTTONS.L },
  { name: 'R', bit: GBA_BUTTONS.R },
  { name: 'START', bit: GBA_BUTTONS.START },
  { name: 'SELECT', bit: GBA_BUTTONS.SELECT },
  { name: 'UP', bit: GBA_BUTTONS.UP },
  { name: 'DOWN', bit: GBA_BUTTONS.DOWN },
  { name: 'LEFT', bit: GBA_BUTTONS.LEFT },
  { name: 'RIGHT', bit: GBA_BUTTONS.RIGHT },
];

const PAD_BUTTON_NAMES = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'Select', 'Start', 'L3', 'R3', 'D-Up', 'D-Down', 'D-Left', 'D-Right'];

function formatKeyCode(code) {
  if (code.startsWith('Key')) return code.slice(3);
  if (code.startsWith('Digit')) return code.slice(5);
  const names = {
    ArrowUp: '\u2191', ArrowDown: '\u2193', ArrowLeft: '\u2190', ArrowRight: '\u2192',
    Space: 'Space', Enter: 'Enter', Backspace: 'Bksp',
    Comma: ',', Period: '.', BracketLeft: '[', BracketRight: ']',
  };
  return names[code] || code;
}

function formatPadButton(idx) {
  return PAD_BUTTON_NAMES[idx] ? `${PAD_BUTTON_NAMES[idx]} (${idx})` : `Btn ${idx}`;
}

function keyForBit(map, bit) {
  for (const code in map) if (map[code] === bit) return formatKeyCode(code);
  return null;
}

function padButtonForBit(map, bit) {
  for (const idx in map) if (map[idx] === bit) return formatPadButton(parseInt(idx, 10));
  return null;
}

// ---- Global hotkeys (turbo / quick save / quick load / pause) ----
// Global (not per-player) system actions. Keyboard keys plus optional gamepad
// buttons, persisted independently of the per-player control maps.
const HOTKEYS_KEY = 'dualboy_hotkeys_v1';

const HOTKEY_ACTIONS = [
  { id: 'turbo', label: 'Turbo' },
  { id: 'save', label: 'Quick Save State' },
  { id: 'load', label: 'Quick Load State' },
  { id: 'pause', label: 'Pause' },
  { id: 'cycle_view', label: 'Cycle View Mode' },
  { id: 'cycle_focus', label: 'Cycle Focus Player' },
];

function defaultHotkeys() {
  return {
    turbo: { keyboard: 'Tab', gamepad: null },
    save: { keyboard: 'F5', gamepad: null },
    load: { keyboard: 'F7', gamepad: null },
    pause: { keyboard: 'Escape', gamepad: null },
    cycle_view: { keyboard: 'F8', gamepad: null },
    cycle_focus: { keyboard: 'F9', gamepad: null },
  };
}

let hotkeys = defaultHotkeys();

function loadHotkeys() {
  hotkeys = defaultHotkeys();
  try {
    const raw = localStorage.getItem(HOTKEYS_KEY);
    if (!raw) return;
    const saved = JSON.parse(raw);
    for (const k in hotkeys) {
      if (saved[k] && typeof saved[k] === 'object') {
        hotkeys[k] = { ...hotkeys[k], ...saved[k] };
      }
    }
  } catch (e) {
    // Corrupt storage: keep the defaults.
  }
}

function saveHotkeys() {
  try {
    localStorage.setItem(HOTKEYS_KEY, JSON.stringify(hotkeys));
  } catch (e) {
    // localStorage unavailable (private mode): remaps just won't persist.
  }
  refreshHotkeyLabels();
  refreshFocusLabel();
}

// Pretty name(s) for a hotkey's current binding(s).
function hotkeyLabel(id) {
  const hk = hotkeys[id];
  const parts = [];
  if (hk.keyboard) parts.push(formatKeyCode(hk.keyboard));
  if (hk.gamepad !== null && hk.gamepad !== undefined) parts.push(formatPadButton(hk.gamepad));
  return parts.length ? parts.join(' / ') : 'unbound';
}

// Keep the menu buttons' "(F5)"-style hints in sync with the live bindings.
function refreshHotkeyLabels() {
  const map = {
    turbo: '#toggle-turbo',
    save: '#quick-save-state',
    load: '#quick-load-state',
    pause: '#toggle-pause',
    cycle_view: '#cycle-view',
  };
  for (const id in map) {
    const el = document.querySelector(map[id]);
    if (!el) continue;
    const action = HOTKEY_ACTIONS.find((a) => a.id === id);
    el.textContent = `${action.label} (${hotkeyLabel(id)})`;
  }
}

// Player color coding (like name borders on a video call): P1 red, P2 blue,
// P3 green, P4 orange. Same palette for the border and the bottom-right tag.
const PLAYER_TAGS = ['player-1', 'player-2', 'player-3', 'player-4'];

const OVERLAY_MAX = 6;
let playerCount = 2;
let screens = []; // { canvas, ctx, imgData }
let keyStates = []; // keyboard-derived mask per player
let padStates = []; // gamepad-derived mask per player
let socket = null;
let turboOn = false;
let paused = false;
let debugOn = false; // Debug overlay is off by default (View menu toggles it).
// View layout: 'grid' | 'speaker' (1 big + smalls) | 'focus' (single screen) |
// 'overlay' (one tile floating enlarged on top of the grid). focusPlayer selects
// which player's screen is enlarged/shown in speaker/focus/overlay modes.
let viewMode = 'grid';
let focusPlayer = 0;

// ---- In-browser engine (fully client-side, no backend) ----
// When no dualboy-web backend is reachable (e.g. GitHub Pages), the mGBA core
// compiled to WebAssembly runs right here in the page: all N linked instances
// step cooperatively on the JS thread through the same lockstep coordinator
// the desktop app uses. Every backend call below branches on `wasmMode`.
let wasmMode = false;
let wasmModule = null;     // emscripten module (Module["_db_*"] + HEAPU8)
let wasmStates = [];       // per-player quick-save blobs (Uint8Array)
let wasmLoopId = 0;        // requestAnimationFrame id
let wasmLast = 0;          // last rAF timestamp
let wasmAccum = 0;         // fixed-timestep accumulator (ms) for 60fps pacing
const FRAME_MS = 1000 / 60;

function setStatus(text) {
  document.getElementById('status').textContent = text;
}

// ---- Menu bar ----

function closeMenus() {
  document.querySelectorAll('.menu[open]').forEach((m) => m.removeAttribute('open'));
  if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
}

function menuOpen() {
  return !!document.querySelector('.menu[open]');
}

// Close any open menu on an outside click; the per-action handlers below call
// closeMenus() themselves and blur, so keyboard focus never lingers on a menu
// button and steals Enter/Space from the emulator.
document.addEventListener('click', (e) => {
  if (!e.target.closest('.menu')) closeMenus();
});

// Only one menu open at a time: opening one closes its siblings (mouse or
// keyboard). Fixes the overlap bug where clicking a second menu without
// dismissing the first left both open.
document.querySelectorAll('.menu').forEach((m) => {
  m.addEventListener('toggle', () => {
    if (!m.open) return;
    document.querySelectorAll('.menu[open]').forEach((other) => {
      if (other !== m) other.removeAttribute('open');
    });
  });
});

// ---- Turbo / fast-forward ----

function highlightTurbo(on) {
  const btn = document.getElementById('toggle-turbo');
  if (btn) btn.classList.toggle('active', on);
}

async function toggleTurbo() {
  turboOn = !turboOn;
  if (wasmMode) {
    // The frame loop honors the flag; flush buffered audio so fast-forwarded
    // sound doesn't burst out when turbo is released.
    if (!turboOn) wasmFlushAudio();
  } else if (IS_TAURI) {
    await invoke('set_turbo', { enabled: turboOn });
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'turbo', enabled: turboOn }));
  }
  highlightTurbo(turboOn);
  setStatus(turboOn ? `TURBO ON — fast-forwarding (${hotkeyLabel('turbo')})` : `Turbo off (${hotkeyLabel('turbo')})`);
}

// ---- Pause (all players together) ----

function highlightPause(on) {
  const btn = document.getElementById('toggle-pause');
  if (btn) btn.classList.toggle('active', on);
}

async function togglePause() {
  paused = !paused;
  if (wasmMode) {
    // The frame loop freezes all cores while paused; nothing to send.
  } else if (IS_TAURI) {
    await invoke('set_paused', { paused });
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'pause', paused }));
  }
  highlightPause(paused);
  if (paused) openPauseMenu();
  else closePauseMenu();
  setStatus(paused ? `PAUSED — all players frozen (${hotkeyLabel('pause')})` : 'Resumed');
}

// Dispatch a global hotkey id (shared by the keyboard and gamepad paths).
function triggerHotkey(id) {
  if (id === 'turbo') toggleTurbo();
  else if (id === 'save') quickSaveState();
  else if (id === 'load') quickLoadState();
  else if (id === 'pause') togglePause();
  else if (id === 'cycle_view') cycleViewMode();
  else if (id === 'cycle_focus') cycleFocusPlayer();
}

// The backend recreates the emulator (and resets turbo/pause to off) on
// quit-game and player-count changes; keep the frontend flags in lockstep.
function resetRuntimeState() {
  turboOn = false;
  highlightTurbo(false);
  paused = false;
  highlightPause(false);
  if (pauseOpen()) closePauseMenu();
}

// ---- Audio routing ----
// The core mixes each game's music + SFX into ONE stereo stream per instance;
// there is no music-vs-SFX split at the core level. These options pick WHOSE
// mix you hear (default Player 1) or blend all players together.

let audioSource = 1;

function highlightAudio(n) {
  document.querySelectorAll('#audio-menu button').forEach((btn) => {
    btn.classList.toggle('active', parseInt(btn.dataset.audio, 10) === n);
  });
}

async function setAudioSource(n) {
  audioSource = n;
  if (wasmMode) {
    wasmModule._db_set_audio_source(n);
    if (n === 0) wasmFlushAudio();
  } else if (IS_TAURI) {
    await invoke('set_audio_source', { source: n });
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'audio_source', source: n }));
  }
  highlightAudio(n);
  const label = n === 0 ? 'Muted' : n === 5 ? 'Mix all players' : `Player ${n}`;
  setStatus(`Audio: ${label}`);
}

// ---- Quit game (unload ROM, keep the app open) ----

function clearScreens() {
  for (const s of screens) {
    s.ctx.fillStyle = '#000';
    s.ctx.fillRect(0, 0, GBA_WIDTH, GBA_HEIGHT);
  }
}

async function quitGame() {
  closeMenus();
  if (wasmMode) {
    wasmModule._db_quit();
    wasmStates = [];
    // Re-create empty cores so the loop keeps idling cheaply; the next ROM
    // load re-arms them (mirrors the backend's recreate-on-quit).
    wasmModule._db_init(playerCount);
  } else if (IS_TAURI) {
    await invoke('quit_game');
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'quit_game' }));
  }
  resetRuntimeState();
  clearScreens();
  setStatus('Waiting for ROM\u2026');
  // Back to the launcher so the next game is one D-pad+A away.
  openLibrary();
}

// ---- Save state (quick save/load, all players together) ----

async function quickSaveState() {
  closeMenus();
  if (wasmMode) {
    wasmStates = [];
    const M = wasmModule;
    let ok = playerCount > 0;
    for (let i = 0; i < playerCount; i++) {
      const sz = M._db_save_state(i);
      if (!sz) { ok = false; break; }
      const ptr = M._db_state_ptr();
      wasmStates.push(new Uint8Array(M.HEAPU8.slice(ptr, ptr + sz)));
    }
    setStatus(ok ? `Quick state saved (${wasmStates.length} player${wasmStates.length === 1 ? '' : 's'})` : 'Save state failed');
    return;
  }
  if (IS_TAURI) {
    try {
      await invoke('save_state');
      setStatus('Quick state saved (F5)');
    } catch (err) {
      setStatus('Save state failed: ' + err);
    }
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'save_state' }));
    setStatus('Quick state saved (F5)');
  }
}

async function quickLoadState() {
  closeMenus();
  if (wasmMode) {
    const M = wasmModule;
    if (!wasmStates.length) { setStatus('No quick state to load'); return; }
    let ok = true;
    for (let i = 0; i < Math.min(playerCount, wasmStates.length); i++) {
      const blob = wasmStates[i];
      const ptr = M._malloc(blob.length);
      if (!ptr) { ok = false; break; }
      M.HEAPU8.set(blob, ptr);
      const rc = M._db_load_state_bytes(i, ptr, blob.length);
      M._free(ptr);
      if (rc !== 0) { ok = false; break; }
    }
    setStatus(ok ? 'Quick state loaded (F7)' : 'Load state failed');
    return;
  }
  if (IS_TAURI) {
    try {
      await invoke('load_state');
      setStatus('Quick state loaded (F7)');
    } catch (err) {
      setStatus('Load state failed: ' + err);
    }
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'load_state' }));
    setStatus('Quick state loaded (F7)');
  }
}

// ---- Screen layout + view modes (video-call style) ----
// Four arrangements, mirroring video-call platforms:
//   grid    — all screens equal (default)
//   speaker — one big screen on top, the rest in a strip underneath
//   focus   — only one screen visible (the others still emulate behind it)
//   overlay — equal grid behind, with one screen floating enlarged on top (PiP)
const VIEW_MODES = [
  { id: 'grid', label: 'Grid' },
  { id: 'speaker', label: 'Speaker (1 big + small)' },
  { id: 'focus', label: 'Focus (single screen)' },
  { id: 'overlay', label: 'Overlay (PiP)' },
];
const VIEW_KEY = 'dualboy_view_v1';
const BG_KEY = 'dualboy_background_v1';
const OUTLINES_KEY = 'dualboy_outlines_v1';
let outlinesOn = true; // Colored per-player borders around each screen

function layout() {
  const el = document.getElementById('screens');
  const count = playerCount;
  const wide = window.innerWidth >= window.innerHeight;

  el.dataset.view = viewMode;

  if (viewMode === 'grid') {
    let cols, rows;
    if (count === 1) { cols = 1; rows = 1; } // Single player: full width.
    else if (count === 2) { cols = wide ? 2 : 1; rows = wide ? 1 : 2; }
    else { cols = 2; rows = 2; } // 3 players sit in the 4-slot grid, 4th empty.
    el.style.gridTemplateColumns = `repeat(${cols}, minmax(0, 1fr))`;
    el.style.gridTemplateRows = `repeat(${rows}, minmax(0, 1fr))`;
  } else if (viewMode === 'speaker') {
    el.style.gridTemplateColumns = `repeat(${Math.max(1, count - 1)}, minmax(0, 1fr))`;
    el.style.gridTemplateRows = '3fr 1fr';
  } else if (viewMode === 'focus') {
    el.style.gridTemplateColumns = '1fr';
    el.style.gridTemplateRows = '1fr';
  } else if (viewMode === 'overlay') {
    // The rest of the screens fill the grid; the focused one floats on top (CSS).
    el.style.gridTemplateColumns = `repeat(${Math.max(1, count - 1)}, minmax(0, 1fr))`;
    el.style.gridTemplateRows = '1fr';
  }

  // Focus mode hides every non-focused tile (they still emulate behind it).
  for (let i = 0; i < screens.length; i++) {
    const cell = document.getElementById('screens').children[i];
    if (!cell) continue;
    cell.style.display = (viewMode === 'focus' && i !== focusPlayer) ? 'none' : '';
  }
}

function refreshFocusLabel() {
  const btn = document.getElementById('cycle-focus');
  if (btn) btn.textContent = `Focus: P${focusPlayer + 1} (${hotkeyLabel('cycle_focus')})`;
}

function applyViewMode() {
  const container = document.getElementById('screens');
  container.dataset.view = viewMode;
  document.querySelectorAll('#view-menu [data-view]').forEach((b) => {
    b.classList.toggle('active', b.dataset.view === viewMode);
  });
  document.querySelectorAll('.screen-cell').forEach((c, i) => {
    c.classList.toggle('focused-tile', i === focusPlayer);
  });
  layout();
  refreshFocusLabel();
  saveViewPrefs();
}

function saveViewPrefs() {
  try {
    localStorage.setItem(VIEW_KEY, JSON.stringify({ mode: viewMode, focus: focusPlayer }));
  } catch (e) {}
}

function loadViewPrefs() {
  try {
    const raw = localStorage.getItem(VIEW_KEY);
    if (!raw) return;
    const v = JSON.parse(raw);
    if (VIEW_MODES.some((m) => m.id === v.mode)) viewMode = v.mode;
    if (Number.isInteger(v.focus) && v.focus >= 0) focusPlayer = v.focus;
  } catch (e) {}
}

function setViewMode(mode) {
  const m = VIEW_MODES.find((x) => x.id === mode);
  if (!m) return;
  viewMode = m.id;
  applyViewMode();
  setStatus(`View: ${m.label}`);
}

function cycleViewMode() {
  const idx = VIEW_MODES.findIndex((m) => m.id === viewMode);
  setViewMode(VIEW_MODES[(idx + 1) % VIEW_MODES.length].id);
}

function cycleFocusPlayer() {
  focusPlayer = (focusPlayer + 1) % Math.max(1, playerCount);
  applyViewMode();
  setStatus(`Focus: P${focusPlayer + 1}`);
}

// ---- Image background (wallpaper behind the tiles) ----

function setBackground(dataUrl) {
  const el = document.getElementById('screens');
  if (dataUrl) {
    el.style.backgroundImage = `url("${dataUrl}")`;
    el.style.backgroundSize = 'cover';
    el.style.backgroundPosition = 'center';
    try { localStorage.setItem(BG_KEY, dataUrl); } catch (e) {}
  } else {
    el.style.backgroundImage = '';
    try { localStorage.removeItem(BG_KEY); } catch (e) {}
  }
}

function loadBackground() {
  try {
    const data = localStorage.getItem(BG_KEY);
    if (data) setBackground(data);
  } catch (e) {}
}

function pickBackground() {
  closeMenus();
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = 'image/*';
  input.onchange = () => {
    const f = input.files && input.files[0];
    if (!f) return;
    const r = new FileReader();
    r.onload = () => { setBackground(r.result); setStatus('Background image set'); };
    r.onerror = () => setStatus('Failed to read image');
    r.readAsDataURL(f);
  };
  input.click();
}

function clearBackground() {
  closeMenus();
  setBackground(null);
  setStatus('Background cleared');
}

// ---- Player outline toggle ----
// The colored per-player borders around each screen are on by default; some
// setups (splitscreen capture, projector) want a clean grid without them.
function applyOutlines() {
  const container = document.getElementById('screens');
  if (container) container.classList.toggle('no-outlines', !outlinesOn);
  const btn = document.getElementById('toggle-outlines');
  if (btn) {
    btn.textContent = `Player Outlines: ${outlinesOn ? 'On' : 'Off'}`;
    btn.classList.toggle('active', outlinesOn);
  }
}

function toggleOutlines() {
  outlinesOn = !outlinesOn;
  try { localStorage.setItem(OUTLINES_KEY, JSON.stringify(outlinesOn)); } catch (e) {}
  applyOutlines();
  setStatus(outlinesOn ? 'Player outlines on' : 'Player outlines off');
}

function loadOutlinesPref() {
  try {
    const raw = localStorage.getItem(OUTLINES_KEY);
    if (raw !== null) outlinesOn = JSON.parse(raw) === true;
  } catch (e) {}
  applyOutlines();
}

function initScreens(count) {
  playerCount = count;
  focusPlayer = Math.min(focusPlayer, Math.max(0, count - 1));
  const container = document.getElementById('screens');
  container.innerHTML = '';
  screens = [];
  keyStates = new Array(count).fill(0);
  padStates = new Array(count).fill(0);

  const saveMenu = document.getElementById('save-menu');
  saveMenu.innerHTML = '';

  for (let i = 0; i < count; i++) {
    const cell = document.createElement('div');
    cell.className = 'screen-cell ' + (PLAYER_TAGS[i] || '');

    const label = document.createElement('div');
    label.className = 'screen-label';
    label.textContent = `P${i + 1}`;

    // Bottom-right tag, colored to match the tile border.
    const tag = document.createElement('div');
    tag.className = 'screen-tag ' + (PLAYER_TAGS[i] || '');
    tag.textContent = `P${i + 1}`;

    const canvas = document.createElement('canvas');
    canvas.width = GBA_WIDTH;
    canvas.height = GBA_HEIGHT;

    cell.appendChild(canvas);
    cell.appendChild(label);
    cell.appendChild(tag);
    container.appendChild(cell);

    const ctx = canvas.getContext('2d');
    screens.push({ canvas, ctx, imgData: ctx.createImageData(GBA_WIDTH, GBA_HEIGHT) });

    const exportBtn = document.createElement('button');
    exportBtn.textContent = `Export Save P${i + 1}\u2026`;
    exportBtn.addEventListener('click', () => {
      closeMenus();
      IS_TAURI ? exportSaveTauri(i + 1) : exportSaveBrowser(i + 1);
    });

    const importBtn = document.createElement('button');
    importBtn.textContent = `Import Save P${i + 1}\u2026`;
    importBtn.addEventListener('click', () => {
      closeMenus();
      IS_TAURI ? importSaveTauri(i + 1) : importSaveBrowser(i + 1);
    });

    saveMenu.appendChild(exportBtn);
    saveMenu.appendChild(importBtn);
  }

  applyViewMode();
}

// The backend broadcasts every emulated frame at 60 FPS and never blocks on us.
// Rendering is coalesced to the display refresh rate with requestAnimationFrame
// and always shows the LATEST frame: if the compositor can't keep up, stale
// frames are dropped (not queued), so the video never lags behind real-time.
let pendingFrame = null;
let renderScheduled = false;

function onFrame(data) {
  pendingFrame = data;
  if (renderScheduled) return;
  renderScheduled = true;
  requestAnimationFrame(() => {
    renderScheduled = false;
    const frame = pendingFrame;
    pendingFrame = null;
    if (!frame) return;
    const bytes = new Uint8ClampedArray(frame);
    for (let i = 0; i < playerCount; i++) {
      if (frame.byteLength < FRAME_SIZE * (i + 1)) break;
      const off = i * FRAME_SIZE;
      screens[i].imgData.data.set(bytes.subarray(off, off + FRAME_SIZE));
      screens[i].ctx.putImageData(screens[i].imgData, 0, 0);
    }
  });
}

// ---- Debug overlay (backend status text frames) ----

let overlayLastAt = 0;
function pushOverlay(text) {
  if (!debugOn) return; // Debug log toggled off: skip all DOM work.
  // Defensive throttle: a debug flood (e.g. SIO chatter) must never DOM-thrash
  // the main thread and make inputs feel laggy. Cap updates at ~30/s; the full
  // stream is still in the server log, and WARN/ERROR lines are rare.
  const now = performance.now();
  if (now - overlayLastAt < 33) return;
  overlayLastAt = now;
  const el = document.getElementById('overlay');
  const line = document.createElement('div');
  line.className = 'line';
  if (text.startsWith('WARN') || text.startsWith('[mGBA]')) line.className += ' warn';
  else if (text.startsWith('ERROR') || text.includes('fail')) line.className += ' err';
  else if (text.startsWith('OK') || text.startsWith('ROM loaded')) line.className += ' ok';
  line.textContent = text;
  el.appendChild(line);
  while (el.children.length > OVERLAY_MAX) el.removeChild(el.firstChild);
}

// ---- Debug log toggle ----
// The overlay (per-second stats + mGBA WARN/ERROR lines) can be shown/hidden at
// runtime; the preference persists so a "quiet" setting sticks across sessions.
const DEBUG_KEY = 'dualboy_debug_v2'; // v2: default is now OFF (bump discards the old stored "on")

function applyDebugToggle() {
  const el = document.getElementById('overlay');
  if (el) el.style.display = debugOn ? '' : 'none';
  const btn = document.getElementById('toggle-debug');
  if (btn) {
    btn.textContent = `Debug Log: ${debugOn ? 'On' : 'Off'}`;
    btn.classList.toggle('active', debugOn);
  }
}

function toggleDebug() {
  debugOn = !debugOn;
  try { localStorage.setItem(DEBUG_KEY, JSON.stringify(debugOn)); } catch (e) {}
  applyDebugToggle();
  if (!debugOn) {
    const el = document.getElementById('overlay');
    if (el) el.innerHTML = '';
  }
  setStatus(debugOn ? 'Debug log on' : 'Debug log off');
}

function loadDebugToggle() {
  try {
    const raw = localStorage.getItem(DEBUG_KEY);
    if (raw !== null) debugOn = JSON.parse(raw) === true;
  } catch (e) {}
  applyDebugToggle();
}

async function setKeys(player, keys) {
  if (wasmMode) {
    // Frontend keys are 1-indexed; the bridge expects 0-indexed players.
    wasmModule._db_set_keys(player - 1, keys);
  } else if (IS_TAURI) {
    await invoke('set_keys', { player, keys });
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'keys', player, keys }));
  }
}

// The backend gets the union of keyboard + gamepad input for a player.
function sendKeys(p) {
  setKeys(p + 1, keyStates[p] | padStates[p]);
}

async function handleKey(e, isDown) {
  // Remap overlay open: keys go to remapping, never to the game.
  const overlay = document.getElementById('remap-overlay');
  if (overlay && !overlay.hidden) {
    if (e.code === 'Escape') {
      if (isDown) closeRemap();
      return;
    }
    if (isDown) remapCaptureKey(e);
    return;
  }

  // Pause menu open: arrows move, Enter/Space select, Escape (or the pause
  // hotkey itself) resumes.
  if (pauseOpen()) {
    if (!isDown) return;
    if (e.code === 'Escape') { doResume(); return; }
    const pauseCode = hotkeys.pause && hotkeys.pause.keyboard;
    if (pauseCode && e.code === pauseCode) { triggerHotkey('pause'); return; }
    if (e.code === 'ArrowUp') { pauseNavigate(-1); e.preventDefault(); return; }
    if (e.code === 'ArrowDown') { pauseNavigate(1); e.preventDefault(); return; }
    if (e.code === 'Enter' || e.code === 'Space') { pauseSelect(); e.preventDefault(); return; }
    return;
  }

  // Library overlay open: arrow keys navigate, Enter selects, Escape closes.
  if (libraryOpen()) {
    if (!isDown) return;
    if (e.code === 'Escape') { closeLibrary(); return; }
    if (e.code === 'ArrowLeft') { libNavigate(-1, 0); e.preventDefault(); return; }
    if (e.code === 'ArrowRight') { libNavigate(1, 0); e.preventDefault(); return; }
    if (e.code === 'ArrowUp') { libNavigate(0, -1); e.preventDefault(); return; }
    if (e.code === 'ArrowDown') { libNavigate(0, 1); e.preventDefault(); return; }
    if (e.code === 'Enter' || e.code === 'Space') { libSelect(); e.preventDefault(); return; }
    return;
  }

  // A menu is open: let the menu have the keys; Escape closes it.
  if (menuOpen()) {
    if (e.code === 'Escape') closeMenus();
    return;
  }

  // Global hotkeys (turbo / quick save / quick load / pause) — remappable, and
  // checked before per-player keys so a hotkey never doubles as game input.
  for (const action of HOTKEY_ACTIONS) {
    const code = hotkeys[action.id].keyboard;
    if (!code || e.code !== code) continue;
    e.preventDefault();
    if (isDown) triggerHotkey(action.id);
    return;
  }

  let handled = false;
  for (let p = 0; p < playerCount; p++) {
    const map = controls[p].keyboard;
    const bit = map[e.code];
    if (bit === undefined) continue;

    handled = true;
    if (isDown) keyStates[p] |= bit;
    else keyStates[p] &= ~bit;
    sendKeys(p);
  }
  // Stop browser defaults (scrolling, focused-button activation) for every key
  // the emulator uses, so game input never leaks into the UI.
  if (handled) e.preventDefault();
}

// ---- Game library (controller-navigable launcher) ----
// Two sources of games shown as one grid:
//   - recents: games loaded this session (persisted; click/select to relaunch)
//   - library: ROMs scanned from a folder the user added (Tauri: scan_games_dir
//     command; web: <input webkitdirectory>). Box art comes from a sibling image
//     with the same stem; otherwise a generated gradient tile stands in.
const RECENTS_KEY = 'dualboy_recents_v1';
const LIBRARY_KEY = 'dualboy_library_v1';

let recents = [];        // { name, path (null on web), boxArtPath }
let libraryGames = [];   // Tauri: { name, path, boxArtPath } | web: { name, file, boxArtUrl }
let libItems = [];       // focusable tiles + footer buttons, in order
let libFocus = 0;
let libCols = 4;

function loadRecents() {
  recents = [];
  try {
    const raw = localStorage.getItem(RECENTS_KEY);
    if (raw) recents = JSON.parse(raw).filter((r) => r && r.name);
  } catch (e) {}
}

function saveRecents() {
  try { localStorage.setItem(RECENTS_KEY, JSON.stringify(recents)); } catch (e) {}
}

function recordRecent(name, path, boxArtPath, key) {
  recents = recents.filter((r) => (path ? r.path !== path : r.name !== name));
  recents.unshift({ name, path: path || null, boxArtPath: boxArtPath || null, key: key || null });
  if (recents.length > 12) recents.length = 12;
  saveRecents();
}

// Persisted library (Tauri only — paths are stable; web File objects aren't).
function loadLibrary() {
  libraryGames = [];
  if (!IS_TAURI) return;
  try {
    const raw = localStorage.getItem(LIBRARY_KEY);
    if (raw) libraryGames = JSON.parse(raw).filter((g) => g && g.path);
  } catch (e) {}
}

function saveLibrary() {
  if (!IS_TAURI) return;
  try {
    localStorage.setItem(
      LIBRARY_KEY,
      JSON.stringify(libraryGames.map((g) => ({ name: g.name, path: g.path, boxArtPath: g.boxArtPath || null })))
    );
  } catch (e) {}
}

function libraryOpen() {
  const el = document.getElementById('library-overlay');
  return !!(el && !el.hidden);
}

function openLibrary() {
  closeMenus();
  document.getElementById('library-overlay').hidden = false;
  renderLibrary();
}

function closeLibrary() {
  document.getElementById('library-overlay').hidden = true;
  libMode = 'browse';
  pendingGame = null;
  setStatus('Library closed');
}

function gradientFor(s) {
  let h = 0;
  for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) >>> 0;
  const hue = h % 360;
  return `linear-gradient(135deg, hsl(${hue},52%,36%), hsl(${(hue + 70) % 360},52%,20%))`;
}

function makePlaceholderArt(title) {
  const ph = document.createElement('div');
  ph.className = 'library-placeholder';
  ph.style.background = gradientFor(title);
  const t = document.createElement('span');
  t.textContent = title;
  ph.appendChild(t);
  return ph;
}

function makeLibraryTile(title, boxArtUrl, badge, onSelect) {
  const tile = document.createElement('button');
  tile.className = 'library-tile';
  tile.addEventListener('click', onSelect);

  const art = document.createElement('div');
  art.className = 'library-art';
  if (boxArtUrl) {
    const img = document.createElement('img');
    img.src = boxArtUrl;
    img.alt = '';
    img.loading = 'lazy';
    img.addEventListener('error', () => {
      img.remove();
      art.appendChild(makePlaceholderArt(title));
    });
    art.appendChild(img);
  } else {
    art.appendChild(makePlaceholderArt(title));
  }

  if (badge) {
    const b = document.createElement('span');
    b.className = 'library-badge';
    b.textContent = badge;
    art.appendChild(b);
  }

  const label = document.createElement('span');
  label.className = 'library-name';
  label.textContent = title;

  tile.append(art, label);
  return tile;
}

function addSectionTitle(grid, text) {
  const h = document.createElement('div');
  h.className = 'library-section-title';
  h.textContent = text;
  grid.appendChild(h);
}

function computeLibCols() {
  const grid = document.getElementById('library-grid');
  const cols = getComputedStyle(grid).gridTemplateColumns.split(' ').filter(Boolean).length;
  return cols || 4;
}

function renderLibrary() {
  const grid = document.getElementById('library-grid');
  grid.innerHTML = '';
  libItems = [];
  libFocus = 0;
  libCols = 1;

  // Player-count step: pick the game first, then how many linked players.
  if (libMode === 'players') {
    document.querySelector('.library-actions').style.display = 'none';
    const title = document.createElement('div');
    title.className = 'library-section-title';
    title.textContent = `How many players? \u2014 ${pendingGame ? pendingGame.name : ''}`;
    grid.appendChild(title);
    for (let n = 1; n <= 4; n++) {
      const b = document.createElement('button');
      b.className = 'library-choice';
      b.textContent = `${n} Player${n > 1 ? 's' : ''}`;
      b.addEventListener('click', () => startGameWithPlayers(n));
      grid.appendChild(b);
      libItems.push(b);
    }
    const back = document.createElement('button');
    back.className = 'library-choice';
    back.textContent = 'Back';
    back.addEventListener('click', () => { libMode = 'browse'; renderLibrary(); });
    grid.appendChild(back);
    libItems.push(back);
    document.getElementById('library-hint').textContent =
      'D-pad to move \u00b7 A to select \u00b7 B to go back';
    applyLibraryFocus();
    return;
  }
  document.querySelector('.library-actions').style.display = '';

  if (recents.length) {
    addSectionTitle(grid, 'Recent');
    for (const r of recents) {
      const tile = makeLibraryTile(r.name, null, '\u2605', () => selectRecent(r));
      grid.appendChild(tile);
      libItems.push(tile);
      if (r.boxArtPath) loadBoxArt({ boxArtPath: r.boxArtPath }, tile);
      else maybeLoadOnlineBoxArt(tile, r.name);
    }
  }

  addSectionTitle(grid, 'Library');
  if (!libraryGames.length) {
    const empty = document.createElement('div');
    empty.className = 'library-empty';
    empty.textContent = 'No games yet \u2014 use Add Folder to scan a folder of ROMs.';
    grid.appendChild(empty);
  } else {
    for (const g of libraryGames) {
      const tile = makeLibraryTile(g.name, g.boxArtUrl || null, null, () => selectLibraryGame(g));
      grid.appendChild(tile);
      libItems.push(tile);
      if (g.boxArtUrl) {
        // Local art already set; nothing more to do.
      } else if (g.boxArtPath) loadBoxArt(g, tile);
      else maybeLoadOnlineBoxArt(tile, g.name);
    }
  }

  // Footer actions join the focus list (reachable with D-pad Down from the grid).
  for (const id of ['library-add-folder', 'library-load-rom', 'library-close']) {
    libItems.push(document.getElementById(id));
  }

  libCols = computeLibCols();
  applyLibraryFocus();
  document.getElementById('library-hint').textContent =
    'D-pad to move \u00b7 A to select \u00b7 B to close';
}

function applyLibraryFocus() {
  libItems.forEach((el, i) => el.classList.toggle('focused', i === libFocus));
  const el = libItems[libFocus];
  if (el && el.scrollIntoView) el.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}

function libNavigate(dx, dy) {
  if (!libItems.length) return;
  if (dx) libFocus = Math.max(0, Math.min(libItems.length - 1, libFocus + dx));
  else if (dy) libFocus = Math.max(0, Math.min(libItems.length - 1, libFocus + dy * Math.max(1, libCols)));
  applyLibraryFocus();
}

function libSelect() {
  const el = libItems[libFocus];
  if (el) el.click();
}

async function loadBoxArt(game, tile) {
  try {
    const dataUrl = await invoke('read_box_art', { path: game.boxArtPath });
    const art = tile.querySelector('.library-art');
    const img = document.createElement('img');
    img.src = dataUrl;
    img.alt = '';
    img.addEventListener('error', () => img.remove());
    art.insertBefore(img, art.firstChild);
    const ph = art.querySelector('.library-placeholder');
    if (ph) ph.remove();
  } catch (e) {
    // Keep the generated placeholder.
  }
}

async function selectRecent(r) {
  if (IS_TAURI && r.path) {
    choosePlayers({ name: r.name, path: r.path, boxArtPath: r.boxArtPath });
    return;
  }
  // Web: relaunch from the IndexedDB cache if we still have the bytes;
  // otherwise the user re-picks the file.
  if (r.key) {
    const cached = await getCachedGame(r.key);
    if (cached) {
      choosePlayers({ name: r.name, key: r.key });
      return;
    }
  }
  closeLibrary();
  pickRomBrowser();
}

async function selectLibraryGame(g) {
  if (IS_TAURI) choosePlayers({ name: g.name, path: g.path, boxArtPath: g.boxArtPath });
  else choosePlayers({ name: g.name, file: g.file });
}

async function loadGamePath(path, name, boxArtPath) {
  setStatus(`Loading: ${name}`);
  await invoke('load_rom', { path });
  resetRuntimeState();
  setStatus(`Running: ${name}`);
  recordRecent(name, path, boxArtPath);
  closeLibrary();
}

async function loadGameFile(file, name) {
  setStatus(`Loading: ${name}`);
  if (wasmMode) {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const ok = wasmLoadRomBytes(bytes) === 0;
    if (ok) resetRuntimeState();
    setStatus(ok ? `Running: ${name}` : 'Error: ROM load failed in browser engine');
    let key = null;
    if (ok) {
      key = `rom_${name}`;
      try { await cacheGameBytes(key, name, bytes); } catch (e) { key = null; }
    }
    recordRecent(name, null, null, key);
    closeLibrary();
    return;
  }
  const resp = await fetch('/load_rom', { method: 'POST', body: file });
  if (resp.ok) resetRuntimeState();
  setStatus(resp.ok ? `Running: ${name}` : 'Error: ' + (await resp.text()));
  // Web: cache the bytes so this recent can relaunch without re-picking.
  let key = null;
  if (resp.ok) {
    key = `rom_${name}`;
    try { await cacheGameBytes(key, name, new Uint8Array(await file.arrayBuffer())); } catch (e) { key = null; }
  }
  recordRecent(name, null, null, key);
  closeLibrary();
}

async function addFolder() {
  if (IS_TAURI) await addFolderTauri();
  else addFolderWeb();
}

async function addFolderTauri() {
  const { open } = window.__TAURI__.dialog;
  const dir = await open({ directory: true, multiple: false });
  if (!dir) return;
  setStatus('Scanning folder\u2026');
  const entries = await invoke('scan_games_dir', { path: dir });
  libraryGames = entries.map((e) => ({ name: e.name, path: e.path, boxArtPath: e.box_art || null }));
  saveLibrary();
  renderLibrary();
  setStatus(`Library: ${libraryGames.length} game${libraryGames.length === 1 ? '' : 's'}`);
}

function addFolderWeb() {
  const input = document.createElement('input');
  input.type = 'file';
  input.webkitdirectory = true;
  input.accept = '.gba';
  input.onchange = () => {
    const files = [...input.files];
    const images = new Map();
    for (const f of files) {
      if (/\.(png|jpe?g|webp|gif)$/i.test(f.name)) {
        const stem = f.name.replace(/\.[^.]+$/, '').toLowerCase();
        if (!images.has(stem)) images.set(stem, URL.createObjectURL(f));
      }
    }
    libraryGames = files
      .filter((f) => f.name.toLowerCase().endsWith('.gba'))
      .map((f) => ({
        name: f.name.replace(/\.[^.]+$/, ''),
        file: f,
        boxArtUrl: images.get(f.name.replace(/\.[^.]+$/, '').toLowerCase()) || null,
      }));
    renderLibrary();
    setStatus(`Library: ${libraryGames.length} game${libraryGames.length === 1 ? '' : 's'}`);
  };
  input.click();
}

// ---- Web game cache (IndexedDB) ----
// Browsers can't re-open an arbitrary local file path from JS, so the web build
// caches the bytes of recently played games in IndexedDB (GBA ROMs are 4-64MB;
// localStorage's ~5MB cap is far too small). A cached recent relaunches instantly
// instead of forcing the user to re-pick the file.
const CACHE_DB = 'dualboy_cache';
const CACHE_STORE = 'games';
const CACHE_MAX = 6;

function openCacheDb() {
  return new Promise((resolve, reject) => {
    if (!window.indexedDB) return reject(new Error('IndexedDB unavailable'));
    const req = indexedDB.open(CACHE_DB, 1);
    req.onupgradeneeded = () => {
      if (!req.result.objectStoreNames.contains(CACHE_STORE)) {
        req.result.createObjectStore(CACHE_STORE, { keyPath: 'key' });
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function txDone(tx) {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error);
  });
}

function idbGet(store, key) {
  return new Promise((resolve, reject) => {
    const req = store.get(key);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function cacheGameBytes(key, name, bytes) {
  try {
    const db = await openCacheDb();
    const tx = db.transaction(CACHE_STORE, 'readwrite');
    tx.objectStore(CACHE_STORE).put({ key, name, bytes, ts: Date.now() });
    await txDone(tx);
    // Drop the oldest entries beyond CACHE_MAX (LRU by timestamp).
    const all = await idbGet(db.transaction(CACHE_STORE, 'readonly').objectStore(CACHE_STORE), '__all__')
      .catch(() => null);
    const list = all ? [all] : await new Promise((res, rej) => {
      const r = db.transaction(CACHE_STORE, 'readonly').objectStore(CACHE_STORE).getAll();
      r.onsuccess = () => res(r.result);
      r.onerror = () => rej(r.error);
    });
    if (list.length > CACHE_MAX) {
      list.sort((a, b) => (a.ts || 0) - (b.ts || 0));
      const drop = list.slice(0, list.length - CACHE_MAX);
      const dtx = db.transaction(CACHE_STORE, 'readwrite');
      const ds = dtx.objectStore(CACHE_STORE);
      for (const item of drop) ds.delete(item.key);
      await txDone(dtx);
    }
    db.close();
  } catch (e) {
    // Cache is best-effort; the game still loads fine either way.
  }
}

async function getCachedGame(key) {
  try {
    const db = await openCacheDb();
    const item = await idbGet(db.transaction(CACHE_STORE, 'readonly').objectStore(CACHE_STORE), key);
    db.close();
    return item || null;
  } catch (e) {
    return null;
  }
}

// ---- Box art: online fallback ----
// libretro-thumbnails hosts a large GBA box-art collection keyed by the game's
// (region-stripped) title. Best-effort: if a tile has no local image, try the
// network; on any failure the generated gradient placeholder stays put.
function libretroLookupName(name) {
  return name
    .replace(/\s*\((?:U|USA|E|Europe|J|Japan|F|G|S|AU|World)\)/gi, '')
    .replace(/\s*\[.*?\]/g, '')
    .replace(/\s+/g, ' ')
    .trim();
}

function maybeLoadOnlineBoxArt(tile, name) {
  const art = tile.querySelector('.library-art');
  if (!art || art.querySelector('img')) return;
  const img = document.createElement('img');
  img.alt = '';
  img.loading = 'lazy';
  img.addEventListener('error', () => img.remove());
  img.addEventListener('load', () => {
    art.insertBefore(img, art.firstChild);
    const ph = art.querySelector('.library-placeholder');
    if (ph) ph.remove();
  });
  img.src = `https://thumbnails.libretro.com/GBA/Named_Boxarts/${encodeURIComponent(libretroLookupName(name))}.png`;
}

// ---- Player-count step in the library flow (game -> players -> start) ----
let libMode = 'browse'; // 'browse' | 'players'
let pendingGame = null;

function choosePlayers(game) {
  pendingGame = game;
  libMode = 'players';
  renderLibrary();
}

async function startGameWithPlayers(n) {
  const g = pendingGame;
  if (!g) return;
  await setPlayerCount(n);
  if (g.path) {
    await loadGamePath(g.path, g.name, g.boxArtPath);
  } else if (g.file) {
    await loadGameFile(g.file, g.name);
  } else if (g.key) {
    // Web: relaunch from the IndexedDB cache when available.
    const cached = await getCachedGame(g.key);
    if (cached) {
      setStatus(`Loading: ${g.name}`);
      if (wasmMode) {
        const ok = wasmLoadRomBytes(cached.bytes) === 0;
        if (ok) resetRuntimeState();
        setStatus(ok ? `Running: ${g.name}` : 'Error: ROM load failed');
      } else {
        const resp = await fetch('/load_rom', { method: 'POST', body: cached.bytes });
        if (resp.ok) resetRuntimeState();
        setStatus(resp.ok ? `Running: ${g.name}` : 'Error: ' + (await resp.text()));
      }
      recordRecent(g.name, null, null, g.key);
      closeLibrary();
    } else {
      closeLibrary();
      pickRomBrowser();
    }
  }
}

// ---- Pause menu (controller-navigable) ----
// Opens automatically when emulation is paused; every in-game action (resume,
// save/load state, player count, library, quit ROM) is one D-pad+A away.
let pauseItems = [];
let pauseFocus = 0;
let pauseSub = 'main'; // 'main' | 'players'

function pauseOpen() {
  const el = document.getElementById('pause-overlay');
  return !!(el && !el.hidden);
}

function openPauseMenu() {
  pauseSub = 'main';
  renderPauseMenu();
  document.getElementById('pause-overlay').hidden = false;
}

function closePauseMenu() {
  document.getElementById('pause-overlay').hidden = true;
}

function renderPauseMenu() {
  const container = document.getElementById('pause-items');
  container.innerHTML = '';
  pauseItems = [];
  const addItem = (label, fn, id) => {
    const b = document.createElement('button');
    if (id) b.id = id;
    b.textContent = label;
    b.addEventListener('click', fn);
    container.appendChild(b);
    pauseItems.push(b);
  };
  if (pauseSub === 'players') {
    for (let n = 1; n <= 4; n++) {
      addItem(`${n} Player${n > 1 ? 's' : ''}`, async () => {
        // setPlayerCount rebuilds the emulator and unpauses (which closes the
        // pause menu via resetRuntimeState), then the game is running again.
        await setPlayerCount(n);
        pauseSub = 'main';
        setStatus(`${n} players`);
      });
    }
    addItem('Back', () => { pauseSub = 'main'; renderPauseMenu(); });
  } else {
    addItem('Resume', doResume, 'pause-resume');
    addItem('Quick Save State', quickSaveState, 'pause-save');
    addItem('Quick Load State', quickLoadState, 'pause-load');
    addItem('Players\u2026', () => { pauseSub = 'players'; renderPauseMenu(); }, 'pause-players');
    addItem('Games Library', () => { closePauseMenu(); openLibrary(); }, 'pause-library');
    addItem('Quit ROM', async () => { closePauseMenu(); await quitGame(); }, 'pause-quit');
  }
  pauseFocus = 0;
  applyPauseFocus();
}

function applyPauseFocus() {
  pauseItems.forEach((el, i) => el.classList.toggle('focused', i === pauseFocus));
  const el = pauseItems[pauseFocus];
  if (el && el.scrollIntoView) el.scrollIntoView({ block: 'nearest' });
}

function pauseNavigate(dy) {
  if (!pauseItems.length) return;
  pauseFocus = Math.max(0, Math.min(pauseItems.length - 1, pauseFocus + dy));
  applyPauseFocus();
}

function pauseSelect() {
  const el = pauseItems[pauseFocus];
  if (el) el.click();
}

async function doResume() {
  paused = false;
  if (IS_TAURI) {
    await invoke('set_paused', { paused: false });
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'pause', paused: false }));
  }
  highlightPause(false);
  closePauseMenu();
  setStatus('Resumed');
}

async function resumeIfPaused() {
  if (!paused) return;
  await doResume();
}

// ---- Gamepad polling ----
// Poll navigator.getGamepads() on the animation frame. Controller slot i drives
// player i+1; each player's mask is the union of its button + axis maps.
const PAD_AXIS_DEADZONE = 0.5;
const prevPadPressed = new Set(); // "playerIdx:buttonIdx" pressed last frame

function pollGamepads() {
  if (!navigator.getGamepads) return;
  const pads = navigator.getGamepads();
  const nowPressed = new Set();
  const overlayOpen = remapOverlayOpen();
  const libOpen = libraryOpen();

  // Populate nowPressed for all four pads (not just active players) so remap
  // navigation/capture works even for a player beyond the current playerCount.
  for (let p = 0; p < 4; p++) {
    const pad = pads[p];
    if (!pad) continue;
    for (let i = 0; i < pad.buttons.length; i++) {
      if (pad.buttons[i].pressed) nowPressed.add(`${p}:${i}`);
    }
  }

  // Game input: only when the remap overlay is closed, and only for active
  // players. padStates is left untouched while the overlay is open so closing
  // it resumes cleanly from the next fresh poll.
  const hotkeyConsumed = new Set(); // "p:idx" consumed by a global hotkey
  if (!overlayOpen && !libOpen) {
    // Global hotkeys from gamepads: fire on a fresh press and consume the
    // button so it doesn't ALSO feed the player's game mask this frame.
    for (let p = 0; p < 4; p++) {
      const pad = pads[p];
      if (!pad) continue;
      for (const action of HOTKEY_ACTIONS) {
        const gb = hotkeys[action.id].gamepad;
        if (gb === null || gb === undefined) continue;
        const key = `${p}:${gb}`;
        if (pad.buttons[gb] && pad.buttons[gb].pressed && !prevPadPressed.has(key)) {
          hotkeyConsumed.add(key);
          triggerHotkey(action.id);
        }
      }
    }

    for (let p = 0; p < playerCount; p++) {
      const pad = pads[p];
      let mask = 0;
      if (pad) {
        const gb = controls[p].gamepadButtons;
        for (const idxStr in gb) {
          if (hotkeyConsumed.has(`${p}:${idxStr}`)) continue;
          const b = pad.buttons[parseInt(idxStr, 10)];
          if (b && b.pressed) mask |= gb[idxStr];
        }
        const ax = controls[p].gamepadAxes;
        for (const axStr in ax) {
          const v = pad.axes[parseInt(axStr, 10)];
          if (v === undefined) continue;
          if (v <= -PAD_AXIS_DEADZONE) mask |= ax[axStr].neg;
          else if (v >= PAD_AXIS_DEADZONE) mask |= ax[axStr].pos;
        }
      }
      if (mask !== padStates[p]) {
        padStates[p] = mask;
        sendKeys(p);
      }
    }
  }

  let captured = false;

  // Remap capture / cancel: a button freshly pressed on the remapped player's
  // controller. Runs BEFORE navigation so the A press that selects a cell can't
  // also be captured as the new binding in the same frame.
  if (remapListening && pads[remapPlayer]) {
    const id = remapListening.id;
    const prefix = remapMode === 'hotkeys' ? 'Hotkey' : `Player ${remapPlayer + 1}`;
    for (const key of nowPressed) {
      if (!key.startsWith(`${remapPlayer}:`) || prevPadPressed.has(key)) continue;
      const idx = parseInt(key.split(':')[1], 10);

      if (remapListening.source === 'keyboard') {
        // A keyboard capture can't be completed by a gamepad: treat any fresh
        // button as "cancel" so a gamepad-only user is never stuck.
        remapListening = null;
        captured = true;
        renderRemap();
        setStatus(`${prefix}: key capture cancelled`);
        break;
      }

      if (idx === NAV_B) {
        // B cancels a pending gamepad capture instead of binding.
        remapListening = null;
        captured = true;
        renderRemap();
        setStatus(`${prefix}: capture cancelled`);
        break;
      }

      setRemapGamepad(id, idx);
      const action = remapActions().find((a) => remapActionId(a) === id);
      remapListening = null;
      captured = true;
      renderRemap();
      setStatus(`${prefix}: ${remapActionLabel(action)} = ${formatPadButton(idx)}`);
      break;
    }
  }

  // Remap navigation (overlay open, not mid-capture): D-pad moves, A selects,
  // B closes. `captured` stops a just-bound button from also moving focus.
  if (overlayOpen && !remapListening && !captured && pads[remapPlayer]) {
    const edge = (i) => nowPressed.has(`${remapPlayer}:${i}`) && !prevPadPressed.has(`${remapPlayer}:${i}`);
    const up = edge(NAV_UP), down = edge(NAV_DOWN), left = edge(NAV_LEFT), right = edge(NAV_RIGHT);
    const a = edge(NAV_A), b = edge(NAV_B);
    if (up) remapNavigate(0, -1);
    if (down) remapNavigate(0, 1);
    if (left) remapNavigate(-1, 0);
    if (right) remapNavigate(1, 0);
    if (a) remapSelect();
    else if (b) remapCancel();
  }

  // Pause menu navigation (paused): controller #1 D-pad moves, A selects,
  // B resumes. Takes precedence over game input (which is frozen anyway).
  const pauseMenuOpen = pauseOpen();
  if (pauseMenuOpen && !captured && pads[0]) {
    const edge = (i) => nowPressed.has(`0:${i}`) && !prevPadPressed.has(`0:${i}`);
    const up = edge(NAV_UP), down = edge(NAV_DOWN), a = edge(NAV_A), b = edge(NAV_B);
    if (up) pauseNavigate(-1);
    else if (down) pauseNavigate(1);
    if (a) pauseSelect();
    else if (b) doResume();
  }

  // Library navigation (launcher open): controller #1 D-pad moves, A selects,
  // B closes. Mutually exclusive with the remap overlay (only one is open).
  if (libOpen && !pauseMenuOpen && !captured && pads[0]) {
    const edge = (i) => nowPressed.has(`0:${i}`) && !prevPadPressed.has(`0:${i}`);
    const up = edge(NAV_UP), down = edge(NAV_DOWN), left = edge(NAV_LEFT), right = edge(NAV_RIGHT);
    const a = edge(NAV_A), b = edge(NAV_B);
    if (up) libNavigate(0, -1);
    else if (down) libNavigate(0, 1);
    else if (left) libNavigate(-1, 0);
    else if (right) libNavigate(1, 0);
    if (a) libSelect();
    else if (b) closeLibrary();
  }

  prevPadPressed.clear();
  for (const k of nowPressed) prevPadPressed.add(k);
  requestAnimationFrame(pollGamepads);
}

// ---- Control re-mapping overlay ----
// One overlay serves two modes: 'player' remaps a player's GBA button bindings,
// 'hotkeys' remaps the global turbo/save/load/pause actions. Both share the same
// grid, navigation, and capture machinery; they differ only in the action list
// and the binding store they read/write.
let remapMode = 'player'; // 'player' | 'hotkeys'
let remapPlayer = 0;
let remapListening = null; // { id, source: 'keyboard' | 'gamepad' }

// Gamepad UI navigation uses a FIXED set of standard buttons (never remapped,
// since remapping targets game input, not the overlay UI): D-pad moves focus,
// A selects, B closes.
const NAV_UP = 12, NAV_DOWN = 13, NAV_LEFT = 14, NAV_RIGHT = 15, NAV_A = 0, NAV_B = 1;

let remapNav = [];      // N rows x 2 cols of focusable elements (actions + footer)
let remapFocus = { row: 0, col: 0 };

// ---- Remap action abstraction (player vs hotkeys) ----
function remapActions() {
  return remapMode === 'hotkeys' ? HOTKEY_ACTIONS : REMAP_ACTIONS;
}
function remapActionId(action) {
  return remapMode === 'hotkeys' ? action.id : action.bit;
}
function remapActionLabel(action) {
  return remapMode === 'hotkeys' ? action.label : action.name;
}
// Current keyboard binding code (or null) for an action id.
function remapKeyboardCode(id) {
  if (remapMode === 'hotkeys') return hotkeys[id].keyboard || null;
  const map = controls[remapPlayer].keyboard;
  for (const code in map) if (map[code] === id) return code;
  return null;
}
// Current gamepad button index (or null) for an action id.
function remapGamepadIndex(id) {
  if (remapMode === 'hotkeys') {
    const g = hotkeys[id].gamepad;
    return (g === null || g === undefined) ? null : g;
  }
  const map = controls[remapPlayer].gamepadButtons;
  for (const idx in map) if (map[idx] === id) return parseInt(idx, 10);
  return null;
}
// Bind a keyboard code to an action id (one key drives one action; replace).
function setRemapKeyboard(id, code) {
  if (remapMode === 'hotkeys') {
    for (const k in hotkeys) if (hotkeys[k].keyboard === code) hotkeys[k].keyboard = null;
    hotkeys[id].keyboard = code;
    saveHotkeys();
  } else {
    const kb = controls[remapPlayer].keyboard;
    for (const c in kb) if (c === code || kb[c] === id) delete kb[c];
    kb[code] = id;
    saveControls();
  }
}
// Bind a gamepad button index to an action id (one button drives one action).
function setRemapGamepad(id, idx) {
  if (remapMode === 'hotkeys') {
    for (const k in hotkeys) if (hotkeys[k].gamepad === idx) hotkeys[k].gamepad = null;
    hotkeys[id].gamepad = idx;
    saveHotkeys();
  } else {
    const gb = controls[remapPlayer].gamepadButtons;
    for (const k in gb) if (parseInt(k, 10) === idx || gb[k] === id) delete gb[k];
    gb[idx] = id;
    saveControls();
  }
}

function openRemap(p) {
  remapMode = 'player';
  remapPlayer = p;
  remapListening = null;
  remapFocus = { row: 0, col: 0 };
  document.getElementById('remap-title').textContent = `Player ${p + 1} Controls`;
  renderRemap();
  document.getElementById('remap-overlay').hidden = false;
  closeMenus();
}

function openHotkeysRemap() {
  remapMode = 'hotkeys';
  remapPlayer = 0; // global hotkeys: navigate/capture with controller #1
  remapListening = null;
  remapFocus = { row: 0, col: 0 };
  document.getElementById('remap-title').textContent = 'Global Hotkeys';
  renderRemap();
  document.getElementById('remap-overlay').hidden = false;
  closeMenus();
}

function remapOverlayOpen() {
  const overlay = document.getElementById('remap-overlay');
  return !!(overlay && !overlay.hidden);
}

function closeRemap() {
  remapListening = null;
  document.getElementById('remap-overlay').hidden = true;
}

function renderRemap() {
  const rows = document.getElementById('remap-rows');
  rows.innerHTML = '';
  remapNav = [];
  for (const action of remapActions()) {
    const id = remapActionId(action);
    const row = document.createElement('div');
    row.className = 'remap-row';

    const label = document.createElement('span');
    label.className = 'remap-action';
    label.textContent = remapActionLabel(action);

    const kbCode = remapKeyboardCode(id);
    const kbBtn = document.createElement('button');
    kbBtn.className = 'remap-cell';
    kbBtn.textContent = kbCode ? formatKeyCode(kbCode) : '\u2014';
    kbBtn.addEventListener('click', () => beginListen(id, 'keyboard'));

    const padIdx = remapGamepadIndex(id);
    const padBtn = document.createElement('button');
    padBtn.className = 'remap-cell';
    padBtn.textContent = padIdx !== null ? formatPadButton(padIdx) : '\u2014';
    padBtn.addEventListener('click', () => beginListen(id, 'gamepad'));

    row.append(label, kbBtn, padBtn);
    rows.appendChild(row);
    remapNav.push([kbBtn, padBtn]);
  }
  // Footer: Reset (col 0) and Done (col 1) as the last "row".
  remapNav.push([
    document.getElementById('remap-reset'),
    document.getElementById('remap-done'),
  ]);
  applyRemapFocus();
  updateRemapHint();
}

function applyRemapFocus() {
  document.querySelectorAll('#remap-overlay .focused').forEach((el) => el.classList.remove('focused'));
  const row = remapNav[remapFocus.row];
  const el = row ? row[remapFocus.col] : null;
  if (el) {
    el.classList.add('focused');
    if (el.scrollIntoView) el.scrollIntoView({ block: 'nearest' });
  }
}

function remapNavigate(dCol, dRow) {
  const rows = remapNav.length || 1;
  remapFocus.row = (remapFocus.row + dRow + rows) % rows;
  if (dCol < 0) remapFocus.col = 0;
  else if (dCol > 0) remapFocus.col = 1;
  applyRemapFocus();
}

function remapSelect() {
  const { row, col } = remapFocus;
  if (row >= remapNav.length - 1) {
    // Footer row: Reset / Done.
    if (col === 0) resetRemapCurrent();
    else closeRemap();
    return;
  }
  const action = remapActions()[row];
  beginListen(remapActionId(action), col === 0 ? 'keyboard' : 'gamepad');
}

function remapCancel() {
  closeRemap();
}

function updateRemapHint() {
  if (remapListening) return; // beginListen set a specific "press a button" hint.
  document.getElementById('remap-hint').textContent =
    'D-pad to move \u00b7 A to select \u00b7 B to close \u2014 or click a cell and press a key/button.';
}

function beginListen(id, source) {
  document.querySelectorAll('.remap-cell.listening').forEach((c) => c.classList.remove('listening'));
  remapListening = { id, source };
  const hint = document.getElementById('remap-hint');
  hint.textContent =
    source === 'keyboard'
      ? 'Press the key you want to bind\u2026 (any gamepad button cancels)'
      : 'Press the gamepad button you want to bind\u2026 (B cancels)';
  const actions = remapActions();
  [...document.querySelectorAll('.remap-row')].forEach((row, i) => {
    if (actions[i] && remapActionId(actions[i]) === id) {
      row.children[source === 'keyboard' ? 1 : 2].classList.add('listening');
    }
  });
}

function remapCaptureKey(e) {
  if (!remapListening || remapListening.source !== 'keyboard') return;
  const id = remapListening.id;
  setRemapKeyboard(id, e.code);
  const action = remapActions().find((a) => remapActionId(a) === id);
  remapListening = null;
  renderRemap();
  const prefix = remapMode === 'hotkeys' ? 'Hotkey' : `Player ${remapPlayer + 1}`;
  setStatus(`${prefix}: ${remapActionLabel(action)} = ${formatKeyCode(e.code)}`);
  e.preventDefault();
}

function resetRemapCurrent() {
  if (remapMode === 'hotkeys') {
    hotkeys = defaultHotkeys();
    saveHotkeys();
    setStatus('Hotkeys reset to defaults');
  } else {
    controls[remapPlayer] = defaultControlFor(remapPlayer);
    saveControls();
    setStatus(`Player ${remapPlayer + 1} controls reset to defaults`);
  }
  renderRemap();
}

// ---- Player count (1-4 linked instances) ----

function highlightPlayersMenu(count) {
  document.querySelectorAll('#player-menu button').forEach((btn) => {
    btn.classList.toggle('active', parseInt(btn.dataset.players, 10) === count);
  });
}

async function setPlayerCount(n) {
  if (n < 1 || n > 4) return;
  if (wasmMode) {
    wasmModule._db_init(n);
    playerCount = n;
  } else if (IS_TAURI) {
    await invoke('set_player_count', { n });
    playerCount = await invoke('player_count');
  } else {
    await fetch('/set_player_count', { method: 'POST', body: String(n) });
    playerCount = parseInt(await (await fetch('/player_count')).text(), 10) || n;
  }
  resetRuntimeState();
  initScreens(playerCount);
  highlightPlayersMenu(playerCount);
  setStatus(`${playerCount} linked player${playerCount > 1 ? 's' : ''} selected\u2014load a ROM`);
}

// ---- ROM loading ----

async function pickRomTauri() {
  closeMenus();
  const { open } = window.__TAURI__.dialog;
  const selected = await open({
    multiple: false,
    filters: [{ name: 'GBA ROM', extensions: ['gba'] }],
  });
  if (selected) {
    setStatus('Loading: ' + selected);
    await invoke('load_rom', { path: selected });
    setStatus('Running: ' + selected);
    const base = selected.split(/[\\/]/).pop().replace(/\.[^.]+$/, '') || selected;
    recordRecent(base, selected);
  }
  closeMenus();
}

function pickRomBrowser() {
  closeMenus();
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.gba';
  input.onchange = async () => {
    const file = input.files[0];
    if (!file) return;
    const name = file.name.replace(/\.[^.]+$/, '');
    setStatus('Loading: ' + file.name);
    if (wasmMode) {
      const bytes = new Uint8Array(await file.arrayBuffer());
      const ok = wasmLoadRomBytes(bytes) === 0;
      if (ok) resetRuntimeState();
      setStatus(ok ? 'Running: ' + file.name : 'Error: ROM load failed');
      let key = null;
      if (ok) {
        key = `rom_${name}`;
        try { await cacheGameBytes(key, name, bytes); } catch (e) { key = null; }
      }
      recordRecent(name, null, null, key);
      closeMenus();
      return;
    }
    const resp = await fetch('/load_rom', { method: 'POST', body: file });
    if (resp.ok) resetRuntimeState();
    if (resp.ok) setStatus('Running: ' + file.name);
    else setStatus('Error: ' + (await resp.text()));
    let key = null;
    if (resp.ok) {
      key = `rom_${name}`;
      try { await cacheGameBytes(key, name, new Uint8Array(await file.arrayBuffer())); } catch (e) { key = null; }
    }
    recordRecent(name, null, null, key);
    closeMenus();
  };
  input.click();
}

// ---- Save import/export ----

async function exportSaveTauri(player) {
  const { save } = window.__TAURI__.dialog;
  const path = await save({
    filters: [{ name: 'GBA Save', extensions: ['sav'] }],
    defaultPath: `player${player}.sav`,
  });
  if (path) {
    await invoke('export_save', { player, path });
    setStatus(`Saved player ${player} save to ${path}`);
  }
}

async function exportSaveBrowser(player) {
  const resp = await fetch(`/save/${player}`);
  if (!resp.ok) {
    setStatus('Error: ' + (await resp.text()));
    return;
  }
  const url = URL.createObjectURL(await resp.blob());
  const a = document.createElement('a');
  a.href = url;
  a.download = `player${player}.sav`;
  a.click();
  URL.revokeObjectURL(url);
  setStatus(`Downloaded player ${player} save`);
}

async function importSaveTauri(player) {
  const { open } = window.__TAURI__.dialog;
  const selected = await open({
    multiple: false,
    filters: [{ name: 'GBA Save', extensions: ['sav'] }],
  });
  if (selected) {
    await invoke('import_save', { player, path: selected });
    setStatus(`Imported save into player ${player}`);
  }
}

function importSaveBrowser(player) {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.sav';
  input.onchange = async () => {
    const file = input.files[0];
    if (!file) return;
    const resp = await fetch(`/save/${player}`, { method: 'POST', body: file });
    setStatus(resp.ok ? `Imported save into player ${player}` : 'Error: ' + (await resp.text()));
  };
  input.click();
}

async function exportSetTauri() {
  closeMenus();
  const { save } = window.__TAURI__.dialog;
  const path = await save({
    filters: [{ name: 'DualBoy Save Set', extensions: ['dualbysave'] }],
    defaultPath: 'dualboy.dualbysave',
  });
  if (path) {
    await invoke('export_save_set', { path });
    setStatus(`Saved all saves to ${path}`);
  }
  closeMenus();
}

async function exportSetBrowser() {
  closeMenus();
  const resp = await fetch('/save_set');
  if (!resp.ok) {
    setStatus('Error: ' + (await resp.text()));
    return;
  }
  const url = URL.createObjectURL(await resp.blob());
  const a = document.createElement('a');
  a.href = url;
  a.download = 'dualboy.dualbysave';
  a.click();
  URL.revokeObjectURL(url);
  setStatus('Downloaded save set');
}

async function importSetTauri() {
  closeMenus();
  const { open } = window.__TAURI__.dialog;
  const selected = await open({
    multiple: false,
    filters: [{ name: 'DualBoy Save Set', extensions: ['dualbysave'] }],
  });
  if (selected) {
    await invoke('import_save_set', { path: selected });
    setStatus('Imported save set');
  }
  closeMenus();
}

function importSetBrowser() {
  closeMenus();
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.dualbysave';
  input.onchange = async () => {
    const file = input.files[0];
    if (!file) return;
    const resp = await fetch('/save_set', { method: 'POST', body: file });
    setStatus(resp.ok ? 'Imported save set' : 'Error: ' + (await resp.text()));
  };
  input.click();
}

// ---- Browser audio (WebAudio playback) ----
// The web server streams the selected mix (Player 1 / mix all / etc.) as tagged
// binary frames: u32 LE sample rate + interleaved stereo s16. The desktop app
// plays via ALSA instead, so this path only runs in the browser (!IS_TAURI).

let audioCtx = null;
let audioNode = null;
let audioBuf = new Float32Array(0); // interleaved L,R awaiting playback
let audioSrcRate = 32768;
let audioPos = 0; // fractional sample-frame read position within audioBuf

function unlockAudio() {
  if (IS_TAURI) return; // desktop: ALSA handles audio, not WebAudio
  if (audioCtx) {
    if (audioCtx.state === 'suspended') audioCtx.resume().catch(() => {});
    return;
  }
  try {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return;
    audioCtx = new Ctx();
  } catch {
    return;
  }
  // ScriptProcessorNode is deprecated but universally supported and simple;
  // AudioWorklet + a SharedArrayBuffer ring is the modern upgrade path. 2048
  // samples (~46ms) keeps latency low without underrunning on the main thread.
  audioNode = audioCtx.createScriptProcessor(2048, 0, 2);
  audioNode.onaudioprocess = onAudioProcess;
  audioNode.connect(audioCtx.destination);
}

// Catmull-Rom cubic interpolation between p1 and p2 (neighbors p0, p3).
// Same as what a decent audio resampler uses; much flatter passband than linear.
function cubicInterp(p0, p1, p2, p3, t) {
  const t2 = t * t;
  const t3 = t2 * t;
  return 0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}

function onAudioProcess(e) {
  const outL = e.outputBuffer.getChannelData(0);
  const outR = e.outputBuffer.getChannelData(1);
  const n = outL.length;
  const frames = audioBuf.length >> 1;
  if (frames < 4) {
    // Not enough history for cubic (startup/underrun): output silence.
    outL.fill(0);
    outR.fill(0);
    audioPos = 0;
    return;
  }
  // Resample the GBA rate (32768/65536) to the AudioContext rate with 4-tap
  // cubic interpolation. The GBA's native output is 8-bit-ish PCM, so the
  // resampler is the only quality-sensitive stage in this whole path.
  const ratio = audioSrcRate / audioCtx.sampleRate;
  for (let i = 0; i < n; i++) {
    const k = audioPos | 0;
    const frac = audioPos - k;
    if (k < 1 || k > frames - 3) {
      outL[i] = 0;
      outR[i] = 0;
    } else {
      const k0 = (k - 1) * 2;
      const k1 = k * 2;
      const k2 = (k + 1) * 2;
      const k3 = (k + 2) * 2;
      outL[i] = cubicInterp(audioBuf[k0], audioBuf[k1], audioBuf[k2], audioBuf[k3], frac);
      outR[i] = cubicInterp(audioBuf[k0 + 1], audioBuf[k1 + 1], audioBuf[k2 + 1], audioBuf[k3 + 1], frac);
    }
    audioPos += ratio;
  }
  // Discard fully-consumed frames so the queue can't grow unbounded.
  const consumed = audioPos | 0;
  if (consumed > 0) {
    audioBuf = audioBuf.subarray(consumed * 2);
    audioPos -= consumed;
  }
}

function onAudio(data) {
  if (data.byteLength < 4) return;
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const rate = dv.getUint32(0, true);
  const count = (data.byteLength - 4) >> 1;
  if (count <= 0) return;
  if (rate !== audioSrcRate) {
    // Rate changed (SOUNDBIAS resolution switch): flush and restart at new rate.
    audioBuf = new Float32Array(0);
    audioPos = 0;
    audioSrcRate = rate;
  }
  // Copy the sample bytes: the 1-byte tag makes the absolute offset odd, which
  // Int16Array rejects (needs 2-byte alignment). slice() returns a fresh,
  // zero-offset, aligned buffer.
  const sampleBytes = data.slice(4);
  const src = new Int16Array(sampleBytes.buffer, sampleBytes.byteOffset, count);
  const flt = new Float32Array(count);
  for (let i = 0; i < count; i++) flt[i] = src[i] / 32768;
  const merged = new Float32Array(audioBuf.length + flt.length);
  merged.set(audioBuf, 0);
  merged.set(flt, audioBuf.length);
  // Cap the pending buffer (~4s) so a stalled consumer can't balloon memory.
  // When we drop the head, adjust the read position too (it indexes frames into
  // the buffer), otherwise the next callback jumps and glitches.
  const max = audioSrcRate * 2 * 4;
  if (merged.length > max) {
    const dropped = merged.length - max;
    audioBuf = merged.subarray(dropped);
    audioPos = Math.max(0, audioPos - (dropped >> 1));
  } else {
    audioBuf = merged;
  }
}

// ---- In-browser engine (WASM) ----

// The emscripten output only exposes `DualBoyWasm` as a plain global (its
// export guards target CommonJS/AMD, which browsers don't provide), so load it
// as a classic script and grab the factory it declares.
function loadWasmModule() {
  return new Promise((resolve, reject) => {
    if (window.DualBoyWasm) { resolve(window.DualBoyWasm); return; }
    const s = document.createElement('script');
    s.src = new URL('dualboy-web.js', location.href).href;
    s.onload = () => resolve(window.DualBoyWasm);
    s.onerror = () => reject(new Error('failed to load dualboy-web.js'));
    document.head.appendChild(s);
  }).then(async (factory) => {
    wasmModule = await factory({ locateFile: (path) => new URL('dualboy-web.wasm', location.href).href });
    return wasmModule;
  });
}

// Push a ROM's bytes into every linked core. Returns 0 on success.
function wasmLoadRomBytes(bytes) {
  const M = wasmModule;
  const ptr = M._malloc(bytes.length);
  if (!ptr) return -99;
  M.HEAPU8.set(bytes, ptr);
  const rc = M._db_load_rom(ptr, bytes.length);
  M._free(ptr);
  return rc;
}

// Drain the audio buffer without playing (turbo mute-on-exit, like desktop).
function wasmFlushAudio() {
  if (!wasmModule) return;
  wasmModule._db_get_audio();
}

// Copy every player's latest finished frame into the canvases.
function wasmRenderVideo() {
  const M = wasmModule;
  const heap = M.HEAPU8;
  for (let i = 0; i < playerCount; i++) {
    const ptr = M._db_get_video(i);
    if (!ptr) continue;
    screens[i].imgData.data.set(heap.subarray(ptr, ptr + FRAME_SIZE));
    screens[i].ctx.putImageData(screens[i].imgData, 0, 0);
  }
}

// Feed this frame's mixed audio into the existing WebAudio resampler, tagged
// the same way the backend streams it (u32 LE rate + interleaved stereo s16).
function wasmPumpAudio() {
  if (!audioCtx) return;
  const M = wasmModule;
  const frames = M._db_audio_frames();
  if (frames <= 0) return;
  const ptr = M._db_get_audio();
  const sampleBytes = M.HEAPU8.subarray(ptr, ptr + frames * 4);
  const tagged = new Uint8Array(4 + sampleBytes.length);
  new DataView(tagged.buffer).setUint32(0, 32768, true);
  tagged.set(sampleBytes, 4);
  onAudio(tagged.buffer);
}

// Fixed-timestep 60 fps loop. At 60 Hz displays one frame runs per rAF; on
// 120/144 Hz panels the accumulator throttles to exactly 60 game-fps. Turbo
// runs as many frames per rAF as the browser can chew, with audio muted.
function wasmFrame(now) {
  wasmLoopId = requestAnimationFrame(wasmFrame);
  const M = wasmModule;
  let delta = now - wasmLast;
  wasmLast = now;
  if (delta > 100) delta = 100; // tab hidden: don't spiral the accumulator
  if (turboOn) {
    const TURBO_FRAMES = 4; // ~4x at 60 Hz rAF; adjust for stronger fast-forward
    for (let i = 0; i < TURBO_FRAMES; i++) {
      if (paused) break;
      M._db_run_frame();
    }
  } else {
    wasmAccum += delta;
    let ran = 0;
    while (wasmAccum >= FRAME_MS && ran < 4) {
      if (!paused) M._db_run_frame();
      wasmAccum -= FRAME_MS;
      ran++;
    }
    if (wasmAccum >= FRAME_MS * 4) wasmAccum = 0; // too far behind: reset
  }
  wasmRenderVideo();
  if (!turboOn) wasmPumpAudio();
}

function wasmStartLoop() {
  if (wasmLoopId || !wasmModule) return;
  wasmLast = performance.now();
  wasmAccum = 0;
  wasmLoopId = requestAnimationFrame(wasmFrame);
}

function wasmStopLoop() {
  if (wasmLoopId) {
    cancelAnimationFrame(wasmLoopId);
    wasmLoopId = 0;
  }
}

// ---- WebSocket (frames; also input in browser mode) ----

function connectWebSocket() {
  const url = IS_TAURI ? 'ws://127.0.0.1:8088' : 'ws://127.0.0.1:8080/ws';
  socket = new WebSocket(url);
  socket.binaryType = 'arraybuffer';
  socket.onmessage = (event) => {
    // Text frames are backend status/overlay lines. Binary frames are tagged:
    // 0 = video (RGBA, all players concatenated), 1 = audio (rate + stereo s16).
    if (typeof event.data === 'string') { pushOverlay(event.data); return; }
    const bytes = new Uint8Array(event.data);
    if (bytes[0] === 0) onFrame(bytes.subarray(1));
    else if (bytes[0] === 1) onAudio(bytes.subarray(1));
  };
  socket.onclose = () => setTimeout(connectWebSocket, 1000);
}

window.addEventListener('DOMContentLoaded', async () => {
  if (IS_TAURI) {
    try {
      playerCount = await invoke('player_count');
    } catch {
      playerCount = 2;
    }
  } else {
    // Probe for the dualboy-web backend. If it's absent (GitHub Pages, or the
    // binary isn't running), fall back to the in-browser WASM engine so the
    // page is fully playable with no server at all.
    let backendUp = false;
    try {
      const ctrl = new AbortController();
      const t = setTimeout(() => ctrl.abort(), 1500);
      const resp = await fetch('/player_count', { signal: ctrl.signal });
      clearTimeout(t);
      backendUp = resp.ok;
    } catch {
      backendUp = false;
    }
    if (backendUp) {
      try {
        playerCount = parseInt(await (await fetch('/player_count')).text(), 10) || 2;
      } catch {
        playerCount = 2;
      }
    } else {
      playerCount = 2;
      wasmMode = true;
      try {
        await loadWasmModule();
        wasmModule._db_init(playerCount);
        setStatus('In-browser engine ready \u2014 2 linked GBAs (load a ROM)');
      } catch (err) {
        // Engine failed AND no backend: only now show the hosted-shell notice.
        wasmMode = false;
        document.getElementById('hosted-note').hidden = false;
        setStatus('Engine failed to start: ' + err);
      }
    }
  }
  loadControls();
  loadHotkeys();
  refreshHotkeyLabels();
  loadDebugToggle();
  loadViewPrefs();
  loadOutlinesPref();
  loadBackground();
  loadRecents();
  loadLibrary();
  initScreens(playerCount);
  highlightPlayersMenu(playerCount);

  document.querySelectorAll('#controls-menu [data-remap]').forEach((btn) => {
    btn.addEventListener('click', () => openRemap(parseInt(btn.dataset.remap, 10)));
  });
  document.getElementById('remap-hotkeys').addEventListener('click', () => {
    closeMenus();
    openHotkeysRemap();
  });
  document.getElementById('remap-reset').addEventListener('click', resetRemapCurrent);
  document.getElementById('remap-done').addEventListener('click', closeRemap);
  document.getElementById('remap-overlay').addEventListener('click', (e) => {
    if (e.target.id === 'remap-overlay') closeRemap(); // click outside the panel
  });

  document.getElementById('open-library').addEventListener('click', openLibrary);
  document.getElementById('library-add-folder').addEventListener('click', addFolder);
  document.getElementById('library-load-rom').addEventListener('click', () => {
    closeLibrary();
    IS_TAURI ? pickRomTauri() : pickRomBrowser();
  });
  document.getElementById('library-close').addEventListener('click', closeLibrary);
  document.getElementById('library-overlay').addEventListener('click', (e) => {
    if (e.target.id === 'library-overlay') closeLibrary(); // click outside the panel
  });

  document.querySelectorAll('#player-menu button').forEach((btn) => {
    btn.addEventListener('click', () => {
      closeMenus();
      setPlayerCount(parseInt(btn.dataset.players, 10));
    });
  });

  document.getElementById('load-rom').addEventListener('click', () =>
    IS_TAURI ? pickRomTauri() : pickRomBrowser());
  document.getElementById('quit-game').addEventListener('click', quitGame);
  document.getElementById('quick-save-state').addEventListener('click', quickSaveState);
  document.getElementById('quick-load-state').addEventListener('click', quickLoadState);
  document.getElementById('toggle-turbo').addEventListener('click', () => {
    closeMenus();
    toggleTurbo();
  });
  document.getElementById('toggle-pause').addEventListener('click', () => {
    closeMenus();
    togglePause();
  });
  document.getElementById('toggle-debug').addEventListener('click', () => {
    closeMenus();
    toggleDebug();
  });
  document.querySelectorAll('#view-menu [data-view]').forEach((btn) => {
    btn.addEventListener('click', () => {
      closeMenus();
      setViewMode(btn.dataset.view);
    });
  });
  document.getElementById('cycle-view').addEventListener('click', () => {
    closeMenus();
    cycleViewMode();
  });
  document.getElementById('cycle-focus').addEventListener('click', () => {
    closeMenus();
    cycleFocusPlayer();
  });
  document.getElementById('bg-image').addEventListener('click', pickBackground);
  document.getElementById('bg-clear').addEventListener('click', clearBackground);
  document.getElementById('toggle-outlines').addEventListener('click', () => {
    closeMenus();
    toggleOutlines();
  });
  document.querySelectorAll('#audio-menu button').forEach((btn) => {
    btn.addEventListener('click', () => {
      closeMenus();
      setAudioSource(parseInt(btn.dataset.audio, 10));
    });
  });
  highlightAudio(audioSource);
  document.getElementById('export-set').addEventListener('click', () =>
    IS_TAURI ? exportSetTauri() : exportSetBrowser());
  document.getElementById('import-set').addEventListener('click', () =>
    IS_TAURI ? importSetTauri() : importSetBrowser());

  window.addEventListener('keydown', (e) => handleKey(e, true));
  window.addEventListener('keyup', (e) => handleKey(e, false));
  window.addEventListener('resize', () => layout());

  // Browsers require a user gesture before audio may start; unlock on any
  // click or keypress so game sound starts the moment the user interacts.
  window.addEventListener('pointerdown', unlockAudio);
  window.addEventListener('keydown', unlockAudio);

  // Gamepad polling runs on the animation frame; controller slot i -> player i+1.
  pollGamepads();

  if (wasmMode) {
    wasmStartLoop();
  } else {
    connectWebSocket();
  }
});
