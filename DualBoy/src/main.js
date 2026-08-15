const IS_TAURI = typeof window.__TAURI__ !== 'undefined';

const GBA_WIDTH = 240;
const GBA_HEIGHT = 160;
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
  'Enter': GBA_BUTTONS.START,
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
  'KeyP': GBA_BUTTONS.START,
  'KeyO': GBA_BUTTONS.SELECT,
};

// One entry per player; extended if the backend runs more than two instances.
const PLAYER_MAPS = [P1_MAP, P2_MAP];

let playerCount = 2;
let screens = []; // { canvas, ctx }
let keyStates = [];
let socket = null;

function setStatus(text) {
  document.getElementById('status').textContent = text;
}

function initScreens(count) {
  playerCount = count;
  const container = document.getElementById('screens');
  container.innerHTML = '';
  screens = [];
  keyStates = new Array(count).fill(0);

  const select = document.getElementById('player-select');
  select.innerHTML = '';

  for (let i = 0; i < count; i++) {
    const wrapper = document.createElement('div');
    wrapper.className = 'screen-wrapper';

    const label = document.createElement('h2');
    label.textContent = `Player ${i + 1}`;

    const canvas = document.createElement('canvas');
    canvas.width = GBA_WIDTH;
    canvas.height = GBA_HEIGHT;

    wrapper.appendChild(label);
    wrapper.appendChild(canvas);
    container.appendChild(wrapper);
    screens.push({ canvas, ctx: canvas.getContext('2d') });

    const option = document.createElement('option');
    option.value = i + 1;
    option.textContent = `Player ${i + 1}`;
    select.appendChild(option);
  }
}

function onFrame(data) {
  const bytes = new Uint8ClampedArray(data);
  for (let i = 0; i < playerCount; i++) {
    if (data.byteLength < FRAME_SIZE * (i + 1)) break;
    const frame = bytes.subarray(i * FRAME_SIZE, (i + 1) * FRAME_SIZE);
    const img = new ImageData(frame, GBA_WIDTH, GBA_HEIGHT);
    screens[i].ctx.putImageData(img, 0, 0);
  }
}

function selectedPlayer() {
  return parseInt(document.getElementById('player-select').value, 10);
}

async function setKeys(player, keys) {
  if (IS_TAURI) {
    await invoke('set_keys', { player, keys });
  } else if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: 'keys', player, keys }));
  }
}

async function handleKey(e, isDown) {
  for (let p = 0; p < playerCount; p++) {
    const map = PLAYER_MAPS[p];
    if (!map) continue;
    const bit = map[e.code];
    if (bit === undefined) continue;

    if (isDown) keyStates[p] |= bit;
    else keyStates[p] &= ~bit;
    await setKeys(p + 1, keyStates[p]);
  }
}

// ---- ROM loading ----

async function pickRomTauri() {
  const { invoke } = window.__TAURI__.core;
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
}

function pickRomBrowser() {
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
  };
  input.click();
}

// ---- Save import/export ----

async function exportSaveTauri(player) {
  const { invoke } = window.__TAURI__.core;
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
  const { invoke } = window.__TAURI__.core;
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
  const { invoke } = window.__TAURI__.core;
  const { save } = window.__TAURI__.dialog;
  const path = await save({
    filters: [{ name: 'DualBoy Save Set', extensions: ['dualbysave'] }],
    defaultPath: 'dualboy.dualbysave',
  });
  if (path) {
    await invoke('export_save_set', { path });
    setStatus(`Saved all saves to ${path}`);
  }
}

async function exportSetBrowser() {
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
  const { invoke } = window.__TAURI__.core;
  const { open } = window.__TAURI__.dialog;
  const selected = await open({
    multiple: false,
    filters: [{ name: 'DualBoy Save Set', extensions: ['dualbysave'] }],
  });
  if (selected) {
    await invoke('import_save_set', { path: selected });
    setStatus('Imported save set');
  }
}

function importSetBrowser() {
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
  socket.onmessage = (event) => onFrame(event.data);
  socket.onclose = () => setTimeout(connectWebSocket, 1000);
}

window.addEventListener('DOMContentLoaded', async () => {
  if (IS_TAURI) {
    const { invoke } = window.__TAURI__.core;
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
  document.getElementById('export-save').addEventListener('click', () =>
    IS_TAURI ? exportSaveTauri(selectedPlayer()) : exportSaveBrowser(selectedPlayer()));
  document.getElementById('import-save').addEventListener('click', () =>
    IS_TAURI ? importSaveTauri(selectedPlayer()) : importSaveBrowser(selectedPlayer()));
  document.getElementById('export-set').addEventListener('click', () =>
    IS_TAURI ? exportSetTauri() : exportSetBrowser());
  document.getElementById('import-set').addEventListener('click', () =>
    IS_TAURI ? importSetTauri() : importSetBrowser());

  window.addEventListener('keydown', (e) => handleKey(e, true));
  window.addEventListener('keyup', (e) => handleKey(e, false));

  connectWebSocket();
});
