/* DualBoy in-browser emulation bridge.
 *
 * Compiles libmgba to WebAssembly (single-threaded) and exposes a small C API
 * the frontend drives: N GBA instances linked through mGBA's lockstep
 * coordinator, stepped cooperatively on the JS thread (one frame per call),
 * mirroring DualBoy's desktop wrapper (EmulationManager + GbaInstance).
 *
 * Exported functions (all prefixed db_):
 *   db_init(count)                  create `count` cores + lockstep cable
 *   db_load_rom(ptr, len)           load the same ROM bytes into every core
 *   db_run_frame()                  advance every non-asleep core one frame
 *   db_get_video(player) -> ptr     RGBA8888 frame buffer (240x160)
 *   db_set_keys(player, keys)       GBA key mask (active-high, mGBA order)
 *   db_get_audio() -> ptr           mixed stereo s16 chunk @ 32768 Hz
 *   db_audio_frames() -> int        number of stereo frames in that chunk
 *   db_save_state(player) -> size   capture one core's save state
 *   db_state_ptr() -> ptr           the captured save state bytes
 *   db_load_state(player) -> int    restore the captured state into a core
 *   db_quit()                       tear everything down
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/core/lockstep.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/gba/interface.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>
#include <emscripten/emscripten.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PLAYERS 4
#define W 240
#define H 160
#define FRAME_PIXELS (W * H)
#define AUDIO_MAX_FRAMES 2048

/* mGBA's GBA key masks (bit for a pressed button). */
#define KEY_A 0x001
#define KEY_B 0x002
#define KEY_SELECT 0x004
#define KEY_START 0x008
#define KEY_RIGHT 0x010
#define KEY_LEFT 0x020
#define KEY_UP 0x040
#define KEY_DOWN 0x080
#define KEY_R 0x100
#define KEY_L 0x200

struct Player {
	struct mCore* core;
	bool running;
	uint32_t video[FRAME_PIXELS];
	uint8_t rgba[FRAME_PIXELS * 4];
};

/* Logging.
 *
 * mGBA's mLog() falls back to printf/vprintf when NO logger is installed, which
 * formats AND prints every message — including the lockstep driver's DEBUG
 * chatter on every transfer event. On the web build that meant hundreds of
 * console.log calls per second during link-heavy play (the FS linking screen),
 * and browser console output is expensive, tanking the frame rate exactly when
 * the link is busiest. So we always install a silent logger: the filter test is
 * a cheap bitmask, DEBUG/INFO are suppressed by default, and db_enable_debug()
 * flips the filter to mLOG_ALL when you want the lockstep trace in the console.
 */
static struct mStandardLogger g_logger;
static bool g_logger_ready = false;

/* Install the logger if needed and set its filter level. db_init always wants
 * the silent defaults, but must NOT clobber a level db_enable_debug() already
 * raised (call order isn't guaranteed), so force=false only installs the first
 * time and force=true always sets the level. */
static void install_logger(int levels, bool force) {
	if (!g_logger_ready) {
		mStandardLoggerInit(&g_logger);
		g_logger.logToStdout = true;
		mLogSetDefaultLogger(&g_logger.d);
		g_logger_ready = true;
	}
	if (force || g_logger.d.filter->defaultLevels == 0) {
		g_logger.d.filter->defaultLevels = levels;
	}
}

EMSCRIPTEN_KEEPALIVE
void db_enable_debug(void) {
	install_logger(mLOG_ALL, true);
}

static struct Player g_players[MAX_PLAYERS];
static int g_count = 0;

/* Per-frame stepping stats, for profiling the cooperative loop. */
static int g_stat_steps = 0;
static int g_stat_sleeps = 0;
static int g_stat_wakes = 0;
static int64_t g_stat_cycles = 0;

/* Last completed frame counter per player, for tear-free snapshots in
 * db_run_frame (the live buffer is only complete between finishFrame and the
 * next vblank clear, exactly when frameCounter increments). */
static uint32_t g_last_fc[MAX_PLAYERS];

static struct GBASIOLockstepCoordinator g_coord;
static bool g_has_coord = false;
static struct GBASIOLockstepDriver g_drivers[MAX_PLAYERS];
static struct mLockstepUser g_users[MAX_PLAYERS];
static bool g_asleep[MAX_PLAYERS];

/* The lockstep calls sleep/wake while a player waits for the others to catch
 * up (e.g. mid-transfer). We step every core sequentially on one thread, so
 * these just flip a flag and db_run_frame skips sleeping cores — the same
 * cooperative model as the desktop wrapper. */
