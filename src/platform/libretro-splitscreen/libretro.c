/* Copyright (c) 2026 mgba-splitscreen contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * mgba-splitscreen libretro core: 2-4 linked mGBA GBA cores inside ONE core
 * instance, linked over the lockstep SIO coordinator (the same driver the
 * web/desktop apps drive). Single-ROM loads run ONE player; multi-ROM
 * subsystem loads (GBA Link 2/3/4 Player) run N linked players; a core
 * option also splits a single ROM across N players (TGB Dual's
 * MODE_SINGLE_GAME_DUAL pattern).
 */
#include "libretro.h"

#include <mgba-util/common.h>

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/core/version.h>
#include <mgba/gba/interface.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#include "instances.h"
#include <mgba/internal/gba/sio/lockstep.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static retro_environment_t environCallback;
static retro_video_refresh_t videoCallback;
static retro_audio_sample_batch_t audioCallback;
static retro_input_poll_t inputPollCallback;
static retro_input_state_t inputCallback;
static retro_log_printf_t logCallback;

static struct sp_manager sp;
static struct sp_video vid;
static void* romCopy;       /* single-ROM quick path: the shared ROM bytes */
static size_t romCopySize;
static char romPath[PATH_MAX];
static bool havePath;
static bool pendingLoad;    /* deferred until first retro_run (options ready) */
static int pendingPlayers;
static unsigned lastWidth = 240;
static unsigned lastHeight = 160;

/* Forward: option values cached per frame. */
static enum spLayout layoutOpt = SP_LAYOUT_AUTO;
static enum spAudio audioOpt = SP_AUDIO_P1;
static int playersOpt = 1;
static bool fsAssistOpt = false;

/* The upstream keymap order: libretro pad -> GBA keymask bit. */
static const int keymap[] = {
	RETRO_DEVICE_ID_JOYPAD_A,
	RETRO_DEVICE_ID_JOYPAD_B,
	RETRO_DEVICE_ID_JOYPAD_SELECT,
	RETRO_DEVICE_ID_JOYPAD_START,
	RETRO_DEVICE_ID_JOYPAD_RIGHT,
	RETRO_DEVICE_ID_JOYPAD_LEFT,
	RETRO_DEVICE_ID_JOYPAD_UP,
	RETRO_DEVICE_ID_JOYPAD_DOWN,
	RETRO_DEVICE_ID_JOYPAD_R,
	RETRO_DEVICE_ID_JOYPAD_L,
};

/* Subsystem ids: documented custom range, chosen to avoid TGB Dual's 0x101. */
#define RETRO_GAME_TYPE_GBA_LINK_2P 0x201
#define RETRO_GAME_TYPE_GBA_LINK_3P 0x202
#define RETRO_GAME_TYPE_GBA_LINK_4P 0x203

/* Per-instance subsystem memory ids (TGB Dual's convention: >= 0x100). */
#define SP_MEMORY_P2 0x100
#define SP_MEMORY_P3 0x101
#define SP_MEMORY_P4 0x102

static void _spLogLine(const char* line) {
	if (logCallback) {
		logCallback(RETRO_LOG_INFO, "[splitscreen] %s\n", line);
	}
}

static const struct retro_subsystem_memory_info _gba1_mem[] = {
	{ "sav", RETRO_MEMORY_SAVE_RAM },
};
static const struct retro_subsystem_memory_info _gba2_mem[] = {
	{ "sav2", SP_MEMORY_P2 },
};
static const struct retro_subsystem_memory_info _gba3_mem[] = {
	{ "sav3", SP_MEMORY_P3 },
};
static const struct retro_subsystem_memory_info _gba4_mem[] = {
	{ "sav4", SP_MEMORY_P4 },
};

