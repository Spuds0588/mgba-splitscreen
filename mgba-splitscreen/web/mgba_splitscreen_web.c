/* mgba-splitscreen in-browser emulation bridge.
 *
 * Compiles libmgba to WebAssembly (single-threaded) and exposes a small C API
 * the frontend drives: N GBA instances linked through mGBA's lockstep
 * coordinator, stepped cooperatively on the JS thread (one frame per call),
 * mirroring mgba-splitscreen's desktop wrapper (EmulationManager + GbaInstance).
 *
 * Exported functions (all prefixed mgs_):
 *   mgs_init(count)                  create `count` cores + platform link cable
 *   mgs_load_rom(ptr, len)           load the same ROM bytes into every core
 *   mgs_run_frame()                  advance every non-asleep core one frame
 *   mgs_get_video(player) -> ptr     RGBA8888 frame buffer (dynamic dimensions)
 *   mgs_get_video_width/height(player) -> current frame dimensions
 *   mgs_set_keys(player, keys)       GBA key mask (active-high, mGBA order)
 *   mgs_get_audio() -> ptr           mixed stereo s16 chunk @ 32768 Hz
 *   mgs_audio_frames() -> int        number of stereo frames in that chunk
 *   mgs_save_state(player) -> size   capture one core's save state
 *   mgs_state_ptr() -> ptr           the captured save state bytes
 *   mgs_load_state(player) -> int    restore the captured state into a core
 *   mgs_quit()                       tear everything down
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/core/lockstep.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>
#include <mgba/internal/gb/sio/lockstep.h>
#include <mgba/gba/interface.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>
#include <emscripten/emscripten.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PLAYERS 4
#define GBA_W 240
#define GBA_H 160
/* Covers the largest mGBA GB output (SGB border) while keeping one stable
 * WASM allocation per player. The frontend uses the reported dimensions. */
#define MAX_W 256
#define MAX_H 224
#define FRAME_PIXELS (MAX_W * MAX_H)
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
	enum mPlatform platform;
	unsigned width;
	unsigned height;
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
 * a cheap bitmask, DEBUG/INFO are suppressed by default, and mgs_enable_debug()
 * flips the filter to mLOG_ALL when you want the lockstep trace in the console.
 */
static struct mStandardLogger g_logger;
static bool g_logger_ready = false;

/* Install the logger if needed and set its filter level. mgs_init always wants
 * the silent defaults, but must NOT clobber a level mgs_enable_debug() already
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
void mgs_enable_debug(void) {
	install_logger(mLOG_ALL, true);
}

static struct Player g_players[MAX_PLAYERS];
static int g_count = 0;
static enum mPlatform g_platform = mPLATFORM_NONE;

/* Per-frame stepping stats, for profiling the cooperative loop. */
static int g_stat_steps = 0;
static int g_stat_sleeps = 0;
static int g_stat_wakes = 0;
static int64_t g_stat_cycles = 0;

/* Last completed frame counter per player, for tear-free snapshots in
 * mgs_run_frame (the live buffer is only complete between finishFrame and the
 * next vblank clear, exactly when frameCounter increments). */
static uint32_t g_last_fc[MAX_PLAYERS];

static struct GBASIOLockstepCoordinator g_coord;
static bool g_has_coord = false;
static struct GBASIOLockstepDriver g_drivers[MAX_PLAYERS];
static struct mLockstepUser g_users[MAX_PLAYERS];
static bool g_asleep[MAX_PLAYERS];

/* GB/GBC uses mGBA's separate two-device serial lockstep driver. Its
 * mLockstep callbacks are normally supplied by MultiplayerController's
 * threaded Qt frontend. The browser is cooperative, so provide the same
 * cycle-accounting contract locally and keep the GB link limited to two cores. */
struct GBLinkContext {
	struct GBSIOLockstep* lockstep;
	int32_t cyclesPosted[MAX_GBS];
	unsigned waitMask;
	bool awake[MAX_GBS];
};
static struct GBSIOLockstep g_gb_coord;
static struct GBLinkContext g_gb_context;
static struct GBSIOLockstepNode g_gb_nodes[MAX_GBS];
static bool g_has_gb_coord = false;

