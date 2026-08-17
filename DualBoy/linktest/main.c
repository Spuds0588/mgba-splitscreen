/*
 * DualBoy GBA link test ROM (v2.0)
 *
 * What this is for: DualBoy runs two or more GBA instances on one thread and syncs
 * them over mGBA's lockstep link cable. The backend log can count run_frame() calls
 * and video broadcasts, but it cannot see what the GAMES are doing inside those
 * frames — so when the game itself crawls (e.g. Four Swords on the link-heavy
 * title-select screen) while the log reads a healthy "emu 58 video 60", nothing
 * in the log explains it. This ROM is that missing instrument.
 *
 * It programs the GBA link port in MULTI mode (the same mode Four Swords'
 * multi-pak uses) and renders live diagnostics. Up to 4 linked units are
 * supported (DualBoy: launch with `--players 2|3|4`); the display adapts:
 *
 *   - a per-device GAME frame counter (advances once per emulated frame; this is
 *     the number that reveals whether a device's game time is running at speed)
 *   - all four link slots S0-S3, so you can see exactly what data each unit put
 *     on the wire
 *   - TX/RX transfer counters (data actually transferred between the GBAs)
 *   - the MASTER measures round-trip time per slave (ping sent -> echo received,
 *     in frames) with best/worst; each SLAVE measures the master's ping cadence
 *   - the current device state machine (ME), the partner's last reported state
 *     (PEER, carried in the link data), and the expected partner state (EXP)
 *   - a stall counter (frames since the last completed transfer) + a live
 *     sparkline of recent round-trip times
 *   - on the master, a live "PEERS" count of slaves that have echoed at least once
 *
 * Expected healthy readouts in the DualBoy wrapper:
 *   - Master RTT ~2 frames per slave (ping leaves in transfer N, echo returns in
 *     N+2) for every connected peer,
 *   - stall stays 0,
 *   - TX/RX climb together,
 *   - and crucially: ALL devices' FRM counters climb at the same rate.
 *
 * If one device's FRM counter runs visibly slower than the others', that device's
 * game time is being starved by the wrapper (frames cut short), which is exactly
 * the kind of thing the backend log cannot show. With 3-4 units attached, this
 * ROM is the fastest way to see whether the wrapper keeps every instance in step.
 *
 * Performance notes (both matter for a faithful instrument):
 *   - SIOCNT baud is set to 3 (fastest MULTI clock). At the default baud 0 a
 *     4-player transfer takes ~126k cycles, and the master's transfer + hard-sync
 *     pushed its loop to 2 frames while the slaves still ran 1 — FRM showed a
 *     false 30/60 desync that looked like a wrapper bug. Fast baud keeps the
 *     transfer tiny so all devices hold ~60 fps and FRM stays in parity.
 *   - Static labels are drawn ONCE at boot and only the live value cells are
 *     cleared+redrawn each frame. The first version cleared all 38,400 pixels
 *     and redrew ~250 glyphs every frame, which spilled vblank, hit mode-3
 *     bitmap VRAM contention, and ran the whole instrument at ~8 fps.
 *
 * No libc, no interrupts: pure register access + polling, mode 3 framebuffer.
 * Build with build.sh (clang targeting arm-none-eabi + arm-none-eabi-ld).
 */

typedef volatile unsigned short vu16;
typedef volatile unsigned int   vu32;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned char  u8;

/* ---- GBA hardware registers ---- */
#define REG_DISPCNT    (*(vu16*)0x04000000)
#define REG_VCOUNT     (*(vu16*)0x04000006)
#define REG_SIOCNT     (*(vu16*)0x04000128)
#define REG_RCNT       (*(vu16*)0x04000134)
#define REG_SIOMULTI0  (*(vu16*)0x04000120)
#define REG_SIOMULTI1  (*(vu16*)0x04000122)
#define REG_SIOMULTI2  (*(vu16*)0x04000124)
#define REG_SIOMULTI3  (*(vu16*)0x04000126)
#define REG_SIOMLT_SEND (*(vu16*)0x0400012A)
#define REG_TM0D       (*(vu16*)0x04000100)
#define REG_TM0CNT     (*(vu16*)0x04000102)
#define REG_KEYINPUT   (*(vu16*)0x04000130)
#define VRAM           ((vu16*)0x06000000)