#define SP_ROM_INFO(n, mem) \
	{ "Player " #n, "gba", false, false, true, mem, sizeof(mem) / sizeof(*mem) }

static const struct retro_subsystem_rom_info _gba_roms2[] = {
	SP_ROM_INFO(1, _gba1_mem), SP_ROM_INFO(2, _gba2_mem),
};
static const struct retro_subsystem_rom_info _gba_roms3[] = {
	SP_ROM_INFO(1, _gba1_mem), SP_ROM_INFO(2, _gba2_mem), SP_ROM_INFO(3, _gba3_mem),
};
static const struct retro_subsystem_rom_info _gba_roms4[] = {
	SP_ROM_INFO(1, _gba1_mem), SP_ROM_INFO(2, _gba2_mem), SP_ROM_INFO(3, _gba3_mem),
	SP_ROM_INFO(4, _gba4_mem),
};

static const struct retro_subsystem_info _subsystems[] = {
	{ "GBA Link 2 Player", "gba_link_2p", _gba_roms2, 2, RETRO_GAME_TYPE_GBA_LINK_2P },
	{ "GBA Link 3 Player", "gba_link_3p", _gba_roms3, 3, RETRO_GAME_TYPE_GBA_LINK_3P },
	{ "GBA Link 4 Player", "gba_link_4p", _gba_roms4, 4, RETRO_GAME_TYPE_GBA_LINK_4P },
	{ NULL, NULL, NULL, 0, 0 },
};

static void _applyFsAssist(void) {
	if (sp.linkAttached) {
		sp_set_fs_assist(&sp, fsAssistOpt);
	}
}

static void _readOptions(void) {
	struct retro_variable var;
	var.value = NULL;

	var.key = "splitscreen_layout";
	layoutOpt = SP_LAYOUT_AUTO;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "2x1") == 0) layoutOpt = SP_LAYOUT_2X1;
		else if (strcmp(var.value, "1x2") == 0) layoutOpt = SP_LAYOUT_1X2;
		else if (strcmp(var.value, "2x2") == 0) layoutOpt = SP_LAYOUT_2X2;
	}

	var.key = "splitscreen_audio";
	audioOpt = SP_AUDIO_P1;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "player 1") == 0) audioOpt = SP_AUDIO_P1;
		else if (strcmp(var.value, "player 2") == 0) audioOpt = SP_AUDIO_P2;
		else if (strcmp(var.value, "player 3") == 0) audioOpt = SP_AUDIO_P3;
		else if (strcmp(var.value, "player 4") == 0) audioOpt = SP_AUDIO_P4;
		else if (strcmp(var.value, "mixed") == 0) audioOpt = SP_AUDIO_MIX;
	}

	var.key = "splitscreen_players";
	playersOpt = 1;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		playersOpt = atoi(var.value);
		if (playersOpt < 1 || playersOpt > SP_MAX_PLAYERS) {
			playersOpt = 1;
		}
	}

	var.key = "splitscreen_fs_assist";
	fsAssistOpt = false;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		fsAssistOpt = strcmp(var.value, "on") == 0;
	}
	_applyFsAssist();
}

static void _applyGeometry(void) {
	int n = sp.nPlayers ? sp.nPlayers : pendingPlayers;
	unsigned wantW, wantH;
	switch (layoutOpt) {
	case SP_LAYOUT_2X1:
		wantW = 480;
		wantH = 160;
		break;
	case SP_LAYOUT_1X2:
		wantW = 240;
		wantH = 320;
		break;
	case SP_LAYOUT_2X2:
		wantW = 480;
		wantH = 320;
		break;
	case SP_LAYOUT_AUTO:
	default:
		if (n <= 2) {
			wantW = 480;
			wantH = 160;
		} else {
			wantW = 480;
			wantH = 320;
		}
		break;
	}
	if (wantW != lastWidth || wantH != lastHeight) {
		struct retro_system_av_info info;
		retro_get_system_av_info(&info);
		info.geometry.base_width = wantW;
		info.geometry.base_height = wantH;
		info.geometry.max_width = 480;
		info.geometry.max_height = 320;
		environCallback(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
		lastWidth = wantW;
		lastHeight = wantH;
	}
	if (vid.width != wantW || vid.height != wantH) {
		vid.width = wantW;
		vid.height = wantH;
		size_t need = wantW * wantH * BYTES_PER_PIXEL;
		if (vid.outSize < need) {
			free(vid.out);
			vid.out = malloc(need);
			vid.outSize = need;
		}
	}
}

static void _geometryFromPlayers(int n) {
	/* Set vid geometry + buffer BEFORE the first composite. */
	lastWidth = 0;
	lastHeight = 0;
	vid.width = 0;
	vid.height = 0;
	pendingPlayers = n;
	_applyGeometry();
}

static bool _doLoad(int nPlayers, const void* data, size_t size, const char* path) {
	if (!sp_load(&sp, nPlayers, data, size, &vid)) {
		return false;
	}
	_geometryFromPlayers(nPlayers);
	_applyFsAssist();
	_spLogLine(nPlayers == 1 ? "loaded 1 player (single mode)"
	                          : "loaded N linked players");
	return true;
}

/* ---- libretro API ---- */

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t env) {
	environCallback = env;

	struct retro_variable vars[] = {
		{ "splitscreen_players",
		  "Players per ROM (requires reload); 1 shares the cartridge across players. splitscreen_players; 1|2|3|4" },
		{ "splitscreen_layout",
		  "Screen layout; restart required. splitscreen_layout; 2x1|1x2|2x2" },
		{ "splitscreen_audio",
		  "Audio source. splitscreen_audio; player 1|player 2|player 3|player 4|mixed" },
		{ "splitscreen_fs_assist",
		  "Four Swords link handshake assist (matches the app's default: off). splitscreen_fs_assist; off|on" },
		{ NULL, NULL },
	};
	environCallback(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
	environCallback(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*) _subsystems);
}