static void gb_lock(struct mLockstep* lockstep) { (void) lockstep; }
static void gb_unlock(struct mLockstep* lockstep) { (void) lockstep; }

static bool gb_signal(struct mLockstep* lockstep, unsigned mask) {
	struct GBLinkContext* context = lockstep->context;
	/* The GB lockstep driver's Qt implementation waits the master (player 0)
	 * while it catches the slave up. A signal releases that master; the mask
	 * names the slave that has reached the rendezvous. */
	context->waitMask &= ~mask;
	if (!context->waitMask) {
		context->awake[0] = true;
	}
	return true;
}

static bool gb_wait(struct mLockstep* lockstep, unsigned mask) {
	struct GBLinkContext* context = lockstep->context;
	/* Only the master thread sleeps in the GB implementation. The cooperative
	 * browser loop must keep invoking the timing callbacks, so this records the
	 * rendezvous without blocking the JS thread. */
	context->waitMask |= mask;
	context->awake[0] = false;
	return true;
}

static void gb_add_cycles(struct mLockstep* lockstep, int id, int32_t cycles) {
	struct GBLinkContext* context = lockstep->context;
	if (cycles < 0) {
		return;
	}
	/* Match MultiplayerController: the master posts elapsed time to the
	 * slave and wakes it whenever it is waiting. The slave accumulates its
	 * own elapsed time as a debt consumed by useCycles(). */
	int target = id ? id : 1;
	if (target >= MAX_GBS) {
		return;
	}
	context->cyclesPosted[target] += cycles;
	if (!id) {
		struct GBSIOLockstepNode* node = context->lockstep->players[target];
		if (node && !context->awake[target]) {
			node->nextEvent += context->cyclesPosted[target];
		}
		context->awake[target] = true;
	}
}

static int32_t gb_use_cycles(struct mLockstep* lockstep, int id, int32_t cycles) {
	struct GBLinkContext* context = lockstep->context;
	if (id < 0 || id >= MAX_GBS) {
		return 0;
	}
	context->cyclesPosted[id] -= cycles;
	if (context->cyclesPosted[id] <= 0) {
		context->awake[id] = false;
	}
	return context->cyclesPosted[id];
}

static int32_t gb_unused_cycles(struct mLockstep* lockstep, int id) {
	struct GBLinkContext* context = lockstep->context;
	return id >= 0 && id < MAX_GBS ? context->cyclesPosted[id] : 0;
}

static void gb_unload(struct mLockstep* lockstep, int id) {
	struct GBLinkContext* context = lockstep->context;
	if (id >= 0 && id < MAX_GBS) {
		context->cyclesPosted[id] = 0;
		context->awake[id] = true;
	}
	context->waitMask = 0;
	for (int i = 0; i < MAX_GBS; ++i) {
		context->awake[i] = true;
	}
}

static void init_gb_coord(void) {
	mLockstepInit(&g_gb_coord.d);
	GBSIOLockstepInit(&g_gb_coord);
	memset(&g_gb_context, 0, sizeof(g_gb_context));
	g_gb_context.lockstep = &g_gb_coord;
	g_gb_coord.d.context = &g_gb_context;
	g_gb_coord.d.lock = gb_lock;
	g_gb_coord.d.unlock = gb_unlock;
	g_gb_coord.d.signal = gb_signal;
	g_gb_coord.d.wait = gb_wait;
	g_gb_coord.d.addCycles = gb_add_cycles;
	g_gb_coord.d.useCycles = gb_use_cycles;
	g_gb_coord.d.unusedCycles = gb_unused_cycles;
	g_gb_coord.d.unload = gb_unload;
	for (int i = 0; i < MAX_GBS; ++i) {
		memset(&g_gb_nodes[i], 0, sizeof(g_gb_nodes[i]));
		GBSIOLockstepNodeCreate(&g_gb_nodes[i]);
		g_gb_context.awake[i] = true;
		GBSIOLockstepAttachNode(&g_gb_coord, &g_gb_nodes[i]);
	}
	g_has_gb_coord = true;
}

