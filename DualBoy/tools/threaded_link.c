/* DualBoy threaded lockstep reference harness.
 *
 * Runs N GBA cores on N real threads linked through mGBA's own
 * GBASIORendezvousCoordinator + GBASIORendezvousDriver. The per-player thread uses
 * the same semantics as mGBA's mCoreThread: the lockstep `sleep`/`wake` are
 * deferred to the thread loop (sleep sets a flag and returns -- it is called
 * under the coordinator mutex and MUST NOT block -- and the loop blocks on a
 * condvar after runLoop returns), and each thread paces itself to ~60 FPS so
 * wall-clock scripted inputs are reproducible. This is the ground truth for
 * whether the lockstep DRIVER links a game, independent of DualBoy's
 * single-threaded wrapper.
 *
 * Modes:
 *   threaded_link <rom.gba> <players> <seconds> [script.txt]   # timed script
 *   threaded_link --fs <rom.gba>                               # drive FS, watch link
 *
 * script.txt lines: `time_ms player keymask` (player 0-based).
 * GBA keys: A=0x1 B=0x2 Sel=0x4 Start=0x8 Right=0x10 Left=0x20 Up=0x40
 *           Down=0x80 R=0x100 L=0x200.
 *
 * The SIO DEBUG chatter goes to stdout; the FS watcher prints screen labels to
 * stderr. Judge the link by stdout counts of `Transfer starting`,
 * `MULTI transfer finished`, and `did not receive`.
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/core/lockstep.h>
#include "rendezvous.h"
#include <mgba/gba/interface.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#define MAX_PLAYERS 4
#define W 240
#define H 160
#define MAX_KEYS_EVENTS 65536

/* GBA input bits (mGBA order). */
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

enum Screen {
    S_UNKNOWN, S_TITLE, S_FILE, S_NAME, S_CHOOSE, S_ALTP, S_MULTIPAK, S_SAVING,
};

struct Player {
    struct mCore* core;
    struct GBASIORendezvousDriver driver;
    struct mLockstepUser user;
    pthread_t thread;
    pthread_mutex_t mutex;   /* guards asleep/stop */
    pthread_cond_t cond;
    bool asleep;
    bool stop;
    uint32_t video[W * H];

    pthread_mutex_t snapMutex;  /* guards snapshot/frameReady */
    uint32_t snapshot[W * H];
    bool frameReady;
    uint32_t lastFc;
    int framesProduced;
    int preferredId;
};

struct KeyEvent {
    int64_t time_ms;
    int player;
    uint32_t keys;
};

static struct Player g_players[MAX_PLAYERS];
static int g_playerCount;

static struct Player* user_player(struct mLockstepUser* u) {
    return (struct Player*) ((char*) u - offsetof(struct Player, user));
}

static int requested_id(struct mLockstepUser* u) {
    return user_player(u)->preferredId;
}

static void user_sleep(struct mLockstepUser* u) {
    struct Player* p = user_player(u);
    pthread_mutex_lock(&p->mutex);
    p->asleep = true;
    pthread_mutex_unlock(&p->mutex);
}