static int user_index(struct mLockstepUser* u) {
	return (int)(u - g_users);
}
static void user_sleep(struct mLockstepUser* u) {
	g_asleep[user_index(u)] = true;
	++g_stat_sleeps;
}
static void user_wake(struct mLockstepUser* u) {
	g_asleep[user_index(u)] = false;
	++g_stat_wakes;
}

EMSCRIPTEN_KEEPALIVE
void db_init(int count) {
	/* Silent-by-default logger: suppress DEBUG/INFO so mLog never falls back to
	 * printf/vprintf (see the logging note above). WARN/ERROR/FATAL still pass. */
	install_logger(mLOG_WARN | mLOG_ERROR | mLOG_FATAL | mLOG_GAME_ERROR, false);
	g_count = count < 1 ? 1 : (count > MAX_PLAYERS ? MAX_PLAYERS : count);
	memset(g_players, 0, sizeof(g_players));
	memset(g_asleep, 0, sizeof(g_asleep));
	memset(g_last_fc, 0, sizeof(g_last_fc));
	if (g_has_coord) {
		GBASIOLockstepCoordinatorDeinit(&g_coord);
		g_has_coord = false;
	}
	if (g_count >= 2) {
		memset(&g_coord, 0, sizeof(g_coord));
		GBASIOLockstepCoordinatorInit(&g_coord);
		for (int i = 0; i < g_count; ++i) {
			memset(&g_users[i], 0, sizeof(g_users[i]));
			memset(&g_drivers[i], 0, sizeof(g_drivers[i]));
			g_users[i].sleep = user_sleep;
			g_users[i].wake = user_wake;
			GBASIOLockstepDriverCreate(&g_drivers[i], &g_users[i]);
			GBASIOLockstepCoordinatorAttach(&g_coord, &g_drivers[i]);
		}
		g_has_coord = true;
	}
}

EMSCRIPTEN_KEEPALIVE
int db_load_rom(const uint8_t* rom, size_t len) {
	if (len < 256 || len > (32u * 1024 * 1024)) {
		return -1;
	}
	for (int i = 0; i < g_count; ++i) {
		struct Player* p = &g_players[i];
		if (p->core) {
			if (p->running) {
				p->core->unloadROM(p->core);
			}
			p->core->deinit(p->core);
			free(p->core);
			p->core = NULL;
			p->running = false;
		}
		p->core = mCoreCreate(mPLATFORM_GBA);
		if (!p->core) {
			return -2;
		}
		if (!p->core->init(p->core)) {
			return -3;
		}
		/* Match the desktop wrapper's exact init order. */
		if (p->core->setAudioBufferSize) {
			p->core->setAudioBufferSize(p->core, 2048);
		}
		p->core->opts.volume = 0x100;
		mCoreInitConfig(p->core, NULL);
		mCoreLoadConfig(p->core);

		p->core->setVideoBuffer(p->core, p->video, W);
		if (g_has_coord) {
			p->core->setPeripheral(p->core, 0x1001 /* mPERIPH_GBA_LINK_PORT */, &g_drivers[i]);
		}
		/* VFileMemChunk copies the ROM bytes into core-owned memory. */
		struct VFile* vf = VFileMemChunk(rom, len);
		if (!vf) {
			return -4;
		}
		if (!p->core->loadROM(p->core, vf)) {
			return -5;
		}
		p->core->reset(p->core);
		p->running = true;
	}
	memset(g_last_fc, 0, sizeof(g_last_fc));
	return 0;
}

#define FRAME_CYCLES 280896 /* GBA VIDEO_TOTAL_LENGTH */

