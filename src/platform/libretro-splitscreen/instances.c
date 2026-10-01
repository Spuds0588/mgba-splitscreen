/* Copyright (c) 2026 mgba-splitscreen contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Instance manager: N linked mGBA cores on one thread. The frame loop is a C
 * port of the Rust EmulationManager's cooperative stepping
 * (mgba-splitscreen/src-tauri/src/emulation.rs): every player is advanced one
 * video frame by budgeted runLoop() steps, switching players whenever the
 * lockstep SIO driver puts one to sleep — the other player's transfers are
 * what wake it. Video composites the last COMPLETE frame per player
 * (snapshot on frame-counter change, before the ROM's vblank handler clears
 * the buffer), audio mixes per the selected source.
 */
#include "instances.h"

#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/core/timing.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#include <stdio.h>
#include <string.h>

#include <stddef.h> /* offsetof */

/* Livelock guard, same cap as the Rust loop. */
#define SP_MAX_STEPS 100000
/* Fallback per-frame cycle budget for an unrecognized core (~280896 for GBA). */
#define SP_FRAME_CYCLES_FALLBACK 280896

/* The lockstep callbacks recover the owning sp_player by casting the passed
 * &p->lockstepUser back to struct sp_player*, which is only valid when the
 * lockstepUser member sits at offset 0. Keep this assert with the struct. */
_Static_assert(offsetof(struct sp_player, lockstepUser) == 0,
               "mLockstepUser must be the first member of sp_player");

static void _spLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void (*spLogCallback)(const char* line) = NULL; /* wired by libretro.c */

