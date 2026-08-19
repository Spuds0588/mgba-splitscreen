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
  2: GBA_BUTTONS.L,
  3: GBA_BUTTONS.R,
  4: GBA_BUTTONS.L, // LB as an alternate L
  5: GBA_BUTTONS.R, // RB as an alternate R
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

const CONTROLS_KEY = 'dualboy_controls_v1';
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
  setStatus(turboOn ? 'TURBO ON — fast-forwarding (Tab to toggle)' : 'Turbo off (Tab to toggle)');
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

// ---- Screen grid (video-call style) ----

function layout(count) {
  const el = document.getElementById('screens');
  const wide = window.innerWidth >= window.innerHeight;
  let cols, rows;
  if (count === 2) { cols = wide ? 2 : 1; rows = wide ? 1 : 2; }
  else if (count === 3) { cols = wide ? 3 : 1; rows = wide ? 1 : 3; }
  else { cols = 2; rows = 2; }
  el.style.gridTemplateColumns = `repeat(${cols}, minmax(0, 1fr))`;
  el.style.gridTemplateRows = `repeat(${rows}, minmax(0, 1fr))`;
}

function initScreens(count) {
  playerCount = count;
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

  layout(count);
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
    if (isDown) remapCaptureKey(e);
    return;
  }

  // A menu is open: let the menu have the keys; don't also drive the game.
  if (menuOpen()) return;

  // Tab toggles turbo (fast-forward) — never routed to a player.
  if (e.code === 'Tab') {
    e.preventDefault();
    if (isDown) toggleTurbo();
    return;
  }

  // F5/F7 quick save/load state (all players together) — never routed to a player.
  if (e.code === 'F5') {
    e.preventDefault();
    if (isDown) quickSaveState();
    return;
  }
  if (e.code === 'F7') {
    e.preventDefault();
    if (isDown) quickLoadState();
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
  for (let p = 0; p < playerCount; p++) {
    const pad = pads[p];
    let mask = 0;
    if (pad) {
      const gb = controls[p].gamepadButtons;
      for (const idxStr in gb) {
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
      for (let i = 0; i < pad.buttons.length; i++) {
        if (pad.buttons[i].pressed) nowPressed.add(`${p}:${i}`);
      }
    }
    if (mask !== padStates[p]) {
      padStates[p] = mask;
      sendKeys(p);
    }
  }

  // Remap capture: a button freshly pressed on this player's controller.
  if (remapListening && remapListening.source === 'gamepad' && pads[remapPlayer]) {
    const bit = remapListening.bit;
    for (const key of nowPressed) {
      if (!prevPadPressed.has(key)) {
        const idx = parseInt(key.split(':')[1], 10);
        const gb = controls[remapPlayer].gamepadButtons;
        // A button can only drive one action: free it from any other action,
        // and replace this action's previous button(s) with the new one.
        for (const k in gb) if (parseInt(k, 10) === idx) delete gb[k];
        for (const k in gb) if (gb[k] === bit) delete gb[k];
        gb[idx] = bit;
        saveControls();
        const action = REMAP_ACTIONS.find((a) => a.bit === bit);
        remapListening = null;
        renderRemap();
        setStatus(`Player ${remapPlayer + 1}: ${action.name} = ${formatPadButton(idx)}`);
        break;
      }
    }
  }

  prevPadPressed.clear();
  for (const k of nowPressed) prevPadPressed.add(k);
  requestAnimationFrame(pollGamepads);
}

// ---- Control re-mapping overlay ----
let remapPlayer = 0;
let remapListening = null; // { bit, source: 'keyboard' | 'gamepad' }

function openRemap(p) {
  remapPlayer = p;
  remapListening = null;
  document.getElementById('remap-title').textContent = `Player ${p + 1} Controls`;
  document.getElementById('remap-hint').textContent =
    'Click a cell, then press the key or gamepad button you want.';
  renderRemap();
  document.getElementById('remap-overlay').hidden = false;
  closeMenus();
}

function closeRemap() {
  remapListening = null;
  document.getElementById('remap-overlay').hidden = true;
}

function renderRemap() {
  const rows = document.getElementById('remap-rows');
  rows.innerHTML = '';
  for (const action of REMAP_ACTIONS) {
    const row = document.createElement('div');
    row.className = 'remap-row';

    const label = document.createElement('span');
    label.className = 'remap-action';
    label.textContent = action.name;

    const kbBtn = document.createElement('button');
    kbBtn.className = 'remap-cell';
    kbBtn.textContent = keyForBit(controls[remapPlayer].keyboard, action.bit) || '\u2014';
    kbBtn.addEventListener('click', () => beginListen(action.bit, 'keyboard'));

    const padBtn = document.createElement('button');
    padBtn.className = 'remap-cell';
    padBtn.textContent = padButtonForBit(controls[remapPlayer].gamepadButtons, action.bit) || '\u2014';
    padBtn.addEventListener('click', () => beginListen(action.bit, 'gamepad'));

    row.append(label, kbBtn, padBtn);
    rows.appendChild(row);
  }
}

function beginListen(bit, source) {
  document.querySelectorAll('.remap-cell.listening').forEach((c) => c.classList.remove('listening'));
  remapListening = { bit, source };
  const hint = document.getElementById('remap-hint');
  hint.textContent =
    source === 'keyboard' ? 'Press the key you want to bind\u2026' : 'Press the gamepad button you want to bind\u2026';
  [...document.querySelectorAll('.remap-row')].forEach((row, i) => {
    if (REMAP_ACTIONS[i].bit === bit) {
      row.children[source === 'keyboard' ? 1 : 2].classList.add('listening');
    }
  });
}

function remapCaptureKey(e) {
  if (!remapListening || remapListening.source !== 'keyboard') return;
  const bit = remapListening.bit;
  const kb = controls[remapPlayer].keyboard;
  // A key can only drive one action: free it from every other action, and
  // replace this action's previous key(s) with the newly pressed one.
  for (const code in kb) if (code === e.code) delete kb[code];
  for (const code in kb) if (kb[code] === bit) delete kb[code];
  kb[e.code] = bit;
  saveControls();
  const action = REMAP_ACTIONS.find((a) => a.bit === bit);
  remapListening = null;
  renderRemap();
  setStatus(`Player ${remapPlayer + 1}: ${action.name} = ${formatKeyCode(e.code)}`);
  e.preventDefault();
}

function resetRemapPlayer() {
  controls[remapPlayer] = defaultControlFor(remapPlayer);
  saveControls();
  renderRemap();
  setStatus(`Player ${remapPlayer + 1} controls reset to defaults`);
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
  initScreens(playerCount);
  highlightPlayersMenu(playerCount);

  document.querySelectorAll('#controls-menu button').forEach((btn) => {
    btn.addEventListener('click', () => openRemap(parseInt(btn.dataset.remap, 10)));
  });
  document.getElementById('remap-reset').addEventListener('click', resetRemapPlayer);
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
  window.addEventListener('resize', () => layout(playerCount));

  // Browsers require a user gesture before audio may start; unlock on any
  // click or keypress so game sound starts the moment the user interacts.
  window.addEventListener('pointerdown', unlockAudio);
  window.addEventListener('keydown', unlockAudio);

  // Gamepad polling runs on the animation frame; controller slot i -> player i+1.
  pollGamepads();

  connectWebSocket();
});
