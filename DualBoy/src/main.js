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
let debugOn = true;
// View layout: 'grid' | 'speaker' (1 big + smalls) | 'focus' (single screen) |
// 'overlay' (one tile floating enlarged on top of the grid). focusPlayer selects
// which player's screen is enlarged/shown in speaker/focus/overlay modes.
let viewMode = 'grid';
let focusPlayer = 0;

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
  if (IS_TAURI) {
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
  if (IS_TAURI) {
    await invoke('set_paused', { paused });
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'pause', paused }));
  }
  highlightPause(paused);
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
  if (IS_TAURI) {
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
  if (IS_TAURI) {
    await invoke('quit_game');
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'quit_game' }));
  }
  resetRuntimeState();
  clearScreens();
  setStatus('Waiting for ROM\u2026');
}

// ---- Save state (quick save/load, all players together) ----

async function quickSaveState() {
  closeMenus();
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

function layout() {
  const el = document.getElementById('screens');
  const count = playerCount;
  const wide = window.innerWidth >= window.innerHeight;

  el.dataset.view = viewMode;

  if (viewMode === 'grid') {
    let cols, rows;
    if (count === 2) { cols = wide ? 2 : 1; rows = wide ? 1 : 2; }
    else if (count === 3) { cols = wide ? 3 : 1; rows = wide ? 1 : 3; }
    else { cols = 2; rows = 2; }
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
const DEBUG_KEY = 'dualboy_debug_v1';

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
  if (IS_TAURI) {
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
  if (!overlayOpen) {
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
  if (IS_TAURI) {
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
    setStatus('Loading: ' + file.name);
    const resp = await fetch('/load_rom', { method: 'POST', body: file });
    if (resp.ok) setStatus('Running: ' + file.name);
    else setStatus('Error: ' + (await resp.text()));
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
    try {
      const resp = await fetch('/player_count');
      playerCount = parseInt(await resp.text(), 10) || 2;
    } catch {
      playerCount = 2;
    }
  }
  loadControls();
  loadHotkeys();
  refreshHotkeyLabels();
  loadDebugToggle();
  loadViewPrefs();
  loadBackground();
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

  connectWebSocket();
});
