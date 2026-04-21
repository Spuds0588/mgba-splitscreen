const { invoke } = window.__TAURI__.core;
const { open } = window.__TAURI__.dialog;

let screen1, ctx1;
let screen2, ctx2;

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

let p1State = 0;
let p2State = 0;

window.addEventListener("DOMContentLoaded", () => {
  screen1 = document.getElementById("screen1");
  ctx1 = screen1.getContext("2d");
  
  screen2 = document.getElementById("screen2");
  ctx2 = screen2.getContext("2d");

  document.getElementById("load-rom").addEventListener("click", () => {
    pickAndLoadRom();
  });

  window.addEventListener('keydown', (e) => handleKey(e, true));
  window.addEventListener('keyup', (e) => handleKey(e, false));

  connectWebSocket();
});

async function handleKey(e, isDown) {
  let changed = false;
  if (P1_MAP[e.code]) {
    if (isDown) p1State |= P1_MAP[e.code];
    else p1State &= ~P1_MAP[e.code];
    changed = true;
    await invoke("set_keys", { player: 1, keys: p1State });
  } else if (P2_MAP[e.code]) {
    if (isDown) p2State |= P2_MAP[e.code];
    else p2State &= ~P2_MAP[e.code];
    changed = true;
    await invoke("set_keys", { player: 2, keys: p2State });
  }
}

async function pickAndLoadRom() {
  const status = document.getElementById("status");
  
  try {
    const selected = await open({
      multiple: false,
      filters: [{
        name: 'GBA ROM',
        extensions: ['gba']
      }]
    });

    if (selected) {
      status.textContent = "Loading: " + selected;
      await invoke("load_rom", { path: selected });
      status.textContent = "Running: " + selected;
    }
  } catch (e) {
    console.error(e);
    status.textContent = "Error: " + e;
  }
}

function connectWebSocket() {
  const socket = new WebSocket('ws://127.0.0.1:8080');
  socket.binaryType = 'arraybuffer';

  socket.onopen = () => {
    console.log("Connected to frame stream");
  };

  socket.onmessage = (event) => {
    const data = new Uint8ClampedArray(event.data);
    const frameSize = 240 * 160 * 4;
    
    if (data.length >= frameSize * 2) {
      const p1Frame = data.slice(0, frameSize);
      const p2Frame = data.slice(frameSize, frameSize * 2);
      
      const img1 = new ImageData(p1Frame, 240, 160);
      const img2 = new ImageData(p2Frame, 240, 160);
      
      ctx1.putImageData(img1, 0, 0);
      ctx2.putImageData(img2, 0, 0);
    }
  };

  socket.onclose = () => {
    setTimeout(connectWebSocket, 1000);
  };
}
