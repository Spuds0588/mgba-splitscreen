# JOIN_CODES.md — 6-digit join codes for cross-platform online play

> **Implementation-ready spec.** Written 2026-09-28 so any session can build
> this without re-deriving decisions. Plan/rationale: `to-do.md` (online
> section) and `history.md` (2026-09-28 cross-platform entry). The protocol
> itself needs NO changes for mixed platforms — Android APK, desktop app,
> and every browser already run the same frontend and the same PeerJS
> messages. This spec is discovery/join UX only.

## Goal

A session like: **Android gaming handheld + phone browser + desktop app +
desktop browser**, where three guests join the host by typing or hearing a
**6-digit code** ("join 4 8 2 9 1 7") instead of passing long token URLs.
Magic links/QRs stay supported for scan/DM flows.

## Non-goals (deliberately)

- No signaling/registry backend. Codes map onto PeerJS's public broker.
- No accounts, matchmaking, or public room list.
- No TURN relay in this slice (strict-NAT pairs may fail to connect today;
  that is hardening, see "Parked risks").

## Design in one paragraph

The host's PeerJS **peer ID is derived from the code**: code `482917` → peer
ID `mgs-482917`. PeerJS's public broker already resolves arbitrary IDs, so a
guest who enters the code connects to that ID directly — the code *is* the
address. The ID exists only while the host's session is live, so codes are
ephemeral by construction. Because a code carries no secret (unlike today's
per-slot tokens), the host **approves** each incoming join before a seat is
assigned: a guessed code yields a declined prompt, never silent entry.

## Detailed design

### 1. Host side (`mgba-splitscreen/src/online.js`)

- `startHost()`: generate `code = 6 random digits (100000..999999)`, call
  `new Peer('mgs-' + code)`. The broker rejects taken IDs with
  `PeerError('unavailable-id')` → catch, regenerate, retry (max ~5, then
  surface "broker busy" in the status line). Keep the existing
  `state.session`/token invite machinery untouched — both entry methods run
  in parallel on the same peer.
- Expose `currentCode()` and `pendingJoins()` on `window.mgbaOnline` for the
  UI and the crash-reporter session details.
- **New message: guest → host `join_request`** (replaces bare `hello` for
  code joins; `hello` stays for token invites):
  `{ type: 'join_request', protocol: 1 }`
- **New message: host → guest** after approval:
  `{ type: 'welcome', session, player, players }` (unchanged shape — the
  guest pipeline needs nothing new), or on decline:
  `{ type: 'error', message: 'The host declined' | 'The game is full' | ... }`
  then `conn.close()`.
- **Approval flow in `hostMessage()`:** a `join_request` connection is NOT
  added to `state.connections` yet. Push it onto
  `state.pendingJoins: [{ conn, requestedAt }]` and fire a new callback
  `callbacks.joinRequest({ approve(seat), deny(reason) })`. The host UI
  renders the prompt; unresponded requests auto-deny after 30s (timer in
  online.js; the connection may also close first — handle idempotently).
  `approve(seat)` does what `hello` handling does today: mark conn with
  `__player`, push to connections, send `welcome`, publish invites/status.
  Seat = lowest free of `1..playerCount-1` chosen by the host UI (default
  suggestion: lowest free).
- Player count for seats comes from `guestCount()` (the host's configured
  player count) as today; if all seats are full, auto-decline with
  "The game is full" **without** prompting the host.
- Rate-limit the prompts: if a second request arrives while one is pending,
  queue it; never stack more than 2 prompts.

### 2. Guest side (`mgba-splitscreen/src/online.js` + UI)

- `startGuest()` gains a code path: `?online=join&code=NNNNNN` (query param
  takes precedence; fragment not needed since codes are not secrets). It
  connects with `state.peer.connect('mgs-' + code, { reliable: true })` and
  sends `join_request` on open. Token-invite behavior is unchanged when
  `token` is present instead.
- Connection states to surface in the status line (keep the
  `Online guest:` prefix — `body.guest-session` CSS hides the menubar off
  it): `Connecting to code 482 917…`, `Waiting for the host to accept…`,
  `The host declined`, `No game found with that code` (broker
  `peer-unavailable` error → friendly text), `The game is full`.

### 3. UI (`index.html` + `styles.css` + `main.js`)

