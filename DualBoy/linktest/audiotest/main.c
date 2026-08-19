/*
 * DualBoy GBA audio test ROM.
 *
 * The absolute minimal audio program: enable the APU, start a square wave on
 * channel 2 (pure PSG, no DMA, no FIFO, no link, no display), and spin.
 *
 * Critical ordering: mGBA ignores writes to sound registers while SOUNDCNT_X
 * bit 7 is clear (hardware-accurate), so NR52 is enabled FIRST, then the
 * channel + routing registers. To be immune to any emulator reset/zeroing
 * behavior, EVERY register (including NR52) is re-written every loop.
 *
 * Registers (GBATEK):
 *   SOUNDCNT_L  0x4000080  bits 0-1 PSG master volume (2 = 100%),
 *                          bit 9 ch2 -> left, bit 13 ch2 -> right
 *   SOUND2CNT_L 0x4000068  bits 12-14 duty (2 = 50%), bits 8-11 envelope
 *                          volume (0xF), bit 15 length disable
 *   SOUND2CNT_H 0x400006C  bits 0-10 frequency, bit 15 restart
 *   SOUNDCNT_X  0x4000084  bit 7 master enable; bits 0-3 ch1-4 enable
 *
 * Frequency 0x76B -> ~440 Hz (65536 / (2048 - 0x76B)).
 */
#define REG16(addr) (*(volatile unsigned short*) (addr))

static void write_audio_regs(void) {
    REG16(0x4000084) = 0x80 | 0x0F; /* SOUNDCNT_X (NR52): master on, all ch on */
    /* SOUNDCNT_L (NR50): bits 0-2 right vol, bits 4-6 left vol (7 = max),
     * bit 9 ch2->right, bit 13 ch2->left */
    REG16(0x4000080) = 0x77 | (1 << 9) | (1 << 13);
    REG16(0x4000068) = (2 << 12) | (0xF << 8) | 0x8000; /* SOUND2CNT_L: duty 50%, vol 15, no length */
    REG16(0x400006C) = 0x8000 | 0x76B;                  /* SOUND2CNT_H: freq + restart */
}

int main(void) {
    for (;;) {
        volatile unsigned i;
        for (i = 0; i < 280896; ++i) {
        }
        write_audio_regs();
    }
}