static void deinit_gb_coord(void) {
	if (g_has_gb_coord) {
		mLockstepDeinit(&g_gb_coord.d);
		g_has_gb_coord = false;
	}
}

static bool player_is_asleep(int i) {
	if (g_platform == mPLATFORM_GB && g_has_gb_coord) {
		return !g_gb_context.awake[i];
	}
	return g_asleep[i];
}

static bool player_can_run(int i) {
	return !player_is_asleep(i);
}

/* The lockstep calls sleep/wake while a player waits for the others to catch
 * up (e.g. mid-transfer). We step every core sequentially on one thread, so
 * these just flip a flag and mgs_run_frame skips sleeping cores — the same
 * cooperative model as native threaded play. */
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
void mgs_init(int count) {
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
	deinit_gb_coord();
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
	g_platform = mPLATFORM_NONE;
	if (g_count == MAX_GBS) {
		init_gb_coord();
	}
}

EMSCRIPTEN_KEEPALIVE
int mgs_load_rom(const uint8_t* rom, size_t len) {
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
		/* Detect the platform from the ROM header instead of trusting the file
		 * extension. This lets a .gb/.gbc image use the GB core and also keeps
		 * renamed or extensionless ROMs working. */
		struct VFile* detect = VFileMemChunk(rom, len);
		if (!detect) {
			return -2;
		}
		enum mPlatform platform = mCoreIsCompatible(detect);
		detect->close(detect);
		if (platform == mPLATFORM_NONE) {
			return -3;
		}
		if (g_platform == mPLATFORM_NONE) {
			g_platform = platform;
		} else if (g_platform != platform) {
			return -10;
		}
		if (platform == mPLATFORM_GB && g_count > MAX_GBS) {
			/* The mGBA GB serial link is a two-device cable. A solo GB/GBC
			 * session remains valid; 3/4-player GB groups are not. */
			return -9;
		}
		p->platform = platform;
		p->core = mCoreCreate(platform);
		if (!p->core) {
			return -4;
		}
		if (!p->core->init(p->core)) {
			return -5;
		}
		/* Match the desktop wrapper's exact init order. */
		if (p->core->setAudioBufferSize) {
			p->core->setAudioBufferSize(p->core, 2048);
		}
		p->core->opts.volume = 0x100;
		mCoreInitConfig(p->core, NULL);
		mCoreLoadConfig(p->core);

		/* Use a fixed maximum stride; currentVideoSize tells the frontend how
		 * many pixels in each row are meaningful. */
		p->core->setVideoBuffer(p->core, p->video, MAX_W);
		if (g_has_coord && platform == mPLATFORM_GBA) {
			p->core->setPeripheral(p->core, 0x1001 /* mPERIPH_GBA_LINK_PORT */, &g_drivers[i]);
		} else if (g_has_gb_coord && platform == mPLATFORM_GB) {
			struct GB* gb = p->core->board;
			GBSIOSetDriver(&gb->sio, &g_gb_nodes[i].d);
		}
		/* VFileMemChunk copies the ROM bytes into core-owned memory. */
		struct VFile* vf = VFileMemChunk(rom, len);
		if (!vf) {
			return -6;
		}
		if (!p->core->loadROM(p->core, vf)) {
			return -7;
		}
		p->core->reset(p->core);
		p->core->currentVideoSize(p->core, &p->width, &p->height);
		if (p->width > MAX_W || p->height > MAX_H) {
			return -8;
		}
		/* GB uses the same mGBA key enum values as the shared UI mask after
		 * translation in mgs_set_keys. GB link wiring uses the dedicated
		 * two-device GBSIOLockstep path above. */
		p->running = true;
	}
	memset(g_last_fc, 0, sizeof(g_last_fc));
	return 0;
}

#define FRAME_CYCLES 280896 /* GBA VIDEO_TOTAL_LENGTH; GB supplies its own value */

