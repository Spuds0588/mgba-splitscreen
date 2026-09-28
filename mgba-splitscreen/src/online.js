/* Optional v0.5 online transport.
 *
 * The host remains authoritative: it owns the mGBA/WASM session and guests send
 * only input messages. The transport uses PeerJS, which is WebRTC DataChannels
 * under the hood — payloads go peer-to-peer, only the tiny signaling handshake
 * touches a broker. It is intentionally opt-in and does not affect local play
 * when PeerJS is blocked.
 *
 * Video: each guest receives ONLY its own seat's screen, JPEG-encoded on the
 * host (~10-20KB/frame vs ~300KB raw RGBA for a 2P broadcast). Raw RGBA is kept
 * available for future local-network use, but JPEG is the default because a
 * reliable DataChannel backed up by megabytes-per-second of raw frames stalls
 * to a few fps and the backlog grows unboundedly (observed: guests minutes
 * behind at 3-4 fps). A 30 fps cap keeps latency at a fraction of a second.
 *
 * Audio: chunks are COALESCED, never dropped. The host batches ~30ms of PCM
 * into one send (instead of 60 tiny sends/s), and guests keep a bounded backlog
 * that discards the OLDEST audio when they fall behind, so a slow device hears
 * slightly compressed recent audio rather than increasingly stale garbage.
 */