static void user_wake(struct mLockstepUser* u) {
    struct Player* p = user_player(u);
    pthread_mutex_lock(&p->mutex);
    p->asleep = false;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
}

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long) (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void* run_thread(void* arg) {
    struct Player* p = arg;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (1) {
        p->core->runLoop(p->core);

        /* Deferred lockstep sleep: block after the current runLoop returns. */
        pthread_mutex_lock(&p->mutex);
        bool stop = p->stop;
        while (p->asleep && !p->stop) {
            pthread_cond_wait(&p->cond, &p->mutex);
        }
        pthread_mutex_unlock(&p->mutex);
        if (stop) {
            break;
        }

        uint32_t fc = p->core->frameCounter(p->core);
        if (fc != p->lastFc) {
            p->lastFc = fc;
            ++p->framesProduced;
            pthread_mutex_lock(&p->snapMutex);
            memcpy(p->snapshot, p->video, sizeof(p->video));
            p->frameReady = true;
            pthread_mutex_unlock(&p->snapMutex);

            /* Pace to ~60 FPS against a fixed start, so scripted inputs are
             * reproducible. If lockstep stalled us, the deadline is in the past
             * and we don't sleep -- we just catch up. */
            int64_t target_ns = (int64_t) p->framesProduced * 16666667LL;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t elapsed_ns = (now.tv_sec - start.tv_sec) * 1000000000LL +
                                 (now.tv_nsec - start.tv_nsec);
            int64_t wait_ns = target_ns - elapsed_ns;
            if (wait_ns > 0) {
                struct timespec ts = {wait_ns / 1000000000LL, wait_ns % 1000000000LL};
                nanosleep(&ts, NULL);
            }
        }
    }
    return NULL;
}

static void set_keys(int i, uint32_t keys) {
    g_players[i].core->setKeys(g_players[i].core, keys);
}

/* ---- pixel-accurate screen detection (ported from nav_fs.py) ---- */

static void rgb(uint32_t p, int* r, int* g, int* b) {
    *r = p & 0xFF;
    *g = (p >> 8) & 0xFF;
    *b = (p >> 16) & 0xFF;
}

static int pred_gold(int r, int g, int b) { return r > 170 && g > 110 && b < 120; }
static int pred_dark(int r, int g, int b) { return r < 60 && g < 60 && b < 90; }
static int pred_purple(int r, int g, int b) { return r > 90 && b > 110 && r > g + 40; }
static int pred_light(int r, int g, int b) { return r > 150 && g > 150 && b > 170; }
static int pred_green(int r, int g, int b) { return g > 100 && g > r + 30 && g > b + 20; }

static double frac(const uint32_t* frame, int x0, int y0, int x1, int y1,
                   int (*pred)(int, int, int)) {
    int n = 0, tot = 0;
    for (int y = y0; y < y1; y += 2) {
        for (int x = x0; x < x1; x += 2) {
            int r, g, b;
            rgb(frame[y * W + x], &r, &g, &b);
            ++tot;
            if (pred(r, g, b)) {
                ++n;
            }
        }
    }
    return tot ? (double) n / tot : 0.0;
}

static enum Screen detect(const uint32_t* frame) {
    double gold_top = frac(frame, 40, 20, 200, 95, pred_gold);
    double dark_mid = frac(frame, 10, 40, 230, 120, pred_dark);
    double purple_mid = frac(frame, 10, 40, 230, 130, pred_purple);
    double green_mid = frac(frame, 10, 60, 230, 120, pred_green);

    int light_rows = 0, light_total = 0;
    for (int y = 70; y < 135; y += 2) {
        int row = 0;
        for (int x = 20; x < 220; x += 2) {
            int r, g, b;
            rgb(frame[y * W + x], &r, &g, &b);
            if (pred_light(r, g, b)) {
                ++row;
            }
        }
        light_total += row;
        if (row >= 2) {
            ++light_rows;
        }
    }

    if (dark_mid > 0.5) return S_MULTIPAK;
    if (purple_mid > 0.25) return S_SAVING;
    if (gold_top > 0.08) return S_TITLE;
    if (green_mid > 0.25 || light_total > 400) return S_ALTP;
    if (light_rows >= 8 && light_total > 100) return S_NAME;
    if (green_mid > 0.10) return S_CHOOSE;
    return S_FILE;
}

static const char* screen_name(enum Screen s) {
    switch (s) {
    case S_TITLE: return "title";
    case S_FILE: return "file";
    case S_NAME: return "name";
    case S_CHOOSE: return "choose";
    case S_ALTP: return "alttp";
    case S_MULTIPAK: return "multipak";
    case S_SAVING: return "saving";
    default: return "unknown";
    }
}

/* Dump a u32 frame as a P6 PPM (PIL-compatible) for visual inspection. */
static void dump_ppm(const char* path, const uint32_t* frame) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int r, g, b;
            rgb(frame[y * W + x], &r, &g, &b);
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
}

/* Local per-player frame buffers the controller reads from. */
static uint32_t g_cur[MAX_PLAYERS][W * H];
static bool g_curValid[MAX_PLAYERS];