/* ---- MULTI-mode SIOCNT bits (mGBA layout: busy = bit 7, irq = bit 14,
       mode select = bits 12-13, player id = bits 4-5) ---- */
#define SI_MULTI_MODE  0x2003   /* bits 12-13 = MULTI, bits 0-1 = 256Kbps (fastest baud) */
#define SI_BUSY        0x0080
#define SI_IRQ         0x4000

#define MODE3          0x0403

/* ---- colors (RGB555) ---- */
#define C_BLACK   0x0000
#define C_WHITE   0x7FFF
#define C_RED     0x001F
#define C_GREEN   0x03E0
#define C_BLUE    0x7C00
#define C_YELLOW  0x03FF
#define C_CYAN    0x7FE0
#define C_GRAY    0x5294
#define C_DIM     0x2108
#define C_ORANGE  0x001E

/* ---- link test state machine ---- */
#define ST_IDLE   0
#define ST_SEND   1   /* master: ping written + transfer started */
#define ST_WAIT   2   /* master: transfer in flight */
#define ST_GOT    3   /* master: echo received */
#define ST_RECV   4   /* slave: new ping received */
#define ST_ECHO   5   /* slave: echoed ping back */
#define ST_NOLNK  6   /* no partner responding */

static const char* const ST_NAMES[7] = {
    "IDLE", "SEND", "WAIT", "GOT ", "RECV", "ECHO", "NOLN"
};

/* ---- globals ---- */
static int is_master;
static u8  my_id;                /* 0-3 player id from SIOCNT bits 4-5 */
static u32 g_frame;              /* game frames since boot (one per vblank) */
static u8  ping;                 /* master: current ping counter */
static u8  last_ping;            /* slave: last ping seen from the master */
static u8  last_echo[4];         /* master: last echoed ping per slot */
static u16 send_frame[64];       /* master: frame each ping was sent */
static u32 tx_count, rx_count;
static u32 rtt[4];               /* master: round-trip in frames, per slot */
static u32 rtt_best[4], rtt_worst[4];
static u32 gap, gap_best, gap_worst;   /* slave: frames between ping arrivals */
static u32 last_ping_frame;            /* slave: frame the previous ping arrived */
static u32 stall;                /* frames since last completed transfer */
static u8  my_state, peer_state, exp_state;
static u8  slot_state[4];        /* state byte carried in each link slot */
static u8  connected[4];         /* master: has slot echoed at least once */
static int n_connected;          /* master: how many slaves have echoed */
static u8  spark[40];            /* recent rtt/cadence history for the bar graph */
static u8  spark_pos;

/* ---- tiny helpers ---- */

static void wait_vblank_start(void) {
    while (REG_VCOUNT < 160) { }
}
static void wait_vblank_end(void) {
    while (REG_VCOUNT >= 160) { }
}

static void put_px(int x, int y, u16 color) {
    if (x < 0 || x >= 240 || y < 0 || y >= 160) return;
    VRAM[y * 240 + x] = color;
}

/* NOTE: there is intentionally no per-frame full-screen clear. The old version
   cleared all 38,400 pixels and redrew ~250 glyphs every frame, which spilled
   vblank and hit mode-3 bitmap VRAM contention, running the game at ~8 fps.
   Static labels are drawn once and only the dynamic value cells are cleared and
   redrawn each frame (see draw_dynamic), which fits in vblank and holds 60 fps.
*/

/* ---- 5x7 font: 7 rows of 5 columns, '1' = lit ---- */

#define FONT_ROWS 7
#define FONT_COLS 5

