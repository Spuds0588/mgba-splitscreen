/* Headless verification harness for the splitscreen core's boot menu.
 * dlopens the INSTALLED core, feeds a scripted RetroPad for P1, and asserts:
 *   run 1: menu shows without input, no sessions boot, serialize size 0
 *   run 2: RIGHT then A boots 3 linked players (default was 2)
 * Build: gcc -O2 scripts/sp_menu_test.c -o /tmp/sp_menu_test -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform/libretro/libretro.h"

static char logBuf[1 << 16];
static size_t logLen;
static void coreLog(enum retro_log_level level, const char* fmt, ...) {
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	/* Keep only the core's own messages; mGBA's per-frame DEBUG spam would
	 * drown the assertions (and the transcript). */
	if (level >= RETRO_LOG_ERROR || strstr(line, "[splitscreen]")) {
		logLen += snprintf(logBuf + logLen, sizeof(logBuf) - logLen, "%s", line);
	}
}
static struct retro_log_callback logCb = { .log = coreLog };

static unsigned frameCount;
static int frameMax; /* brightest blue pixel this frame (0 = all-black) */

/* Dump one 480x160 RGB565 frame as a 24-bit BMP so a human can eyeball it. */
static void dumpBmp(const void* data, size_t pitch, const char* path) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		return;
	}
	const unsigned w = 480, h = 160, rowBytes = w * 3;
	const unsigned pad = (4 - (rowBytes % 4)) % 4;
	const unsigned dataSize = (rowBytes + pad) * h;
	const unsigned offset = 54;
	const unsigned fileSize = offset + dataSize;
	uint8_t hdr[54] = {
		'B', 'M',
	};
	uint32_t u32s[] = { fileSize, 0, offset, 40, w, h, 1, 24, 0, dataSize, 2835, 2835 };
	memcpy(hdr + 2, u32s, sizeof(u32s));
	fwrite(hdr, 1, sizeof(hdr), f);
	for (unsigned y = h; y-- > 0;) {
		const uint16_t* row = (const uint16_t*) ((const uint8_t*) data + y * pitch);
		for (unsigned x = 0; x < w; ++x) {
			uint16_t px = row[x];
			uint8_t b = (px & 0x1F) << 3;
			uint8_t g = ((px >> 5) & 0x3F) << 2;
			uint8_t r = ((px >> 11) & 0x1F) << 3;
			uint8_t bgr[3] = { b, g, r };
			fwrite(bgr, 1, 3, f);
		}
		for (unsigned p = 0; p < pad; ++p) {
			fputc(0, f);
		}
	}
	fclose(f);
	printf("frame dumped: %s\n", path);
}

static void videoCB(const void* data, unsigned w, unsigned h, size_t pitch) {
	if (!data || !((w == 480 && h == 160) || (w == 480 && h == 320) || (w == 480 && h == 480))) {
		printf("BAD FRAME %ux%u\n", w, h);
		exit(1);
	}
	frameMax = 0;
	for (unsigned y = 0; y < h; ++y) {
		const uint16_t* row = (const uint16_t*) ((const uint8_t*) data + y * pitch);
		for (unsigned x = 0; x < w; ++x) {
			uint16_t px = row[x] & 0x1F; /* blue channel for brightness */
			if (px > frameMax) {
				frameMax = px;
			}
		}
	}
	++frameCount;
	if (frameCount == 20) {
		dumpBmp(data, pitch, "/tmp/sp_menu.bmp");
	}
}

static size_t audioCB(const int16_t* data, size_t frames) {
	(void) data;
	return frames;
}
static void pollCB(void) {}
static bool environCB(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		*(struct retro_log_callback*) data = logCb;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool*) data = true;
		return true;
	default:
		return false;
	}
}

/* Scripted P1 pad: one bitset per FRAME. The core polls key ids 0..9 each
 * frame; id 0 marks the frame boundary. Bit positions are libretro joypad
 * ids, which match the core's keymap order. */
static const uint32_t* script;
static size_t scriptLen;
static size_t frameCursor;
static int16_t inputStateCB(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0 || id >= 10) {
		return 0;
	}
	if (id == 0) {
		++frameCursor;
	}
	size_t f = frameCursor - 1;
	if (f >= scriptLen) {
		return 0;
	}
	/* RetroPad ids are SNES-style (B=0, Y=1, Select=2, Start=3, Up=4,
	 * Down=5, Left=6, Right=7, A=8, X=9, L=10, R=11). The core maps them to
	 * GBA keymask bits (A->0, B->1, Right->4, Left->5, R->8, L->9), so the
	 * script is written in GBA bit order and translated per id here. */
	static const int maskBit[12] = { 1, -1, 2, 3, 6, 7, 5, 4, 0, -1, 9, 8 };
	int bit = maskBit[id];
	return bit >= 0 ? ((script[f] >> bit) & 1) : 0;
}

