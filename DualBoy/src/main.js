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

// One entry per player; the backend runs 2-4 instances (--players N), so all four
// maps are always defined and the frame loop only uses the first playerCount.
const PLAYER_MAPS = [P1_MAP, P2_MAP, P3_MAP, P4_MAP];

// Player color coding (like name borders on a video call): P1 red, P2 blue,
// P3 green, P4 orange. Same palette for the border and the bottom-right tag.
const PLAYER_TAGS = ['player-1', 'player-2', 'player-3', 'player-4'];

const OVERLAY_MAX = 6;
let playerCount = 2;
let screens = []; // { canvas, ctx, imgData }
let keyStates = [];
let socket = null;

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

function pushOverlay(text) {
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

async function handleKey(e, isDown) {
  // A menu is open: let the menu have the keys; don't also drive the game.
  if (menuOpen()) return;

  let handled = false;
  for (let p = 0; p < playerCount; p++) {
    const map = PLAYER_MAPS[p];
    if (!map) continue;
    const bit = map[e.code];
    if (bit === undefined) continue;

    handled = true;
    if (isDown) keyStates[p] |= bit;
    else keyStates[p] &= ~bit;
    await setKeys(p + 1, keyStates[p]);
  }
  // Stop browser defaults (scrolling, focused-button activation) for every key
  // the emulator uses, so game input never leaks into the UI.
  if (handled) e.preventDefault();
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

// ---- WebSocket (frames; also input in browser mode) ----

function connectWebSocket() {
  const url = IS_TAURI ? 'ws://127.0.0.1:8088' : 'ws://127.0.0.1:8080/ws';
  socket = new WebSocket(url);
  socket.binaryType = 'arraybuffer';
  socket.onmessage = (event) => {
    // Text frames are backend status/overlay lines; binary frames are pixels.
    if (typeof event.data === 'string') pushOverlay(event.data);
    else onFrame(event.data);
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
  initScreens(playerCount);

  document.getElementById('load-rom').addEventListener('click', () =>
    IS_TAURI ? pickRomTauri() : pickRomBrowser());
  document.getElementById('export-set').addEventListener('click', () =>
    IS_TAURI ? exportSetTauri() : exportSetBrowser());
  document.getElementById('import-set').addEventListener('click', () =>
    IS_TAURI ? importSetTauri() : importSetBrowser());

  window.addEventListener('keydown', (e) => handleKey(e, true));
  window.addEventListener('keyup', (e) => handleKey(e, false));
  window.addEventListener('resize', () => layout(playerCount));

  connectWebSocket();
});
