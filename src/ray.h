/* cray: RAY video poker board emulator core (C port of pyray).
 *
 * The core has no OS calls besides time() / localtime() for the clock
 * chip, so the same code can run under Linux or bare metal. There is one
 * machine (Musashi is a single global CPU), kept in the global `ray`.
 */
#ifndef RAY_H
#define RAY_H

#include <stdint.h>

#define CPU_HZ          8000000
#define ROM_SIZE        0x40000
#define RAM_BASE        0x40000
#define RAM_SIZE        0x10000
#define TICK_HZ         100              /* run_tick() = one 100 Hz mains period */
#define SCREEN_W        608
#define SCREEN_H        292
#define AUDIO_RATE      44100

/* ---------------------------------------------------------------- PIA */
typedef struct {
    uint8_t pins;           /* external input levels; not reset */
    uint8_t ddr, out, cr;
    uint8_t irq1, irq2, c1, c2;
} PiaSide;

typedef struct {
    PiaSide a, b;
} Pia;

void pia_reset(Pia *p);
uint8_t pia_read(Pia *p, int reg);
void pia_write(Pia *p, int reg, uint8_t val);   /* no callbacks; caller handles */
uint8_t pia_output(const PiaSide *s);
int pia_side_irq(const PiaSide *s);
void pia_pulse_c1(PiaSide *s);
void pia_pulse_c2(PiaSide *s);
void pia_set_input(PiaSide *s, int bit, int level);

/* --------------------------------------------------------------- ACIA */
#define ACIA_RXQ 256
typedef struct {
    uint8_t ctrl, rdr, rdrf, overrun;
    uint8_t rx[ACIA_RXQ];
    int rx_head, rx_len;
    int has_peer;           /* ACIA1: coin processor */
} Acia;

void acia_reset(Acia *a);
void acia_receive(Acia *a, uint8_t b);
void acia_tick(Acia *a);
int acia_irq(const Acia *a);
uint8_t acia_read(Acia *a, int reg);
void acia_write(Acia *a, int reg, uint8_t val);

/* -------------------------------------------------- coin processor */
#define COIN_OUTQ 64
#define COIN_PENDING 512
typedef struct {
    uint8_t len;
    uint8_t d[3];
} CoinMsg;

typedef struct {
    int delay;
    CoinMsg msg;
} CoinPending;

typedef struct {
    /* link layer */
    uint8_t rxbuf[4];
    int rxlen;
    uint8_t out_bytes[256];
    int out_head, out_len;
    uint64_t next_byte_at;
    CoinMsg outbox[COIN_OUTQ];
    int outbox_len;
    int state;
    int toggle;
    uint8_t current[4];
    int current_len;        /* 0 = none */
    int wait, wait_timeout;
    /* application */
    int controls;
    int hopper_coins;
    int paid, to_cashbox;
    int coins_in;           /* coins put in since power on */
    int counters[3];
    CoinPending pending[COIN_PENDING];
    int npending;
    int configured;
    int announce_timer;
} Coin;

void coin_reset(Coin *c);
void coin_power_on(Coin *c);
void coin_tx(Coin *c, uint8_t byte);           /* byte from the game */
void coin_poll(Coin *c, uint64_t cycles);
void coin_tick_100hz(Coin *c);
void coin_insert(Coin *c, int channel);

/* --------------------------------------------------------- HD63484 */
#define VRAM_WORDS 0x100000        /* room for the 5502 board; the 5501 has half */
#define VRAM_WORDS_5501 0x80000
#define ACRTC_FIFO_MAX 0x20010
#define ACRTC_PENDING 0x10000
#define ACRTC_READFIFO 0x10000

typedef struct {
    uint16_t vram[VRAM_WORDS];
    uint32_t addr_mask;     /* video memory size - 1 (5501: 512K words, 5502: 1M) */
    uint16_t regs[128];
    int ch, reg8cnt;
    uint16_t r8latch;
    uint16_t fifo[ACRTC_FIFO_MAX];
    int fifo_len;
    uint16_t pending[ACRTC_PENDING];
    int pend_head, pend_len;
    uint64_t busy_until;
    uint16_t readfifo[ACRTC_READFIFO];
    int rf_head, rf_len;
    int error;
    int32_t org;
    int org_dpd;
    int32_t rwp;
    int rwp_dn;
    uint16_t cl0, cl1, ccmp, edg, mask;
    int ppy, pzcy, ppx, pzcx, psy, psx, pey, pzy, pex, pzx;
    uint16_t xmin, ymin, xmax, ymax;
    int cpx, cpy;
    uint16_t pattern[16];
    uint16_t raster;
    double cpu_per_cycle;   /* CPU cycles per 2CLK cycle, 0 = instant */
    uint64_t dots;
} Acrtc;

void acrtc_reset(Acrtc *a);
uint8_t acrtc_read8(Acrtc *a, int offset);
uint16_t acrtc_read16(Acrtc *a, int offset);
void acrtc_write8(Acrtc *a, int offset, uint8_t data);
void acrtc_write16(Acrtc *a, int offset, uint16_t data);
int acrtc_irq(Acrtc *a);
static inline uint16_t acrtc_reg(const Acrtc *a, int r) { return a->regs[(r >> 1) & 0x7F]; }