void retro_set_video_refresh(retro_video_refresh_t video) {
	videoCallback = video;
}

void retro_set_audio_sample(retro_audio_sample_t audio) {
	UNUSED(audio);
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t audioBatch) {
	audioCallback = audioBatch;
}

void retro_set_input_poll(retro_input_poll_t inputPoll) {
	inputPollCallback = inputPoll;
}

void retro_set_input_state(retro_input_state_t input) {
	inputCallback = input;
}

void retro_get_system_info(struct retro_system_info* info) {
	info->need_fullpath = false;
	info->valid_extensions = "gba";
	info->library_version = "0.1.0";
	info->library_name = "mGBA Splitscreen";
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	unsigned w = lastWidth ? lastWidth : (unsigned) (sp.nPlayers > 2 ? 480 : 480);
	unsigned h = lastHeight ? lastHeight : (unsigned) (sp.nPlayers > 2 ? 320 : 160);
	info->geometry.base_width = w;
	info->geometry.base_height = h;
	info->geometry.max_width = 480;
	info->geometry.max_height = 320;
	info->geometry.aspect_ratio = w / (double) h;
	/* 59.7275 Hz GBA timing; sample rate 32768 (GBA SOUNDBIAS default). */
	info->timing.fps = 16777272.0f / 280896.0f;
	info->timing.sample_rate = 32768.0f;
}

void retro_init(void) {
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	environCallback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

	struct retro_input_descriptor descs[SP_MAX_PLAYERS * 10 + 1];
	int d = 0;
	for (unsigned port = 0; port < SP_MAX_PLAYERS; ++port) {
		static const char* names[10] = { "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L" };
		for (unsigned k = 0; k < 10; ++k) {
			static char labels[SP_MAX_PLAYERS][10][16];
			snprintf(labels[port][k], sizeof(labels[port][k]), "P%u %s", port + 1, names[k]);
			descs[d].port = port;
			descs[d].device = RETRO_DEVICE_JOYPAD;
			descs[d].index = 0;
			descs[d].id = keymap[k];
			descs[d].description = labels[port][k];
			++d;
		}
	}
	descs[d].description = NULL;
	environCallback(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descs);

	struct retro_log_callback log;
	if (environCallback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
		logCallback = log.log;
	} else {
		logCallback = NULL;
	}
	spLogCallback = _spLogLine;

	memset(&sp, 0, sizeof(sp));
	memset(&vid, 0, sizeof(vid));
	vid.layout = SP_LAYOUT_AUTO;
	vid.audio = SP_AUDIO_P1;

	static struct mLogger logger; /* lives for the process: fine, global state allowed */
	logger.log = 0;
	(void) logger;
}

void retro_deinit(void) {
	sp_deinit(&sp);
	free(vid.out);
	memset(&vid, 0, sizeof(vid));
	free(romCopy);
	romCopy = NULL;
	romCopySize = 0;
}

void retro_reset(void) {
	sp_reset(&sp);
}

static uint32_t _readKeys(unsigned port) {
	uint32_t keys = 0;
	for (unsigned i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i) {
		keys |= (uint32_t) (!!inputCallback(port, RETRO_DEVICE_JOYPAD, 0, keymap[i])) << i;
	}
	return keys;
}