/* Copy each player's latest ready snapshot into g_cur. */
static void poll_frames(void) {
    for (int i = 0; i < g_playerCount; ++i) {
        pthread_mutex_lock(&g_players[i].snapMutex);
        if (g_players[i].frameReady) {
            memcpy(g_cur[i], g_players[i].snapshot, sizeof(g_cur[i]));
            g_players[i].frameReady = false;
            g_curValid[i] = true;
        }
        pthread_mutex_unlock(&g_players[i].snapMutex);
    }
}

static enum Screen cur_screen(int i) {
    poll_frames();
    return g_curValid[i] ? detect(g_cur[i]) : S_UNKNOWN;
}

static void tap(int i, uint32_t btn, int hold_ms, int gap_ms) {
    set_keys(i, btn);
    sleep_ms(hold_ms);
    set_keys(i, 0);
    sleep_ms(gap_ms);
}

/* Poll until cur_screen(i) is one of `want`, or timeout. */
static enum Screen wait_screen(int i, const enum Screen* want, int nwant, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    enum Screen last = S_UNKNOWN;
    while (now_ms() < deadline) {
        last = cur_screen(i);
        for (int k = 0; k < nwant; ++k) {
            if (last == want[k]) {
                return last;
            }
        }
        sleep_ms(25);
    }
    return last;
}

/* Keyboard cursor (orange-ish pixel centroid) or -1,-1 when not found. */
static void cursor_pos(int i, int* cx, int* cy) {
    poll_frames();
    long xs = 0, ys = 0, n = 0;
    for (int y = 66; y < 150; y += 2) {
        for (int x = 0; x < 240; x += 2) {
            int r, g, b;
            rgb(g_cur[i][y * W + x], &r, &g, &b);
            if (r > 170 && g > 60 && g < 170 && b > 60 && b < 170 && r > g + 30) {
                xs += x;
                ys += y;
                ++n;
            }
        }
    }
    if (!n) {
        *cx = *cy = -1;
        return;
    }
    *cx = (int) (xs / n);
    *cy = (int) (ys / n);
}

static void reset_cursor_to_a(int i) {
    for (int k = 0; k < 8; ++k) {
        int cx, cy;
        cursor_pos(i, &cx, &cy);
        if (cx < 0) {
            sleep_ms(300);
            continue;
        }
        if (cy > 72) {
            tap(i, KEY_UP, 200, 250);
        } else if (cx > 34) {
            tap(i, KEY_LEFT, 200, 250);
        } else {
            return;
        }
    }
}

static void nav_player_to_fs_title(int i) {
    /* Boot -> name entry (whatever boot screen we landed on). */
    fprintf(stderr, "P%d: booting to name entry\n", i + 1);
    for (int k = 0; k < 20; ++k) {
        enum Screen s = cur_screen(i);
        if (s == S_NAME) break;
        if (s == S_TITLE || s == S_FILE) {
            tap(i, KEY_A, 200, 250);
            sleep_ms(400);
        } else if (s == S_CHOOSE || s == S_UNKNOWN || s == S_ALTP) {
            tap(i, KEY_A, 200, 250);
            sleep_ms(400);
        }
        sleep_ms(300);
    }
    if (cur_screen(i) != S_NAME) {
        fprintf(stderr, "P%d: WARN not at name entry (got %s); continuing\n",
                i + 1, screen_name(cur_screen(i)));
    }

    /* Type AAAA, then walk to END. */
    reset_cursor_to_a(i);
    for (int k = 0; k < 4; ++k) {
        tap(i, KEY_A, 200, 250);
        sleep_ms(150);
    }
    for (int k = 0; k < 5; ++k) tap(i, KEY_DOWN, 200, 250);
    for (int k = 0; k < 3; ++k) tap(i, KEY_RIGHT, 200, 250);
    tap(i, KEY_A, 200, 250);
    sleep_ms(600);

    enum Screen savewant[] = {S_FILE, S_CHOOSE, S_SAVING};
    enum Screen after = wait_screen(i, savewant, 3, 20000);
    fprintf(stderr, "P%d: after save -> %s\n", i + 1, screen_name(after));

    /* Choose the saved slot, then into CHOOSE A GAME. */
    tap(i, KEY_A, 200, 250);
    sleep_ms(800);
    enum Screen gw[] = {S_CHOOSE, S_FILE};
    enum Screen g = wait_screen(i, gw, 2, 10000);
    if (g == S_FILE) {
        tap(i, KEY_A, 200, 250);
        sleep_ms(800);
        enum Screen gc[] = {S_CHOOSE};
        g = wait_screen(i, gc, 1, 8000);
    }
    fprintf(stderr, "P%d: game select -> %s\n", i + 1, screen_name(g));

    /* Four Swords is the RIGHT panel. */
    tap(i, KEY_RIGHT, 200, 250);
    sleep_ms(400);
    tap(i, KEY_A, 200, 250);
    sleep_ms(3000);
    fprintf(stderr, "P%d: selected Four Swords, screen=%s\n", i + 1, screen_name(cur_screen(i)));
}