static const char FONT_CHARS[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ :[]-<>._!?*/+=,'()#|%\"";

static const char* const FONT_GLYPHS[] = {
    /* 0 */ "01110" "10001" "10011" "10101" "11001" "10001" "01110",
    /* 1 */ "00100" "01100" "00100" "00100" "00100" "00100" "01110",
    /* 2 */ "01110" "10001" "00001" "00010" "00100" "01000" "11111",
    /* 3 */ "11111" "00010" "00100" "00010" "00001" "10001" "01110",
    /* 4 */ "00010" "00110" "01010" "10010" "11111" "00010" "00010",
    /* 5 */ "11111" "10000" "11110" "00001" "00001" "10001" "01110",
    /* 6 */ "00110" "01000" "10000" "11110" "10001" "10001" "01110",
    /* 7 */ "11111" "00001" "00010" "00100" "01000" "01000" "01000",
    /* 8 */ "01110" "10001" "10001" "01110" "10001" "10001" "01110",
    /* 9 */ "01110" "10001" "10001" "01111" "00001" "00010" "01100",
    /* A */ "01110" "10001" "10001" "11111" "10001" "10001" "10001",
    /* B */ "11110" "10001" "10001" "11110" "10001" "10001" "11110",
    /* C */ "01110" "10001" "10000" "10000" "10000" "10001" "01110",
    /* D */ "11110" "10001" "10001" "10001" "10001" "10001" "11110",
    /* E */ "11111" "10000" "10000" "11110" "10000" "10000" "11111",
    /* F */ "11111" "10000" "10000" "11110" "10000" "10000" "10000",
    /* G */ "01110" "10001" "10000" "10111" "10001" "10001" "01111",
    /* H */ "10001" "10001" "10001" "11111" "10001" "10001" "10001",
    /* I */ "01110" "00100" "00100" "00100" "00100" "00100" "01110",
    /* J */ "00111" "00010" "00010" "00010" "00010" "10010" "01100",
    /* K */ "10001" "10010" "10100" "11000" "10100" "10010" "10001",
    /* L */ "10000" "10000" "10000" "10000" "10000" "10000" "11111",
    /* M */ "10001" "11011" "10101" "10101" "10001" "10001" "10001",
    /* N */ "10001" "11001" "10101" "10011" "10001" "10001" "10001",
    /* O */ "01110" "10001" "10001" "10001" "10001" "10001" "01110",
    /* P */ "11110" "10001" "10001" "11110" "10000" "10000" "10000",
    /* Q */ "01110" "10001" "10001" "10001" "10101" "10010" "01101",
    /* R */ "11110" "10001" "10001" "11110" "10100" "10010" "10001",
    /* S */ "01111" "10000" "10000" "01110" "00001" "00001" "11110",
    /* T */ "11111" "00100" "00100" "00100" "00100" "00100" "00100",
    /* U */ "10001" "10001" "10001" "10001" "10001" "10001" "01110",
    /* V */ "10001" "10001" "10001" "10001" "10001" "01010" "00100",
    /* W */ "10001" "10001" "10001" "10101" "10101" "11011" "10001",
    /* X */ "10001" "10001" "01010" "00100" "01010" "10001" "10001",
    /* Y */ "10001" "10001" "01010" "00100" "00100" "00100" "00100",
    /* Z */ "11111" "00001" "00010" "00100" "01000" "10000" "11111",
    /*   */ "00000" "00000" "00000" "00000" "00000" "00000" "00000",
    /* : */ "00000" "00100" "00100" "00000" "00100" "00100" "00000",
    /* [ */ "00110" "00100" "00100" "00100" "00100" "00100" "00110",
    /* ] */ "01100" "00100" "00100" "00100" "00100" "00100" "01100",
    /* - */ "00000" "00000" "00000" "11111" "00000" "00000" "00000",
    /* < */ "00010" "00100" "01000" "10000" "01000" "00100" "00010",
    /* > */ "01000" "00100" "00010" "00001" "00010" "00100" "01000",
    /* . */ "00000" "00000" "00000" "00000" "00000" "01100" "01100",
    /* _ */ "00000" "00000" "00000" "00000" "00000" "00000" "11111",
    /* ! */ "00100" "00100" "00100" "00100" "00100" "00000" "00100",
    /* ? */ "01110" "10001" "00001" "00010" "00100" "00000" "00100",
    /* * */ "00000" "10101" "01110" "11111" "01110" "10101" "00000",
    /* / */ "00001" "00010" "00010" "00100" "01000" "01000" "10000",
    /* + */ "00000" "00100" "00100" "11111" "00100" "00100" "00000",
    /* = */ "00000" "00000" "11111" "00000" "11111" "00000" "00000",
    /* , */ "00000" "00000" "00000" "00000" "01100" "00100" "01000",
    /* ( */ "00010" "00100" "01000" "01000" "01000" "00100" "00010",
    /* ) */ "01000" "00100" "00010" "00010" "00010" "00100" "01000",
    /* # */ "01010" "01010" "11111" "01010" "11111" "01010" "01010",
    /* | */ "00100" "00100" "00100" "00100" "00100" "00100" "00100",
    /* % */ "11001" "11010" "00010" "00100" "01000" "01011" "10011",
};

static u8 font_bitmap[128][FONT_ROWS];

static void font_init(void) {
    int c, r, i;
    for (c = 0; c < 128; c++) {
        for (r = 0; r < FONT_ROWS; r++) font_bitmap[c][r] = 0;
    }
    for (c = 0; FONT_CHARS[c]; c++) {
        const char* g = FONT_GLYPHS[c];
        for (r = 0; r < FONT_ROWS; r++) {
            u8 row = 0;
            for (i = 0; i < FONT_COLS; i++) {
                if (g[r * FONT_COLS + i] == '1') row |= (1u << (4 - i));
            }
            font_bitmap[(u8)FONT_CHARS[c]][r] = row;
        }
    }
}

static void draw_char(int x, int y, char ch, u16 color) {
    int r, i;
    u8* g = font_bitmap[(u8)ch];
    for (r = 0; r < FONT_ROWS; r++) {
        u8 row = g[r];
        for (i = 0; i < FONT_COLS; i++) {
            if (row & (1u << (4 - i))) put_px(x + i, y + r, color);
        }
    }
}

static void draw_str(int x, int y, const char* s, u16 color) {
    while (*s) {
        draw_char(x, y, *s, color);
        x += 6;
        s++;
    }
}

/* decimal, fixed width, right-aligned */
static void draw_dec(int x, int y, u32 v, int width, u16 color) {
    char buf[12];
    int n = 0, i;
    if (v == 0) buf[n++] = '0';
    while (v) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n < width) buf[n++] = ' ';
    for (i = n - 1; i >= 0; i--) {
        draw_char(x, y, buf[i], color);
        x += 6;
    }
}