EMSCRIPTEN_KEEPALIVE
void mgs_run_frame(void) {
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
		budgets[i] = g_players[i].running && g_players[i].core->frameCycles
			? g_players[i].core->frameCycles(g_players[i].core)
			: FRAME_CYCLES;
		if (budgets[i] < 1) budgets[i] = FRAME_CYCLES;
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
			if (!p->running || !player_can_run(i) || budgets[i] <= 0) {
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
		for (unsigned y = 0; y < p->height; ++y) {
			for (unsigned x = 0; x < p->width; ++x) {
				uint32_t px = v[y * MAX_W + x];
				size_t j = (size_t)y * p->width + x;
				out[j * 4 + 0] = (uint8_t)(px & 0xFF);
				out[j * 4 + 1] = (uint8_t)((px >> 8) & 0xFF);
				out[j * 4 + 2] = (uint8_t)((px >> 16) & 0xFF);
				out[j * 4 + 3] = 0xFF;
			}
		}
	}
}

EMSCRIPTEN_KEEPALIVE
uint8_t* mgs_get_video(int player) {
	if (player < 0 || player >= g_count) {
		return NULL;
	}
	return g_players[player].rgba;
}

EMSCRIPTEN_KEEPALIVE
unsigned mgs_get_video_width(int player) {
	return player >= 0 && player < g_count ? g_players[player].width : 0;
}

EMSCRIPTEN_KEEPALIVE
unsigned mgs_get_video_height(int player) {
	return player >= 0 && player < g_count ? g_players[player].height : 0;
}

EMSCRIPTEN_KEEPALIVE
int mgs_get_platform(int player) {
	return player >= 0 && player < g_count ? g_players[player].platform : mPLATFORM_NONE;
}

