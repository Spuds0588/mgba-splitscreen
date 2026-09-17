/* Optional v0.5 online transport.
 *
 * The host remains authoritative: it owns the mGBA/WASM session and guests send
 * only input messages. The current prototype uses PeerJS's public broker and
 * sends the latest composited RGBA frame over each data connection. It is
 * intentionally opt-in and does not affect local play when PeerJS is blocked.
 */
(() => {
  const state = {
    role: 'idle',
    peer: null,
    host: null,
    connections: [],
    token: null,
    session: null,
    assignedPlayer: 0,
    callbacks: {},
    lastFrame: null,
    frameBusy: false,
    inviteExpires: 0,
    inviteConsumed: false,
  };

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
  }

  function hostMessage(conn, message) {
    if (!message || typeof message !== 'object') return;
    if (message.type === 'hello') {
      const expired = !state.inviteExpires || Date.now() >= state.inviteExpires;
      // The capability is cryptographically random, time-limited, and consumed
      // before the welcome is sent. A copied link therefore cannot be reused,
      // even if the first guest disconnects immediately after joining.
      if (state.inviteConsumed || expired || message.token !== state.token) {
        send(conn, { type: 'error', message: expired ? 'This invite has expired' : 'This invite is no longer valid' });
        conn.close();
        return;
      }
      const used = new Set(state.connections.map((item) => item.__player).filter(Boolean));
      let player = 1;
      while (used.has(player) && player < 4) player++;
      if (player >= 4 && used.has(player)) {
        send(conn, { type: 'error', message: 'This session is full' });
        conn.close();
        return;
      }
      state.inviteConsumed = true;
      state.token = null;
      state.callbacks.inviteUsed?.();
      conn.__player = player;
      state.connections.push(conn);
      send(conn, {
        type: 'welcome',
        session: state.session,
        player,
        players: state.callbacks.playerCount?.() || 2,
      });
      report(`Online host: Player ${player + 1} joined (${state.connections.length} guest${state.connections.length === 1 ? '' : 's'})`);
      return;
    }
    if (message.type === 'input' && Number.isInteger(conn.__player)) {
      const player = conn.__player;
      if (Number.isInteger(message.keys) && state.callbacks.input) {
        state.callbacks.input(player, message.keys >>> 0);
      }
    }
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
      state.callbacks.playerCount?.(message.players || 2);
      report(`Online guest: connected as Player ${message.player + 1}`);
    } else if (message.type === 'frame' && message.data) {
      const bytes = message.data instanceof ArrayBuffer
        ? new Uint8Array(message.data)
        : new Uint8Array(message.data.buffer || message.data);
      state.callbacks.frame?.(bytes);
    } else if (message.type === 'error') {
      report(`Online guest: ${message.message || 'host rejected the connection'}`);
    }
  }

  function setupGuestConnection(conn) {
    state.host = conn;
    conn.on('open', () => {
      send(conn, { type: 'hello', token: state.token, protocol: 1 });
      report('Online guest: connecting to host…');
    });
    conn.on('data', guestMessage);
    conn.on('close', () => report('Online guest: host disconnected'));
    conn.on('error', (err) => report(`Online guest: ${err.message || 'connection failed'}`));
  }

  function buildInvite() {
    const url = new URL(window.location.href);
    url.search = '';
    url.hash = '';
    url.searchParams.set('online', 'join');
    url.searchParams.set('peer', state.peer.id);
    url.searchParams.set('expires', String(state.inviteExpires));
    url.searchParams.set('players', String(state.callbacks.playerCount?.() || 2));
    const rom = query('rom');
    if (rom) url.searchParams.set('rom', rom);
    // Keep the bearer token in the fragment: browsers do not send fragments in
    // HTTP Referer headers, reducing accidental leakage through third-party
    // resources. The host still validates it and consumes it once.
    url.hash = new URLSearchParams({
      session: state.session,
      token: state.token,
      expires: String(state.inviteExpires),
    }).toString();
    return url.href;
  }

  async function startHost() {
    // Re-issue a fresh capability after the one-time invite is consumed or
    // expires, while keeping the existing host PeerJS connection alive.
    if (state.role === 'host' && (state.inviteConsumed || Date.now() >= state.inviteExpires)) {
      state.session = randomToken(8);
      state.token = randomToken();
      state.inviteExpires = Date.now() + 10 * 60 * 1000;
      state.inviteConsumed = false;
      state.invite = buildInvite();
      state.callbacks.invite?.(state.invite);
      report('Online host: issued a fresh single-use invite');
      return state.invite;
    }
    if (state.role !== 'idle') return state.invite;
    try {
      const Peer = peerConstructor();
      state.role = 'host';
      state.session = randomToken(8);
      state.token = randomToken();
      state.inviteExpires = Date.now() + 10 * 60 * 1000;
      state.inviteConsumed = false;
      state.peer = new Peer();
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('PeerJS broker timeout')), 10000);
        state.peer.on('open', () => { clearTimeout(timer); resolve(); });
        state.peer.on('error', (err) => { clearTimeout(timer); reject(err); });
      });
      state.peer.on('connection', setupHostConnection);
      state.invite = buildInvite();
      state.callbacks.invite?.(state.invite);
      report('Online host ready — copy the invite link for guests');
      return state.invite;
    } catch (err) {
      state.role = 'idle';
      report(`Online unavailable: ${err.message || err}`);
      return null;
    }
  }

  async function startGuest() {
    if (state.role !== 'idle') return;
    const peerId = query('peer');
    state.token = fragment('token') || query('token');
    state.session = fragment('session') || query('session');
    state.inviteExpires = Number(query('expires') || fragment('expires') || 0);
    if (!peerId || !state.token || !state.session || !state.inviteExpires || Date.now() >= state.inviteExpires) {
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
      setupGuestConnection(state.peer.connect(peerId, { reliable: true }));
      report('Online guest: contacting host…');
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

  function broadcastFrame(bytes) {
    if (state.role !== 'host' || !state.connections.length || state.frameBusy) return;
    // Keep only the newest frame. A slow guest must never back up emulation.
    state.lastFrame = bytes instanceof Uint8Array ? bytes.slice() : new Uint8Array(bytes);
    state.frameBusy = true;
    const payload = state.lastFrame.buffer;
    for (const conn of state.connections) send(conn, { type: 'frame', data: payload });
    queueMicrotask(() => { state.frameBusy = false; });
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
    isGuest: () => state.role === 'guest',
    isHost: () => state.role === 'host',
    invite: () => state.invite || null,
  };
})();
