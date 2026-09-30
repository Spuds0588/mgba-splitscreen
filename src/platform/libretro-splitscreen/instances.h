/* Copyright (c) 2026 mgba-splitscreen contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * N linked mGBA cores driven sequentially on the caller's thread — the C
 * port of mgba-splitscreen's EmulationManager (src-tauri/src/emulation.rs).
 * The lockstep SIO driver sleeps a player via user->sleep; the frame loop
 * skips sleeping players and steps the others until they wake, which is the
 * single-thread equivalent of mGBA's threaded lockstep.
 */
#ifndef LIBRETRO_SPLITSCREEN_INSTANCES_H
#define LIBRETRO_SPLITSCREEN_INSTANCES_H

#include "libretro.h"

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/core.h>
#include <mgba/core/lockstep.h>
#include <mgba/internal/gba/sio/lockstep.h>

#define SP_MAX_PLAYERS 4
#define SP_VIDEO_W 240
#define SP_VIDEO_H 160

/* Per-screen player overlays (ported from the app's screen-cell styling):
 * a 3px inset outline in the player's color plus a bottom-right P-number
 * badge, sized in composite pixels so they match across views. */
#define SP_OUTLINE_PX 3
#define SP_TAG_W 26
#define SP_TAG_H 14
extern const uint16_t spPlayerColors[SP_MAX_PLAYERS]; /* RGB565, app palette */

/* Log sink wired by libretro.c (routes into the frontend's log callback). */
extern void (*spLogCallback)(const char* line);

/* Per-player audio pacing: nominal samples per video frame at 32768 Hz. */
#define SP_AUDIO_SAMPLES_PER_FRAME 533

enum spLayout {
	SP_LAYOUT_AUTO = 0, /* 2P: 2x1, 3P/4P: 2x2 */
	SP_LAYOUT_2X1,
	SP_LAYOUT_1X2,
	SP_LAYOUT_2X2,
	SP_LAYOUT_SPEAKER,  /* focused 2x on top, others in a strip below */
	SP_LAYOUT_FOCUS,    /* focused player only, 2x */
	SP_LAYOUT_OVERLAY,  /* focused 2x + others as small PiPs (app's overlay) */
};

enum spAudio {
	SP_AUDIO_P1 = 0,
	SP_AUDIO_P2,
	SP_AUDIO_P3,
	SP_AUDIO_P4,
	SP_AUDIO_MIX,
};

struct sp_player {
	/* MUST stay first: the lockstep callbacks receive &p->lockstepUser and
	 * recover the owning sp_player by casting the pointer back (base-first
	 * pattern, same as the Rust LockstepUserCtx { base: mLockstepUser, .. }).
	 * A compile-time assert in instances.c enforces offset 0. */
	struct mLockstepUser lockstepUser;
	struct mCore* core;
	mColor* videoBuf;          /* the core renders here (RGB565, stride 240) */
	mColor* snapshot;          /* last COMPLETE frame (tear-free composite source) */
	struct mAudioBuffer* audio;
	struct GBASIOLockstepDriver driver;
	bool asleep;
	int playerId;              /* wire id assigned by the coordinator */
	uint32_t lastFrameCounter;
	uint32_t framesProduced;
	void* romData;             /* per-player private ROM copy (mmap) */
	size_t romSize;
	void* saveData;            /* per-player battery RAM (flash-sized) */
	size_t saveSize;
};

struct sp_manager {
	struct sp_player players[SP_MAX_PLAYERS];
	int nPlayers;
	struct GBASIOLockstepCoordinator coordinator;
	bool linkAttached;         /* true when nPlayers > 1 (link wired up) */
	bool fsSuppressed;         /* host FS-assist policy, re-applied on reset */
	unsigned stallRun;         /* consecutive 0-step frames (stall watchdog) */
};

/* State of the last compositors + outputs, owned by libretro.c but shared with
 * the frame loop for geometry math. */
struct sp_video {
	unsigned width;
	unsigned height;
	enum spLayout layout;
	enum spAudio audio;
	int focused;               /* player index for speaker/focus/overlay */
	bool overlays;             /* draw per-screen outline + P-number badge */
	mColor* out;               /* composite RGB565 buffer, WxH */
	size_t outSize;
};

/* Load `nPlayers` copies of `rom` (all players may share the same source
 * buffer; each player gets its own memory copy + save). Returns true on
 * success; on failure sp_deinit leaves the manager zeroed. */
bool sp_load(struct sp_manager* sp, int nPlayers, const void* rom, size_t romSize,
             struct sp_video* vid);

/* Deinit everything (safe on a zeroed manager). */
void sp_deinit(struct sp_manager* sp);

/* Reset all cores (retro_reset). */
void sp_reset(struct sp_manager* sp);

/* Advance every player by one video frame cooperatively. `poll` is called
 * first (frontend input poll contract), then each player's RetroPad state is
 * read via `readKeys(port)` and fed to that player's core. Snapshots update
 * per player on frame-counter change. Returns false on livelock bailout. */
bool sp_run_frame(struct sp_manager* sp, void (*poll)(void),
                  uint32_t (*readKeys)(unsigned port));

/* Blit the per-player snapshots into vid->out per vid->layout. When
 * vid->overlays is set, each screen also gets the player's colored outline
 * and P-number badge (the app's screen-cell treatment). */
void sp_composite(struct sp_manager* sp, struct sp_video* vid, bool overlays);

/* Read up to `samples` stereo frames of mixed audio into out; returns frames.
 * `source` picks whose mix (players 0..3) or SP_AUDIO_MIX for a blend. */
size_t sp_audio(struct sp_manager* sp, enum spAudio source, int16_t* out, size_t samples);

/* Wire the FS assist default (SetFSSuppressed) after a (re)attach. */
void sp_set_fs_assist(struct sp_manager* sp, bool on);

/* Deterministic save-state serialization: coordinator + every core, padded to
 * a fixed size (retro_serialize_size must never shrink mid-session). */
size_t sp_serialize_size(struct sp_manager* sp);
bool sp_serialize(struct sp_manager* sp, void* data, size_t size);
bool sp_unserialize(struct sp_manager* sp, const void* data, size_t size);

/* Memory interface: player 0's save on RETRO_MEMORY_SAVE_RAM, players 1..3 on
 * RETRO_MEMORY_GAMEBOY_1_SRAM.. (TGB Dual's convention). */
void* sp_memory_data(struct sp_manager* sp, unsigned id);
size_t sp_memory_size(struct sp_manager* sp, unsigned id);

CXX_GUARD_END

#endif