EMSCRIPTEN_KEEPALIVE
void db_run_frame(void) {
	/* Event-by-event cooperative stepping, mirroring the desktop wrapper's
	 * frame loop: advance every player by one video frame's worth of cycles,
	 * switching between players whenever one sleeps on the lockstep link (a
	 * sleeping player is skipped, and the other player's work — delivering
	 * transfer data / acking — is what wakes it).
	 *
	 * Whole-frame stepping (runFrame per player) broke the link: the master's
	 * frame — including the transfer-complete cycle — finished before ANY slave
	 * ran, so the master's completeEvent fired before the slaves acked and it
	 * received 0xFFFF for every slave slot (master showed NO LINK while the
	 * slaves saw the master's pings fine). Stepping one timing event at a time
	 * lets a player pause mid-frame and resume exactly where it left off, so
	 * all players rendezvous at the transfer before the master's completeEvent
	 * fires — the same model the desktop app uses. */
	int32_t budgets[MAX_PLAYERS];
	for (int i = 0; i < g_count; ++i) {
		budgets[i] = FRAME_CYCLES;
	}
	g_stat_steps = 0;
	g_stat_sleeps = 0;
	g_stat_wakes = 0;
	g_stat_cycles = 0;
	bool made_progress = true;
	int steps = 0;
	while (made_progress && steps < 100000) {
		made_progress = false;
		for (int i = 0; i < g_count; ++i) {
			struct Player* p = &g_players[i];
			if (!p->running || g_asleep[i] || budgets[i] <= 0) {
				continue;
			}
			made_progress = true;
			int32_t before = mTimingCurrentTime(p->core->timing);
			p->core->runLoop(p->core);
			int32_t delta = mTimingCurrentTime(p->core->timing) - before;
			if (delta < 1) {
				delta = 1;
			}
			budgets[i] -= delta;
			g_stat_cycles += delta;
			++steps;
		}
	}
	g_stat_steps = steps;

	/* Pack each player's latest COMPLETED frame as RGBA8888 for direct
	 * putImageData use. A player that ends the tick mid-frame keeps its last
	 * complete frame (the software renderer draws scanlines incrementally and
	 * the ROM clears the buffer in vblank, so the live buffer is only complete
	 * for the instant between finishFrame and the next vblank clear — exactly
	 * when frameCounter increments). Snapshotting on frame-counter change, like
	 * the desktop wrapper, gives tear-free output. */
	for (int i = 0; i < g_count; ++i) {
		struct Player* p = &g_players[i];
		if (!p->running) {
			continue;
		}
		uint32_t fc = p->core->frameCounter(p->core);
		if (fc == g_last_fc[i]) {
			continue;
		}
		g_last_fc[i] = fc;
		const uint32_t* v = p->video;
		uint8_t* out = p->rgba;
		for (int j = 0; j < FRAME_PIXELS; ++j) {
			uint32_t px = v[j];
			out[j * 4 + 0] = (uint8_t)(px & 0xFF);
			out[j * 4 + 1] = (uint8_t)((px >> 8) & 0xFF);
			out[j * 4 + 2] = (uint8_t)((px >> 16) & 0xFF);
			out[j * 4 + 3] = 0xFF;
		}
	}
}

EMSCRIPTEN_KEEPALIVE
uint8_t* db_get_video(int player) {
	if (player < 0 || player >= g_count) {
		return NULL;
	}
	return g_players[player].rgba;
}

EMSCRIPTEN_KEEPALIVE
void db_set_keys(int player, uint32_t keys) {
	if (player < 0 || player >= g_count || !g_players[player].running) {
		return;
	}
	g_players[player].core->setKeys(g_players[player].core, keys);
}

static int16_t g_mix[AUDIO_MAX_FRAMES * 2];
static int g_mix_frames = 0;
static int16_t g_tmp[AUDIO_MAX_FRAMES * 2];
/* Audio routing, mirroring the desktop Audio menu: 0 = mute, 1-4 = that
 * player's whole mix (music + SFX together — the core can't split them),
 * 5 = blend all players (default, so in-browser play hears everyone). */
static int g_audio_source = 5;

static inline int16_t clamp16(int32_t v) {
	if (v > 32767) {
		return 32767;
	}
	if (v < -32768) {
		return -32768;
	}
	return (int16_t) v;
}

EMSCRIPTEN_KEEPALIVE
void db_set_audio_source(int n) {
	g_audio_source = n;
}

