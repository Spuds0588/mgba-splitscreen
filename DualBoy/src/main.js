const { invoke } = window.__TAURI__.core;
const { open, save } = window.__TAURI__.dialog;

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
let screens = []; // { canvas, ctx, wrapper }
let keyStates = [];

async function initScreens(count) {
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

async function handleKey(e, isDown) {
  for (let p = 0; p < playerCount; p++) {
    const map = PLAYER_MAPS[p];
    if (!map) continue;
    const bit = map[e.code];
    if (bit === undefined) continue;

    if (isDown) keyStates[p] |= bit;
    else keyStates[p] &= ~bit;
    await invoke('set_keys', { player: p + 1, keys: keyStates[p] });
  }
}

async function pickRom() {
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

async function exportSave(player) {
  const path = await save({
    filters: [{ name: 'GBA Save', extensions: ['sav'] }],
    defaultPath: `player${player}.sav`,
  });
  if (path) {
    await invoke('export_save', { player, path });
    setStatus(`Saved player ${player} save to ${path}`);
  }
}

async function importSave(player) {
  const selected = await open({
    multiple: false,
    filters: [{ name: 'GBA Save', extensions: ['sav'] }],
  });
  if (selected) {
    await invoke('import_save', { player, path: selected });
    setStatus(`Imported save into player ${player}`);
  }
}

async function exportSet() {
  const path = await save({
    filters: [{ name: 'DualBoy Save Set', extensions: ['dualbysave'] }],
    defaultPath: 'dualboy.dualbysave',
  });
  if (path) {
    await invoke('export_save_set', { path });
    setStatus(`Saved all saves to ${path}`);
  }
}

async function importSet() {
  const selected = await open({
    multiple: false,
    filters: [{ name: 'DualBoy Save Set', extensions: ['dualbysave'] }],
  });
  if (selected) {
    await invoke('import_save_set', { path: selected });
    setStatus('Imported save set');
  }
}

function setStatus(text) {
  document.getElementById('status').textContent = text;
}

function connectWebSocket() {
  const socket = new WebSocket('ws://127.0.0.1:8088');
  socket.binaryType = 'arraybuffer';
  socket.onmessage = (event) => onFrame(event.data);
  socket.onclose = () => setTimeout(connectWebSocket, 1000);
}

window.addEventListener('DOMContentLoaded', async () => {
  try {
    playerCount = await invoke('player_count');
  } catch {
    playerCount = 2;
  }
  await initScreens(playerCount);

  document.getElementById('load-rom').addEventListener('click', pickRom);
  document.getElementById('export-save').addEventListener('click', () =>
    exportSave(parseInt(document.getElementById('player-select').value, 10)));
  document.getElementById('import-save').addEventListener('click', () =>
    importSave(parseInt(document.getElementById('player-select').value, 10)));
  document.getElementById('export-set').addEventListener('click', exportSet);
  document.getElementById('import-set').addEventListener('click', importSet);

  window.addEventListener('keydown', (e) => handleKey(e, true));
  window.addEventListener('keyup', (e) => handleKey(e, false));

  connectWebSocket();
});