(() => {
  const state = {
    role: 'idle',
    peer: null,
    host: null,
    connections: [],
    invites: new Map(), // player number -> { token, expires, consumed }
    session: null,
    assignedPlayer: 0,
    callbacks: {},
    // Video: per-seat JPEG encoders, one per guest (seat -> { canvas, ctx }).
    // Encoding is capped at VIDEO_FPS per guest and skipped entirely if the
    // previous JPEG is still in flight, so a slow link never builds a backlog.
    videoEncoders: new Map(),
    videoLastSent: 0,
    frameBusy: false,
    // Audio coalescing: the pump produces ~60 chunks/s of a few KB each; we
    // batch ~30ms worth into one message instead of dropping under load.
    audioPending: null,
    audioLastSent: 0,
    audioBusy: false,
    audioRate: 0,
    // Guest-side video health (decoded frames; windowed fps for the probe).
    videoFrames: 0,
    videoBytes: 0,
    videoFps: 0,
    videoWindowStart: 0,
  };

  // ~2 GBA screens' worth of raw RGBA for reference; per-seat JPEGs run 10-20KB.
  const FRAME_BYTES = 240 * 160 * 4;
  const VIDEO_FPS = 30;          // guests are phones on Wi-Fi; 30 looks fluid
  const VIDEO_MIN_INTERVAL = 1000 / VIDEO_FPS;
  const AUDIO_BATCH_MS = 30;     // coalesce window before one send
  const AUDIO_MAX_PENDING = 16384; // ~250ms of stereo s16 at 32kHz; then coalesce tail-drop;

  function query(name) {
    try { return new URLSearchParams(window.location.search).get(name); } catch (_) { return null; }
  }

  function fragment(name) {
    try {
      const raw = window.location.hash.startsWith('#') ? window.location.hash.slice(1) : window.location.hash;
      return new URLSearchParams(raw).get(name);
    } catch (_) { return null; }
  }

  function randomToken(size = 32) {
    const bytes = new Uint8Array(size);
    crypto.getRandomValues(bytes);
    return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');
  }

  function report(text) {
    state.callbacks.status?.(text);
  }

  function peerConstructor() {
    if (typeof window.Peer === 'function') return window.Peer;
    throw new Error('PeerJS did not load; check the network or Content-Security-Policy');
  }

  function send(conn, message) {
    try {
      if (conn && conn.open) conn.send(message);
    } catch (err) {
      console.warn('Online transport send failed:', err);
    }
  }

  function removeConnection(conn) {
    state.connections = state.connections.filter((item) => item !== conn);
    state.videoEncoders.delete(conn.__player);
  }

  // ---- Per-seat JPEG video encoding ----
  // main.js hands us raw RGBA for one player (or a seat->pixels provider). We
  // push it into a per-guest offscreen canvas and JPEG-encode it: 240x160 at
  // 0.55 quality is ~8-15KB, which a DataChannel moves in a few milliseconds.
  // Encoding serializes per guest: while one encode runs we keep only the
  // newest frame — we always want the freshest picture, never a stale queue.
  function encoderFor(seat) {
    let enc = state.videoEncoders.get(seat);
    if (!enc) {
      const canvas = document.createElement('canvas');
      canvas.width = 240;
      canvas.height = 160;
      enc = { canvas, ctx: canvas.getContext('2d'), busy: false, pending: null };
      state.videoEncoders.set(seat, enc);
    }
    return enc;
  }

  // GBA is 240x160 RGBA (FRAME_BYTES); GB/GBC cores produce 160x144. Anything
  // else: derive a height at 240 wide rather than crash the encoder.
  function frameDims(len) {
    if (len >= FRAME_BYTES) return { w: 240, h: 160 };
    if (len === 160 * 144 * 4) return { w: 160, h: 144 };
    return { w: 240, h: Math.max(1, Math.round(len / 4 / 240)) };
  }

  function encodeAndSendFrame(conn, seat, bytes) {
    const enc = encoderFor(seat);
    if (enc.busy) {
      // An encode is running; keep only the newest frame and return.
      enc.pending = bytes;
      return;
    }
    enc.busy = true;
    let imageBytes;
    try {
      const dims = frameDims(bytes.length);
      if (enc.canvas.width !== dims.w || enc.canvas.height !== dims.h) {
        enc.canvas.width = dims.w;
        enc.canvas.height = dims.h;
      }
      const imgData = enc.ctx.createImageData(dims.w, dims.h);
      imgData.data.set(bytes.length > dims.w * dims.h * 4 ? bytes.subarray(0, dims.w * dims.h * 4) : bytes);
      enc.ctx.putImageData(imgData, 0, 0);
      // toDataURL is synchronous; at 240x160 it costs ~1-2ms, acceptable on
      // the host's frame budget at 30 fps per guest.
      const dataUrl = enc.canvas.toDataURL('image/jpeg', 0.55);
      imageBytes = dataUrlToBytes(dataUrl);
    } catch (err) {
      console.warn('Online video encode failed:', err);
      enc.busy = false;
      return;
    }
    send(conn, { type: 'frame', seat, jpg: true, data: imageBytes.buffer });
    enc.busy = false;
    if (enc.pending) {
      const next = enc.pending;
      enc.pending = null;
      encodeAndSendFrame(conn, seat, next);
    }
  }

  function dataUrlToBytes(dataUrl) {
    const base64 = dataUrl.slice(dataUrl.indexOf(',') + 1);
    const bin = atob(base64);
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return bytes;
  }

  function guestCount() {
    return state.callbacks.playerCount?.() || 2;
  }

  function inviteDescriptors() {
    return [...state.invites.entries()].map(([player, invite]) => ({
      player,
      expires: invite.expires,
      consumed: invite.consumed,
      url: invite.consumed ? null : buildInvite(player),
    }));
  }

  function publishInvites() {
    state.callbacks.invites?.(inviteDescriptors());
  }

  function hostMessage(conn, message) {
    if (!message || typeof message !== 'object') return;
    if (message.type === 'hello') {
      const player = Number(message.player);
      const invite = state.invites.get(player);
      const expired = !invite || Date.now() >= invite.expires;
      // Each guest slot has its own high-entropy, time-limited capability.
      // Consuming P2's URL does not consume P3 or P4's URL.
      if (expired || invite.consumed || message.token !== invite.token) {
        send(conn, { type: 'error', message: expired ? 'This player invite has expired' : 'This player invite is no longer valid' });
        conn.close();
        return;
      }
      if (state.connections.some((item) => item.__player === player)) {
        send(conn, { type: 'error', message: `Player ${player + 1} is already connected` });
        conn.close();
        return;
      }
      invite.consumed = true;
      invite.token = null;
      conn.__player = player;
      state.connections.push(conn);
      publishInvites();
      state.callbacks.inviteUsed?.(player);
      send(conn, {
        type: 'welcome',
        session: state.session,
        player,
        players: guestCount(),
      });
      report(`Online host: Player ${player + 1} joined (${state.connections.length} guest${state.connections.length === 1 ? '' : 's'})`);
      return;
    }
    if (message.type === 'input' && Number.isInteger(conn.__player)) {
      if (Number.isInteger(message.keys) && state.callbacks.input) {
        state.callbacks.input(conn.__player, message.keys >>> 0);
      }
    }
  }

  // ---- Host video broadcast: per-seat, JPEG, rate-capped ----
  // Called from main.js once per emulated frame. `source` is either a seat
  // provider ((seat) => Uint8Array RGBA, the wasm path — zero copies) or a raw
  // RGBA buffer; a buffer LONGER than one frame is the legacy concatenated
  // all-players layout (desktop socket path) and is sliced per connection.
  // Rate limit: encode at most VIDEO_FPS per guest no matter how fast the
  // emulator runs, and skip entirely while the previous encode round is
  // pending, so a slow link NEVER builds a backlog — that backlog was the
  // "minutes behind at 3-4 fps" bug (300KB raw frames × 60/s saturated the
  // reliable DataChannel and queued indefinitely).
  function broadcastFrame(source, seat) {
    if (state.role !== 'host' || !state.connections.length || state.frameBusy) return;
    state.frameBusy = true;
    const now = performance.now();
    if (now - state.videoLastSent < VIDEO_MIN_INTERVAL) {
      queueMicrotask(() => { state.frameBusy = false; });
      return;
    }
    state.videoLastSent = now;
    for (const conn of state.connections) {
      const guestSeat = conn.__player;
      if (!Number.isInteger(guestSeat)) continue;
      let bytes = null;
      if (typeof source === 'function') {
        bytes = source(guestSeat);
      } else {
        const buf = source instanceof Uint8Array ? source : new Uint8Array(source);
        if (!Number.isInteger(seat)) {
          const off = guestSeat * FRAME_BYTES;
          if (buf.length < off + 4) continue;
          bytes = buf.subarray(off);
        } else {
          bytes = buf;
        }
      }
      if (!bytes || !bytes.length) continue;
      encodeAndSendFrame(conn, guestSeat, bytes);
    }
    queueMicrotask(() => { state.frameBusy = false; });
  }

  function setupHostConnection(conn) {
    conn.on('open', () => report('Online host: guest connected; authenticating…'));
    conn.on('data', (message) => hostMessage(conn, message));
    conn.on('close', () => {
      removeConnection(conn);
      report(`Online host: ${state.connections.length} guest${state.connections.length === 1 ? '' : 's'} connected`);
    });
    conn.on('error', () => removeConnection(conn));
  }

  function guestMessage(message) {
    if (!message || typeof message !== 'object') return;
    if (message.type === 'welcome') {
      state.assignedPlayer = message.player;
      // Guests render exactly ONE screen (their own seat). The host's `players`
      // count describes its local grid; `seat` tells the guest which slice of
      // the old concatenated format it used to receive — with per-seat JPEG
      // frames the guest just plays what arrives.
      state.callbacks.playerCount?.(1);
      state.callbacks.guestSeat?.(message.player);
      report(`Online guest: connected as Player ${message.player + 1}`);
    } else if (message.type === 'frame' && message.data) {
      const bytes = message.data instanceof ArrayBuffer
        ? new Uint8Array(message.data)
        : new Uint8Array(message.data.buffer || message.data);
      if (message.jpg) {
        // Per-seat JPEG: decode and hand the guest's single screen raw pixels.
        decodeJpegFrame(bytes);
      } else {
        state.callbacks.frame?.(bytes);
      }
    } else if (message.type === 'audio' && message.data) {
      // Tagged audio chunk (u32 LE rate + interleaved stereo s16), same format
      // the desktop WebSocket streams. Guests play it through the same
      // resampler the local browser path uses.
      const bytes = message.data instanceof ArrayBuffer
        ? new Uint8Array(message.data)
        : new Uint8Array(message.data.buffer || message.data);
      state.callbacks.audio?.(bytes);
    } else if (message.type === 'error') {
      report(`Online guest: ${message.message || 'host rejected the connection'}`);
    }
  }

  // Per-seat JPEG: decode asynchronously, then blit. Decoding serializes so a
  // burst of frames never stacks dozens of pending Image objects.
  const guestDecoder = { busy: false, pending: null };
  function decodeJpegFrame(jpegBytes) {
    if (guestDecoder.busy) {
      guestDecoder.pending = jpegBytes;
      return;
    }
    guestDecoder.busy = true;
    const blob = new Blob([jpegBytes], { type: 'image/jpeg' });
    const url = URL.createObjectURL(blob);
    const img = new Image();
    img.onload = () => {
      URL.revokeObjectURL(url);
      // Windowed fps for the videoStats() probe: cheap and self-resetting.
      const now = performance.now();
      if (!state.videoWindowStart || now - state.videoWindowStart >= 1000) {
        state.videoWindowStart = now;
        state.videoFps = 0;
      }
      state.videoFrames++;
      state.videoBytes += jpegBytes.length;
      state.videoFps++;
      state.callbacks.frame?.({ jpeg: img });
      guestDecoder.busy = false;
      if (guestDecoder.pending) {
        const next = guestDecoder.pending;
        guestDecoder.pending = null;
        decodeJpegFrame(next);
      }
    };
    img.onerror = () => {
      URL.revokeObjectURL(url);
      guestDecoder.busy = false;
      guestDecoder.pending = null;
    };
    img.src = url;
  }



  function setupGuestConnection(conn) {
    state.host = conn;
    conn.on('open', () => {
      send(conn, {
        type: 'hello',
        token: fragment('token') || query('token'),
        player: Number(fragment('player') || query('player')),
        protocol: 1,
      });
      report('Online guest: connecting to host…');
    });
    conn.on('data', guestMessage);
    conn.on('close', () => report('Online guest: host disconnected'));
    conn.on('error', (err) => report(`Online guest: ${err.message || 'connection failed'}`));
  }

  function buildInvite(player) {
    const invite = state.invites.get(player);
    if (!invite || invite.consumed) return null;
    const url = new URL(window.location.href);
    url.search = '';
    url.hash = '';
    url.searchParams.set('online', 'join');
    url.searchParams.set('peer', state.peer.id);
    url.searchParams.set('player', String(player));
    url.searchParams.set('players', String(guestCount()));
    const rom = query('rom');
    if (rom) url.searchParams.set('rom', rom);
    // Keep bearer data in the fragment: browsers do not send it in referrers.
    url.hash = new URLSearchParams({
      session: state.session,
      token: invite.token,
      expires: String(invite.expires),
      player: String(player),
    }).toString();
    return url.href;
  }

  function createInvites() {
    state.invites = new Map();
    const expires = Date.now() + 10 * 60 * 1000;
    for (let player = 1; player < guestCount(); player++) {
      state.invites.set(player, { token: randomToken(), expires, consumed: false });
    }
  }

  async function startHost() {
    const needsFresh = state.role === 'host' && [...state.invites.values()].every((invite) =>
      invite.consumed || Date.now() >= invite.expires);
    if (state.role === 'host' && needsFresh) {
      state.session = randomToken(8);
      createInvites();
      publishInvites();
      report('Online host: issued fresh invites for all guest slots');
      return buildInvite(1);
    }
    if (state.role !== 'idle') return buildInvite(1);
    try {
      const Peer = peerConstructor();
      state.role = 'host';
      state.session = randomToken(8);
      createInvites();
      state.peer = new Peer();
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('PeerJS broker timeout')), 10000);
        state.peer.on('open', () => { clearTimeout(timer); resolve(); });
        state.peer.on('error', (err) => { clearTimeout(timer); reject(err); });
      });
      state.peer.on('connection', setupHostConnection);
      publishInvites();
      report('Online host ready — one invite is available for each guest slot');
      return buildInvite(1);
    } catch (err) {
      state.role = 'idle';
      state.invites.clear();
      report(`Online unavailable: ${err.message || err}`);
      return null;
    }
  }

  async function startGuest() {
    if (state.role !== 'idle') return;
    const peerId = query('peer');
    const token = fragment('token') || query('token');
    const player = Number(fragment('player') || query('player'));
    const expires = Number(query('expires') || fragment('expires') || 0);
    const session = fragment('session') || query('session');
    if (!peerId || !token || !session || !Number.isInteger(player) || player < 1 || !expires || Date.now() >= expires) {
      report('Online invite is missing, expired, or invalid');
      return;
    }
    try {
      const Peer = peerConstructor();
      state.role = 'guest';
      state.peer = new Peer();
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('PeerJS broker timeout')), 10000);
        state.peer.on('open', () => { clearTimeout(timer); resolve(); });
        state.peer.on('error', (err) => { clearTimeout(timer); reject(err); });
      });
      state.host = state.peer.connect(peerId, { reliable: true });
      state.host.on('open', () => {
        send(state.host, { type: 'hello', token, player, protocol: 1 });
        report('Online guest: connecting to host…');
      });
      state.host.on('data', guestMessage);
      state.host.on('close', () => report('Online guest: host disconnected'));
      state.host.on('error', (err) => report(`Online guest: ${err.message || 'connection failed'}`));
    } catch (err) {
      state.role = 'idle';
      report(`Online unavailable: ${err.message || err}`);
    }
  }

  function sendInput(player, keys) {
    if (state.role !== 'guest') return false;
    send(state.host, { type: 'input', player, keys: keys >>> 0, t: performance.now() });
    return true;
  }

  // Host -> guest audio: coalesce, never drop. The pump delivers ~60 tagged
  // chunks/s (u32 LE rate + interleaved stereo s16). Small chunks are batched
  // into ~30ms sends: per-message overhead drops, and latency is bounded to
  // one batch window. The previous behavior DROPPED whole chunks whenever a
  // send was in flight — periodic sample gaps the guest hears as crackle.
  // The merge strips every per-chunk rate header and emits ONE tagged payload:
  // concatenating raw chunks would deliver header bytes as audio samples.
  function broadcastAudio(bytes) {
    if (state.role !== 'host' || !state.connections.length) return;
    const chunk = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (chunk.length < 5) return;
    const rate = new DataView(chunk.buffer, chunk.byteOffset, chunk.byteLength).getUint32(0, true);
    const samples = chunk.subarray(4);
    // Rate change (SOUNDBIAS switch): flush the old-rate batch first so the
    // guest's resampler never sees mixed-rate samples in one payload.
    if (state.audioPending && state.audioPending.length && rate !== state.audioRate) flushAudio();
    state.audioRate = rate;
    if (!state.audioPending) state.audioPending = [];
    state.audioPending.push(samples);
    let pending = 0;
    for (const c of state.audioPending) pending += c.length;
    // Overflow guard: if sends stall longer than the window allows, coalesce
    // away the OLDEST samples (head drop) rather than grow unbounded.
    while (pending > AUDIO_MAX_PENDING && state.audioPending.length > 1) {
      const dropped = state.audioPending.shift();
      pending -= dropped.length;
    }
    const now = performance.now();
    if (now - state.audioLastSent < AUDIO_BATCH_MS || state.audioBusy) return;
    flushAudio();
  }

  function flushAudio() {
    if (!state.audioPending || !state.audioPending.length || state.audioBusy) return;
    state.audioLastSent = performance.now();
    state.audioBusy = true;
    let total = 4;
    for (const c of state.audioPending) total += c.length;
    const merged = new Uint8Array(total);
    new DataView(merged.buffer).setUint32(0, state.audioRate, true);
    let off = 4;
    for (const c of state.audioPending) { merged.set(c, off); off += c.length; }
    state.audioPending = [];
    for (const conn of state.connections) send(conn, { type: 'audio', data: merged.buffer });
    queueMicrotask(() => { state.audioBusy = false; });
  }

  function init(callbacks) {
    state.callbacks = callbacks || {};
    if (query('online') === 'join') startGuest();
  }

  window.mgbaOnline = {
    init,
    startHost,
    sendInput,
    broadcastFrame,
    broadcastAudio,
    isGuest: () => state.role === 'guest',
    isHost: () => state.role === 'host',
    // Read-only introspection for the crash reporter's session details.
    assignedPlayer: () => state.assignedPlayer,
    guestCount: () => state.connections.length,
    invite: () => buildInvite(1),
    // Guest AV health probe (crash reporter / manual debugging).
    videoStats: () => ({ frames: state.videoFrames, bytes: state.videoBytes, fps: state.videoFps }),
  };
})();