/* hex, fixed 4/8 width */
static void draw_hex(int x, int y, u32 v, int width, u16 color) {
    int i, shift = (width - 1) * 4;
    for (i = 0; i < width; i++, shift -= 4) {
        int d = (v >> shift) & 0xF;
        draw_char(x, y, d < 10 ? (char)('0' + d) : (char)('A' + d - 10), color);
        x += 6;
    }
}

static void draw_sparkline(int x, int y, u16 color) {
    int i, h;
    for (i = 0; i < 40; i++) {
        for (h = 0; h <= 6; h++) put_px(x + i, y + h, C_BLACK);
    }
    for (i = 0; i < 40; i++) {
        int v = spark[(spark_pos + i) % 40];
        if (v > 7) v = 7;
        for (h = 0; h < v; h++) put_px(x + i, y + 6 - h, color);
        if (v == 0) put_px(x + i, y + 6, C_DIM);
    }
}

/* ---- link protocol ---- */

/* Master: process the echo slots (S1-S3) left over from the transfer that
   completed at the start of this frame, and measure per-slot round-trip time. */
static void master_tick(u16 s1, u16 s2, u16 s3) {
    u16 slots[3] = { s1, s2, s3 };
    int i;
    if (REG_SIOCNT & SI_BUSY) {
        /* transfer from last frame still in flight: the partners haven't
           delivered yet — this is where wrapper-induced delay shows up */
        my_state = ST_WAIT;
    }
    peer_state = ST_IDLE;
    for (i = 0; i < 3; i++) {
        int slot = i + 1;
        u8 e = (u8)(slots[i] >> 8);
        u8 pstate = (u8)(slots[i] & 0xFF);
        slot_state[slot] = pstate;
        if (e != 0xFF && e != last_echo[slot]) {
            u32 sent = send_frame[e & 63];
            if (sent != 0xFFFF && sent < g_frame) {
                u32 rt = g_frame - sent;
                if (rt < 64) {
                    rtt[slot] = rt;
                    if (rtt_best[slot] == 0 || rt < rtt_best[slot]) rtt_best[slot] = rt;
                    if (rt > rtt_worst[slot]) rtt_worst[slot] = rt;
                    last_echo[slot] = e;
                    if (!connected[slot]) {
                        connected[slot] = 1;
                        n_connected++;
                    }
                    rx_count++;
                    stall = 0;
                    my_state = ST_GOT;
                    spark[spark_pos] = (u8)rt;
                    spark_pos = (spark_pos + 1) % 40;
                }
            }
        }
        /* peer_state: the first slave that has connected */
        if (connected[slot] && peer_state == ST_IDLE) {
            peer_state = slot_state[slot];
        }
    }
}

/* Master: bump the ping, put it in our own slot (S0), and start the transfer.
   MUST be the last link action of the frame: the lockstep ends the primary's
   frame here while it waits for the slaves to deliver their slot data. */