EMSCRIPTEN_KEEPALIVE
void mgs_set_keys(int player, uint32_t keys) {
	if (player < 0 || player >= g_count || !g_players[player].running) {
		return;
	}
	/* The frontend keeps the GBA bit layout for controls. GB's input enum
	 * orders directions after A/B/Select/Start differently, so translate the
	 * shared UI mask before handing it to the SM83 core. */
	if (g_players[player].platform == mPLATFORM_GB) {
		uint32_t gb = 0;
		if (keys & KEY_A) gb |= 1u << 0;
		if (keys & KEY_B) gb |= 1u << 1;
		if (keys & KEY_SELECT) gb |= 1u << 2;
		if (keys & KEY_START) gb |= 1u << 3;
		if (keys & KEY_RIGHT) gb |= 1u << 4;
		if (keys & KEY_LEFT) gb |= 1u << 5;
		if (keys & KEY_UP) gb |= 1u << 6;
		if (keys & KEY_DOWN) gb |= 1u << 7;
		keys = gb;
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
void mgs_set_audio_source(int n) {
	g_audio_source = n;
}

EMSCRIPTEN_KEEPALIVE
int16_t* mgs_get_audio(void) {
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
int mgs_audio_frames(void) {
	return g_mix_frames;
}

EMSCRIPTEN_KEEPALIVE
unsigned mgs_get_audio_rate(void) {
	if (g_audio_source >= 1 && g_audio_source <= 4) {
		int i = g_audio_source - 1;
		if (i < g_count && g_players[i].running && g_players[i].core->audioSampleRate) {
			return g_players[i].core->audioSampleRate(g_players[i].core);
		}
	}
	/* A mixed stream can only have one advertised rate. All players currently
	 * share a ROM/platform; keep the historical GBA rate as the safe fallback. */
	return 32768;
}

static uint8_t* g_state = NULL;
static size_t g_state_size = 0;

EMSCRIPTEN_KEEPALIVE
size_t mgs_save_state(int player) {
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
uint8_t* mgs_state_ptr(void) {
	return g_state;
}

EMSCRIPTEN_KEEPALIVE
int mgs_load_state(int player) {
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
int mgs_load_state_bytes(int player, const uint8_t* data, size_t size) {
	if (player < 0 || player >= g_count || !g_players[player].running) {
		return -2;
	}
	if (!data || size == 0) {
		return -1;
	}
	return g_players[player].core->loadState(g_players[player].core, data) ? 0 : -3;
}

/* After a state-set import, the lockstep coordinator's per-player event
 * queues and asleep flags come back with the restored cores (mid-link saves
 * carry them in the driver state), and if they're left in place the games
 * freeze or corrupt their first handshake rounds. The desktop wrapper resets
 * every driver after loading states (EmulationManager::load_state_set);
 * mirror that here so browser imports behave identically. Call AFTER all
 * players' states are loaded. */
EMSCRIPTEN_KEEPALIVE
void mgs_reset_sio(void) {
	if (!g_has_coord) {
		return;
	}
	for (int i = 0; i < g_count; ++i) {
		if (g_drivers[i].d.p && g_drivers[i].d.reset) {
			g_drivers[i].d.reset(&g_drivers[i].d);
		}
	}
}

/* Turn the Four Swords link assist (and its deadlock kick) on or off.
 *
 * The assist pokes the game's private link state every ~2 s while the games sit
 * in the link-screen mode. It has never moved them off the linking screen in any
 * model, and sessions that run with it show the emulated game fetching from
 * unmapped memory (Out of bounds ROM 0x0D000000, Bad BIOS 0x00000000, Bad memory
 * 0xffffeXXX), so the frontend leaves it OFF unless explicitly asked for it with
 * `?fsassist=1`. See GBASIOLockstepCoordinatorSetFSSuppressed. */
EMSCRIPTEN_KEEPALIVE
void mgs_set_fs_assist(int enabled) {
	if (!g_has_coord) {
		return;
	}
	GBASIOLockstepCoordinatorSetFSSuppressed(&g_coord, !enabled);
}

/* Stepping stats for the most recent mgs_run_frame: loop iterations, lockstep
 * sleep/wake counts (transfer rendezvous activity), and emulated cycles. */
EMSCRIPTEN_KEEPALIVE
void mgs_get_stats(int out[4]) {
	out[0] = g_stat_steps;
	out[1] = g_stat_sleeps;
	out[2] = g_stat_wakes;
	out[3] = (int) g_stat_cycles;
}

EMSCRIPTEN_KEEPALIVE
void mgs_gb_test_transfer(int player, unsigned value, int fast) {
	if (!g_has_gb_coord || g_platform != mPLATFORM_GB || player < 0 || player >= MAX_GBS ||
			player >= g_count || !g_players[player].running) {
		return;
	}
	/* A GB transfer is initiated only after both ends have enabled their
	 * serial clocks.  The old probe enabled one node, which correctly left the
	 * coordinator idle and made the smoke test look like a broken link.  Drive
	 * both nodes here, just as two real games do during a link transaction. */
	for (int i = 0; i < MAX_GBS; ++i) {
		if (i >= g_count || !g_players[i].running) {
			continue;
		}
		struct GB* gb = (struct GB*) g_players[i].core->board;
		uint8_t tx = (uint8_t)(i == player ? value : (value ^ 0xFF));
		gb->memory.io[GB_REG_SB] = tx;
		GBSIOWriteSB(&gb->sio, tx);
		uint8_t sc = (uint8_t) (0x81 | (fast ? 0x02 : 0x00));
		gb->memory.io[GB_REG_SC] = sc;
		GBSIOWriteSC(&gb->sio, sc);
	}
}

EMSCRIPTEN_KEEPALIVE
unsigned mgs_gb_read_sb(int player) {
	if (g_platform != mPLATFORM_GB || player < 0 || player >= g_count || !g_players[player].running) {
		return 0xFF;
	}
	struct GB* gb = (struct GB*) g_players[player].core->board;
	return gb->memory.io[GB_REG_SB];
}

EMSCRIPTEN_KEEPALIVE
unsigned mgs_gb_read_sc(int player) {
	if (g_platform != mPLATFORM_GB || player < 0 || player >= g_count || !g_players[player].running) {
		return 0xFF;
	}
	struct GB* gb = (struct GB*) g_players[player].core->board;
	return gb->memory.io[GB_REG_SC];
}

EMSCRIPTEN_KEEPALIVE
void mgs_quit(void) {
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
	deinit_gb_coord();
	free(g_state);
	g_state = NULL;
	g_state_size = 0;
	g_count = 0;
	g_platform = mPLATFORM_NONE;
}