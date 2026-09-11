/* mgba-splitscreen threaded lockstep reference harness.
 *
 * Runs N GBA cores on N real threads linked through mGBA's own
 * GBASIORendezvousCoordinator + GBASIORendezvousDriver. The per-player thread uses
 * the same semantics as mGBA's mCoreThread: the lockstep `sleep`/`wake` are
 * deferred to the thread loop (sleep sets a flag and returns -- it is called
 * under the coordinator mutex and MUST NOT block -- and the loop blocks on a
 * condvar after runLoop returns), and each thread paces itself to ~60 FPS so
 * wall-clock scripted inputs are reproducible. This is the ground truth for
 * whether the lockstep DRIVER links a game, independent of mgba-splitscreen's
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
#include <mgba/internal/gba/sio/lockstep.h>
#include "rendezvous.h"
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/arm/arm.h>

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

/* Both driver types embed struct GBASIODriver d at offset 0, so a union lets
 * the harness exercise the app's real lockstep driver (--fs6) as well as the
 * local rendezvous fork (default). Access the shared prefix via driver.d. */
union DriverBox {
    struct GBASIORendezvousDriver rv;
    struct GBASIOLockstepDriver ls;
};

struct Player {
    struct mCore* core;
    union DriverBox driver;
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
    volatile int heartbeat;  /* run_thread iterations (freeze diagnostic) */
    int preferredId;
};

struct KeyEvent {
    int64_t time_ms;
    int player;
    uint32_t keys;
};

static struct Player g_players[MAX_PLAYERS];
static int g_playerCount;
/* --fs6/--fs7: use the app's real GBASIOLockstep driver (with the driver-side
 * FS deadlock kick) instead of the local rendezvous fork. --fs6 validates the
 * DRIVER's inline kick only; --fs7 additionally fires the HARNESS kick (the
 * known-good one from the rendezvous runs) so we can isolate whether the
 * lockstep driver itself can reach gameplay when kicked the way --fs4 does. */
static bool g_useLockstep;
static bool g_harnessKick;
static bool g_noInlineKick;
static int g_fsPlayers = 2;  /* --fs10: drive N players through the FS flow */
static const char* g_stateFile;  /* --fs11: DUALSTATE blob to load instead of nav */
static uint8_t* g_stateBlobs[4];
static size_t g_stateSizes[4];
static bool g_armed;  /* arm the echo assist (fs3 only) */
static struct GBASIOLockstepCoordinator g_lsCoord;
static struct GBASIORendezvousCoordinator g_rvCoord;

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
        ++p->heartbeat;

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

/* Dump FS's link state machine (EWRAM struct at 0x02030790) for both players.
 * Fields (from disassembly of the SIO handler at 0x800C7E8):
 *   +0  byte  role flag (master/slave)
 *   +2  byte  checksum input A
 *   +3  byte  checksum input B (buf[1] = A ^ B)
 *   +4  byte  send-buffer ready flag
 *   +5  byte  "got 12" flag (recv counter reached 12)
 *   +7  byte  busy-latched
 *   +9  byte  probe round counter (0..3)
 *   +11 byte  send counter input (buf[0])
 *   +14 word  send counter
 *   +20 word  send index (table offset)
 *   +24 word  recv index
 *   +28/32   send buffer ptr / alt
 *   +36/40   recv buffer ptr / alt
 */