void retro_run(void) {
	if (pendingLoad) {
		pendingLoad = false;
		if (!_doLoad(pendingPlayers, romCopy, romCopySize, havePath ? romPath : NULL)) {
			_spLogLine("ERROR: load failed; refusing to run");
			return;
		}
	}

	if (!sp.nPlayers) {
		return;
	}

	/* Option updates are cheap; layout/audio changes apply live. */
	_readOptions();
	vid.layout = layoutOpt;
	vid.audio = audioOpt;
	_applyGeometry();

	if (!sp_run_frame(&sp, inputPollCallback, _readKeys)) {
		/* Livelock bailout already returned; still present the last frames. */
	}

	sp_composite(&sp, &vid);
	videoCallback(vid.out, vid.width, vid.height, vid.width * BYTES_PER_PIXEL);

	int16_t mix[SP_AUDIO_SAMPLES_PER_FRAME * 4];
	size_t produced = sp_audio(&sp, audioOpt, mix, SP_AUDIO_SAMPLES_PER_FRAME * 2);
	if (produced && audioCallback) {
		audioCallback(mix, produced);
	}
}

bool retro_load_game(const struct retro_game_info* game) {
	/* Copy the ROM now (need_fullpath=false guarantees game->data), defer the
	 * heavy N-instance construction until retro_run: the frontend's core
	 * options are only reliable after the load call returns. */
	romCopySize = game->size;
	romCopy = malloc(romCopySize);
	memcpy(romCopy, game->data, romCopySize);
	if (game->path) {
		snprintf(romPath, sizeof(romPath), "%s", game->path);
		havePath = true;
	} else {
		havePath = false;
	}
	_readOptions();
	pendingPlayers = playersOpt; /* quick path: N copies of this one ROM */
	pendingLoad = true;
	return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
	int n = 0;
	switch (game_type) {
	case RETRO_GAME_TYPE_GBA_LINK_2P:
		n = 2;
		break;
	case RETRO_GAME_TYPE_GBA_LINK_3P:
		n = 3;
		break;
	case RETRO_GAME_TYPE_GBA_LINK_4P:
		n = 4;
		break;
	default:
		_spLogLine("unsupported subsystem type");
		return false;
	}
	if (num_info < (size_t) n) {
		char msg[96];
		snprintf(msg, sizeof(msg), "subsystem needs %d ROMs, got %u", n, (unsigned) num_info);
		_spLogLine(msg);
		return false;
	}
	/* All entries must be GBA ROMs; player 1's is the primary content. */
	size_t size = info[0].size;
	romCopySize = size;
	romCopy = malloc(romCopySize);
	memcpy(romCopy, info[0].data, romCopySize);
	if (info[0].path) {
		snprintf(romPath, sizeof(romPath), "%s", info[0].path);
		havePath = true;
	} else {
		havePath = false;
	}
	_readOptions();
	/* Spec edge case: distinct ROMs per player are NOT loaded yet — v1 links
	 * N copies of the primary ROM (matches the quick path). When we support
	 * per-player ROMs, _doLoad takes an array. */
	pendingPlayers = n;
	pendingLoad = true;
	return true;
}

void retro_unload_game(void) {
	sp_deinit(&sp);
	free(romCopy);
	romCopy = NULL;
	romCopySize = 0;
	free(vid.out);
	memset(&vid, 0, sizeof(vid));
	pendingLoad = false;
}

unsigned retro_get_region(void) {
	return RETRO_REGION_NTSC;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	/* Per-player joypads are read directly by port; nothing to negotiate. */
	UNUSED(port);
	UNUSED(device);
}

size_t retro_serialize_size(void) {
	if (!sp.nPlayers) {
		return 0;
	}
	/* Fixed at first call per session via the padded measure in instances.c:
	 * the value must never INCREASE during a session, so measure generously. */
	static size_t cached = 0;
	if (!cached) {
		cached = sp_serialize_size(&sp);
	}
	return cached;
}

bool retro_serialize(void* data, size_t size) {
	if (!sp.nPlayers) {
		return false;
	}
	return sp_serialize(&sp, data, size);
}

bool retro_unserialize(const void* data, size_t size) {
	if (!sp.nPlayers) {
		return false;
	}
	return sp_unserialize(&sp, data, size);
}

void retro_cheat_reset(void) {
	/* Per-player cheats land in phase 6 (per-player core options). */
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	UNUSED(index);
	UNUSED(enabled);
	UNUSED(code);
}

void* retro_get_memory_data(unsigned id) {
	if (!sp.nPlayers) {
		return NULL;
	}
	return sp_memory_data(&sp, id);
}

size_t retro_get_memory_size(unsigned id) {
	if (!sp.nPlayers) {
		return 0;
	}
	return sp_memory_size(&sp, id);
}