- **Host panel (Online menu):** after "Host Online Session", show
  `Your join code:` + the code in large monospace grouped `482 917` +
  Copy + QR (QR encodes `<origin>/?online=join&code=NNNNNN`) + Share. The
  per-slot token buttons stay below under a "or share a one-time link"
  note.
- **Join prompt (host, any platform):** a small overlay
  (`#online-join-prompt`, pattern-match the existing `#online-qr-overlay`
  structure/z-index): "Player wants to join with code 482 917" + seat
  buttons (P2/P3/P4, only free seats) + Decline. Buttons must be real
  `<button>`s so D-pad navigation works on Android TV (same pattern as the
  pause menu). While emulation runs the prompt must not steal game input —
  it is a modal like the QR dialog; game keys are held in `keyStates` and
  unaffected.
- **Guest keypad:** Online menu gains "Join with Code…". Overlay with six
  digit boxes + 0-9 keypad (`inputmode=numeric` on a hidden input for
  hardware keyboards), Backspace, auto-submit on the 6th digit, paste
  support (strip non-digits, tolerate the `482 917` grouping). Error banner
  inside the overlay for the failure states above. Keep it usable on a
  phone: big targets, `touch-action: manipulation`.
- `main.js` wiring: pass `joinRequest` through `mgbaOnline.init({...})`;
  render the prompt; on approve call the callback with the seat. Status
  strings go through `setStatus` as usual (mind the `guest-session` class).

### 4. Compatibility

- Token invite URLs (`#token=…&player=…`) keep working exactly as today —
  no consent prompt (they are bearer secrets). Both flows can be active in
  one session.
- Old cached shells (v6 SW) still connect to new hosts? Token invites: yes.
  Code joins: guests need the new code path, so a stale guest just won't
  see "Join with Code…" — acceptable; bump SW to v7 with this change.
- `crash-report.js`: add the join code + pending-join count to the Online
  session details (via `mgbaOnline.currentCode()`).

### 5. Edge cases checklist

- [ ] Host reloads/regenerates → new code; old code dies with the old peer.
- [ ] Guest connects then walks away pre-approval → 30s auto-deny, no leak.
- [ ] All seats full → auto-decline without host prompt.
- [ ] Two guests approved to the same seat → impossible: approve(seat) must
      re-check freeness and fail loudly (console.warn) if raced.
- [ ] Host declines while guest already got `welcome` → impossible: decline
      only pre-welcome; post-welcome departure is the normal close path.
- [ ] Broker down → existing 10s timeout message covers both paths.
- [ ] `?code=` AND `#token=` both present → token wins (stronger auth), note
      in status.

### 6. Parked risks (not this slice)

- **NAT traversal:** PeerJS defaults to STUN only; some phone-hotspot pairs
  will fail (`peer-unavailable` never fires — the connection just never
  opens). Fix later: optional TURN (env/config for credentials, `iceServers`
  in both `new Peer` calls).
- **Code squatting/abuse:** pointless today (no directory); if codes ever
  become guessable-and-valuable, move to a tiny KV registry or 7 digits.
- **Host uplink with 3 guests** pulling per-seat JPEG at 30fps is untested.

### 7. Verification plan (mirror the 2026-09-28 2-tab sweep)

1. `node --check` all touched JS. SW bump v6→v7 (no new files, but content
   changed) + `?crashreport=test` smoke on staging.
2. Staging (`python3 -m http.server 8093` on `.freebuff/pages-staging`):
   host Mario Kart (`?rom=`), note the code; guest tab joins via keypad →
   host approves P2 → verify: single-seat view, P2 tag, menubar hidden,
   `videoStats()` climbing, audio buffered pinned ~32768, input spy sees
   RIGHT+A on seat 1.
3. Decline path: second guest tab → host declines → guest shows declined,
   host still streams to P2.
4. Full-game path: approve into a full 2P game → auto-decline message.
5. Token URL join still works alongside a code join in the same session.
6. Deep link: guest opens `?online=join&code=…` cold (no keypad).
7. Update `history.md` + tick the `to-do.md` phases; no engine/C changes.

### 8. Suggested build order (one session)

Phases 1-2 (code host + derived ID + approval flow) → phase 3 (keypad UI) →
the verification sweep → SW bump → commit/merge/deploy → optionally cut
v0.3.2 so testers get join codes + the stable signing key in one build.