static void dump_fs_state(int i, int t, const char* tag) {
    size_t sz = 0;
    uint8_t* m = mCoreGetMemoryBlock(g_players[i].core, 0x02030790, &sz);
    if (!m || sz < 48) {
        fprintf(stderr, "[%ds] P%d %s: no mem\n", t, i + 1, tag);
        return;
    }
    uint16_t u16_at = 0; (void) u16_at;
    uint32_t w20, w24;
    memcpy(&w20, m + 20, 4);
    memcpy(&w24, m + 24, 4);
    uint32_t w8;
    memcpy(&w8, m + 8, 4);
    /* SIOCNT at 0x04000128: bits 15-14 = mode (0=8bit,1=32bit,2=multi,3=uart).
     * mCoreGetMemoryBlock can't read the IO region (returns NULL), so read the
     * latch the SIO layer keeps on the driver's GBASIO struct. */
    uint16_t siocnt = 0xFFFF;
    uint16_t siom[5] = {0, 0, 0, 0, 0};
    if (g_players[i].driver.rv.d.p && g_players[i].driver.rv.d.p->p) {
        siocnt = g_players[i].driver.rv.d.p->siocnt;
        uint16_t* io = g_players[i].driver.rv.d.p->p->memory.io;
        siom[0] = io[GBA_REG(SIOMULTI0) >> 1];
        siom[1] = io[GBA_REG(SIOMULTI1) >> 1];
        siom[2] = io[GBA_REG(SIOMULTI2) >> 1];
        siom[3] = io[GBA_REG(SIOMULTI3) >> 1];
        siom[4] = io[GBA_REG(SIOMLT_SEND) >> 1];
    }
    /* Game-side screen state: mode byte at 0x03006D10 dispatches the main
     * loop; sub-mode at 0x03005050 drives modes 5/6; [0x03000520/21] gate the
     * SIO screen dispatcher; 0x03000900..07 must become 0F x8 for the link to
     * be declared complete (0x8068108 compares it to a ROM table). */
    uint8_t gm = 0, gs = 0, s20 = 0, s21 = 0, ssub = 0;
    uint8_t linkok[8] = {0};
    uint8_t s168[40] = {0};
    uint8_t lstat[16] = {0};
    uint8_t shadow[4] = {0};
    uint8_t payload[16] = {0};
    uint8_t ist[48] = {0};
    uint8_t isend[24] = {0};
    uint8_t* w = mCoreGetMemoryBlock(g_players[i].core, 0x03006D10, &sz);
    if (w && sz >= 1) gm = w[0];
    w = mCoreGetMemoryBlock(g_players[i].core, 0x03005050, &sz);
    if (w && sz >= 1) gs = w[0];
    w = mCoreGetMemoryBlock(g_players[i].core, 0x03000520, &sz);
    if (w && sz >= 2) { s20 = w[0]; s21 = w[1]; }
    w = mCoreGetMemoryBlock(g_players[i].core, 0x03000BFC, &sz);
    if (w && sz >= 1) ssub = w[0];
    w = mCoreGetMemoryBlock(g_players[i].core, 0x03000900, &sz);
    if (w && sz >= 8) memcpy(linkok, w, 8);
    /* mode-9 link-screen sub-state block (EWRAM 0x02016D60). */
    w = mCoreGetMemoryBlock(g_players[i].core, 0x02016D60, &sz);
    if (w && sz >= 40) memcpy(s168, w, 40);
    /* link-status struct 0x03000FC0: +1 state, +2 counter, +3 status, +16/+17 flags. */
    w = mCoreGetMemoryBlock(g_players[i].core, 0x03000FC0, &sz);
    if (w && sz >= 16) memcpy(lstat, w, 16);
    /* phase-machine result shadow. */
    w = mCoreGetMemoryBlock(g_players[i].core, 0x03000BF0, &sz);
    if (w && sz >= 4) memcpy(shadow, w, 4);
    /* accepted-payload area (scan dest, 20-byte slot stride). */
    w = mCoreGetMemoryBlock(g_players[i].core, 0x03000FF8, &sz);
    if (w && sz >= 16) memcpy(payload, w, 16);
    /* IWRAM link-state block the table builder (0x800c658) uses. */
    uint32_t isendPtr = 0;
    w = mCoreGetMemoryBlock(g_players[i].core, 0x03000FD0, &sz);
    if (w && sz >= 48) {
        memcpy(ist, w, 48);
        memcpy(&isendPtr, ist + 28, 4);
    }
    if (isendPtr) {
        w = mCoreGetMemoryBlock(g_players[i].core, isendPtr, &sz);
        if (w && sz >= 24) memcpy(isend, w, 24);
    }
    fprintf(stderr, "[%ds] P%d %s state: phase=%u role=%u a=%u b=%u ready=%u got12=%u busy=%u round=%u cnt=%u "
                    "send=%u recv=%u w8=%08X siocnt=%04X(mode=%u) mlt=%04X,%04X,%04X,%04X snd=%04X "
                    "game=%u:%u sub=%u sio=%u/%u linkok=%02X%02X%02X%02X%02X%02X%02X%02X "
                    "ls=%02X%02X%02X%02X ls2=%02X%02X%02X%02X lstat=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                    "shad=%02X%02X%02X%02X pay=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
                    "ist=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
                    "isend=%04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X\n",
            t, i + 1, tag,
            m[1], m[0], m[2], m[3], m[4], m[5], m[7], m[9], m[11],
            (unsigned) w20, (unsigned) w24, (unsigned) w8, (unsigned) siocnt, (unsigned) (siocnt >> 14),
            (unsigned) siom[0], (unsigned) siom[1], (unsigned) siom[2], (unsigned) siom[3], (unsigned) siom[4],
            (unsigned) gm, (unsigned) gs, (unsigned) ssub, (unsigned) s20, (unsigned) s21,
            (unsigned) linkok[0], (unsigned) linkok[1], (unsigned) linkok[2], (unsigned) linkok[3],
            (unsigned) linkok[4], (unsigned) linkok[5], (unsigned) linkok[6], (unsigned) linkok[7],
            (unsigned) s168[0], (unsigned) s168[1], (unsigned) s168[2], (unsigned) s168[3],
            (unsigned) s168[4], (unsigned) s168[5], (unsigned) s168[6], (unsigned) s168[7],
            (unsigned) lstat[0], (unsigned) lstat[1], (unsigned) lstat[2], (unsigned) lstat[3],
            (unsigned) lstat[4], (unsigned) lstat[5], (unsigned) lstat[6], (unsigned) lstat[7],
            (unsigned) lstat[8], (unsigned) lstat[9], (unsigned) lstat[10], (unsigned) lstat[11],
            (unsigned) lstat[12], (unsigned) lstat[13], (unsigned) lstat[14], (unsigned) lstat[15],
            (unsigned) shadow[0], (unsigned) shadow[1], (unsigned) shadow[2], (unsigned) shadow[3],
            (unsigned) payload[0], (unsigned) payload[1], (unsigned) payload[2], (unsigned) payload[3],
            (unsigned) payload[4], (unsigned) payload[5], (unsigned) payload[6], (unsigned) payload[7],
            (unsigned) payload[8], (unsigned) payload[9], (unsigned) payload[10], (unsigned) payload[11],
            (unsigned) payload[12], (unsigned) payload[13], (unsigned) payload[14], (unsigned) payload[15],
            (unsigned) ist[0], (unsigned) ist[1], (unsigned) ist[2], (unsigned) ist[3],
            (unsigned) ist[4], (unsigned) ist[5], (unsigned) ist[6], (unsigned) ist[7],
            (unsigned) ist[8], (unsigned) ist[9], (unsigned) ist[10], (unsigned) ist[11],
            (unsigned) ist[12], (unsigned) ist[13], (unsigned) ist[14], (unsigned) ist[15],
            (unsigned) ist[16], (unsigned) ist[17], (unsigned) ist[18], (unsigned) ist[19],
            (unsigned) ist[20], (unsigned) ist[21], (unsigned) ist[22], (unsigned) ist[23],
            (unsigned) ist[24], (unsigned) ist[25], (unsigned) ist[26], (unsigned) ist[27],
            (unsigned) ist[28], (unsigned) ist[29], (unsigned) ist[30], (unsigned) ist[31],
            (unsigned) isend[0] | (unsigned) isend[1] << 8, (unsigned) isend[2] | (unsigned) isend[3] << 8,
            (unsigned) isend[4] | (unsigned) isend[5] << 8, (unsigned) isend[6] | (unsigned) isend[7] << 8,
            (unsigned) isend[8] | (unsigned) isend[9] << 8, (unsigned) isend[10] | (unsigned) isend[11] << 8,
            (unsigned) isend[12] | (unsigned) isend[13] << 8, (unsigned) isend[14] | (unsigned) isend[15] << 8,
            (unsigned) isend[16] | (unsigned) isend[17] << 8, (unsigned) isend[18] | (unsigned) isend[19] << 8,
            (unsigned) isend[20] | (unsigned) isend[21] << 8, (unsigned) isend[22] | (unsigned) isend[23] << 8);
}

/* TEMP: Four Swords post-name deadlock kick (experiment, 2026-09-07).
 *
 * Root cause (fully traced): after the name entry, both games sit in the ROM
 * phase machine (0x800C54C) with phase=1, role=0, sendIdx=13, recvIdx=0
 * (master) / 13 (slave). Phase-0 completion (which sets role=8 and lets the
 * master build a busy SIOCNT and re-drive transfers) needs recvIdx==13 plus
 * (SIOCNT&0x88)==0x08 and !(SIOCNT&4); the acceptance scan (0x800C6A8) needs
 * a received 12-halfword block summing to -15 in the +44 recv histories, and
 * got12 (state[5]) set. Neither ever happens: the games' own tables ARE valid
 * (-15) but the receive side never stores anything (histories all zero), so
 * both games deadlock. This kick writes the partner's valid table into each
 * game's recv histories, sets got12 + recvIdx, and resets sendIdx so the
 * games' own handlers re-send their tables -- letting their own state machines
 * advance. Kick ONCE, only when the freeze signature is present.
 */