static void _spLog(const char* fmt, ...) {
	char line[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	if (spLogCallback) {
		spLogCallback(line);
	}
}

/* ---- Lockstep user callbacks (set the sleep flag; see emulation.rs) ---- */

static void _lockstepSleep(struct mLockstepUser* user) {
	struct sp_player* p = (struct sp_player*) user;
	p->asleep = true;
}

static void _lockstepWake(struct mLockstepUser* user) {
	struct sp_player* p = (struct sp_player*) user;
	p->asleep = false;
}

static int _lockstepRequestedId(struct mLockstepUser* user) {
	/* The driver asks which wire id this player wants; report the assigned
	 * slot (0..3). The coordinator renumbers by attach order anyway. */
	struct sp_player* p = (struct sp_player*) user;
	return p->playerId;
}

static void _lockstepPlayerIdChanged(struct mLockstepUser* user, int id) {
	struct sp_player* p = (struct sp_player*) user;
	p->playerId = id;
	_spLog("[splitscreen] player assigned wire id %d", id);
}

/* ---- Per-player audio pacing (port of the upstream core's leaky average) ---- */

#define SP_AUDIO_ALPHA (1.0f / 180.0f)

/* ---- Loading ---- */

static bool _spLoadOne(struct sp_player* p, int index, const void* rom, size_t romSize) {
	memset(p, 0, sizeof(*p));
	p->playerId = index;

	/* Per-player ROM copy: save data must not alias between players. */
	p->romData = anonymousMemoryMap(romSize);
	if (!p->romData) {
		return false;
	}
	memcpy(p->romData, rom, romSize);
	p->romSize = romSize;

	struct VFile* vfm = VFileFromMemory(p->romData, romSize);
	if (!vfm) {
		return false;
	}

	p->core = mCoreFindVF(vfm);
	if (!p->core) {
		vfm->close(vfm);
		return false;
	}
	if (p->core->platform(p->core) != mPLATFORM_GBA) {
		_spLog("[splitscreen] player %d: not a GBA ROM (GB/GBC in the GBA link subsystem is rejected)", index);
		vfm->close(vfm);
		return false;
	}

	mCoreInitConfig(p->core, NULL);
	/* Repo gotcha: core->init() BEFORE handing the core any buffers. */
	p->core->init(p->core);

	p->videoBuf = anonymousMemoryMap(SP_VIDEO_W * SP_VIDEO_H * BYTES_PER_PIXEL);
	if (!p->videoBuf) {
		return false;
	}
	memset(p->videoBuf, 0xFF, SP_VIDEO_W * SP_VIDEO_H * BYTES_PER_PIXEL);
	p->core->setVideoBuffer(p->core, p->videoBuf, SP_VIDEO_W);
	p->audio = p->core->getAudioBuffer(p->core);

	p->snapshot = anonymousMemoryMap(SP_VIDEO_W * SP_VIDEO_H * BYTES_PER_PIXEL);
	if (!p->snapshot) {
		return false;
	}
	memset(p->snapshot, 0, SP_VIDEO_W * SP_VIDEO_H * BYTES_PER_PIXEL);

	/* Audio: GBA produces ~533 stereo samples per frame at 32768 Hz. Give the
	 * internal buffer 2x headroom (blip buffer limit is 0x4000). */
	p->core->setAudioBufferSize(p->core, SP_AUDIO_SAMPLES_PER_FRAME * 2);

	/* Battery save: flash-1M-sized buffer per player, wired after reset like
	 * the upstream core's deferred setup (the frontend may fill it late). */
	p->saveData = anonymousMemoryMap(GBA_SIZE_FLASH1M);
	if (!p->saveData) {
		return false;
	}
	memset(p->saveData, 0xFF, GBA_SIZE_FLASH1M);
	p->saveSize = GBA_SIZE_FLASH1M;

	/* Link driver: create + attach BEFORE reset, or games see the cable
	 * mid-boot (repo gotcha). */
	p->lockstepUser.sleep = _lockstepSleep;
	p->lockstepUser.wake = _lockstepWake;
	p->lockstepUser.requestedId = _lockstepRequestedId;
	p->lockstepUser.playerIdChanged = _lockstepPlayerIdChanged;
	GBASIOLockstepDriverCreate(&p->driver, &p->lockstepUser);

	p->core->loadROM(p->core, vfm);
	return true;
}

static void _spAttachLink(struct sp_manager* sp) {
	int n = sp->nPlayers;
	if (n < 2) {
		return;
	}
	GBASIOLockstepCoordinatorInit(&sp->coordinator);
	for (int i = 0; i < n; ++i) {
		GBASIOLockstepCoordinatorAttach(&sp->coordinator, &sp->players[i].driver);
	}
	sp->linkAttached = true;
	_spLog("[splitscreen] link attached: %d players on the virtual cable", n);
}

static void _spFinishLoad(struct sp_manager* sp, struct sp_video* vid) {
	/* Attach link first, then load saves + reset (upstream defers save wiring
	 * until after loadROM because the frontend copies into the buffer late;
	 * here the buffer is ours, so wiring it before reset is safe and simpler). */
	_spAttachLink(sp);
	for (int i = 0; i < sp->nPlayers; ++i) {
		struct sp_player* p = &sp->players[i];
		struct VFile* save = VFileFromMemory(p->saveData, GBA_SIZE_FLASH1M);
		if (!p->core->loadSave(p->core, save)) {
			save->close(save);
		}
		if (sp->linkAttached) {
			p->core->setPeripheral(p->core, mPERIPH_GBA_LINK_PORT, &p->driver);
		}
		p->core->reset(p->core);
		p->lastFrameCounter = p->core->frameCounter(p->core);
		(void) vid;
	}
}

bool sp_load(struct sp_manager* sp, int nPlayers, const void* rom, size_t romSize,
             struct sp_video* vid) {
	memset(sp, 0, sizeof(*sp));
	if (nPlayers < 1 || nPlayers > SP_MAX_PLAYERS || !rom || !romSize) {
		return false;
	}
	sp->nPlayers = nPlayers;
	bool ok = true;
	for (int i = 0; i < nPlayers && ok; ++i) {
		ok = _spLoadOne(&sp->players[i], i, rom, romSize);
		if (!ok) {
			_spLog("[splitscreen] player %d failed to load", i);
		}
	}
	if (ok) {
		_spFinishLoad(sp, vid);
	} else {
		sp_deinit(sp);
	}
	return ok;
}

void sp_deinit(struct sp_manager* sp) {
	for (int i = 0; i < SP_MAX_PLAYERS; ++i) {
		struct sp_player* p = &sp->players[i];
		if (p->core) {
			mCoreConfigDeinit(&p->core->config);
			p->core->deinit(p->core);
			p->core = NULL;
		}
		if (p->videoBuf) {
			mappedMemoryFree(p->videoBuf, SP_VIDEO_W * SP_VIDEO_H * BYTES_PER_PIXEL);
			p->videoBuf = NULL;
		}
		if (p->snapshot) {
			mappedMemoryFree(p->snapshot, SP_VIDEO_W * SP_VIDEO_H * BYTES_PER_PIXEL);
			p->snapshot = NULL;
		}
		if (p->saveData) {
			mappedMemoryFree(p->saveData, GBA_SIZE_FLASH1M);
			p->saveData = NULL;
		}
		p->saveSize = 0;
		if (p->romData) {
			mappedMemoryFree(p->romData, p->romSize);
			p->romData = NULL;
		}
	}
	if (sp->linkAttached) {
		GBASIOLockstepCoordinatorDeinit(&sp->coordinator);
		sp->linkAttached = false;
	}
	sp->nPlayers = 0;
}

void sp_set_fs_assist(struct sp_manager* sp, bool on) {
	if (sp->linkAttached) {
		/* Default OFF matches the desktop/web apps; the driver's own hard sync
		 * is the real fix (RETROARCH_CORE.md section 8). */
		GBASIOLockstepCoordinatorSetFSSuppressed(&sp->coordinator, !on);
		sp->fsSuppressed = !on;
	}
}

void sp_reset(struct sp_manager* sp) {
	for (int i = 0; i < sp->nPlayers; ++i) {
		sp->players[i].asleep = false;
		sp->players[i].lastFrameCounter = sp->players[i].core->frameCounter(sp->players[i].core);
	}
	/* The coordinator carries round state across a reset; tear the link layer
	 * down cleanly and re-attach (mirrors the app's mgs_reset_sio after state
	 * import). Drivers must be detached BEFORE the coordinator dies, or their
	 * stale lockstepIds point into a freed table on the next reset. */
	if (sp->linkAttached) {
		for (int i = 0; i < sp->nPlayers; ++i) {
			GBASIOLockstepCoordinatorDetach(&sp->coordinator, &sp->players[i].driver);
		}
		GBASIOLockstepCoordinatorDeinit(&sp->coordinator);
		GBASIOLockstepCoordinatorInit(&sp->coordinator);
		for (int i = 0; i < sp->nPlayers; ++i) {
			GBASIOLockstepCoordinatorAttach(&sp->coordinator, &sp->players[i].driver);
		}
		/* CoordinatorInit restores library defaults; re-apply the host's
		 * FS-assist policy so a reset can't silently re-enable the kick. */
		GBASIOLockstepCoordinatorSetFSSuppressed(&sp->coordinator, sp->fsSuppressed);
	}
	for (int i = 0; i < sp->nPlayers; ++i) {
		sp->players[i].core->reset(sp->players[i].core);
	}
	_spLog("[splitscreen] %d players reset", sp->nPlayers);
}

/* ---- Frame loop (port of the Rust cooperative stepping) ---- */

bool sp_run_frame(struct sp_manager* sp, void (*poll)(void),
                  uint32_t (*readKeys)(unsigned port)) {
	if (poll) {
		poll();
	}

	int n = sp->nPlayers;
	int32_t budget[SP_MAX_PLAYERS];
	int32_t frameCycles[SP_MAX_PLAYERS];
	bool anyCore = false;
	for (int i = 0; i < n; ++i) {
		struct sp_player* p = &sp->players[i];
		uint32_t keys = readKeys ? readKeys((unsigned) i) : 0;
		p->core->setKeys(p->core, keys);
		int32_t fc = p->core->frameCycles(p->core);
		if (fc <= 0) {
			fc = SP_FRAME_CYCLES_FALLBACK;
		}
		frameCycles[i] = fc;
		budget[i] = fc;
		anyCore = true;
	}
	(void) anyCore;

	bool progress = true;
	int32_t steps = 0;
	while (progress && steps < SP_MAX_STEPS) {
		progress = false;
		for (int i = 0; i < n; ++i) {
			struct sp_player* p = &sp->players[i];
			if (budget[i] <= 0 || p->asleep) {
				continue;
			}
			progress = true;
			int32_t before = mTimingCurrentTime(p->core->timing);
			p->core->runLoop(p->core);
			int32_t delta = mTimingCurrentTime(p->core->timing) - before;
			if (delta < 1) {
				delta = 1;
			}
			budget[i] -= delta;
			++steps;
			/* Frame complete? Snapshot the fully-drawn buffer before the
			 * ROM's vblank handler can clear it (tear-free composite source). */
			uint32_t fc = p->core->frameCounter(p->core);
			if (fc != p->lastFrameCounter) {
				p->lastFrameCounter = fc;
				memcpy(p->snapshot, p->videoBuf, SP_VIDEO_W * SP_VIDEO_H * BYTES_PER_PIXEL);
				++p->framesProduced;
			}
		}
	}
	if (steps >= SP_MAX_STEPS && progress) {
		/* Livelock bailout: leave players on their last complete frames and
		 * keep the frontend's audio thread alive (log-once would spam; the
		 * condition is self-reporting via the next frame's snapshot age). */
		return false;
	}
	if (steps == 0) {
		/* Every player slept without stepping: the link layer deadlocked
		 * (all cores asleep waiting on a barrier that nothing will clear --
		 * a sleeping core cannot service its own event queue). Self-heal:
		 * after ~60 frozen frames (~1 s) clear the coordinator barrier and
		 * wake everyone so the games re-handshake; the games treat it like
		 * link static (see GBASIOLockstepCoordinatorRecover). */
		if (sp->stallRun < 90) {
			++sp->stallRun;
		}
		if (sp->stallRun == 30 && sp->linkAttached) {
			int asleep = 0;
			for (int i = 0; i < n; ++i) {
				asleep += sp->players[i].asleep;
			}
			_spLog("[splitscreen] STALL: 0 steps on all %d players for ~%d frames; asleep=%d, transferActive=%d, waiting=%u", n, sp->stallRun, asleep, sp->coordinator.transferActive, sp->coordinator.waiting);
		}
		if (sp->stallRun >= 60 && sp->linkAttached) {
			GBASIOLockstepCoordinatorRecover(&sp->coordinator);
			sp->stallRun = 0;
		}
	} else {
		sp->stallRun = 0;
	}
	return true;
}

/* ---- Video composition ---- */

/* Player color palette, ported from the app (src/styles.css): P1 red,
 * P2 blue, P3 green, P4 orange — outlines and P-number badges. */
const uint16_t spPlayerColors[SP_MAX_PLAYERS] = {
	0xF908, /* #ff4444 */
	0x445F, /* #4488ff */
	0x2D48, /* #2eaa44 */
	0xE443, /* #e08a1e */
};

static void _hLine(struct sp_video* vid, int x0, int x1, int y, uint16_t color) {
	if (y < 0 || y >= (int) vid->height) {
		return;
	}
	if (x0 < 0) {
		x0 = 0;
	}
	if (x1 >= (int) vid->width) {
		x1 = (int) vid->width - 1;
	}
	for (int x = x0; x <= x1; ++x) {
		vid->out[y * vid->width + x] = color;
	}
}

static void _vLine(struct sp_video* vid, int y0, int y1, int x, uint16_t color) {
	if (x < 0 || x >= (int) vid->width) {
		return;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (y1 >= (int) vid->height) {
		y1 = (int) vid->height - 1;
	}
	for (int y = y0; y <= y1; ++y) {
		vid->out[y * vid->width + x] = color;
	}
}

static void _fillRect(struct sp_video* vid, int ox, int oy, int w, int h, uint16_t color) {
	for (int y = oy; y < oy + h; ++y) {
		_hLine(vid, ox, ox + w - 1, y, color);
	}
}

/* 3x5 glyphs (MSB = left column) for "P" and digits 1-4. */
static const uint8_t _glyphP[5] = { 0x7, 0x5, 0x7, 0x4, 0x4 };
/* Digits from Tom Thumb (same source as the boot-menu font), so the
 * badges match the menu and no glyph is hand-drawn. */
static const uint8_t _glyphDigits[4][5] = {
	{ 0x2, 0x6, 0x2, 0x2, 0x2 }, /* 1 */
	{ 0x6, 0x1, 0x2, 0x4, 0x7 }, /* 2 */
	{ 0x6, 0x1, 0x2, 0x1, 0x6 }, /* 3 */
	{ 0x5, 0x5, 0x7, 0x1, 0x1 }, /* 4 */
};

static void _drawPText(struct sp_video* vid, int ox, int oy, int player) {
	const uint8_t* glyphs[2] = { _glyphP, _glyphDigits[player & 3] };
	for (int gi = 0; gi < 2; ++gi) {
		for (int row = 0; row < 5; ++row) {
			for (int col = 0; col < 3; ++col) {
				if (glyphs[gi][row] & (0x4 >> col)) {
					int px = ox + gi * 4 + col;
					int py = oy + row;
					if (px >= 0 && px < (int) vid->width && py >= 0 && py < (int) vid->height) {
						vid->out[py * vid->width + px] = 0xFFFF;
					}
				}
			}
		}
	}
}

/* One player's screen decoration: colored inset outline (the app's
 * .screen-cell) plus a bottom-right "P<n>" badge on the player's color
 * (the app's .screen-tag). Drawn AFTER the pixels, so it survives every
 * layout at any scale. */
static void _drawOverlayCell(struct sp_video* vid, int player, int ox, int oy,
                             int w, int h, bool draw) {
	if (!draw || player < 0 || player >= SP_MAX_PLAYERS) {
		return;
	}
	const uint16_t color = spPlayerColors[player];
	for (int i = 0; i < SP_OUTLINE_PX; ++i) {
		int x0 = ox + i;
		int y0 = oy + i;
		int x1 = ox + w - 1 - i;
		int y1 = oy + h - 1 - i;
		if (x0 > x1 || y0 > y1) {
			break;
		}
		_hLine(vid, x0, x1, y0, color);
		_hLine(vid, x0, x1, y1, color);
		_vLine(vid, y0, y1, x0, color);
		_vLine(vid, y0, y1, x1, color);
	}
	int bw = SP_TAG_W;
	int bh = SP_TAG_H;
	int bx = ox + w - bw - 1;
	int by = oy + h - bh - 1;
	if (bx < ox + SP_OUTLINE_PX) {
		bx = ox + SP_OUTLINE_PX;
	}
	if (by < oy + SP_OUTLINE_PX) {
		by = oy + SP_OUTLINE_PX;
	}
	_fillRect(vid, bx, by, bw, bh, color);
	_drawPText(vid, bx + (bw - 7) / 2, by + (bh - 5) / 2, player);
}

static void _blitRow(mColor* dst, const mColor* src, int w) {
	memcpy(dst, src, w * BYTES_PER_PIXEL);
}

/* Blit a snapshot at half scale (nearest 2x2 downsample), used by overlay
 * PiPs and narrow speaker-strip cells. Clips against the WxH output. */
static void _blitHalf(struct sp_video* vid, const mColor* snap, int ox, int oy) {
	const int W = (int) vid->width;
	const int H = (int) vid->height;
	for (int y = 0; y < SP_VIDEO_H; y += 2) {
		int dy = oy + y / 2;
		if (dy >= H) {
			break;
		}
		if (dy < 0) {
			continue;
		}
		mColor* dstRow = vid->out + dy * W;
		const mColor* srcRow = snap + y * SP_VIDEO_W;
		for (int x = 0; x < SP_VIDEO_W; x += 2) {
			int dx = ox + x / 2;
			if (dx >= 0 && dx < W) {
				dstRow[dx] = srcRow[x];
			}
		}
	}
}

/* Blit one player's snapshot at integer scale `scale` with top-left (ox, oy);
 * clips against the WxH output. Grid uses scale 1; the focused views use 2. */
static void _blitScaled(struct sp_video* vid, const mColor* snap, int ox, int oy, int scale) {
	const int W = (int) vid->width;
	const int H = (int) vid->height;
	const int sw = SP_VIDEO_W * scale;
	const int sh = SP_VIDEO_H * scale;
	if (ox >= W || oy >= H || ox + sw <= 0 || oy + sh <= 0) {
		return;
	}
	for (int y = 0; y < sh; ++y) {
		int dy = oy + y;
		if (dy < 0 || dy >= H) {
			continue;
		}
		const mColor* srcRow = snap + (y / scale) * SP_VIDEO_W;
		mColor* dstRow = vid->out + dy * W;
		for (int x = 0; x < sw; ++x) {
			int dx = ox + x;
			if (dx < 0 || dx >= W) {
				continue;
			}
			dstRow[dx] = srcRow[x / scale];
		}
	}
}

void sp_composite(struct sp_manager* sp, struct sp_video* vid, bool overlays) {
	const int W = (int) vid->width;
	const int H = (int) vid->height;
	memset(vid->out, 0, vid->outSize);
	int n = sp->nPlayers;

	/* Clamp the focused player into range; grid layouts ignore it. */
	int f = vid->focused;
	if (f < 0 || f >= n) {
		f = 0;
	}

	enum spLayout layout = vid->layout;
	if (layout == SP_LAYOUT_FOCUS) {
		/* App's focus view: the focused player alone, 2x. */
		_blitScaled(vid, sp->players[f].snapshot, 0, 0, 2);
		_drawOverlayCell(vid, f, 0, 0, SP_VIDEO_W * 2, SP_VIDEO_H * 2, overlays);
		return;
	}
	if (layout == SP_LAYOUT_SPEAKER) {
		/* App's speaker view: focused 2x centered on top, remaining players
		 * in a 1x strip beneath. */
		int bigX = (W - SP_VIDEO_W * 2) / 2;
		_blitScaled(vid, sp->players[f].snapshot, bigX, 0, 2);
		_drawOverlayCell(vid, f, bigX, 0, SP_VIDEO_W * 2, SP_VIDEO_H * 2, overlays);
		int rest = n - 1;
		if (rest > 0) {
			int stripY = SP_VIDEO_H * 2;
			int stripH = H - stripY;
			int step = W / rest; /* equal cells across the full width */
			/* Cells narrower than a full screen drop to half scale so the
			 * strip never overlaps (4P: three 160px cells). */
			int half = step < SP_VIDEO_W;
			int dw = half ? SP_VIDEO_W / 2 : SP_VIDEO_W;
			int dh = half ? SP_VIDEO_H / 2 : SP_VIDEO_H;
			int ox = (W - step * rest) / 2; /* center the group of cells */
			int cy = stripY + (stripH - dh) / 2;
			if (cy < stripY) {
				cy = stripY;
			}
			int idx = 0;
			for (int i = 0; i < n; ++i) {
				if (i == f) {
					continue;
				}
				int cx = ox + step * idx + (step - dw) / 2;
				if (half) {
					_blitHalf(vid, sp->players[i].snapshot, cx, cy);
				} else {
					_blitScaled(vid, sp->players[i].snapshot, cx, cy, 1);
				}
				_drawOverlayCell(vid, i, cx, cy, dw, dh, overlays);
				++idx;
			}
		}
		return;
	}
	if (layout == SP_LAYOUT_OVERLAY) {
		/* App's overlay view: focused 2x fills the frame, others as small
		 * (half-scale) PiPs along the right edge. */
		_blitScaled(vid, sp->players[f].snapshot, 0, 0, 2);
		_drawOverlayCell(vid, f, 0, 0, SP_VIDEO_W * 2, SP_VIDEO_H * 2, overlays);
		const int pipW = SP_VIDEO_W / 2;
		int idx = 0;
		for (int i = 0; i < n; ++i) {
			if (i == f) {
				continue;
			}
			int py = idx * (SP_VIDEO_H / 2);
			_blitHalf(vid, sp->players[i].snapshot, W - pipW, py);
			_drawOverlayCell(vid, i, W - pipW, py, pipW, SP_VIDEO_H / 2, overlays);
			++idx;
		}
		return;
	}

	/* Grid layouts (2x1 / 1x2 / 2x2 / auto). */
	int cols, rows;
	switch (layout) {
	case SP_LAYOUT_2X1:
		cols = 2;
		rows = 1;
		break;
	case SP_LAYOUT_1X2:
		cols = 1;
		rows = 2;
		break;
	case SP_LAYOUT_2X2:
		cols = 2;
		rows = 2;
		break;
	case SP_LAYOUT_AUTO:
	default:
		if (n <= 2) {
			cols = 2;
			rows = 1;
		} else {
			cols = 2;
			rows = 2;
		}
		break;
	}
	if (cols * rows < n) {
		/* Requested layout can't hold the players: fall back to quadrant. */
		cols = 2;
		rows = 2;
	}
	int cellW = W / cols;
	int cellH = H / rows;

	for (int i = 0; i < n; ++i) {
		struct sp_player* p = &sp->players[i];
		int col = i % cols;
		int row = i / cols;
		int ox = col * cellW + (cellW - SP_VIDEO_W) / 2;
		int oy = row * cellH + (cellH - SP_VIDEO_H) / 2;
		if (ox < 0) {
			ox = 0;
		}
		if (oy < 0) {
			oy = 0;
		}
		for (int y = 0; y < SP_VIDEO_H; ++y) {
			int dy = oy + y;
			if (dy >= H) {
				break;
			}
			int dx = ox;
			if (dx + SP_VIDEO_W > W) {
				/* Would overflow: clamp the source columns. */
				int visible = W - dx;
				if (visible <= 0) {
					break;
				}
				_blitRow(vid->out + dy * W + dx, p->snapshot + y * SP_VIDEO_W, visible);
				continue;
			}
			_blitRow(vid->out + dy * W + dx, p->snapshot + y * SP_VIDEO_W, SP_VIDEO_W);
		}
		_drawOverlayCell(vid, i, ox, oy, SP_VIDEO_W, SP_VIDEO_H, overlays);
	}
}

/* ---- Audio ---- */

size_t sp_audio(struct sp_manager* sp, enum spAudio source, int16_t* out, size_t samples) {
	int n = sp->nPlayers;
	/* Pull each player's generated samples into scratch and mix. Scratch is
	 * fixed-size: at 60 fps a player never produces more than ~533/frame plus
	 * jitter, 4x is generous. */
	static int16_t scratch[SP_MAX_PLAYERS][SP_AUDIO_SAMPLES_PER_FRAME * 4];
	size_t produced = 0;

	/* Read every player first: mAudioBufferRead consumes, so all players must
	 * drain every frame regardless of the audio option (otherwise a player's
	 * buffer fills and the emulator blocks). */
	size_t got[SP_MAX_PLAYERS];
	for (int i = 0; i < n; ++i) {
		struct sp_player* p = &sp->players[i];
		size_t avail = mAudioBufferAvailable(p->audio);
		if (avail > SP_AUDIO_SAMPLES_PER_FRAME * 4) {
			avail = SP_AUDIO_SAMPLES_PER_FRAME * 4;
		}
		if (avail > samples) {
			avail = samples;
		}
		got[i] = mAudioBufferRead(p->audio, scratch[i], avail);
		if (got[i] > produced) {
			produced = got[i];
		}
	}
	if (!produced) {
		return 0;
	}

	if (source >= SP_AUDIO_MIX) {
		/* Saturating mix across all players. */
		for (size_t s = 0; s < produced; ++s) {
			int32_t left = 0, right = 0;
			for (int i = 0; i < n; ++i) {
				if (got[i] <= s) {
					continue;
				}
				left += scratch[i][s * 2];
				right += scratch[i][s * 2 + 1];
			}
			/* Saturate to int16. */
			if (left > 0x7FFF) left = 0x7FFF;
			if (left < -0x8000) left = -0x8000;
			if (right > 0x7FFF) right = 0x7FFF;
			if (right < -0x8000) right = -0x8000;
			out[s * 2] = (int16_t) left;
			out[s * 2 + 1] = (int16_t) right;
		}
	} else {
		int pick = (int) source;
		if (pick >= n) {
			pick = 0;
		}
		for (size_t s = 0; s < produced; ++s) {
			if (got[pick] <= s) {
				out[s * 2] = out[s * 2 + 1] = 0;
				continue;
			}
			out[s * 2] = scratch[pick][s * 2];
			out[s * 2 + 1] = scratch[pick][s * 2 + 1];
		}
	}
	return produced;
}

/* ---- Save states ---- */

/* Serialized blob layout: "SPST" | u8 version | u8 nPlayers | u16 pad |
 * u32 coordinatorStateSize | bytes | then each player's mCore state padded to
 * the per-player max. Sizes are fixed at load time so the total never grows. */

#define SP_STATE_MAGIC 0x54535053u /* "SPST" little-endian */
#define SP_STATE_VERSION 1

size_t sp_serialize_size(struct sp_manager* sp) {
	/* Measure once per session: coordinator state is not independently
	 * serializable, so pad generously — each core's state plus the header,
	 * rounded up. The first serialize call fixes the value. */
	size_t total = 16; /* header */
	for (int i = 0; i < sp->nPlayers; ++i) {
		struct VFile* vfm = VFileMemChunk(NULL, 0);
		mCoreSaveStateNamed(sp->players[i].core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
		total += vfm->size(vfm) + 4096; /* per-player padding for growth */
		vfm->close(vfm);
	}
	/* Coordinator bookkeeping is small (a table + event queues); its content
	 * is rebuilt on unserialize via a driver reset, see below. */
	return total;
}

bool sp_serialize(struct sp_manager* sp, void* data, size_t size) {
	/* Layout: u32 magic | u32 version | u32 nPlayers | u32 sliceLen[nPlayers]
	 * | per-player slices (mGBA core state + extdata). Slice lengths live in
	 * the header because mGBA's core state is a raw struct with no in-band
	 * size, and extdata after it is variable-length. */
	size_t headerSize = 3 * sizeof(uint32_t) + (size_t) sp->nPlayers * sizeof(uint32_t);
	if (size < headerSize) {
		return false;
	}
	/* mCoreSaveStateNamed/Named seek the target VFile to 0, so every player is
	 * saved to a scratch VFile first and the slices are concatenated here. */
	uint32_t header[3] = { SP_STATE_MAGIC, SP_STATE_VERSION, (uint32_t) sp->nPlayers };
	memcpy(data, header, sizeof(header));
	size_t offset = headerSize;
	bool ok = true;
	for (int i = 0; i < sp->nPlayers && ok; ++i) {
		struct VFile* vfm = VFileMemChunk(NULL, 0);
		ok = mCoreSaveStateNamed(sp->players[i].core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
		ssize_t n = ok ? vfm->size(vfm) : 0;
		if (ok && offset + (size_t) n > size) {
			_spLog("serialize: player %d needs %zd bytes, %zu left", i, n, size - offset);
			ok = false;
		}
		if (ok) {
			vfm->seek(vfm, 0, SEEK_SET);
			vfm->read(vfm, (char*) data + offset, n);
			uint32_t len = (uint32_t) n;
			memcpy((char*) data + 3 * sizeof(uint32_t) + (size_t) i * sizeof(len), &len, sizeof(len));
			offset += n;
		}
		vfm->close(vfm);
	}
	if (ok) {
		/* Pad the remainder (size must never shrink between frames). */
		memset((char*) data + offset, 0, size - offset);
	}
	return ok;
}

bool sp_unserialize(struct sp_manager* sp, const void* data, size_t size) {
	/* Layout must match sp_serialize: u32 magic | u32 version | u32 nPlayers
	 * | u32 sliceLen[nPlayers] | per-player slices (core state + extdata). */
	size_t headerSize = 3 * sizeof(uint32_t) + (size_t) sp->nPlayers * sizeof(uint32_t);
	uint32_t header[3];
	if (size < headerSize) {
		return false;
	}
	memcpy(header, data, sizeof(header));
	if (header[0] != SP_STATE_MAGIC || header[1] != SP_STATE_VERSION ||
	    (int) header[2] != sp->nPlayers) {
		_spLog("unserialize: header mismatch (magic=%08x ver=%u n=%u want n=%d)",
		       header[0], header[1], header[2], sp->nPlayers);
		return false;
	}
	size_t offset = headerSize;
	for (int i = 0; i < sp->nPlayers; ++i) {
		uint32_t len;
		memcpy(&len, (const char*) data + 3 * sizeof(uint32_t) + (size_t) i * sizeof(len),
		       sizeof(len));
		if (offset + len > size) {
			_spLog("unserialize: player %d slice %u overruns buffer", i, len);
			return false;
		}
		struct VFile* vfm = VFileFromConstMemory((const char*) data + offset, len);
		bool ok = mCoreLoadStateNamed(sp->players[i].core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
		vfm->close(vfm);
		if (!ok) {
			_spLog("unserialize: player %d failed at offset %zu", i, offset);
			return false;
		}
		offset += len;
	}
	/* NOTE: no coordinator reset here. The lockstep driver serializes its full
	 * state through each core's savestate (player queues, cycle offsets, sleep
	 * flags, and — via player 0 — the coordinator bookkeeping), so the link
	 * resumes exactly as saved. Recreating the coordinator would free the
	 * GBASIOLockstepPlayer objects the drivers still reference by lockstepId,
	 * crashing the next SIO event (and losing the link state besides). */
	return true;
}

/* ---- Memory interface ---- */

void* sp_memory_data(struct sp_manager* sp, unsigned id) {
	if (id == RETRO_MEMORY_SAVE_RAM) {
		return sp->players[0].saveData;
	}
	/* TGB Dual convention: RETRO_MEMORY_GAMEBOY_1_SRAM + (n-1) — these ids are
	 * frontend-defined subsystem memory types declared in the manifest; the
	 * core maps them to players 1..3. */
	if (id >= 0x100 && id < 0x100 + SP_MAX_PLAYERS - 1) {
		return sp->players[id - 0x100 + 1].saveData;
	}
	return NULL;
}

size_t sp_memory_size(struct sp_manager* sp, unsigned id) {
	if (id == RETRO_MEMORY_SAVE_RAM) {
		return sp->players[0].saveSize;
	}
	if (id >= 0x100 && id < 0x100 + SP_MAX_PLAYERS - 1) {
		return sp->players[id - 0x100 + 1].saveSize;
	}
	return 0;
}