static void master_send(void) {
    ping++;
    if (ping == 0) ping = 1;
    send_frame[ping & 63] = (u16)g_frame;
    /* Outgoing slot data goes to SIOMLT_SEND (0x0400012A): mGBA's lockstep
       reads each player's slot from this register when the transfer starts.
       SIOMULTI0-3 are RECEIVE registers and are only written by the core. */
    REG_SIOMLT_SEND = ((u16)ping << 8) | ST_SEND;
    tx_count++;
    REG_SIOCNT = SI_MULTI_MODE | SI_BUSY;   /* start the transfer */
    my_state = ST_SEND;
}

/* Slave: read the master's slot (S0) and echo the latest ping back in our own
   slot. Every slave does this, so the master can measure RTT to each of us. */
static void slave_tick(u16 d0) {
    u8 p = (u8)(d0 >> 8);
    u8 pstate = (u8)(d0 & 0xFF);
    peer_state = pstate;
    slot_state[0] = pstate;
    if (p != 0xFF && p != last_ping) {
        /* ping arrival cadence: 1 = the master's transfers land every frame */
        u32 g = g_frame - last_ping_frame;
        if (last_ping != 0) {
            gap = g;
            if (gap_best == 0 || g < gap_best) gap_best = g;
            if (g > gap_worst) gap_worst = g;
            spark[spark_pos] = (u8)g;
            spark_pos = (spark_pos + 1) % 40;
        }
        last_ping = p;
        last_ping_frame = g_frame;
        rx_count++;
        stall = 0;
        my_state = ST_ECHO;
        /* echo this ping back (slot data goes to SIOMLT_SEND) */
        REG_SIOMLT_SEND = ((u16)p << 8) | ST_ECHO;
        tx_count++;
    } else {
        /* keep the slot fresh every frame so the master always gets our state */
        REG_SIOMLT_SEND = ((u16)last_ping << 8) | my_state;
    }
}

/* ---- expected partner state ---- */
static void compute_exp(void) {
    if (is_master) {
        switch (my_state) {
        case ST_SEND: exp_state = ST_RECV; break;
        case ST_GOT:  exp_state = ST_ECHO; break;
        case ST_NOLNK: exp_state = ST_NOLNK; break;
        default:      exp_state = ST_IDLE; break;
        }
    } else {
        switch (my_state) {
        case ST_RECV: exp_state = ST_SEND; break;
        case ST_ECHO: exp_state = ST_WAIT; break;
        case ST_NOLNK: exp_state = ST_NOLNK; break;
        default:      exp_state = ST_IDLE; break;
        }
    }
}

/* ---- display ---- */

/* Clear a cell of `nchars` glyphs (nchars*6 px wide, 7 px tall) so a value's
   old glyph pixels never accumulate into a solid block. */
static void clear_cell(int x, int y, int nchars) {
    int cx, cy;
    for (cy = 0; cy < FONT_ROWS; cy++) {
        for (cx = 0; cx < nchars * 6; cx++) {
            put_px(x + cx, y + cy, C_BLACK);
        }
    }
}

static void draw_dec_c(int x, int y, u32 v, int width, u16 color) {
    clear_cell(x, y, width);
    draw_dec(x, y, v, width, color);
}

static void draw_hex_c(int x, int y, u32 v, int width, u16 color) {
    clear_cell(x, y, width);
    draw_hex(x, y, v, width, color);
}

static void draw_str_c(int x, int y, const char* s, u16 color) {
    int n = 0;
    while (s[n]) n++;
    clear_cell(x, y, n);
    draw_str(x, y, s, color);
}

/* Static labels: drawn ONCE at boot. They are never cleared, which is why the
   per-frame work fits inside vblank. */
static void draw_static(void) {
    draw_str(2, 1, "LINK TEST v2.0", C_WHITE);
    draw_str(2, 19, "SIOCNT", C_GRAY);
    draw_str(2, 28, "S0", C_GRAY);
    draw_str(54, 28, "S1", C_GRAY);
    draw_str(106, 28, "S2", C_GRAY);
    draw_str(158, 28, "S3", C_GRAY);
    draw_str(2, 37, "FRM", C_GRAY);
    draw_str(104, 37, "GAME FRAMES", C_DIM);
    draw_str(2, 46, "TX", C_GRAY);
    draw_str(104, 46, "RX", C_GRAY);
    draw_str(2, 64, "ME", C_GRAY);
    draw_str(74, 64, "PEER", C_GRAY);
    draw_str(146, 64, "EXP", C_GRAY);
    draw_str(2, 73, "STALL", C_GRAY);
    draw_str(92, 73, "PING", C_GRAY);
    draw_str(2, 150, "DUALBOY LINKTEST v2.0 - 4P LINK - watch FRM rates", C_DIM);
}