static bool fs_kick_deadlock(void) {
    size_t sz0 = 0, sz1 = 0;
    uint8_t* m0 = mCoreGetMemoryBlock(g_players[0].core, 0x02030790, &sz0);
    uint8_t* m1 = mCoreGetMemoryBlock(g_players[1].core, 0x02030790, &sz1);
    if (!m0 || !m1 || sz0 < 48 || sz1 < 48) {
        return false;
    }
    uint32_t recv0, recv1;
    memcpy(&recv0, m0 + 24, 4);
    memcpy(&recv1, m1 + 24, 4);
    /* Fire whenever BOTH games are in the post-name phase machine (phase >= 1)
     * -- the phase-1/role-0 deadlock AND the later phase-2 stall where a valid
     * block completes in state+36 but never rotates to state+44 for the
     * acceptance scan. Re-injecting is idempotent (writes the current valid
     * table + got12). Skip once the master has fully received (recvIdx==13)
     * and got12 is set -- that's the game actively validating, not a stall. */
    if (m0[1] < 1 || m1[1] < 1) {
        return false;  /* not yet in the post-name link protocol */
    }
    if (recv0 == 13 && recv1 == 13 && m0[5] == 1 && m1[5] == 1) {
        return false;  /* both blocks complete + got12 -- let it validate */
    }
    fprintf(stderr, "FS KICK: stall detected (P1 phase=%u role=%u recv=%u got12=%u, P2 phase=%u role=%u recv=%u got12=%u), injecting\n",
            (unsigned) m0[1], (unsigned) m0[0], (unsigned) recv0, (unsigned) m0[5],
            (unsigned) m1[1], (unsigned) m1[0], (unsigned) recv1, (unsigned) m1[5]);

    /* The acceptance scan reads 12 halfwords per slot from state+44 at a
     * 28-byte stride and accepts each slot whose sum == -15; the gate
     * (EWRAM 0x02030950) SWAPS state+40 <-> state+44 on every scan call, so
     * fill BOTH (whichever the swap leaves in state+44 carries valid data).
     * Slot layout (matches the EWRAM SIO handler's receive store): slot k
     * receives player k's value each round, so with 2 players slot0 = the
     * shared [counter, checksum, 0x0020, 0x9x0000] block (both games build
     * the identical -15 table) and slots 2-3 = all-FFFF (no device; their
     * sum FFF4 is rejected, which is correct -- only present players count).
     * state+28 is a POINTER to the sendBuf (12 halfwords, self-consistent,
     * sums -15) -- dereference it before copying. */
    uint32_t histPtr[2];
    memcpy(&histPtr[0], m0 + 44, 4);
    memcpy(&histPtr[1], m1 + 44, 4);
    uint32_t altPtr[2];
    memcpy(&altPtr[0], m0 + 40, 4);
    memcpy(&altPtr[1], m1 + 40, 4);
    size_t hsz = 0;
    /* Crafted valid block (12 halfwords, sum == -15): the games' link-status
     * machine (state 5, 0x8036b58) verifies each accepted slot's payload at
     * 0x03000FF8+20*slot starts with 0x21 (0x8037b5c) -- that byte is the
     * low half of the games' "player present / link active" table marker
     * (0x0021 master, 0x0121 slave). The games currently build IDLE tables
     * (marker 0x0020, no ready bit) so the copied payloads read 0x20 and the
     * verification fails forever. Inject a block with the ACTIVE marker
     * 0x0021 at column 2 (which the scan copies to payload byte 0), counter
     * 0x00B0 + checksum 0xFF21 so the block still sums to -15:
     *   0x00B0 + 0xFF21 + 0x0021 + 0xFFFF = 0xFFF1 == -15
     * Slots 2-3 = 0xFFFF x12 (rejected by the scan: no 3rd/4th device). */
    uint16_t blk[12] = {0x00B0, 0xFF21, 0x0021, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int p = 0; p < 2; ++p) {
        for (int which = 0; which < 2; ++which) {
            uint32_t hp = (which == 0) ? altPtr[p] : histPtr[p];
            uint8_t* h = mCoreGetMemoryBlock(g_players[p].core, hp, &hsz);
            if (!h || hsz < 28 * 4) {
                continue;
            }
            /* slot0 = slot1 = the crafted active block; slots 2-3 = FFFF */
            memcpy(h + 28 * 0, blk, 24);
            memcpy(h + 28 * 1, blk, 24);
            for (int k = 2; k < 4; ++k) {
                for (int j = 0; j < 12; ++j) {
                    uint16_t ff = 0xFFFF;
                    memcpy(h + 28 * k + 2 * j, &ff, 2);
                }
            }
        }
        /* got12 = 1, and (master) recvIdx = 13; sendIdx = 0 for both so their
         * handlers re-send the tables on the next transfers. */
        uint8_t* st = (p == 0) ? m0 : m1;
        st[5] = 1;
        uint32_t thirteen = 13;
        uint32_t zero = 0;
        if (p == 0) {
            memcpy(st + 24, &thirteen, 4);
        }
        memcpy(st + 20, &zero, 4);
        fprintf(stderr, "FS KICK: P%d alt=%08X hist=%08X got12=1 sendIdx=0 recvIdx=%s\n",
                p + 1, (unsigned) altPtr[p], (unsigned) histPtr[p],
                p == 0 ? "13" : "unchanged");
    }
    return true;
}

/* TEMP: dump EWRAM bytes for static analysis of FS's SIO handler.
 * mCoreGetMemoryBlock returns a pointer into the emulated EWRAM region. */
