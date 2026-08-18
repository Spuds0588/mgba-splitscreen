/* DualBoy threaded lockstep reference harness.
 *
 * Runs N GBA cores on N real threads using mGBA's own threading + link
 * machinery -- `mCoreThread`, `mLockstepThreadUser`, `GBASIOLockstepDriver`
 * and one shared `GBASIOLockstepCoordinator`. This is the same mechanism the
 * mGBA Qt frontend uses for "new multiplayer window", so it is the ground
 * truth for the question: does the lockstep DRIVER link a game, independent
 * of DualBoy's single-threaded wrapper (which fakes sleep/wake with flags)?
 *
 * Usage:
 *   threaded_link <rom.gba> <players> <seconds> [script.txt]
 *
 * `script.txt` optionally drives inputs. Lines are `time_ms player keymask`:
 *   1000 0 0x0008     # press START on player 1 (1-based in UI, 0-based here)
 * GBA key bits (mGBA order): A=0x001 B=0x002 Select=0x004 Start=0x008
 * Right=0x010 Left=0x020 Up=0x040 Down=0x080 R=0x100 L=0x200.
 *
 * The SIO DEBUG logs (transfer start / finish / "did not receive") are how a
 * pass/fail is judged: count `Transfer starting`, `MULTI transfer finished`,
 * and `did not receive` in stdout.
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/core/thread.h>
#include <mgba/core/lockstep.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/gba/interface.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#define MAX_KEYS_EVENTS 65536

struct Player {
    struct mCore* core;
    struct GBASIOLockstepDriver driver;
    struct mLockstepThreadUser user;
    struct mCoreThread thread;
    uint32_t video[240 * 160];
    int preferredId;
};

struct KeyEvent {
    int64_t time_ms;
    int player;
    uint32_t keys;
};

static struct Player* g_players;
static int g_playerCount;

static int _requestedId(struct mLockstepUser* user) {
    struct Player* p = (struct Player*) ((char*) user - offsetof(struct Player, user));
    return p->preferredId;
}

static void _loadScript(const char* path, struct KeyEvent** outEvents, int* outCount) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open script %s\n", path);
        exit(1);
    }
    struct KeyEvent* events = calloc(MAX_KEYS_EVENTS, sizeof(*events));
    int count = 0;
    long long t;
    int player;
    unsigned keys;
    while (count < MAX_KEYS_EVENTS && fscanf(f, "%lld %d %x", &t, &player, &keys) == 3) {
        events[count].time_ms = t;
        events[count].player = player;
        events[count].keys = keys;
        ++count;
    }
    fclose(f);
    *outEvents = events;
    *outCount = count;
    fprintf(stderr, "loaded %d script events\n", count);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <rom.gba> <players 2-4> <seconds> [script.txt]\n", argv[0]);
        return 1;
    }
    const char* rom = argv[1];
    int players = atoi(argv[2]);
    int seconds = atoi(argv[3]);
    if (players < 2 || players > 4) {
        fprintf(stderr, "players must be 2-4\n");
        return 1;
    }

    struct KeyEvent* events = NULL;
    int eventCount = 0;
    if (argc >= 5) {
        _loadScript(argv[4], &events, &eventCount);
    }

    // Logging: WARN+ by default, DEBUG for gba.sio so the link chatter shows.
    // Must be zeroed: mStandardLoggerInit only sets d.log/d.filter; logFile is
    // otherwise stack garbage and _mCoreStandardLog dereferences it.
    struct mStandardLogger logger = {0};
    mStandardLoggerInit(&logger);
    logger.logToStdout = true;
    if (logger.d.filter) {
        logger.d.filter->defaultLevels = mLOG_WARN | mLOG_ERROR | mLOG_FATAL;
        mLogFilterSet(logger.d.filter, "gba.sio", mLOG_DEBUG);
    }
    mLogSetDefaultLogger(&logger.d);

    struct GBASIOLockstepCoordinator coordinator;
    GBASIOLockstepCoordinatorInit(&coordinator);

    g_playerCount = players;
    g_players = calloc(players, sizeof(*g_players));

    for (int i = 0; i < players; ++i) {
        struct Player* p = &g_players[i];
        p->preferredId = i;

        p->core = mCoreCreate(mPLATFORM_GBA);
        if (!p->core) {
            fprintf(stderr, "mCoreCreate failed for player %d\n", i);
            return 1;
        }
        if (!p->core->init(p->core)) {
            fprintf(stderr, "core init failed for player %d\n", i);
            return 1;
        }
        mCoreInitConfig(p->core, NULL);
        mCoreLoadConfig(p->core);
        p->core->setVideoBuffer(p->core, p->video, 240);

        if (!mCoreLoadFile(p->core, rom)) {
            fprintf(stderr, "mCoreLoadFile failed for player %d\n", i);
            return 1;
        }

        GBASIOLockstepDriverCreate(&p->driver, &p->user.d);
        GBASIOLockstepCoordinatorAttach(&coordinator, &p->driver);
        p->thread.core = p->core;
        mLockstepThreadUserInit(&p->user, &p->thread);
        p->user.d.requestedId = _requestedId;
    }

    // Attach the link driver to every core BEFORE any of them resets, so the
    // game sees the cable present at boot (same as DualBoy/mGBA-Qt).
    for (int i = 0; i < players; ++i) {
        struct Player* p = &g_players[i];
        p->core->setPeripheral(p->core, mPERIPH_GBA_LINK_PORT, &p->driver.d);
    }

    // Start all threads (mCoreThreadStart resets + runs the core).
    for (int i = 0; i < players; ++i) {
        if (!mCoreThreadStart(&g_players[i].thread)) {
            fprintf(stderr, "mCoreThreadStart failed for player %d\n", i);
            return 1;
        }
    }
    fprintf(stderr, "running %d players for %d seconds\n", players, seconds);

    // Main loop: inject scripted inputs, then sleep. Emulation runs on the
    // other threads; we're just the input provider here.
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int nextEvent = 0;
    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ms = (now.tv_sec - start.tv_sec) * 1000LL +
                             (now.tv_nsec - start.tv_nsec) / 1000000LL;
        if (elapsed_ms >= (int64_t) seconds * 1000) {
            break;
        }
        while (nextEvent < eventCount && events[nextEvent].time_ms <= elapsed_ms) {
            struct KeyEvent* e = &events[nextEvent++];
            if (e->player >= 0 && e->player < players) {
                g_players[e->player].core->setKeys(g_players[e->player].core, e->keys);
            }
        }
        struct timespec ts = {0, 1000000}; // 1 ms
        nanosleep(&ts, NULL);
    }

    for (int i = 0; i < players; ++i) {
        mCoreThreadEnd(&g_players[i].thread);
    }
    for (int i = 0; i < players; ++i) {
        mCoreThreadJoin(&g_players[i].thread);
    }
    for (int i = 0; i < players; ++i) {
        g_players[i].core->unloadROM(g_players[i].core);
        g_players[i].core->deinit(g_players[i].core);
    }
    GBASIOLockstepCoordinatorDeinit(&coordinator);
    mStandardLoggerDeinit(&logger);
    fprintf(stderr, "done\n");
    return 0;
}