/* ---------------------------------------------------- AY-3-8910 */
#define AY_EVENTS 4096
typedef struct {
    double clock;
    uint8_t regs[16];
    double tone_phase[3], noise_pos, env_pos, carry, dc;
    struct { uint32_t cyc; uint8_t reg, val; } ev[AY_EVENTS];
    int nev;
} Ay;

void ay_init(Ay *ay, double clock);
void ay_reset(Ay *ay);
void ay_write(Ay *ay, int reg, int val, uint32_t cycle_offset);
/* Render one tick of frame_cycles CPU cycles, times gain; returns samples
 * written. out must hold at least AUDIO_RATE / TICK_HZ + 2 samples. */
int ay_render(Ay *ay, uint32_t frame_cycles, float *out, double gain);

/* ------------------------------------------------- light & sound */
typedef struct {
    uint8_t lights[3];
    uint8_t latch;
    uint16_t old;
    int ay_addr;
    uint8_t ay_regs[16];
} LightSound;

/* ------------------------------------------------ video board 5502 */
/* The later video board ("Videomonitorin ohjain II", PIK-5502-0): 1M words
 * of video memory, an MC68681 DUART at 0C0000 (only what the start-up test
 * needs), 32 KB static RAM at 0D0000 and an IMS G176 palette DAC at 0E0000.
 * See pyray/board5502.py. */
typedef struct {
    uint8_t pal[256][3];    /* 6 bits per gun */
    uint8_t mask, waddr, raddr, wstep, rstep;
    uint32_t version;       /* bumped on every palette or mask change */
    uint8_t ivr;            /* DUART interrupt vector register */
    uint8_t sram[0x8000];
} Board5502;

/* ---------------------------------------------------------- machine */
typedef struct {
    uint8_t rom[ROM_SIZE];
    uint8_t ram[RAM_SIZE];
    Acrtc acrtc;
    Pia pia[3];             /* pia[0] = PIA1 ... */
    Acia acia[5];
    Coin coin;
    Ay ay;
    LightSound ls;

    uint64_t cycles;
    uint64_t frame_start;
    uint64_t frame;
    int in_slice;
    int nmi_line;
    uint64_t wd_last_kick;
    int wd_warned;
    uint8_t sevenseg;
    int hopper_synced;
    int resets;
    int sound;              /* render audio in run_tick */
    int board;              /* video board: 5501 or 5502 */
    Board5502 b5502;
} Ray;

extern Ray ray;

/* Power on with this ROM image and battery RAM image (NULL = blank). */
void ray_init(const uint8_t *rom, const uint8_t *nvram, double ay_clock, double acrtc_clock);
/* Video board 5501 (the default) or 5502; call right after ray_init. */
int ray_set_board(int board);
/* The 16 colours the screen shows now (0xRRGGBB): on the 5501 the fixed
 * colours with the DCR dimming (bright: see video_palette_ex), on the 5502
 * the game's G176 palette through its pixel mask. */
void ray_palette(int bright, uint32_t pal[16]);
/* One 100 Hz tick. If ray.sound, writes the tick's samples to audio and
 * returns how many; else returns 0. */
int ray_tick(float *audio);
uint64_t ray_now(void);
/* wall clock for the clock chip (unix seconds); default time() */
extern long long (*ray_wallclock)(void);
/* Cabinet events for the front end (NULL = off). name is "payout" when
 * the hopper drops one coin into the tray, "coin-in" when a coin is put in. */
extern void (*ray_event)(const char *name);
/* debug: called after every CPU slice (NULL = off) */
extern void (*ray_slice_hook)(void);

/* inputs: buttons are active low */
typedef enum {
    BTN_HOLD1, BTN_HOLD2, BTN_HOLD3, BTN_HOLD4, BTN_HOLD5,
    BTN_DOUBLE, BTN_SMALL, BTN_BIG, BTN_BET, BTN_COLLECT, BTN_DEAL,
    BTN_SERVICE, BTN_SRVC1, BTN_SRVC2, BTN_K13, BTN_K14, BTN_K15, BTN_K16,
    BTN_COUNT
} Button;
extern const char *const button_names[BTN_COUNT];
void ray_set_button(Button b, int pressed);
void ray_set_door(int open);
uint32_t ray_lamps(void);           /* 24 bits: latch 0..2; bit 23 is AY BDIR */

/* bookkeeping / power */
int ray_books_valid(void);
uint32_t ray_book_read(int offset, int size);
void ray_book_write(int offset, uint32_t value, int size);
int ray_ensure_float(int coins);
void ray_set_hopper_level(int coins);
#define FEATURE_WIN_MUSIC (1u << 13)        /* jackpot tune for a win of 50 or more */
#define FEATURE_SAVER_OFF (1u << 29)        /* attract screen not dimmed */
int ray_set_default_features(uint32_t value);
void ray_power_fail(void);
void ray_power_cycle(void);
int ray_books_open(void);           /* closure code says open */
void ray_fault_codes(char *out, int n);   /* "01 92 " ... */

/* video: index image (one byte per pixel, 0..15) and its palette (0xRRGGBB) */
void video_render(const Acrtc *a, uint8_t *img /* SCREEN_W * SCREEN_H */);
void video_palette(uint16_t dcr, uint32_t pal[16]);
void video_palette_ex(uint16_t dcr, int bright, uint32_t pal[16]);

/* rom sets */
int rom_load(const char *arg, const char *romdir, uint8_t *rom,
             char *nvram_path, int nvram_len, char *title, int title_len,
             char *err, int errlen);

#endif