static void dump_ewram(int i, const char* path, uint32_t base, uint32_t len) {
    size_t sz = 0;
    uint8_t* m = mCoreGetMemoryBlock(g_players[i].core, base, &sz);
    if (!m) {
        fprintf(stderr, "no EWRAM at %08X\n", base);
        return;
    }
    if (sz > len) {
        sz = len;
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fwrite(m, 1, sz, f);
    fclose(f);
    fprintf(stderr, "dumped %u bytes from %08X -> %s\n", (unsigned) sz, base, path);
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

static void coord_set_fs_armed(void* coordinator, bool armed) {
    if (g_useLockstep) {
        GBASIOLockstepCoordinatorSetFSArmed((struct GBASIOLockstepCoordinator*) coordinator, armed);
    } else {
        GBASIORendezvousCoordinatorSetFSArmed((struct GBASIORendezvousCoordinator*) coordinator, armed);
    }
}

/* Shared coordinator prefix: both coordinator types lay out the same fields
 * up to the FS-assist flags, so read them through the rendezvous type. */
static int coord_fs_assist_on(void* coordinator) {
    return ((struct GBASIORendezvousCoordinator*) coordinator)->fsAssistOn;
}
static int coord_fs_assist_armed(void* coordinator) {
    return ((struct GBASIORendezvousCoordinator*) coordinator)->fsAssistArmed;
}
static int coord_fs_assist_enabled(void* coordinator) {
    return ((struct GBASIORendezvousCoordinator*) coordinator)->fsAssistEnabled;
}
static int coord_transfer_mode(void* coordinator) {
    return (int) ((struct GBASIORendezvousCoordinator*) coordinator)->transferMode;
}

static int run_fs(const char* rom, void* coordinator) {
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
    /* Mirror of the desktop app's frozen-link-screen detection: only now that
     * the games are committed to the link may the handshake assist engage. */
    coord_set_fs_armed(coordinator, true);
    set_keys(0, KEY_START);
    set_keys(1, KEY_START);
    sleep_ms(200);
    set_keys(0, 0);
    set_keys(1, 0);

    /* Watch: log each player's screen + the link transfer activity. Also probe
     * the link screen with inputs (A, A+B, START) to see if the game is waiting
     * for a confirmation press that never comes. */
    fprintf(stderr, "watching handshake for 60s\n");
    int64_t start = now_ms();
    int nextProbe = 2;
    int probeNo = 0;
    while (now_ms() - start < 60000) {
        sleep_ms(2000);
        int t = (int) ((now_ms() - start) / 1000);
        fprintf(stderr, "[%ds] P1=%s P2=%s | frames P1=%d P2=%d\n", t,
                screen_name(cur_screen(0)), screen_name(cur_screen(1)),
                g_players[0].framesProduced, g_players[1].framesProduced);
        dump_fs_state(0, t, "p1");
        dump_fs_state(1, t, "p2");
        if (t == 10) { dump_ppm("/tmp/fs_watch10_p1.ppm", g_cur[0]); dump_ppm("/tmp/fs_watch10_p2.ppm", g_cur[1]); }
        if (t == 30) { dump_ppm("/tmp/fs_watch30_p1.ppm", g_cur[0]); dump_ppm("/tmp/fs_watch30_p2.ppm", g_cur[1]); }
        if (t >= nextProbe && probeNo < 4) {
            const char* what;
            uint16_t keys;
            switch (probeNo) {
            case 0: what = "A both"; keys = KEY_A; break;
            case 1: what = "A+B both"; keys = KEY_A | KEY_B; break;
            case 2: what = "START both"; keys = KEY_START; break;
            default: what = "A both again"; keys = KEY_A; break;
            }
            fprintf(stderr, "[%ds] PROBE %d: pressing %s on both\n", t, probeNo + 1, what);
            char p1[64], p2[64];
            snprintf(p1, sizeof(p1), "/tmp/fs_probe%d_p1.ppm", probeNo);
            snprintf(p2, sizeof(p2), "/tmp/fs_probe%d_p2.ppm", probeNo);
            dump_ppm(p1, g_cur[0]);
            dump_ppm(p2, g_cur[1]);
            set_keys(0, keys);
            set_keys(1, keys);
            sleep_ms(250);
            set_keys(0, 0);
            set_keys(1, 0);
            ++probeNo;
            nextProbe = t + 3;
        }
    }
    /* High-frequency sampler during START: catch recv==13 and role/phase transitions. */
    {
        int lastRole[2] = {-1, -1};
        int64_t s2 = now_ms();
        while (now_ms() - s2 < 20000) {
            for (int i = 0; i < 2; ++i) {
                size_t sz = 0;
                uint8_t* m = mCoreGetMemoryBlock(g_players[i].core, 0x02030790, &sz);
                if (m && sz >= 32) {
                    int recv, phase, got12, role;
                    memcpy(&recv, m + 24, 4);
                    phase = m[1]; got12 = m[5]; role = m[0];
                    if (recv == 13 || (role != lastRole[i])) {
                        fprintf(stderr, "[SAMP] P%d recv=%d phase=%d got12=%d role=%d\n", i + 1, recv, phase, got12, role);
                    }
                    if (role != lastRole[i]) lastRole[i] = role;
                }
            }
            sleep_ms(1);
        }
    }
    dump_ppm("/tmp/fs_final_p1.ppm", g_cur[0]);
    dump_ppm("/tmp/fs_final_p2.ppm", g_cur[1]);
    fprintf(stderr, "final: P1=%s P2=%s\n",
            screen_name(cur_screen(0)), screen_name(cur_screen(1)));
    return 0;
    dump_ppm("/tmp/fs_final_p1.ppm", g_cur[0]);
    dump_ppm("/tmp/fs_final_p2.ppm", g_cur[1]);
    fprintf(stderr, "final: P1=%s P2=%s\n",
            screen_name(cur_screen(0)), screen_name(cur_screen(1)));
    return 0;
}

/* Full post-link flow: reach FS title, START -> link screen, let the handshake
 * establish, press A+B on both (the "confirm link" step that advances the
 * linking screen to the post-link name entry), then drive the name entry and
 * watch whether the games reach character select / gameplay. */
static int run_fs_postlink(const char* rom, void* coordinator, bool armAssist) {
    /* --fs10 generalizes the flow to N players (default 4): every player is
     * driven through the same boot -> name -> file -> Four Swords -> link
     * steps, reproducing the browser's 4P configuration headlessly on the
     * real lockstep driver. */
    g_playerCount = g_fsPlayers;
    if (g_stateFile) {
        /* --fs11: the games were restored from a DUALSTATE blob saved at the
         * pre-link screen, so skip all menu navigation. */
        fprintf(stderr, "restored %d players from state; skipping menu nav\n", g_fsPlayers);
    } else {
        fprintf(stderr, "driving %d players to the Four Swords title\n", g_fsPlayers);
        for (int i = 0; i < g_fsPlayers; ++i) {
            nav_player_to_fs_title(i);
        }

        fprintf(stderr, "waiting for all players at FS title\n");
        for (int k = 0; k < 60; ++k) {
            bool allTitle = true;
            for (int i = 0; i < g_fsPlayers; ++i) {
                if (cur_screen(i) != S_TITLE) {
                    allTitle = false;
                    break;
                }
            }
            if (allTitle) {
                break;
            }
            sleep_ms(500);
        }
        fprintf(stderr, "FS title reached:");
        for (int i = 0; i < g_fsPlayers; ++i) {
            fprintf(stderr, " P%d=%s", i + 1, screen_name(cur_screen(i)));
        }
        fprintf(stderr, "\n");
    }

    /* TEMP: pre-START mode probe -- is each game on the link screen (mode 9)
     * before START, or did nav land them elsewhere (mode 13 = wrong screen)? */
    for (int k = 0; k < g_fsPlayers; ++k) {
        size_t msz = 0;
        uint8_t* w = mCoreGetMemoryBlock(g_players[k].core, 0x03006D10, &msz);
        fprintf(stderr, "pre-START mode P%d = %u\n", k + 1, (w && msz >= 1) ? w[0] : 255);
        /* TEMP: probe the phase machine + link-status at the title screen, so
         * the driver kick's gate can be told apart from the post-name state. */
        size_t psz = 0;
        uint8_t* p = mCoreGetMemoryBlock(g_players[k].core, 0x02030790, &psz);
        if (p && psz >= 48) {
            uint32_t sendPtr, histPtr, altPtr, recv;
            memcpy(&sendPtr, p + 28, 4);
            memcpy(&histPtr, p + 40, 4);
            memcpy(&altPtr, p + 44, 4);
            memcpy(&recv, p + 24, 4);
            fprintf(stderr, "pre-START phase P%d: phase=%u role=%u a=%u b=%u got12=%u recv=%u "
                            "sendPtr=%08X hist=%08X alt=%08X\n",
                    k + 1, (unsigned) p[1], (unsigned) p[0], (unsigned) p[2], (unsigned) p[3],
                    (unsigned) p[5], (unsigned) recv, (unsigned) sendPtr, (unsigned) histPtr, (unsigned) altPtr);
        }
        size_t lsz = 0;
        uint8_t* l = mCoreGetMemoryBlock(g_players[k].core, 0x03000FC0, &lsz);
        if (l && lsz >= 16) {
            fprintf(stderr, "pre-START lstat P%d: %02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X\n",
                    k + 1, l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7], l[8], l[9],
                    l[10], l[11], l[12], l[13], l[14], l[15]);
        }
        size_t ssz = 0;
        uint8_t* s = mCoreGetMemoryBlock(g_players[k].core, 0x03000BFC, &ssz);
        if (s && ssz >= 1) {
            fprintf(stderr, "pre-START ssub P%d = %u\n", k + 1, (unsigned) s[0]);
        }
    }
    if (g_useLockstep) {
        /* TEMP diagnostic (2026-09-09): show the restored coordinator state so
         * a frozen state-load can be told apart from a genuine link stall. */
        for (int k = 0; k < g_fsPlayers; ++k) {
            struct ARMCore* acpu = (struct ARMCore*) g_players[k].core->cpu;
            uint16_t ime = 0, ie = 0;
            int cpuBlocked = -1;
            if (g_players[k].driver.rv.d.p && g_players[k].driver.rv.d.p->p) {
                struct GBA* gba = g_players[k].driver.rv.d.p->p;
                uint16_t* io = gba->memory.io;
                ime = io[GBA_REG(IME) >> 1];
                ie = io[GBA_REG(IE) >> 1];
                cpuBlocked = (int) gba->cpuBlocked;
            }
            fprintf(stderr, "coord diag P%d: halted=%d ime=%u ie=%04X cpuBlocked=%d\n", k + 1,
                    acpu ? (int) acpu->halted : -1, (unsigned) ime, (unsigned) ie, cpuBlocked);
        }

        fprintf(stderr, "coord diag: cycle=%d wait=%d transferActive=%d transferMode=%d nAtt=%d\n",
                (int) g_lsCoord.cycle, (int) g_lsCoord.waiting, (int) g_lsCoord.transferActive,
                (int) g_lsCoord.transferMode, (int) g_lsCoord.nAttached);
        for (int k = 0; k < g_fsPlayers; ++k) {
            g_players[k].heartbeat = 0;
        struct GBASIOLockstepPlayer* lp = TableLookup(&g_lsCoord.players, g_players[k].driver.ls.lockstepId);
            if (lp) {
                fprintf(stderr, "coord diag P%d: id=%d off=%d asleep=%d dataRecv=%d mode=%d evtSched=%d\n",
                        k + 1, lp->playerId, (int) lp->cycleOffset, (int) lp->asleep,
                        (int) lp->dataReceived, (int) lp->mode,
                        (int) mTimingIsScheduled(g_players[k].core->timing, &g_players[k].driver.ls.event));
            }
        }
    }

    fprintf(stderr, "pressing START on all players simultaneously (armAssist=%d)\n", armAssist);
    if (armAssist) {
        coord_set_fs_armed(coordinator, true);
    }
    for (int i = 0; i < g_fsPlayers; ++i) set_keys(i, KEY_START);
    sleep_ms(200);
    for (int i = 0; i < g_fsPlayers; ++i) set_keys(i, 0);

    /* Let the link handshake establish (phase 2) before confirming. */
    fprintf(stderr, "waiting 8s for link handshake to establish\n");
    sleep_ms(8000);

    /* The confirm step: A+B on all players moves the linking screen to name
     * entry. */
    fprintf(stderr, "A+B all players to confirm link\n");
    for (int i = 0; i < g_fsPlayers; ++i) {
        char path[80];
        snprintf(path, sizeof(path), "/tmp/fs_ab_before_p%d.ppm", i + 1);
        dump_ppm(path, g_cur[i]);
    }
    for (int i = 0; i < g_fsPlayers; ++i) set_keys(i, KEY_A | KEY_B);
    sleep_ms(250);
    for (int i = 0; i < g_fsPlayers; ++i) set_keys(i, 0);

    /* Wait for the post-link name entry screen. */
    fprintf(stderr, "waiting for post-link name entry\n");
    enum Screen seen[MAX_PLAYERS];
    for (int i = 0; i < g_fsPlayers; ++i) seen[i] = S_UNKNOWN;
    for (int k = 0; k < 30; ++k) {
        bool allName = true;
        for (int i = 0; i < g_fsPlayers; ++i) {
            seen[i] = cur_screen(i);
            if (seen[i] != S_NAME) {
                allName = false;
            }
        }
        if (allName) {
            break;
        }
        sleep_ms(500);
    }
    fprintf(stderr, "post-link screens:");
    for (int i = 0; i < g_fsPlayers; ++i) {
        fprintf(stderr, " P%d=%s", i + 1, screen_name(seen[i]));
    }
    fprintf(stderr, "\n");
    for (int i = 0; i < g_fsPlayers; ++i) {
        char path[80];
        snprintf(path, sizeof(path), "/tmp/fs_postlink_p%d.ppm", i + 1);
        dump_ppm(path, g_cur[i]);
        snprintf(path, sizeof(path), "/tmp/fs_ewram_p%d.bin", i + 1);
        dump_ewram(i, path, 0x02030000, 0x1000);
        snprintf(path, sizeof(path), "/tmp/fs_iwram_irq_p%d.bin", i + 1);
        dump_ewram(i, path, 0x03005900, 0x200);
        snprintf(path, sizeof(path), "/tmp/fs_iwram_vec_p%d.bin", i + 1);
        dump_ewram(i, path, 0x03007F00, 0x100);
        snprintf(path, sizeof(path), "/tmp/fs_iwram_shadow_p%d.bin", i + 1);
        dump_ewram(i, path, 0x03000BC0, 0x80);
    }

    /* Drive the name entry (type AAAA, walk to END, confirm). */
    fprintf(stderr, "driving post-link name entry\n");
    for (int i = 0; i < g_fsPlayers; ++i) {
        fprintf(stderr, "  name drive P%d: reset cursor, type AAAA, END, confirm\n", i + 1);
        reset_cursor_to_a(i);
        for (int k = 0; k < 4; ++k) {
            tap(i, KEY_A, 200, 250);
            sleep_ms(150);
        }
        char pp[64];
        snprintf(pp, sizeof(pp), "/tmp/fs_namedrv%d_p%d.ppm", i, i + 1);
        dump_ppm(pp, g_cur[i]);
        fprintf(stderr, "  after typing P%d: fr=%d screen=%s\n", i + 1,
                g_players[i].framesProduced, screen_name(cur_screen(i)));
        for (int k = 0; k < 5; ++k) tap(i, KEY_DOWN, 200, 250);
        for (int k = 0; k < 3; ++k) tap(i, KEY_RIGHT, 200, 250);
        tap(i, KEY_A, 200, 250);
        sleep_ms(600);
        fprintf(stderr, "  after confirm P%d: fr=%d screen=%s\n", i + 1,
                g_players[i].framesProduced, screen_name(cur_screen(i)));
        snprintf(pp, sizeof(pp), "/tmp/fs_namedrv%d_conf_p%d.ppm", i, i + 1);
        dump_ppm(pp, g_cur[i]);
    }

    /* Watch for 90s: do the games reach character select / gameplay?
     * Also probe inputs on both players during the watch: the post-name
     * screen may need a confirmation press (like the link screen needed
     * A+B) to start the actual game session. */
    fprintf(stderr, "watching post-link for 90s\n");
    int64_t start = now_ms();
    int64_t lastTap = start; /* wall-clock accumulator: tap A every 10s reliably
                                (t % 10 == 0 is unreliable because the loop
                                lands on odd seconds when screen polling is slow) */
    int nextProbe = 4;
    int probeNo = 0;
    while (now_ms() - start < 90000) {
        sleep_ms(2000);
        int t = (int) ((now_ms() - start) / 1000);
        char scr[160] = "";
        char frs[160] = "";
        for (int i = 0; i < g_fsPlayers; ++i) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%sP%d=%s", i ? " " : "", i + 1, screen_name(cur_screen(i)));
            strncat(scr, tmp, sizeof(scr) - strlen(scr) - 1);
            snprintf(tmp, sizeof(tmp), " fr%d=%d hb%d=%d", i + 1, g_players[i].framesProduced,
                     i + 1, g_players[i].heartbeat);
            strncat(frs, tmp, sizeof(frs) - strlen(frs) - 1);
        }
        fprintf(stderr, "[%ds] %s |%s | assistOn=%d armed=%d en=%d tm=%d\n", t, scr, frs,
                coord_fs_assist_on(coordinator), coord_fs_assist_armed(coordinator),
                coord_fs_assist_enabled(coordinator), coord_transfer_mode(coordinator));
        for (int i = 0; i < g_fsPlayers; ++i) {
            char tag[8];
            snprintf(tag, sizeof(tag), "p%d", i + 1);
            dump_fs_state(i, t, tag);
        }
        /* The mode-9 link screen waits for A-presses: sub-state 3 needs A to
         * start linking, sub-state 4 needs A again (after >120 frames of
         * active link) to confirm. Tap A on both players every 10s so the
         * games can advance through both waits. The games' own sub-state
         * machine ignores premature presses (sub-state 4's A-check is gated
         * on the link-status bit + counter), so this is safe to spam. */
        if (t >= 20 && now_ms() - lastTap >= 10000) {
            fprintf(stderr, "[%ds] tapping A on all players\n", t);
            for (int i = 0; i < g_fsPlayers; ++i) {
                tap(i, KEY_A, 200, 250);
            }
            lastTap = now_ms();
        }
        /* TEMP FS deadlock kick experiment (2026-09-07): while both games are
         * in the post-name phase machine, re-inject the valid blocks +
         * got12/recvIdx each loop (the game consumes got12 on each
         * acceptance-scan attempt). Idempotent and cheap. Fires from t=2 so
         * it can prevent the driver freeze that sometimes stalls both cores
         * during the mode-9 link screen (fs4x: frozen at 16s, pre-kick).
         * --fs6 disables the harness kick: the app's lockstep driver carries
         * the same logic (lockstep.c _fsAssistKick), which is what we're
         * validating. --fs7 re-enables it on the lockstep driver to isolate
         * driver capability from kick-source capability. */
        if (t >= 2 && t < 90 && (g_harnessKick || !g_useLockstep)) {
            fs_kick_deadlock();
        }
        if (t == 20 || t == 60) {
            for (int i = 0; i < g_fsPlayers; ++i) {
                char path[80];
                snprintf(path, sizeof(path), "/tmp/fs_post%d_p%d.ppm", t, i + 1);
                dump_ppm(path, g_cur[i]);
            }
        }
    }
    fprintf(stderr, "final post-link:");
    for (int i = 0; i < g_fsPlayers; ++i) {
        char path[80];
        snprintf(path, sizeof(path), "/tmp/fs_postfinal_p%d.ppm", i + 1);
        dump_ppm(path, g_cur[i]);
        snprintf(path, sizeof(path), "/tmp/fs_ewram_final_p%d.bin", i + 1);
        dump_ewram(i, path, 0x02030000, 0x1000);
        snprintf(path, sizeof(path), "/tmp/fs_iwram_irq_final_p%d.bin", i + 1);
        dump_ewram(i, path, 0x03005900, 0x200);
        snprintf(path, sizeof(path), "/tmp/fs_iwram_vec_final_p%d.bin", i + 1);
        dump_ewram(i, path, 0x03007F00, 0x100);
        snprintf(path, sizeof(path), "/tmp/fs_iwram_shadow_final_p%d.bin", i + 1);
        dump_ewram(i, path, 0x03000BC0, 0x80);
        fprintf(stderr, " P%d=%s", i + 1, screen_name(cur_screen(i)));
    }
    fprintf(stderr, "\n");

    /* GAMEPLAY PROBE: if the games reached the game session (mode 2), drive
     * all players around for ~20s and verify (a) input moves the sprites and
     * (b) the link survives movement (transfers keep flowing, no freeze). */
    {
        size_t gz = 0;
        uint8_t* gm = mCoreGetMemoryBlock(g_players[0].core, 0x03006D10, &gz);
        if (gm && gz >= 1 && gm[0] == 2) {
            fprintf(stderr, "GAMEPLAY REACHED: driving all players (RIGHT/DOWN/A) for 20s\n");
            int64_t pstart = now_ms();
            int step = 0;
            while (now_ms() - pstart < 20000) {
                sleep_ms(2000);
                int pt = (int) ((now_ms() - pstart) / 1000);
                uint32_t btn = KEY_A;
                switch (step++) {
                case 0: btn = KEY_RIGHT; break;
                case 1: btn = KEY_DOWN; break;
                case 2: btn = KEY_LEFT; break;
                case 3: btn = KEY_A; break;
                case 4: btn = KEY_UP; break;
                default: btn = KEY_A; break;
                }
                for (int i = 0; i < g_fsPlayers; ++i) set_keys(i, btn);
                sleep_ms(step % 5 == 4 ? 300 : 1200);
                for (int i = 0; i < g_fsPlayers; ++i) set_keys(i, 0);
                char scr[160] = "";
                char frs[160] = "";
                for (int i = 0; i < g_fsPlayers; ++i) {
                    char tmp[64];
                    snprintf(tmp, sizeof(tmp), "%sP%d=%s", i ? " " : "", i + 1, screen_name(cur_screen(i)));
                    strncat(scr, tmp, sizeof(scr) - strlen(scr) - 1);
                    snprintf(tmp, sizeof(tmp), " fr%d=%d", i + 1, g_players[i].framesProduced);
                    strncat(frs, tmp, sizeof(frs) - strlen(frs) - 1);
                }
                for (int i = 0; i < g_fsPlayers; ++i) {
                    char path[80];
                    snprintf(path, sizeof(path), "/tmp/fs_gameplay_%02d_p%d.ppm", pt, i + 1);
                    dump_ppm(path, g_cur[i]);
                }
                fprintf(stderr, "[gameplay %ds] %s |%s\n", pt, scr, frs);
                for (int i = 0; i < g_fsPlayers; ++i) {
                    char tag[8];
                    snprintf(tag, sizeof(tag), "p%dg", i + 1);
                    dump_fs_state(i, 900 + pt, tag);
                }
            }
            for (int i = 0; i < g_fsPlayers; ++i) {
                char path[80];
                snprintf(path, sizeof(path), "/tmp/fs_gameplay_final_p%d.ppm", i + 1);
                dump_ppm(path, g_cur[i]);
            }
            fprintf(stderr, "GAMEPLAY PROBE DONE:");
            for (int i = 0; i < g_fsPlayers; ++i) {
                fprintf(stderr, " P%d=%s fr=%d", i + 1, screen_name(cur_screen(i)),
                        g_players[i].framesProduced);
            }
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "GAMEPLAY NOT REACHED (mode=%u); skipping probe\n", gm ? (unsigned) gm[0] : 0);
        }
    }
    return 0;
}

