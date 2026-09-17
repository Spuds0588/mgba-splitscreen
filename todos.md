# mgba-splitscreen expansion roadmap

## v0.4 — local multi-system foundation

- [x] Make ROM loading platform-aware for GBA, GB, GBC, and supported GBX files.
- [x] Enable the native GB core and add a platform-neutral emulator-instance model.
- [ ] Support one ROM per local player, including same-ROM sessions and Pokémon Red/Blue-style combinations.
- [ ] Preserve separate per-player save identities and validate incompatible link topologies.
- [x] Integrate mGBA's existing two-device GB/GBC serial link while preserving 2–4-player GBA linking (WASM bridge; native Tauri wiring remains a follow-up).
- [ ] Refactor video dimensions, aspect handling, audio rates, and audio buffers per instance.
- [ ] Replace browser ScriptProcessor audio with AudioWorklet where supported, retaining a fallback.
- [ ] Add browser output-device selection with capability detection and Android system-route fallback.
- [x] Add v0.4 smoke coverage for ROM detection, GB/GBC loading, and two-player cooperative stepping.
- [ ] Add v0.4 tests for independent saves and topology validation.

## v0.5 — host-star online play and external sidebar

- [ ] Add a versioned PeerJS host-authoritative protocol.
- [ ] Keep ROMs and emulation on the host; guests send input and receive assigned video/audio streams.
- [ ] Add bounded queues, sequence numbers, timestamps, reconnect handling, rate limits, and host validation.
- [x] Prototype PeerJS host-star sessions and magic-link invitations with 10-minute expiry and first-join single-use invalidation.
- [ ] Add explicit host approval/revocation and a private signaling/auth service before production online play.
- [ ] Add an opt-in URL-controlled iframe sidebar with origin labeling, sandboxing, focus isolation, and HTTP warnings.
- [ ] Handle blocked embeds and camera/microphone/clipboard/fullscreen permissions explicitly.
- [x] Add the installable PWA shell with manifest, icon, service worker, mobile metadata, and deployer-compatible headers.
- [x] Add QR-code invite sharing with URL copy, PNG download, and native Web Share fallback.

## v0.6 — handheld and phone packaging

- [ ] Add touch controls and explicit TV/handheld input modes.
- [ ] Normalize Android key and gamepad handling, including safe BACK behavior.
- [ ] Build signed arm64 and armv7 APKs and verify phones, tablets, handhelds, external displays, and TV.
- [ ] Revisit the fixed WASM heap and low-memory WebView behavior.

## Current implementation slice

- [x] Expand ROM pickers, folder scanning, URL loading, and library filtering to recognize GB/GBC/GBX files.
- [x] Enable M_CORE_GB in the native build as groundwork; the full native platform-neutral instance refactor remains required before GB/GBC desktop linking is advertised.
- [x] Make the WASM bridge detect GB/GBC cores and expose dynamic per-instance metadata.
- [x] Wire the WASM bridge's two-player GB/GBC lockstep coordinator; separate-ROM sessions are intentionally deferred because the target audience has no practical use case beyond rare Pokémon trading.
- [x] Ignore the personal GB/GBC test ROM collection and generated save/state artifacts.
- [x] Verify GB/GBC detection, two-instance rendering, and cooperative stepping with the available `.gb` and `.gbc` ROMs.
- [ ] Verify a game-specific GB/GBC cable transaction when an appropriate test flow is available.
