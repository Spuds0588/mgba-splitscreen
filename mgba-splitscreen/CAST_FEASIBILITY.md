# Cast to TV — feasibility research (2026-10-01, no code written)

Question: can the web app add a "Cast" button that puts the session on a big
screen with minimal lag, reusing the existing PeerJS/WebRTC layer? Scenario:
a phone runs 4 linked sessions and throws the picture at a TV.

## Verdict

**Feasible and worth building. Video lag is NOT the blocker.** The existing
PeerJS layer already solves the hard part (NAT-free peer video with ~10KB
frames); a cast is just a fifth peer that receives instead of plays. The real
constraint is the TV-side device, which must be chosen carefully — and the
recommended design (view-only cast, controls stay on phones) makes even
modest cast lag invisible to gameplay.

## What exists today (verified in repo)

- `src/online.js`: host-star PeerJS topology, WebRTC **DataChannels only**
  (no MediaStream/call() used yet). Per-seat JPEG video: 240x160 @ 0.55
  quality ≈ 10-20KB/frame, capped at 30 fps, encode ~1-2ms (`toDataURL`
  comment in `encodeAndSendFrame`), newest-frame-wins so a slow link never
  backlogs. Audio: ~30ms PCM batches, coalesced never dropped.
- `src/main.js`: host already composites every view mode (grid/speaker/
  focus/overlay) into canvases under `#screens` via `applyViewMode()`;
  `broadcastFrame` takes a seat->pixels provider (zero-copy views into the
  screens' ImageData). Guests decode JPEG and blit on rAF (`onFrame`).
- Invite/QR/share plumbing already exists (`buildInvite`, `?online=join`,
  expiring one-time tokens) — a cast invite is this same URL pattern.

## Latency profile (added by casting, LAN)

| Path | Added glass-to-glass | Notes |
| --- | --- | --- |
| A: reuse per-seat JPEG over DataChannel (5th peer) | ~25-50ms encode+send+decode+blit; ~50-150ms incl. Wi-Fi jitter | Matches online.js's "fraction of a second" design note; measured pieces are 1-2ms encode, few ms LAN DC, few ms decode, ≤16.7ms rAF |
| B: canvas.captureStream() → WebRTC media track via PeerJS call() | ~30-80ms typical LAN; tunable with playoutDelayHint | Media capture alone is 10-50ms (Ant Media); adds ~1 frame HW encode; VP8/H264 handled by the browser |

For reference, OS-level screen mirroring (Android Cast/Chromecast mirror of
the phone) is typically 100-300ms — the WebRTC paths above beat it, which is
the argument for building the button instead of telling people to mirror.

## The actual constraint: what's on the other end

- **Chromecast/Google TV cannot run a PeerJS page.** Cast devices only render
  castable media streams; a live WebRTC track needs a receiver app (Google
  Cast SDK custom receiver — heavy, breaks the "no backend" model).
- **Smart TV browsers** (Tizen, webOS, Android TV webviews) are weak and
  unreliable for WebRTC; not a supported target.
- **Practical TV receivers, best first:**
  1. Any laptop/PC with HDMI into the TV opening the viewer URL (trivially
     works — it's just Chrome).
  2. A second phone/tablet propped at the TV, optionally mirroring only
     itself to the TV for display (that mirror lag then doesn't matter for
     controls — nobody plays off the mirror).
  3. Chrome desktop "cast this tab" to a Chromecast from a laptop — adds
     mirror latency but the viewer page keeps decoding locally, so it stays
     watchable.

## Recommended architecture (v1 sketch, NOT built)

- **Viewer peer, not a guest.** Host message loop already iterates
  connections and encodes per seat; a viewer connection is one that receives
  *every* seat's JPEG instead of one. No new encoder, no new protocol type —
  just a role flag on hello + sending all seats to it. 4 seats ≈ 1.8MB/s at
  30 fps: fine on LAN (this is a living-room feature; don't optimize for WAN).
- **View-only, always.** The host already routes input by `conn.__player`;
  viewer connections get no player number, so they cannot send input. The
  phone in your hand stays the controller — cast lag never touches input
  latency. The TV shows the arena view; players watch their own screens for
  tight play (Four Swords party dynamic).
- **Viewer renders its own view mode.** It receives per-seat frames and can
  run the same VIEW_MODES grid/speaker/focus layout locally (CSS already
  does this), so the TV can show 2x2 grid or focus-P1 without host changes.
- **Optional later upgrade:** composite canvas + captureStream() MediaStream
  track (path B) for one hardware-encoded stream instead of 4 JPEGs — only
  worth it if phone CPU under 4-seat load is an issue, or for WAN cast.

## Caveats found during research

- Phone-as-host running 4 WASM cores at 60 fps is the heavier half of this
  scenario (battery/thermals for a multi-hour party: recommend a charger).
  That cost exists today with local play; casting adds only the ~1-2ms
  encodes per seat.
- Reliable DataChannel ordering can add retransmit delay on lossy Wi-Fi;
  the newest-frame-wins encoder already prevents backlog growth.
- The existing PeerJS default cloud broker ( signalling only, <1KB) is
  reused as-is; media never touches it.

## Decision record

- Build recommendation: **yes**, viewer-peer over the existing JPEG channel,
  view-only, viewer-side view modes, invite via the existing QR/URL flow.
- Explicit non-goals: Chromecast receiver app, smart-TV browser support,
  WAN-quality cast, viewer input.
- Research only per request; no code, no UI, no protocol changes made.