static bool fileRead(const char* path, void** out, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	*out = malloc(*size);
	if (fread(*out, 1, *size, f) != *size) {
		fclose(f);
		return false;
	}
	fclose(f);
	return true;
}

static void* (*dlSetEnv)(retro_environment_t);
static void* (*dlSetVideo)(retro_video_refresh_t);
static void* (*dlSetAudioBatch)(retro_audio_sample_batch_t);
static void* (*dlSetPoll)(retro_input_poll_t);
static void* (*dlSetInput)(retro_input_state_t);
static void* (*dlInit)(void);
static void* (*dlLoadGame)(const struct retro_game_info*);
static void* (*dlRun)(void);
static size_t (*dlSerializeSize)(void);

static void* rom;
static size_t romSize;

/* One full session: load + `frames` runs with the scripted pad. */
static void scenario(const uint32_t* keys, size_t nKeys, int frames,
                     int expectPlayers /* 0 = must NOT boot */) {
	logLen = 0;
	frameCount = 0;
	frameCursor = 0;
	script = keys;
	scriptLen = nKeys;
	dlSetEnv(environCB);
	dlInit();
	struct retro_game_info gi = { .path = "fs.gba", .data = rom, .size = romSize };
	if (!dlLoadGame(&gi)) {
		printf("LOAD FAIL\n");
		exit(1);
	}
	dlSetVideo(videoCB);
	dlSetAudioBatch(audioCB);
	dlSetPoll(pollCB);
	dlSetInput(inputStateCB);
	for (int i = 0; i < frames; ++i) {
		dlRun();
	}
	if (expectPlayers > 0) {
		char want[64];
		snprintf(want, sizeof(want), "link attached: %d players", expectPlayers);
		if (!strstr(logBuf, want)) {
			printf("FAIL: expected '%s'; log:\n%.1500s\n", want, logBuf);
			exit(1);
		}
		if (dlSerializeSize() == 0) {
			printf("FAIL: serialize size still 0 after boot\n");
			exit(1);
		}
		if (frameMax <= 0) {
			printf("FAIL: no bright pixels after boot\n");
			exit(1);
		}
		printf("PASS: menu confirm boots %d players (serialize %zu, %u frames, maxBlue %d)\n",
		       expectPlayers, dlSerializeSize(), frameCount, frameMax);
	} else {
		if (strstr(logBuf, "link attached")) {
			printf("FAIL: sessions booted with no input; log:\n%.1500s\n", logBuf);
			exit(1);
		}
		if (dlSerializeSize() != 0) {
			printf("FAIL: serialize size %zu before boot\n", dlSerializeSize());
			exit(1);
		}
		if (frameCount < (unsigned) frames || frameMax <= 0) {
			printf("FAIL: menu frames missing (count %u, maxBlue %d)\n", frameCount, frameMax);
			exit(1);
		}
		printf("PASS: menu holds with no input (serialize 0, %u menu frames, maxBlue %d)\n",
		       frameCount, frameMax);
	}
}

int main(void) {
	const char* corePath = "/home/coreyb/.config/retroarch/cores/mgba_splitscreen_libretro.so";
	if (!fileRead("/home/coreyb/Coding Projects/Applications/mgba-splitscreen/"
	              "Test Roms/Legend of Zelda, The - A Link To The Past Four Swords (U) [!].gba",
	              &rom, &romSize)) {
		printf("ROM READ FAIL\n");
		return 1;
	}
	void* so = dlopen(corePath, RTLD_NOW | RTLD_LOCAL);
	if (!so) {
		printf("DLOPEN FAIL: %s\n", dlerror());
		return 1;
	}
#define SYM(var, name) \
	*(void**) (&var) = dlsym(so, name); \
	if (!var) { \
		printf("MISSING %s\n", name); \
		return 1; \
	}
	SYM(dlSetEnv, "retro_set_environment");
	SYM(dlSetVideo, "retro_set_video_refresh");
	SYM(dlSetAudioBatch, "retro_set_audio_sample_batch");
	SYM(dlSetPoll, "retro_set_input_poll");
	SYM(dlSetInput, "retro_set_input_state");
	SYM(dlInit, "retro_init");
	SYM(dlLoadGame, "retro_load_game");
	SYM(dlRun, "retro_run");
	SYM(dlSerializeSize, "retro_serialize_size");
#undef SYM

	/* Scenario 1: no input for 240 frames. Menu holds, nothing boots. */
	static const uint32_t none[] = { 0 };
	scenario(none, 1, 240, 0);

	/* Scenario 2: 30 idle, RIGHT (2 -> 3), 30 idle, A, then play. */
	static const uint32_t pick3[62] = {
		[30] = 1u << 4, /* RIGHT */
		[60] = 1u << 0, /* A */
	};
	scenario(pick3, 62, 240, 3);
	return 0;
}