EMSCRIPTEN_KEEPALIVE
int16_t* db_get_audio(void) {
	g_mix_frames = 0;
	if (g_audio_source == 0) {
		/* Muted: drain nothing, output silence. */
		return g_mix;
	}
	if (g_audio_source >= 1 && g_audio_source <= 4) {
		/* Single player's whole mix. */
		int i = g_audio_source - 1;
		if (i >= g_count || !g_players[i].running) {
			return g_mix;
		}
		struct mAudioBuffer* buf = g_players[i].core->getAudioBuffer(g_players[i].core);
		if (!buf) {
			return g_mix;
		}
		size_t avail = mAudioBufferAvailable(buf);
		if (avail > AUDIO_MAX_FRAMES) {
			avail = AUDIO_MAX_FRAMES;
		}
		size_t read = mAudioBufferRead(buf, g_mix, avail);
		g_mix_frames = (int) read;
		return g_mix;
	}
	/* Mix all players (default). */
	int running = 0;
	size_t best = AUDIO_MAX_FRAMES;
	for (int i = 0; i < g_count; ++i) {
		struct Player* p = &g_players[i];
		if (!p->running) {
			continue;
		}
		++running;
		struct mAudioBuffer* buf = p->core->getAudioBuffer(p->core);
		if (!buf) {
			continue;
		}
		size_t avail = mAudioBufferAvailable(buf);
		if (avail < best) {
			best = avail;
		}
	}
	if (running == 0 || best == 0) {
		return g_mix;
	}
	if (best > AUDIO_MAX_FRAMES) {
		best = AUDIO_MAX_FRAMES;
	}
	/* First running player seeds the mix; the rest sum in (with clamp). */
	bool seeded = false;
	for (int i = 0; i < g_count; ++i) {
		struct Player* p = &g_players[i];
		if (!p->running) {
			continue;
		}
		struct mAudioBuffer* buf = p->core->getAudioBuffer(p->core);
		if (!buf) {
			continue;
		}
		size_t read = mAudioBufferRead(buf, g_tmp, best);
		if (!seeded) {
			memcpy(g_mix, g_tmp, read * 2 * sizeof(int16_t));
			seeded = true;
		} else {
			for (size_t j = 0; j < read * 2; ++j) {
				g_mix[j] = clamp16((int32_t) g_mix[j] + g_tmp[j]);
			}
		}
	}
	g_mix_frames = (int) best;
	return g_mix;
}

EMSCRIPTEN_KEEPALIVE
int db_audio_frames(void) {
	return g_mix_frames;
}

static uint8_t* g_state = NULL;
static size_t g_state_size = 0;

EMSCRIPTEN_KEEPALIVE
size_t db_save_state(int player) {
	free(g_state);
	g_state = NULL;
	g_state_size = 0;
	if (player < 0 || player >= g_count || !g_players[player].running) {
		return 0;
	}
	struct Player* p = &g_players[player];
	size_t sz = p->core->stateSize(p->core);
	if (sz == 0) {
		return 0;
	}
	g_state = malloc(sz);
	if (!g_state) {
		return 0;
	}
	if (!p->core->saveState(p->core, g_state)) {
		free(g_state);
		g_state = NULL;
		return 0;
	}
	g_state_size = sz;
	return sz;
}

EMSCRIPTEN_KEEPALIVE
uint8_t* db_state_ptr(void) {
	return g_state;
}

EMSCRIPTEN_KEEPALIVE
int db_load_state(int player) {
	if (!g_state || g_state_size == 0) {
		return -1;
	}
	if (player < 0 || player >= g_count || !g_players[player].running) {
		return -2;
	}
	return g_players[player].core->loadState(g_players[player].core, g_state) ? 0 : -3;
}

/* JS-side save management: load a state blob owned by JS into one player.
 * The frontend keeps one state per player (quick-save all), so it malls a
 * buffer, copies the bytes in, and hands them over — no shared global. */
EMSCRIPTEN_KEEPALIVE
int db_load_state_bytes(int player, const uint8_t* data, size_t size) {
	if (player < 0 || player >= g_count || !g_players[player].running) {
		return -2;
	}
	if (!data || size == 0) {
		return -1;
	}
	return g_players[player].core->loadState(g_players[player].core, data) ? 0 : -3;
}

/* Stepping stats for the most recent db_run_frame: loop iterations, lockstep
 * sleep/wake counts (transfer rendezvous activity), and emulated cycles. */
EMSCRIPTEN_KEEPALIVE
void db_get_stats(int out[4]) {
	out[0] = g_stat_steps;
	out[1] = g_stat_sleeps;
	out[2] = g_stat_wakes;
	out[3] = (int) g_stat_cycles;
}

EMSCRIPTEN_KEEPALIVE
void db_quit(void) {
	for (int i = 0; i < g_count; ++i) {
		struct Player* p = &g_players[i];
		if (p->core) {
			if (p->running) {
				p->core->unloadROM(p->core);
			}
			p->core->deinit(p->core);
			free(p->core);
			p->core = NULL;
			p->running = false;
		}
	}
	/* Cores are torn down first: their SIO drivers call back into the
	 * coordinator during teardown, so it must outlive them. */
	if (g_has_coord) {
		GBASIOLockstepCoordinatorDeinit(&g_coord);
		g_has_coord = false;
	}
	free(g_state);
	g_state = NULL;
	g_state_size = 0;
	g_count = 0;
}