/* Parse a DUALSTATE save-state-set blob (b"DUALSTATE" | version:u32(1) |
 * count:u32 | (size:u32, bytes)*) into per-player mGBA save-state blobs -- the
 * exact format the browser exports via File -> Export State Set (and the Rust
 * server's /state). Returns the player count, or -1 on error. */
static int parse_dualstate(const char* path, uint8_t* out[4], size_t outSz[4]) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open state set %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 17) {
        fclose(f);
        fprintf(stderr, "state set too small\n");
        return -1;
    }
    uint8_t* buf = malloc((size_t) len);
    if (!buf || fread(buf, 1, (size_t) len, f) != (size_t) len) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    if (memcmp(buf, "DUALSTATE", 9) != 0) {
        fprintf(stderr, "not a DUALSTATE blob (magic mismatch)\n");
        free(buf);
        return -1;
    }
    uint32_t version = buf[9] | (buf[10] << 8) | (buf[11] << 16) | ((uint32_t) buf[12] << 24);
    uint32_t count = buf[13] | (buf[14] << 8) | (buf[15] << 16) | ((uint32_t) buf[16] << 24);
    if (version != 1 || count < 2 || count > 4) {
        fprintf(stderr, "unsupported state set (version=%u count=%u)\n", (unsigned) version, (unsigned) count);
        free(buf);
        return -1;
    }
    size_t off = 17;
    int n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 4 > (size_t) len) {
            fprintf(stderr, "truncated state set\n");
            free(buf);
            return -1;
        }
        uint32_t sz = buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | ((uint32_t) buf[off + 3] << 24);
        off += 4;
        if (off + sz > (size_t) len) {
            fprintf(stderr, "truncated state %u\n", (unsigned) i);
            free(buf);
            return -1;
        }
        out[n] = malloc(sz);
        if (!out[n]) {
            fprintf(stderr, "out of memory for state %u\n", (unsigned) i);
            free(buf);
            return -1;
        }
        memcpy(out[n], buf + off, sz);
        outSz[n] = sz;
        ++n;
        off += sz;
    }
    free(buf);
    return n;
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
    } else if (argc >= 4 && strcmp(argv[1], "--fs2") == 0) {
        /* --fs2 <rom1> <rom2>: like --fs but load different ROM files per player. */
        fs_mode = true;
        rom = argv[2];
        players = 2;
    } else if (argc >= 3 && strcmp(argv[1], "--fs3") == 0) {
        /* --fs3 <rom>: full post-link flow (A+B confirm + name entry + watch). */
        fs_mode = true;
        rom = argv[2];
        players = 2;
    } else if (argc >= 3 && strcmp(argv[1], "--fs4") == 0) {
        /* --fs4 <rom>: same post-link flow but WITHOUT arming the echo assist,
         * to measure whether A+B alone advances the linking screen. */
        fs_mode = true;
        rom = argv[2];
        players = 2;
    } else if (argc >= 3 && strcmp(argv[1], "--fs5") == 0) {
        /* --fs5 <rom>: no-assist post-link flow WITH full SIO DEBUG, to capture
         * the raw transfer values during the post-name second handshake. */
        fs_mode = true;
        rom = argv[2];
        players = 2;
    } else if (argc >= 3 && strcmp(argv[1], "--fs6") == 0) {
        /* --fs6 <rom>: post-link flow on the app's REAL lockstep driver (with
         * the driver-side FS deadlock kick in lockstep.c) + auto A-taps. The
         * harness's own kick is disabled so we validate the driver kick. */
        fs_mode = true;
        g_useLockstep = true;
        rom = argv[2];
        players = 2;
    } else if (argc >= 3 && strcmp(argv[1], "--fs7") == 0) {
        /* --fs7 <rom>: same as --fs6 but WITH the harness kick too. Decisive
         * isolation experiment: fs4 (rendezvous + harness kick) reaches
         * gameplay; fs6/fs12 (lockstep + inline driver kick) does not. If fs7
         * reaches gameplay, the lockstep driver itself is fine and the inline
         * kick's gating/cadence is the bug. If fs7 still fails, the lockstep
         * driver has a deeper behavioral difference from rendezvous. */
        fs_mode = true;
        g_useLockstep = true;
        g_harnessKick = true;
        rom = argv[2];
        players = 2;
    } else if (argc >= 3 && strcmp(argv[1], "--fs9") == 0) {
        /* --fs9 <rom>: fs6 (lockstep driver, no harness kick) but with the
         * driver-side inline kick ALSO disabled -- PURE driver, zero kicks.
         * Decisive for the minimal patch: fs8/fs8b (gated kick, 0 injections)
         * and fs7b (harness kick) reach gameplay, but fs8c needed 2 gated
         * injections to break a genuine 60s 9:2 stall. If fs9 breaks through
         * on its own, the kick is decorative and the AckPlayer sleep removal
         * is the whole fix. If fs9 stays stuck at 9:2, the boundary-gated
         * kick is a needed deadlock-breaker. */
        fs_mode = true;
        g_useLockstep = true;
        g_noInlineKick = true;
        rom = argv[2];
        players = 2;
    } else if (argc >= 3 && strcmp(argv[1], "--fs10") == 0) {
        /* --fs10 <rom> [n]: fs6 flow (real lockstep driver, driver inline kick
         * enabled, no harness kick) generalized to N players (default 4). The
         * browser 4P configuration (Four Swords with 4 cores in one process)
         * has never been exercised headlessly -- every prior run was 2P -- so
         * this reproduces the user's browser stall locally to tune the fix. */
        fs_mode = true;
        g_useLockstep = true;
        rom = argv[2];
        players = argc >= 4 ? atoi(argv[3]) : 4;
        if (players < 2 || players > 4) {
            players = 4;
        }
        g_fsPlayers = players;
    } else if (argc >= 4 && strcmp(argv[1], "--fs11") == 0) {
        /* --fs11 <rom> <dualstate> [n]: reproduce the user's browser freeze
         * exactly -- load a DUALSTATE save-state set (the format the browser
         * exports via File -> Export State Set) into N lockstep cores (default
         * 4) and run the post-link watch from the saved position, skipping all
         * menu navigation. The state blobs are the same mGBA save states the
         * WASM engine produces, so a state captured at the pre-link screen
         * restarts right where the browser session froze. */
        fs_mode = true;
        g_useLockstep = true;
        rom = argv[2];
        g_stateFile = argv[3];
        players = argc >= 5 ? atoi(argv[4]) : 4;
        if (players < 2 || players > 4) {
            players = 4;
        }
        g_fsPlayers = players;
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
        fprintf(stderr, "       %s --fs10 <rom.gba> [players 2-4]\n", argv[0]);
        fprintf(stderr, "       %s --fs11 <rom.gba> <dualstate> [players 2-4]\n", argv[0]);
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
        /* Full transfer trace is gigabytes per run; keep the default WARN for
         * the post-link flow (state dumps + screens carry the signal). */
        if (strcmp(argv[1], "--fs3") != 0 && strcmp(argv[1], "--fs4") != 0 && strcmp(argv[1], "--fs5") != 0 && strcmp(argv[1], "--fs6") != 0 && strcmp(argv[1], "--fs7") != 0 && strcmp(argv[1], "--fs9") != 0 && strcmp(argv[1], "--fs10") != 0 && strcmp(argv[1], "--fs11") != 0 && strcmp(argv[1], "--fs12") != 0) {
            mLogFilterSet(logger.d.filter, "gba.sio", mLOG_DEBUG);
        }
        if (strcmp(argv[1], "--fs11") == 0 || strcmp(argv[1], "--fs12") == 0) {
            /* State-load runs: the games freeze immediately, so the transfer
             * trace stays small and is the fastest way to see WHY. */
            mLogFilterSet(logger.d.filter, "gba.sio", mLOG_DEBUG);
        }
        if (strcmp(argv[1], "--fs5") == 0) {
            mLogFilterSet(logger.d.filter, "gba.sio", mLOG_DEBUG);
        }
    }
    mLogSetDefaultLogger(&logger.d);

    memset(&g_lsCoord, 0, sizeof(g_lsCoord));
    memset(&g_rvCoord, 0, sizeof(g_rvCoord));
    void* coordinator;
    if (g_useLockstep) {
        GBASIOLockstepCoordinatorInit(&g_lsCoord);
        /* --fs7 isolates the driver: only the HARNESS kick may fire (the
         * known-good one from the rendezvous runs), so disable the driver's
         * inline kick. --fs6 keeps it enabled (inline-only). --fs9 disables
         * it too (pure driver). */
        if (g_harnessKick || g_noInlineKick) {
            GBASIOLockstepCoordinatorSetFSKickEnabled(&g_lsCoord, false);
        }
        coordinator = &g_lsCoord;
    } else {
        GBASIORendezvousCoordinatorInit(&g_rvCoord);
        coordinator = &g_rvCoord;
    }

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
        const char* this_rom = rom;
        if (argc >= 4 && strcmp(argv[1], "--fs2") == 0) {
            this_rom = argv[2 + i];
        }
        if (!mCoreLoadFile(p->core, this_rom)) {
            fprintf(stderr, "mCoreLoadFile failed for player %d (%s)\n", i, this_rom);
            return 1;
        }

        if (g_useLockstep) {
            GBASIOLockstepDriverCreate(&p->driver.ls, &p->user);
            GBASIOLockstepCoordinatorAttach(&g_lsCoord, &p->driver.ls);
        } else {
            GBASIORendezvousDriverCreate(&p->driver.rv, &p->user);
            GBASIORendezvousCoordinatorAttach(&g_rvCoord, &p->driver.rv);
        }
        p->user.sleep = user_sleep;
        p->user.wake = user_wake;
        p->user.requestedId = requested_id;
    }

    for (int i = 0; i < players; ++i) {
        g_players[i].core->setPeripheral(g_players[i].core, mPERIPH_GBA_LINK_PORT,
                                         &g_players[i].driver.rv.d);
    }
    for (int i = 0; i < players; ++i) {
        /* Registers the player in the coordinator (driver reset) and boots. */
        g_players[i].core->reset(g_players[i].core);
    }

    if (g_stateFile) {
        int n = parse_dualstate(g_stateFile, g_stateBlobs, g_stateSizes);
        if (n < 0) {
            return 1;
        }
        int loadN = n < players ? n : players;
        fprintf(stderr, "loading %d saved states (blob has %d)\n", loadN, n);
        for (int i = 0; i < loadN; ++i) {
            if (!g_players[i].core->loadState(g_players[i].core, g_stateBlobs[i])) {
                fprintf(stderr, "mCoreLoadState failed for player %d (%zu bytes)\n",
                        i + 1, g_stateSizes[i]);
                return 1;
            }
            fprintf(stderr, "loaded state %d (%zu bytes)\n", i + 1, g_stateSizes[i]);
        }
        /* Mirror EmulationManager::load_state_set: reset each lockstep driver
         * so no player is left sleeping on a stale transfer after the load.
         * The core state restores the SIO registers but not the coordinator's
         * pending transfer/ack events. */
        for (int i = 0; i < loadN; ++i) {
            g_players[i].driver.ls.d.reset(&g_players[i].driver.ls.d);
        }
    }

    for (int i = 0; i < players; ++i) {
        if (pthread_create(&g_players[i].thread, NULL, run_thread, &g_players[i])) {
            fprintf(stderr, "pthread_create failed for player %d\n", i);
            return 1;
        }
    }
    fprintf(stderr, "running %d players\n", players);

    if (fs_mode) {
        if (argc >= 3 && strcmp(argv[1], "--fs3") == 0) {
            run_fs_postlink(rom, coordinator, true);
        } else if (argc >= 3 && (strcmp(argv[1], "--fs4") == 0 || strcmp(argv[1], "--fs6") == 0 || strcmp(argv[1], "--fs7") == 0 || strcmp(argv[1], "--fs9") == 0 || strcmp(argv[1], "--fs10") == 0 || strcmp(argv[1], "--fs11") == 0)) {
            run_fs_postlink(rom, coordinator, false);
        } else if (argc >= 3 && strcmp(argv[1], "--fs5") == 0) {
            run_fs_postlink(rom, coordinator, false);
        } else {
            run_fs(rom, coordinator);
        }
    } else {
        struct timespec tstart;
        clock_gettime(CLOCK_MONOTONIC, &tstart);
        int nextEvent = 0;
        int lastShot = -1;
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
            int shot = (int) (elapsed_ms / 5000);
            if (shot != lastShot) {
                lastShot = shot;
                for (int i = 0; i < players; ++i) {
                    char path[128];
                    snprintf(path, sizeof(path), "/tmp/sshot_p%d_t%d.ppm", i + 1, shot);
                    pthread_mutex_lock(&g_players[i].snapMutex);
                    if (g_players[i].frameReady) {
                        dump_ppm(path, g_players[i].snapshot);
                    }
                    pthread_mutex_unlock(&g_players[i].snapMutex);
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
    if (g_useLockstep) {
        GBASIOLockstepCoordinatorDeinit(&g_lsCoord);
    } else {
        GBASIORendezvousCoordinatorDeinit(&g_rvCoord);
    }
    mStandardLoggerDeinit(&logger);
    fprintf(stderr, "done\n");
    return 0;
}
