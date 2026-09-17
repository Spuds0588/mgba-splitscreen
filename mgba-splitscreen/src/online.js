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
    invites: new Map(), // player number -> { token, expires, consumed }
    session: null,
    assignedPlayer: 0,
    callbacks: {},
    lastFrame: null,
    frameBusy: false,
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

  function broadcastFrame(bytes) {
    if (state.role !== 'host' || !state.connections.length || state.frameBusy) return;
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
    invite: () => buildInvite(1),
  };
})();
