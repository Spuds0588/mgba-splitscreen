/*
 * Reference C audio probe: drive the mGBA core with the canonical load order
 * (init -> initConfig -> loadConfig -> preload -> loadROM -> reset -> runFrame)
 * and report the audio ring's peak/nonzero. If THIS is silent while stock mGBA
 * isn't, the problem is in this fork's core build or its config, not the Rust
 * wrapper.
 *
 * Build:
 *   gcc -O2 -I include audioprobe_c.c -L <out>/lib -lmgba -lm -o audioprobe_c
 */
#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/audio.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/arm/arm.h>
#include <mgba-util/audio-buffer.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom.gba>\n", argv[0]);
        return 1;
    }
    fprintf(stderr, "step 0: create\n");
    struct mCore* core = mCoreCreate(mPLATFORM_GBA);
    if (!core) {
        fprintf(stderr, "mCoreCreate failed\n");
        return 1;
    }
    fprintf(stderr, "step 1: init\n");
    if (!core->init(core)) {
        fprintf(stderr, "core init failed\n");
        return 1;
    }
    fprintf(stderr, "step 2: config\n");
    /* mGBA frontends default opts.volume to 0x100 (100%); with no config file
     * opts.volume stays 0 and _GBACoreLoadConfig sets masterVolume=0 -> the
     * core emits pure digital silence. Set it explicitly like the frontends do. */
    core->opts.volume = 0x100;
    mCoreInitConfig(core, NULL);
    mCoreLoadConfig(core);
    core->setAudioBufferSize(core, 2048);
    fprintf(stderr, "step 3: preload\n");
    if (!mCorePreloadFile(core, argv[1])) {
        fprintf(stderr, "preload failed\n");
        return 1;
    }
    fprintf(stderr, "step 4: reset\n");
    core->reset(core);
    fprintf(stderr, "step 5: run\n");

    int f;
    for (f = 0; f < 120; ++f) {
        core->runFrame(core);
    }

    unsigned long total = 0;
    unsigned nonzero = 0;
    unsigned peak = 0;
    for (f = 0; f < 300; ++f) {
        core->runFrame(core);
        struct mAudioBuffer* buf = core->getAudioBuffer(core);
        size_t avail = mAudioBufferAvailable(buf);
        total += avail;
        int16_t tmp[2048];
        while (avail) {
            size_t n = avail > 1024 ? 1024 : avail;
            mAudioBufferRead(buf, tmp, n);
            size_t i;
            for (i = 0; i < n * 2; ++i) {
                unsigned a = tmp[i] < 0 ? (unsigned) -tmp[i] : (unsigned) tmp[i];
                if (a > peak) {
                    peak = a;
                }
                if (tmp[i]) {
                    ++nonzero;
                }
            }
            avail -= n;
        }
    }
    printf("total=%lu nonzero=%u peak=%u\n", total, nonzero, peak);
    printf(peak ? "RESULT: CORE PRODUCES AUDIO\n" : "RESULT: CORE PRODUCES SILENCE\n");

    /* Inspect the live APU state to see WHY it is silent. */
    struct GBA* gba = core->board;
    if (gba) {
        struct GBAAudio* a = &gba->audio;
        /* io[] is indexed by reg_addr >> 1: SOUNDCNT_L=0x60, SOUNDCNT_HI=0x64,
         * SOUNDCNT_X=0x84. */
        unsigned soundcnt_x = gba->memory.io[0x42];
        unsigned soundcnt_l = gba->memory.io[0x30];
        unsigned soundcnt_hi = gba->memory.io[0x32];
        printf("APU: SOUNDCNT_X=%04X SOUNDCNT_L=%04X SOUNDCNT_HI=%04X\n",
               soundcnt_x, soundcnt_l, soundcnt_hi);
        printf("APU: gba.enable=%d psg.enable=%d volume=%d sampleInterval=%d soundbias=%04X masterVol=%d lastSample=%d sampleIndex=%d\n",
               a->enable, a->psg.enable, a->volume, a->sampleInterval,
               a->soundbias, a->masterVolume, a->lastSample, a->sampleIndex);
        printf("APU: ch2.playing=%d envelope.cur=%d envelope.dead=%d duty=%d index=%d freq=%d sample=%d\n",
               a->psg.playingCh2, a->psg.ch2.envelope.currentVolume,
               a->psg.ch2.envelope.dead, a->psg.ch2.envelope.duty,
               a->psg.ch2.index, a->psg.ch2.control.frequency, a->psg.ch2.sample);
        printf("APU: ch2Left=%d ch2Right=%d volumeLeft=%d volumeRight=%d\n",
               a->psg.ch2Left, a->psg.ch2Right, a->psg.volumeLeft, a->psg.volumeRight);
        int si;
        for (si = 0; si < 4; ++si) {
            printf("APU: currentSamples[%d]=(%d,%d)\n", si,
                   a->currentSamples[si].left, a->currentSamples[si].right);
        }
    } else {
        printf("APU: core->board is NULL\n");
    }
    core->deinit(core);
    return 0;
}