/* Last-drawn values for fields that only change occasionally. Redrawing them
   only on change keeps the per-frame pixel budget to just the always-changing
   counters (FRM/TX/RX/PING), so the whole readout stays live without spilling
   vblank. */
static u16 cache_siocnt = 0xFFFF;
static u16 cache_slot[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
static u16 cache_role = 0xFFFF;
static u8  cache_my_state = 0xFF;
static u8  cache_peer_state = 0xFF;
static u8  cache_exp_state = 0xFF;
static u32 cache_rtt[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
static u8  cache_conn[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static u32 cache_gap = 0xFFFFFFFF;
static u32 cache_gap_best = 0xFFFFFFFF;
static u32 cache_gap_worst = 0xFFFFFFFF;
static u8  cache_n_connected = 0xFF;
static u8  cache_last_ping = 0xFF;
static u32 cache_stall = 0xFFFFFFFF;
static u8  cache_status = 0xFF;
static u8  cache_status_count = 0xFF;

static void draw_dynamic(void) {
    u16 role_color = is_master ? C_RED
                   : (my_id == 1 ? C_BLUE
                   : (my_id == 2 ? C_GREEN : C_ORANGE));
    u16 role_code = (u16)((my_id << 1) | (is_master ? 1 : 0));
    int slot;

    /* role line + role-specific labels: drawn once, when the role settles */
    if (cache_role != role_code) {
        cache_role = role_code;
        if (is_master) {
            draw_str_c(2, 10, "P1 MASTER", role_color);
            draw_str_c(120, 10, "PEERS", C_GRAY);
            draw_str_c(2, 55, "R1", C_GRAY);
            draw_str_c(76, 55, "R2", C_GRAY);
            draw_str_c(150, 55, "R3", C_GRAY);
            draw_str_c(152, 73, "PEERS", C_GRAY);
            draw_str_c(2, 91, "RTT HISTORY (fr)", C_GRAY);
        } else {
            char role[16];
            role[0] = 'P';
            role[1] = (char)('1' + my_id);
            role[2] = ' ';
            role[3] = 'S'; role[4] = 'L'; role[5] = 'A'; role[6] = 'V'; role[7] = 'E';
            role[8] = (char)('0' + my_id);
            role[9] = 0;
            draw_str_c(2, 10, role, role_color);
            draw_str_c(140, 10, "MULTI 4P", C_GRAY);
            draw_str_c(2, 55, "GAP", C_GRAY);
            draw_str_c(78, 55, "BEST", C_GRAY);
            draw_str_c(150, 55, "WRST", C_GRAY);
            draw_str_c(152, 73, "ECHO", C_GRAY);
            draw_str_c(2, 91, "PING CADENCE (fr)", C_GRAY);
        }
        /* force every value cell to redraw alongside the new labels */
        cache_siocnt = 0xFFFF;
        cache_slot[0] = cache_slot[1] = cache_slot[2] = cache_slot[3] = 0xFFFF;
        cache_conn[1] = cache_conn[2] = cache_conn[3] = 0xFF;
        cache_gap = cache_gap_best = cache_gap_worst = 0xFFFFFFFF;
        cache_n_connected = 0xFF;
        cache_last_ping = 0xFF;
        cache_stall = 0xFFFFFFFF;
        cache_status = 0xFF;
    }

    /* raw link registers: redraw only when the wire changes */
    if (cache_siocnt != REG_SIOCNT) {
        cache_siocnt = REG_SIOCNT;
        draw_hex_c(54, 19, REG_SIOCNT, 4, C_WHITE);
    }
    if (cache_slot[0] != REG_SIOMULTI0) { cache_slot[0] = REG_SIOMULTI0; draw_hex_c(24, 28, REG_SIOMULTI0, 4, C_WHITE); }
    if (cache_slot[1] != REG_SIOMULTI1) { cache_slot[1] = REG_SIOMULTI1; draw_hex_c(76, 28, REG_SIOMULTI1, 4, C_WHITE); }
    if (cache_slot[2] != REG_SIOMULTI2) { cache_slot[2] = REG_SIOMULTI2; draw_hex_c(128, 28, REG_SIOMULTI2, 4, C_WHITE); }
    if (cache_slot[3] != REG_SIOMULTI3) { cache_slot[3] = REG_SIOMULTI3; draw_hex_c(180, 28, REG_SIOMULTI3, 4, C_WHITE); }

    /* live counters that change every frame: always redraw */
    draw_dec_c(36, 37, g_frame, 8, C_CYAN);
    draw_dec_c(36, 46, tx_count, 7, C_WHITE);
    draw_dec_c(138, 46, rx_count, 7, C_WHITE);

    /* round trip / echo delay: redraw only when a new sample lands */
    if (is_master) {
        for (slot = 1; slot <= 3; slot++) {
            int x = 2 + (slot - 1) * 74;
            if (cache_conn[slot] != connected[slot]) {
                cache_conn[slot] = connected[slot];
                if (connected[slot]) {
                    u16 c = rtt[slot] <= 2 ? C_GREEN
                          : rtt[slot] <= 4 ? C_YELLOW : C_RED;
                    draw_dec_c(x + 24, 55, rtt[slot], 2, c);
                    draw_char(x + 38, 55, 'f', c);
                } else {
                    draw_str_c(x + 24, 55, "--", C_DIM);
                }
            } else if (connected[slot] && cache_rtt[slot] != rtt[slot]) {
                cache_rtt[slot] = rtt[slot];
                u16 c = rtt[slot] <= 2 ? C_GREEN
                      : rtt[slot] <= 4 ? C_YELLOW : C_RED;
                draw_dec_c(x + 24, 55, rtt[slot], 2, c);
                draw_char(x + 38, 55, 'f', c);
            }
        }
    } else {
        if (cache_gap != gap) {
            cache_gap = gap;
            draw_dec_c(36, 55, gap, 2, C_GREEN);
            draw_char(54, 55, 'f', C_WHITE);
            draw_char(60, 55, 'r', C_WHITE);
        }
        if (cache_gap_best != gap_best) { cache_gap_best = gap_best; draw_dec_c(114, 55, gap_best, 2, C_WHITE); }
        if (cache_gap_worst != gap_worst) { cache_gap_worst = gap_worst; draw_dec_c(186, 55, gap_worst, 2, C_WHITE); }
    }

    /* state machine: redraw only on state transitions */
    if (cache_my_state != my_state) { cache_my_state = my_state; draw_str_c(30, 64, ST_NAMES[my_state], role_color); }
    if (cache_peer_state != peer_state) { cache_peer_state = peer_state; draw_str_c(110, 64, ST_NAMES[peer_state], C_WHITE); }
    if (cache_exp_state != exp_state) { cache_exp_state = exp_state; draw_str_c(176, 64, ST_NAMES[exp_state], C_WHITE); }

    /* link health */
    if (cache_stall != stall) {
        cache_stall = stall;
        if (stall > 30) {
            draw_dec_c(44, 73, stall, 3, C_RED);
        } else if (stall > 0) {
            draw_dec_c(44, 73, stall, 3, C_YELLOW);
        } else {
            draw_dec_c(44, 73, stall, 3, C_GREEN);
        }
    }
    draw_dec_c(128, 73, ping, 3, C_WHITE);
    if (is_master) {
        if (cache_n_connected != n_connected) {
            cache_n_connected = (u8)n_connected;
            draw_dec_c(162, 10, n_connected, 1, C_WHITE);
            draw_dec_c(196, 73, n_connected, 1, C_WHITE);
        }
    } else {
        if (cache_last_ping != last_ping) {
            cache_last_ping = last_ping;
            draw_dec_c(188, 73, last_ping, 3, C_WHITE);
        }
    }

    /* live status line: redraw only when its state changes */
    {
        u8 st = stall > 30 ? 0 : (stall > 0 ? 1 : 2);
        if (cache_status != st || (is_master && cache_status_count != n_connected)) {
            cache_status = st;
            cache_status_count = (u8)n_connected;
            clear_cell(2, 82, 32);
            if (st == 0) {
                draw_str(2, 82, "NO LINK - NO PARTNER RESPONDING", C_RED);
            } else if (st == 1) {
                draw_str(2, 82, "LINK IDLE - WAITING FOR TRANSFER", C_YELLOW);
            } else if (is_master) {
                char s[24];
                int n = 0;
                s[n++] = 'L'; s[n++] = 'I'; s[n++] = 'N'; s[n++] = 'K';
                s[n++] = ' '; s[n++] = 'A'; s[n++] = 'C'; s[n++] = 'T';
                s[n++] = 'I'; s[n++] = 'V'; s[n++] = 'E'; s[n++] = ' ';
                s[n++] = '-'; s[n++] = ' ';
                s[n++] = (char)('0' + n_connected + 1);
                s[n++] = ' '; s[n++] = 'P';
                if (n_connected + 1 > 1) s[n++] = 'S';
                s[n] = 0;
                draw_str(2, 82, s, C_GREEN);
            } else {
                draw_str(2, 82, "LINK ACTIVE - TRANSFERS FLOWING", C_GREEN);
            }
        }
    }

    /* sparkline (cheap 40-px bar): always redraw */
    draw_sparkline(2, 98, C_WHITE);
}

/* ---- main ---- */

int main(void) {
    int i;
    u16 cnt;

    REG_DISPCNT = MODE3;
    font_init();
    for (i = 0; i < 64; i++) send_frame[i] = 0xFFFF;
    for (i = 0; i < 40; i++) spark[i] = 0;
    for (i = 0; i < 4; i++) {
        last_echo[i] = 0;
        rtt[i] = 0;
        rtt_best[i] = 0;
        rtt_worst[i] = 0;
        connected[i] = 0;
        slot_state[i] = ST_IDLE;
    }

    /* Enter MULTI mode. IMPORTANT: mGBA's SIO mode decode combines RCNT bits
       14-15 with SIOCNT bits 12-13 ((rcnt & 0xC000) | (siocnt & 0x3000)) >> 12.
       RCNT resets to 0x8000 (SC bit 15 set), so WITHOUT clearing it every
       SIOCNT value decodes to GPIO (8), never MULTI — the game would sit in
       GPIO mode forever, no id/slave bits stamped, no transfers started. Real
       GBA homebrew clears RCNT before MULTI mode, and so must we. */
    REG_RCNT = 0;
    REG_SIOCNT = SI_MULTI_MODE;
    cnt = REG_SIOCNT;
    is_master = 1;
    my_id = 0;
    my_state = ST_IDLE;
    peer_state = ST_IDLE;
    exp_state = ST_IDLE;
    ping = 0;
    last_ping = 0;
    g_frame = 0;
    tx_count = 0;
    rx_count = 0;
    gap = 0;
    gap_best = 0;
    gap_worst = 0;
    last_ping_frame = 0;
    stall = 0;
    spark_pos = 0;
    n_connected = 0;

    draw_static();

    while (1) {
        u16 d0, d1, d2, d3;

        wait_vblank_start();
        g_frame++;
        stall++;

        /* Re-stamp RCNT (keep bits 14-15 clear so the mode decode stays MULTI)
           and SIOCNT in MULTI mode, then re-derive our role. The core
           recomputes the id (bits 4-5) and slave (bit 2) bits on every write,
           but the lockstep coordinator assigns real ids only after the
           boot-time mode-set negotiation settles — so role must be sampled
           here, every frame, not once at boot. */
        REG_RCNT = 0;
        REG_SIOCNT = SI_MULTI_MODE;
        cnt = REG_SIOCNT;
        my_id = (u8)((cnt >> 4) & 3);
        is_master = (my_id == 0) && !(cnt & 0x0004);

        d0 = REG_SIOMULTI0;
        d1 = REG_SIOMULTI1;
        d2 = REG_SIOMULTI2;
        d3 = REG_SIOMULTI3;

        if (is_master) {
            master_tick(d1, d2, d3);  /* read echoes from the transfer that
                                         completed at the start of this frame */
            if (stall > 30 && my_state != ST_NOLNK) {
                my_state = ST_NOLNK;
            }
            compute_exp();
            draw_dynamic();
            master_send();            /* MUST be last: the lockstep ends the
                                         primary's frame here */
        } else {
            slave_tick(d0);
            if (stall > 30 && my_state != ST_NOLNK) {
                my_state = ST_NOLNK;
            }
            compute_exp();
            draw_dynamic();
        }

        wait_vblank_end();
    }
}
