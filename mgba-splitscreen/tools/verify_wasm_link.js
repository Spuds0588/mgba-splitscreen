/*
 * verify_wasm_link.js — in-page test for the WASM build.
 *
 * Loads a FRESH module instance (independent of the app's running one), inits
 * 4 linked players, loads linktest.gba, runs frames, then OCRs the linktest
 * diagnostics straight out of each player's framebuffer:
 *   - non-black pixel count (does it render?)
 *   - role label color at (2,10): red=P1 MASTER, blue/green/orange=P2-4 SLAVE
 *   - status line color at (2,82): green=LINK ACTIVE, red=NO LINK, yellow=IDLE
 *   - FRM counter at (36,37) via 5x7 digit OCR (parity across players)
 *
 * Paste the whole file into preview_evaluate and read the JSON result.
 *
 * Pixel layout note: mGBA's software video buffer is 0xAABBGGRR (red in the
 * low byte), so the bridge's RGBA output is effectively [R, G, B, A] — index 0
 * is RED, index 2 is BLUE.
 */

(async () => {
  const BASE = location.href.replace(/\/[^/]*$/, '/');
  const W = 240, H = 160;
  const PLAYERS = (typeof window.__TEST_PLAYERS !== 'undefined' ? window.__TEST_PLAYERS : 4);

  // --- 5x7 font: digits 0-9 (from linktest/main.c), as row bitsets ---
  const DIGIT_ROWS = {
    '0': ['01110', '10001', '10011', '10101', '11001', '10001', '01110'],
    '1': ['00100', '01100', '00100', '00100', '00100', '00100', '01110'],
    '2': ['01110', '10001', '00001', '00010', '00100', '01000', '11111'],
    '3': ['11111', '00010', '00100', '00010', '00001', '10001', '01110'],
    '4': ['00010', '00110', '01010', '10010', '11111', '00010', '00010'],
    '5': ['11111', '10000', '11110', '00001', '00001', '10001', '01110'],
    '6': ['00110', '01000', '10000', '11110', '10001', '10001', '01110'],
    '7': ['11111', '00001', '00010', '00100', '01000', '01000', '01000'],
    '8': ['01110', '10001', '10001', '01110', '10001', '10001', '01110'],
    '9': ['01110', '10001', '10001', '01111', '00001', '00010', '01100'],
  };
  const GLYPH_BITS = {};
  for (const [ch, rows] of Object.entries(DIGIT_ROWS)) {
    GLYPH_BITS[ch] = rows.map((r) => parseInt(r, 2));
  }

  function isLit(rgba, x, y) {
    const i = (y * W + x) * 4;
    return (rgba[i] | rgba[i + 1] | rgba[i + 2]) > 8;
  }

  // Read one 5x7 cell at (x,y); returns the matched digit or '?'.
  function readDigit(rgba, x, y) {
    const got = [];
    for (let r = 0; r < 7; r++) {
      let row = 0;
      for (let i = 0; i < 5; i++) if (isLit(rgba, x + i, y + r)) row |= (1 << (4 - i));
      got.push(row);
    }
    for (const [ch, bits] of Object.entries(GLYPH_BITS)) {
      if (bits.every((b, r) => b === got[r])) return ch;
    }
    return '?';
  }

  function readDec(rgba, x, y, width) {
    let s = '';
    for (let i = 0; i < width; i++) s += readDigit(rgba, x + i * 6, y);
    return s;
  }

  // RGBA is [R, G, B, A] (index 0 = red).
  function classify(rgba, x, y) {
    const i = (y * W + x) * 4;
    const r = rgba[i], g = rgba[i + 1], b = rgba[i + 2];
    if (r > 120 && g < 50 && b < 50) return 'RED';
    if (g > 120 && r < 50 && b < 50) return 'GREEN';
    if (b > 120 && r < 50 && g < 50) return 'BLUE';
    if (r > 120 && g >= 4 && g <= 80 && b < 50) return 'ORANGE';
    if (r > 120 && g > 120 && b < 50) return 'YELLOW';
    return 'OTHER';
  }

  function scanColor(rgba, x0, y0, x1, y1) {
    const counts = { RED: 0, GREEN: 0, BLUE: 0, ORANGE: 0, YELLOW: 0, OTHER: 0 };
    for (let y = y0; y < y1; y++) {
      for (let x = x0; x < x1; x++) {
        const c = classify(rgba, x, y);
        if (c !== 'OTHER') counts[c]++;
      }
    }
    let best = 'OTHER', bestN = 0;
    for (const [c, n] of Object.entries(counts)) if (n > bestN) { best = c; bestN = n; }
    return { best, counts };
  }

  function nonBlack(rgba) {
    let n = 0;
    for (let i = 0; i < W * H * 4; i += 4) {
      if ((rgba[i] | rgba[i + 1] | rgba[i + 2]) > 8) n++;
    }
    return n;
  }

  // --- fresh module instance ---
  const factory = window.DualBoyWasm;
  const M = await factory({ locateFile: (p) => BASE + 'dualboy-web.wasm' });
  M._db_init(PLAYERS);
  const rom = await (await fetch(BASE + 'linktest.gba')).arrayBuffer();
  const bytes = new Uint8Array(rom);
  const ptr = M._malloc(bytes.length);
  M.HEAPU8.set(bytes, ptr);
  const rc = M._db_load_rom(ptr, bytes.length);
  M._free(ptr);
  if (rc !== 0) return { error: 'load_rom rc=' + rc };

  // Let the negotiation settle + counters climb: 480 frames (~8s game time).
  for (let f = 0; f < 480; f++) M._db_run_frame();

  const out = { load_rc: rc, players: [] };
  for (let i = 0; i < PLAYERS; i++) {
    const p = M._db_get_video(i);
    const rgba = M.HEAPU8.subarray(p, p + W * H * 4);
    out.players.push({
      player: i + 1,
      nonBlackPx: nonBlack(rgba),
      roleColor: scanColor(rgba, 2, 10, 200, 17).best,
      statusColor: scanColor(rgba, 2, 82, 194, 89).best,
      frm: readDec(rgba, 36, 37, 8),
    });
  }
  M._db_quit();
  return out;
})()