static int run_fs(const char* rom) {
    /* Two players. */
    g_playerCount = 2;
    fprintf(stderr, "driving both players to the Four Swords title\n");
    nav_player_to_fs_title(0);
    nav_player_to_fs_title(1);

    /* Wait until both are on the FS title (gold logo). */
    fprintf(stderr, "waiting for both players at FS title\n");
    for (int k = 0; k < 60; ++k) {
        enum Screen s0 = cur_screen(0);
        enum Screen s1 = cur_screen(1);
        if (s0 == S_TITLE && s1 == S_TITLE) {
            break;
        }
        sleep_ms(500);
    }
    fprintf(stderr, "FS title reached: P1=%s P2=%s\n",
            screen_name(cur_screen(0)), screen_name(cur_screen(1)));

    /* Simultaneous START on both to enter the link handshake. */
    fprintf(stderr, "pressing START on both simultaneously\n");
    dump_ppm("/tmp/fs_pre_start_p1.ppm", g_cur[0]);
    dump_ppm("/tmp/fs_pre_start_p2.ppm", g_cur[1]);
    set_keys(0, KEY_START);
    set_keys(1, KEY_START);
    sleep_ms(200);
    set_keys(0, 0);
    set_keys(1, 0);

    /* Watch: log each player's screen + the link transfer activity. */
    fprintf(stderr, "watching handshake for 60s\n");
    int64_t start = now_ms();
    while (now_ms() - start < 60000) {
        sleep_ms(2000);
        int t = (int) ((now_ms() - start) / 1000);
        fprintf(stderr, "[%ds] P1=%s P2=%s | frames P1=%d P2=%d\n", t,
                screen_name(cur_screen(0)), screen_name(cur_screen(1)),
                g_players[0].framesProduced, g_players[1].framesProduced);
        if (t == 10) { dump_ppm("/tmp/fs_watch10_p1.ppm", g_cur[0]); dump_ppm("/tmp/fs_watch10_p2.ppm", g_cur[1]); }
        if (t == 30) { dump_ppm("/tmp/fs_watch30_p1.ppm", g_cur[0]); dump_ppm("/tmp/fs_watch30_p2.ppm", g_cur[1]); }
    }
    dump_ppm("/tmp/fs_final_p1.ppm", g_cur[0]);
    dump_ppm("/tmp/fs_final_p2.ppm", g_cur[1]);
    fprintf(stderr, "final: P1=%s P2=%s\n",
            screen_name(cur_screen(0)), screen_name(cur_screen(1)));
    return 0;
}

