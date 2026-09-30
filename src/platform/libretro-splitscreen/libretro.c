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
static int focusedOpt = 0;   /* zero-based player index for speaker/focus/overlay */
static bool fsAssistOpt = false;
static bool overlaysOpt = true;
static unsigned advertisedRate; /* audio rate last sent via SET_SYSTEM_AV_INFO */
static bool coreOptionsChanged; /* option visibility changed since last poll */

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

/* ---- Core options (v2) ----
 * Sent as a full static table; the view-layout VALUE LIST is rebuilt whenever
 * the number of emulated sessions changes (views that cannot hold N players
 * are removed from the menu instead of showing dead entries), and the whole
 * struct is re-sent to the frontend on load. Frontends without v2 support
 * fall back to the legacy SET_VARIABLES string form below. */
#define SP_OPT_LAYOUT 1 /* index of the layout definition in _optionDefs */

static struct retro_core_option_v2_definition _optionDefs[] = {
	{ "splitscreen_players", "Players per ROM (requires reload)", NULL,
	  "Link N instances of one ROM (subsystems load distinct ROMs instead).",
	  NULL, NULL, { { NULL, NULL } }, "1" },
	{ "splitscreen_layout", "View layout", NULL,
	  "Applies live; per-viewer (each netplay client picks their own view).",
	  NULL, NULL, { { NULL, NULL } }, "auto" },
	{ "splitscreen_focus_player", "Focused player", NULL,
	  "Player enlarged in speaker/focus/overlay views. Applies live.",
	  NULL, NULL, { { NULL, NULL } }, "1" },
	{ "splitscreen_audio", "Audio source", NULL,
	  "Whose mix to play (mixed blends all players). Applies live.",
	  NULL, NULL, { { NULL, NULL } }, "player 1" },
	{ "splitscreen_fs_assist", "Four Swords link assist", NULL,
	  "Handshake assist for Four Swords (matches the app default: off).",
	  NULL, NULL, { { NULL, NULL } }, "off" },
	{ "splitscreen_overlays", "Player outlines & badges", NULL,
	  "Colored border and P-number tag on each player's screen (like the app).",
	  NULL, NULL, { { NULL, NULL } }, "on" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_options_v2 _optionsV2 = {
	NULL,          /* categories: top level */
	_optionDefs,
};

/* Fill one option's value list from a NULL-terminated initializer. */
static void _setValues(struct retro_core_option_v2_definition* def,
                       const struct retro_core_option_value* vals, int count) {
	memset(def->values, 0, sizeof(def->values));
	memcpy(def->values, vals, sizeof(*vals) * count);
}

/* (Re)build the view-layout value list for `n` emulated sessions; n <= 0
 * means unknown (all views). Views that cannot hold n players are dropped:
 * 1P sees only Auto, 2P drops Quadrants, 3-4P drop the 2x1/1x2 grids. */
static void _setLayoutValues(int n) {
	static const struct retro_core_option_value all[] = {
		{ "auto", "Auto" },
		{ "2x1", "Side by side (2x1)" },
		{ "1x2", "Stacked (1x2)" },
		{ "2x2", "Quadrants (2x2)" },
		{ "speaker", "Speaker (big + strip)" },
		{ "focus", "Focus (single screen)" },
		{ "overlay", "Overlay (big + thumbnails)" },
	};
	static const struct retro_core_option_value two[] = {
		{ "auto", "Auto (side by side)" },
		{ "2x1", "Side by side (2x1)" },
		{ "1x2", "Stacked (1x2)" },
		{ "speaker", "Speaker (big + strip)" },
		{ "focus", "Focus (single screen)" },
		{ "overlay", "Overlay (big + thumbnails)" },
	};
	static const struct retro_core_option_value multi[] = {
		{ "auto", "Auto (quadrants)" },
		{ "2x2", "Quadrants (2x2)" },
		{ "speaker", "Speaker (big + strip)" },
		{ "focus", "Focus (single screen)" },
		{ "overlay", "Overlay (big + thumbnails)" },
	};
	static const struct retro_core_option_value solo[] = {
		{ "auto", "Single player" },
	};
	if (n <= 0) {
		_setValues(&_optionDefs[SP_OPT_LAYOUT], all, sizeof(all) / sizeof(*all));
	} else if (n == 1) {
		_setValues(&_optionDefs[SP_OPT_LAYOUT], solo, sizeof(solo) / sizeof(*solo));
	} else if (n == 2) {
		_setValues(&_optionDefs[SP_OPT_LAYOUT], two, sizeof(two) / sizeof(*two));
	} else {
		_setValues(&_optionDefs[SP_OPT_LAYOUT], multi, sizeof(multi) / sizeof(*multi));
	}
	_optionDefs[SP_OPT_LAYOUT].default_value = "auto";
	coreOptionsChanged = true;
}

static void _initOptionDefs(void) {
	static const struct retro_core_option_value valsPlayers[] = {
		{ "1", "1 (single)" }, { "2", "2" }, { "3", "3" }, { "4", "4" },
	};
	static const struct retro_core_option_value valsFocus[] = {
		{ "1", "Player 1" }, { "2", "Player 2" }, { "3", "Player 3" }, { "4", "Player 4" },
	};
	static const struct retro_core_option_value valsAudio[] = {
		{ "player 1", "Player 1" }, { "player 2", "Player 2" },
		{ "player 3", "Player 3" }, { "player 4", "Player 4" }, { "mixed", "Mixed" },
	};
	static const struct retro_core_option_value valsAssist[] = {
		{ "off", "Off" }, { "on", "On" },
	};
	static const struct retro_core_option_value valsOverlays[] = {
		{ "on", "On" }, { "off", "Off" },
	};
	_setValues(&_optionDefs[0], valsPlayers, 4);
	_setValues(&_optionDefs[2], valsFocus, 4);
	_setValues(&_optionDefs[3], valsAudio, 5);
	_setValues(&_optionDefs[4], valsAssist, 2);
	_setValues(&_optionDefs[5], valsOverlays, 2);
	_setLayoutValues(0);
}

/* Hide options that make no sense for the current session (called by the
 * frontend's update-display poll and once right after a load). Returns
 * whether visibility changed since the last call, per the libretro spec. */
static bool _updateOptionVisibility(void) {
	int n = sp.nPlayers ? sp.nPlayers : pendingPlayers;
	struct retro_core_option_display d;
	d.visible = n > 1;
	d.key = "splitscreen_focus_player";
	environCallback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &d);
	bool changed = coreOptionsChanged;
	coreOptionsChanged = false;
	return changed;
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

static enum spLayout lastLoggedLayout = SP_LAYOUT_AUTO;
static int lastLoggedFocus = -1;

/* Can session size `n` show this layout? (Mirrors the dynamic option list.) */
static bool _layoutValidFor(int n, enum spLayout layout) {
	if (n <= 1) {
		return layout == SP_LAYOUT_AUTO;
	}
	switch (layout) {
	case SP_LAYOUT_SPEAKER:
	case SP_LAYOUT_FOCUS:
	case SP_LAYOUT_OVERLAY:
		return true;
	case SP_LAYOUT_2X1:
	case SP_LAYOUT_1X2:
		return n == 2;
	case SP_LAYOUT_2X2:
		return n >= 3;
	case SP_LAYOUT_AUTO:
	default:
		return true;
	}
}

static const char* _layoutName(enum spLayout layout) {
	switch (layout) {
	case SP_LAYOUT_2X1: return "side by side";
	case SP_LAYOUT_1X2: return "stacked";
	case SP_LAYOUT_2X2: return "quadrants";
	case SP_LAYOUT_SPEAKER: return "speaker";
	case SP_LAYOUT_FOCUS: return "focus";
	case SP_LAYOUT_OVERLAY: return "overlay";
	case SP_LAYOUT_AUTO:
	default: return "auto";
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
		else if (strcmp(var.value, "speaker") == 0) layoutOpt = SP_LAYOUT_SPEAKER;
		else if (strcmp(var.value, "focus") == 0) layoutOpt = SP_LAYOUT_FOCUS;
		else if (strcmp(var.value, "overlay") == 0) layoutOpt = SP_LAYOUT_OVERLAY;
	}

	var.key = "splitscreen_focus_player";
	focusedOpt = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		focusedOpt = atoi(var.value) - 1;
		if (focusedOpt < 0 || focusedOpt >= SP_MAX_PLAYERS) {
			focusedOpt = 0;
		}
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

	var.key = "splitscreen_overlays";
	overlaysOpt = true;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		overlaysOpt = strcmp(var.value, "on") == 0;
	}

	/* A persisted value may name a view the current session cannot show (e.g.
	 * a saved quadrant layout after dropping to 2 players); fall back to Auto. */
	if (!_layoutValidFor((int) sp.nPlayers ? (int) sp.nPlayers : pendingPlayers, layoutOpt)) {
		layoutOpt = SP_LAYOUT_AUTO;
	}

	if (layoutOpt != lastLoggedLayout || focusedOpt != lastLoggedFocus) {
		char msg[96];
		snprintf(msg, sizeof(msg), "view: %s, focused player %d",
		         _layoutName(layoutOpt), focusedOpt + 1);
		_spLogLine(msg);
		lastLoggedLayout = layoutOpt;
		lastLoggedFocus = focusedOpt;
	}
	_applyFsAssist();
}

#define SP_MAX_W 480
#define SP_MAX_H 480  /* speaker view: 2x focused (320) + 1x strip (160) */
static unsigned baseWidth, baseHeight; /* geometry the aspect was built from */

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
	case SP_LAYOUT_SPEAKER:
		/* 2x focused screen on top (320) + 1x strip beneath (160). */
		wantW = 480;
		wantH = 480;
		break;
	case SP_LAYOUT_OVERLAY:
	case SP_LAYOUT_FOCUS:
		/* Focused player alone at 2x (overlay PiPs draw over the edge). */
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
		/* retro_get_system_av_info derives the aspect from the PREVIOUS
		 * base size; with it stale, the frontend stretched the new composite
		 * to the old view's shape after every live layout switch. */
		info.geometry.aspect_ratio = wantW / (double) wantH;
		info.geometry.max_width = SP_MAX_W;
		info.geometry.max_height = SP_MAX_H;
		environCallback(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
		lastWidth = wantW;
		lastHeight = wantH;
		baseWidth = wantW;
		baseHeight = wantH;
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
	advertisedRate = sp.players[0].core->audioSampleRate(sp.players[0].core);
	_geometryFromPlayers(nPlayers);
	_applyFsAssist();
	_setLayoutValues(nPlayers); /* rebuild the menu for this session size */
	_updateOptionVisibility();
	char msg[96];
	snprintf(msg, sizeof(msg), "loaded %d player%s%s", nPlayers,
	         nPlayers == 1 ? " (single mode)" : " linked",
	         nPlayers > 1 ? " (per-viewer views active)" : "");
	_spLogLine(msg);
	return true;
}

/* ---- libretro API ---- */

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t env) {
	environCallback = env;		struct retro_core_options_update_display_callback udisp = {
			_updateOptionVisibility,
		};
		environCallback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &udisp);
		/* Prefer the v2 interface (per-value visibility, clean labels); fall
		 * back to the legacy string form for frontends without v2. */
		if (!environCallback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &_optionsV2)) {
			static struct retro_variable vars[] = {
				{ "splitscreen_players",
				  "Players per ROM (requires reload); 1|2|3|4" },
				{ "splitscreen_layout",
				  "View layout (applies live); auto|2x1|1x2|2x2|speaker|focus|overlay" },
				{ "splitscreen_focus_player",
				  "Focused player (applies live); 1|2|3|4" },
				{ "splitscreen_audio",
				  "Audio source (applies live); player 1|player 2|player 3|player 4|mixed" },
				{ "splitscreen_fs_assist",
				  "Four Swords link handshake assist; off|on" },
				{ "splitscreen_overlays",
				  "Player outlines & badges; on|off" },
				{ NULL, NULL },
			};
			environCallback(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
		}
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
	unsigned w = baseWidth ? baseWidth : (lastWidth ? lastWidth : 480);
	unsigned h = baseHeight ? baseHeight : (lastHeight ? lastHeight : 160);
	info->geometry.base_width = w;
	info->geometry.base_height = h;
	info->geometry.max_width = SP_MAX_W;
	info->geometry.max_height = SP_MAX_H;
	info->geometry.aspect_ratio = w / (double) h;
	/* 59.7275 Hz GBA video. The sample rate is DYNAMIC: games may rewrite
	 * SOUNDBIAS (Mario Kart Super Circuit does at boot), which changes the
	 * emulator's production rate (32768 -> 65536 Hz). Advertising a stale
	 * rate makes the frontend's audio pacing wedge retro_run entirely. */
	info->timing.fps = 16777272.0f / 280896.0f;
	info->timing.sample_rate = sp.nPlayers
	                               ? (double) sp.players[0].core->audioSampleRate(sp.players[0].core)
	                               : 32768.0;
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
	_initOptionDefs();

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

	/* Option updates are cheap; layout/audio/focus changes apply live. */
	_readOptions();
	vid.layout = layoutOpt;
	vid.audio = audioOpt;
	vid.focused = focusedOpt;
	vid.overlays = overlaysOpt;
	_applyGeometry();

	/* A SOUNDBIAS rewrite can change the audio production rate mid-game;
	 * if it moved, re-advertise timing so the frontend resamples correctly
	 * (upstream core does the same via the audioRateChanged AVStream). */
	unsigned rate = sp.players[0].core->audioSampleRate(sp.players[0].core);
	if (rate != advertisedRate) {
		advertisedRate = rate;
		struct retro_system_av_info info;
		retro_get_system_av_info(&info);
		environCallback(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
	}

	if (!sp_run_frame(&sp, inputPollCallback, _readKeys)) {
		/* Livelock bailout already returned; still present the last frames. */
	}

	sp_composite(&sp, &vid, overlaysOpt);
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