static void load_script(const char* path, struct KeyEvent** outEvents, int* outCount) {
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
    bool fs_mode = false;
    const char* rom;
    int players = 0;
    int seconds = 0;
    const char* script = NULL;

    if (argc >= 3 && strcmp(argv[1], "--fs") == 0) {
        fs_mode = true;
        rom = argv[2];
        players = 2;
    } else if (argc >= 4) {
        rom = argv[1];
        players = atoi(argv[2]);
        seconds = atoi(argv[3]);
        if (argc >= 5) {
            script = argv[4];
        }
    } else {
        fprintf(stderr, "usage: %s <rom.gba> <players 2-4> <seconds> [script.txt]\n", argv[0]);
        fprintf(stderr, "       %s --fs <rom.gba>\n", argv[0]);
        return 1;
    }

    if (players < 2 || players > 4) {
        fprintf(stderr, "players must be 2-4\n");
        return 1;
    }

    struct KeyEvent* events = NULL;
    int eventCount = 0;
    if (script) {
        load_script(script, &events, &eventCount);
    }

    struct mStandardLogger logger = {0};
    mStandardLoggerInit(&logger);
    logger.logToStdout = true;
    if (logger.d.filter) {
        logger.d.filter->defaultLevels = mLOG_WARN | mLOG_ERROR | mLOG_FATAL;
        mLogFilterSet(logger.d.filter, "gba.sio", mLOG_DEBUG);
    }
    mLogSetDefaultLogger(&logger.d);

    struct GBASIORendezvousCoordinator coordinator;
    GBASIORendezvousCoordinatorInit(&coordinator);

    g_playerCount = players;
    memset(g_players, 0, sizeof(g_players));

    for (int i = 0; i < players; ++i) {
        struct Player* p = &g_players[i];
        p->preferredId = i;
        pthread_mutex_init(&p->mutex, NULL);
        pthread_cond_init(&p->cond, NULL);
        pthread_mutex_init(&p->snapMutex, NULL);

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

        GBASIORendezvousDriverCreate(&p->driver, &p->user);
        GBASIORendezvousCoordinatorAttach(&coordinator, &p->driver);
        p->user.sleep = user_sleep;
        p->user.wake = user_wake;
        p->user.requestedId = requested_id;
    }

    for (int i = 0; i < players; ++i) {
        g_players[i].core->setPeripheral(g_players[i].core, mPERIPH_GBA_LINK_PORT,
                                         &g_players[i].driver.d);
    }
    for (int i = 0; i < players; ++i) {
        /* Registers the player in the coordinator (driver reset) and boots. */
        g_players[i].core->reset(g_players[i].core);
    }

    for (int i = 0; i < players; ++i) {
        if (pthread_create(&g_players[i].thread, NULL, run_thread, &g_players[i])) {
            fprintf(stderr, "pthread_create failed for player %d\n", i);
            return 1;
        }
    }
    fprintf(stderr, "running %d players\n", players);

    if (fs_mode) {
        run_fs(rom);
    } else {
        struct timespec tstart;
        clock_gettime(CLOCK_MONOTONIC, &tstart);
        int nextEvent = 0;
        while (1) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t elapsed_ms = (now.tv_sec - tstart.tv_sec) * 1000LL +
                                 (now.tv_nsec - tstart.tv_nsec) / 1000000LL;
            if (elapsed_ms >= (int64_t) seconds * 1000) {
                break;
            }
            while (nextEvent < eventCount && events[nextEvent].time_ms <= elapsed_ms) {
                struct KeyEvent* e = &events[nextEvent++];
                if (e->player >= 0 && e->player < players) {
                    set_keys(e->player, e->keys);
                }
            }
            sleep_ms(1);
        }
    }

    for (int i = 0; i < players; ++i) {
        pthread_mutex_lock(&g_players[i].mutex);
        g_players[i].stop = true;
        pthread_cond_signal(&g_players[i].cond);
        pthread_mutex_unlock(&g_players[i].mutex);
    }
    for (int i = 0; i < players; ++i) {
        pthread_join(g_players[i].thread, NULL);
    }
    for (int i = 0; i < players; ++i) {
        g_players[i].core->unloadROM(g_players[i].core);
        g_players[i].core->deinit(g_players[i].core);
        pthread_mutex_destroy(&g_players[i].mutex);
        pthread_cond_destroy(&g_players[i].cond);
        pthread_mutex_destroy(&g_players[i].snapMutex);
    }
    GBASIORendezvousCoordinatorDeinit(&coordinator);
    mStandardLoggerDeinit(&logger);
    fprintf(stderr, "done\n");
    return 0;
}
