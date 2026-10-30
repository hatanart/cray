/* RAY board: memory map, light/sound board, interrupts, watchdog, clock
 * chip, bookkeeping helpers. Port of pyray/machine.py and lightsound.py.
 *
 *   000000-03FFFF  program ROM          0F6000-0F7FFF  HD63484
 *   040000-04FFFF  RAM (043800- battery) 0FB000-0FB013  ACIAs (FB000+4n+2/3)
 *                                        0FB014-0FB01F  PIA1..3
 * Only A0-A19 are decoded, so the map repeats every 1 MB.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ray.h"
#include "m68k.h"

Ray ray;
int ray_hook_armed;

static long long default_wallclock(void) { return (long long)time(NULL); }
long long (*ray_wallclock)(void) = default_wallclock;
void (*ray_slice_hook)(void);
void (*ray_event)(const char *name);

#define VEC_ACRTC 64
#define VEC_MAINS 67
#define VEC_PIA1A 70
#define VEC_ACIA1 71
#define VECTORED_LEVEL 4
#define WD_WARN_CYCLES (CPU_HZ * 4 / 10)
#define WD_RESET_CYCLES (CPU_HZ / 10)
#define SLICE 1000

#define A6_BASE 0x48B00
#define BOOK_PTR (A6_BASE - 31532)
#define BOOK_COMMIT 0xDC70
#define BOOK_HELD 0
#define BOOK_FLOAT 12
#define CLOSURE 126
#define BOOK_FILL 130
#define HOPPER1_LEVEL 162
#define FEATURES 202             /* game features (long), TESTI 14 */
#define FAULT_LIST (A6_BASE - 30831)

/* light & sound board bits (port A | port B << 8) */
#define LS_MODE 0x0200
#define LS_STROBE 0x0400
#define LS_SSEL 0x8000

/* ------------------------------------------------------- light/sound */
static void ls_reset(LightSound *ls)
{
    memset(ls, 0, sizeof *ls);
}

static void ls_pia_written(int reg)
{
    if (reg != 0 && reg != 2)
        return;
    LightSound *ls = &ray.ls;
    Pia *pia = &ray.pia[0];
    uint8_t a = pia_output(&pia->a);
    uint8_t b = pia_output(&pia->b);
    uint16_t data = a | (b << 8);
    uint16_t changes = ls->old ^ data;
    if ((changes & LS_MODE) && (data & LS_MODE))
        ls->latch = a;
    if ((changes & LS_STROBE) && !(data & LS_STROBE)) {
        int idx = a & 7;
        if (idx < 3)
            ls->lights[idx] = ls->latch;
    }
    int bdir = ls->lights[2] & 0x80;
    int bc1 = data & LS_MODE;
    int bc2 = data & LS_SSEL;
    int psg_read = 0;
    if (!bdir && bc1 && !bc2)
        ls->ay_addr = a & 15;
    else if (!bdir && bc1 && bc2)
        psg_read = 1;
    else if (bdir && !bc1 && !bc2)
        ls->ay_addr = a & 15;
    else if (bdir && bc2 && !bc1) {
        ls->ay_regs[ls->ay_addr] = a;
        ay_write(&ray.ay, ls->ay_addr, a, (uint32_t)(ray.cycles - ray.frame_start));
    } else if (bdir && bc2 && bc1)
        ls->ay_addr = a & 15;
    /* reads of port A see the bus */
    pia->a.pins = psg_read ? ls->ay_regs[ls->ay_addr] : a;
    ls->old = data;
}

uint32_t ray_lamps(void)
{
    return ray.ls.lights[0] | (ray.ls.lights[1] << 8) | ((uint32_t)ray.ls.lights[2] << 16);
}

/* ------------------------------------------------------------ reset */
static void reset_peripherals(void)
{
    acrtc_reset(&ray.acrtc);
    for (int i = 0; i < 3; i++)
        pia_reset(&ray.pia[i]);
    ls_reset(&ray.ls);
    for (int i = 0; i < 5; i++)
        acia_reset(&ray.acia[i]);
}

static void kick_watchdog(void)
{
    ray.wd_last_kick = ray.cycles;
    ray.wd_warned = 0;
}

static void machine_reset(void)
{
    /* the coin unit is a separate board and keeps running */
    reset_peripherals();
    m68k_pulse_reset();
    ray.nmi_line = 0;
    ray.wd_last_kick = ray.cycles;
    ray.wd_warned = 0;
}

/* 68000 RESET instruction: peripheral reset line, also restarts the watchdog */
void ray_reset_instr(void)
{
    reset_peripherals();
    kick_watchdog();
}

static void check_watchdog(void)
{
    uint64_t idle = ray.cycles - ray.wd_last_kick;
    if (!ray.wd_warned && idle > WD_WARN_CYCLES) {
        ray.wd_warned = 1;
        pia_pulse_c2(&ray.pia[2].b);            /* WCHNMI */
    } else if (ray.wd_warned && idle > WD_WARN_CYCLES + WD_RESET_CYCLES) {
        ray.resets++;
        machine_reset();
    }
}

static void set_default_inputs(void)
{
    /* buttons are active low on PIA2 PA0-7, PB0-6 and PIA3 PA2-5 */
    ray.pia[1].a.pins = 0xFF;
    ray.pia[1].b.pins = 0xFF;
    pia_set_input(&ray.pia[1].b, 7, 0);     /* +12V guard: 0 = power OK */
    ray.pia[2].a.pins = 0xFF;
    pia_set_input(&ray.pia[2].a, 0, 0);     /* CBL1? */
    pia_set_input(&ray.pia[2].a, 1, 0);     /* CBL2? */
    ray.pia[2].b.pins = 0xFF;
}

/* ------------------------------------------------------------ I/O */
static uint8_t io_read8(uint32_t addr)
{
    if (addr >= 0xF6000 && addr < 0xF8000)
        return acrtc_read8(&ray.acrtc, addr & 3);
    if (addr >= 0xFB000 && addr < 0xFB020) {
        int off = addr - 0xFB000;
        if (off >= 0x14)
            return pia_read(&ray.pia[(off - 0x14) >> 2], off & 3);
        if (off & 2)
            return acia_read(&ray.acia[off >> 2], off & 1);
    }
    return 0xFF;
}

static void io_write8(uint32_t addr, uint8_t val)
{
    if (addr >= 0xF6000 && addr < 0xF8000) {
        acrtc_write8(&ray.acrtc, addr & 3, val);
        return;
    }
    if (addr >= 0xFB000 && addr < 0xFB020) {
        int off = addr - 0xFB000;
        if (off >= 0x14) {
            int n = (off - 0x14) >> 2;
            pia_write(&ray.pia[n], off & 3, val);
            if (n == 0)
                ls_pia_written(off & 3);
            if (n == 2 && (off & 3) == 2) {
                uint8_t out = pia_output(&ray.pia[2].b);
                if ((out & 0x80) && !(ray.sevenseg & 0x80))
                    kick_watchdog();
                ray.sevenseg = out;
            }
            return;
        }
        if (off & 2)
            acia_write(&ray.acia[off >> 2], off & 1, val);
    }
}

static int is_acrtc(uint32_t a) { return a >= 0xF6000 && a < 0xF8000; }

unsigned int m68k_read_memory_8(unsigned int address)
{
    uint32_t a = address & 0xFFFFF;
    if (a < ROM_SIZE) return ray.rom[a];
    if (a < RAM_BASE + RAM_SIZE) return ray.ram[a - RAM_BASE];
    return io_read8(a);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    uint32_t a = address & 0xFFFFF;
    if (a + 1 < ROM_SIZE) return (ray.rom[a] << 8) | ray.rom[a + 1];
    if (a >= RAM_BASE && a + 1 < RAM_BASE + RAM_SIZE) {
        const uint8_t *p = ray.ram + (a - RAM_BASE);
        return (p[0] << 8) | p[1];
    }
    if (a < RAM_BASE + RAM_SIZE)
        return (m68k_read_memory_8(a) << 8) | m68k_read_memory_8(a + 1);
    if (is_acrtc(a))
        return acrtc_read16(&ray.acrtc, a & 3);
    return (io_read8(a) << 8) | io_read8(a + 1);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    uint32_t a = address & 0xFFFFF;
    if (a >= RAM_BASE && a + 3 < RAM_BASE + RAM_SIZE) {
        const uint8_t *p = ray.ram + (a - RAM_BASE);
        return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    }
    if (a + 3 < ROM_SIZE) {
        const uint8_t *p = ray.rom + a;
        return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    }
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    uint32_t a = address & 0xFFFFF;
    if (a < ROM_SIZE) return;
    if (a < RAM_BASE + RAM_SIZE) { ray.ram[a - RAM_BASE] = (uint8_t)value; return; }
    io_write8(a, value & 0xFF);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    uint32_t a = address & 0xFFFFF;
    if (a >= RAM_BASE && a + 1 < RAM_BASE + RAM_SIZE) {
        uint8_t *p = ray.ram + (a - RAM_BASE);
        p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
        return;
    }
    if (a < RAM_BASE + RAM_SIZE) {
        m68k_write_memory_8(a, (value >> 8) & 0xFF);
        m68k_write_memory_8(a + 1, value & 0xFF);
        return;
    }
    if (is_acrtc(a)) {
        acrtc_write16(&ray.acrtc, a & 3, value & 0xFFFF);
        return;
    }
    io_write8(a, (value >> 8) & 0xFF);
    io_write8(a + 1, value & 0xFF);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    m68k_write_memory_16(address, value >> 16);
    m68k_write_memory_16(address + 2, value & 0xFFFF);
}

unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* ------------------------------------------------------- interrupts */
static int pending_vectored(void)
{
    if (acia_irq(&ray.acia[0])) return VEC_ACIA1;
    if (pia_side_irq(&ray.pia[0].a)) return VEC_PIA1A;
    if (pia_side_irq(&ray.pia[0].b)) return VEC_MAINS;
    if (acrtc_irq(&ray.acrtc)) return VEC_ACRTC;
    return 0;
}

static void take_interrupt(int level, int vector)
{
    unsigned sr = m68k_get_reg(NULL, M68K_REG_SR);
    unsigned pc = m68k_get_reg(NULL, M68K_REG_PC);
    unsigned new_sr = ((sr | 0x2000) & ~0x8000u & ~0x0700u) | (level << 8);
    /* The register-level SR write does not swap stack pointers, so do what
     * the hardware does: save USP, switch A7 to the ISP. */
    unsigned usp = m68k_get_reg(NULL, M68K_REG_USP);
    unsigned isp = m68k_get_reg(NULL, M68K_REG_ISP);
    m68k_set_reg(M68K_REG_SR, new_sr);
    if (!(sr & 0x2000)) {
        m68k_set_reg(M68K_REG_USP, usp);
        m68k_set_reg(M68K_REG_SP, isp);
    }
    unsigned sp = (m68k_get_reg(NULL, M68K_REG_SP) - 6) & 0xFFFFFF;
    m68k_write_memory_16(sp, sr);
    m68k_write_memory_32(sp + 2, pc);
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_PC, m68k_read_memory_32(vector * 4));
}

static void check_interrupts(void)
{
    int mask = (m68k_get_reg(NULL, M68K_REG_SR) >> 8) & 7;
    int nmi = pia_side_irq(&ray.pia[2].b);      /* edge triggered level 7 */
    if (nmi && !ray.nmi_line) {
        ray.nmi_line = 1;
        take_interrupt(7, 31);
        return;
    }
    ray.nmi_line = nmi;
    if (mask < VECTORED_LEVEL) {
        int vec = pending_vectored();
        if (vec)
            take_interrupt(VECTORED_LEVEL, vec);
    }
}

uint64_t ray_now(void)
{
    if (ray.in_slice)
        return ray.cycles + m68k_cycles_run();
    return ray.cycles;
}

static uint32_t run_cycles(uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        check_interrupts();
        coin_poll(&ray.coin, ray.cycles);
        for (int i = 0; i < 5; i++)
            acia_tick(&ray.acia[i]);
        ray.in_slice = 1;
        int r = m68k_execute(SLICE);
        ray.in_slice = 0;
        done += r;
        ray.cycles += r;
        check_watchdog();
        if (ray_slice_hook)
            ray_slice_hook();
    }
    return done;
}

/* ------------------------------------------------------------ clock */
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void update_rtc(void)
{
    time_t now = (time_t)ray_wallclock();
    struct tm t;
    localtime_r(&now, &t);
    uint8_t *c = ray.ram + (0x43FF8 - RAM_BASE);
    if (c[0] & 0xC0)            /* clock being read/written by the program */
        return;
    c[1] = bcd(t.tm_sec > 59 ? 59 : t.tm_sec);
    c[2] = bcd(t.tm_min);
    c[3] = bcd(t.tm_hour);
    c[4] = bcd((t.tm_wday + 6) % 7 + 1);   /* Monday = 1 */
    c[5] = bcd(t.tm_mday);
    c[6] = bcd(t.tm_mon + 1);
    c[7] = bcd(t.tm_year % 100);
}

/* ------------------------------------------------------------ tick */
int ray_tick(float *audio)
{
    ray.frame_start = ray.cycles;
    uint32_t ran = run_cycles(CPU_HZ / TICK_HZ);
    int n = 0;
    if (ray.sound && audio) {
        n = ay_render(&ray.ay, ran, audio);
    } else {
        for (int i = 0; i < ray.ay.nev; i++)
            ray.ay.regs[ray.ay.ev[i].reg] = ray.ay.ev[i].val;
        ray.ay.nev = 0;
    }
    ray.frame++;
    if (!ray.hopper_synced && ray_books_valid()) {
        /* the coin unit's hopper count isn't saved; the books are */
        uint32_t coins = ray_book_read(BOOK_FLOAT, 4);
        if (coins < 0x80000000u)
            ray.coin.hopper_coins = (int)coins;
        ray.hopper_synced = 1;
    }
    pia_pulse_c2(&ray.pia[0].b);                /* 100 Hz mains phase */
    coin_tick_100hz(&ray.coin);
    if (ray.frame % 100 == 0)
        update_rtc();
    return n;
}

/* ------------------------------------------------------------ init */
void ray_init(const uint8_t *rom, const uint8_t *nvram, double ay_clock, double acrtc_clock)
{
    memset(&ray, 0, sizeof ray);
    memcpy(ray.rom, rom, ROM_SIZE);
    if (nvram)
        memcpy(ray.ram, nvram, RAM_SIZE);
    ray.acrtc.cpu_per_cycle = acrtc_clock > 0 ? CPU_HZ / acrtc_clock : 0.0;
    acrtc_reset(&ray.acrtc);
    coin_reset(&ray.coin);
    for (int i = 0; i < 5; i++)
        acia_reset(&ray.acia[i]);
    ray.acia[0].has_peer = 1;
    for (int i = 0; i < 3; i++) {
        ray.pia[i].a.pins = ray.pia[i].b.pins = 0xFF;
        pia_reset(&ray.pia[i]);
    }
    ls_reset(&ray.ls);
    ay_init(&ray.ay, ay_clock);
    set_default_inputs();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    machine_reset();
    coin_power_on(&ray.coin);
}

/* ------------------------------------------------------------ inputs */
const char *const button_names[BTN_COUNT] = {
    "HOLD1", "HOLD2", "HOLD3", "HOLD4", "HOLD5",
    "DOUBLE", "SMALL", "BIG", "BET", "COLLECT", "DEAL",
    "SERVICE", "SRVC1", "SRVC2", "K13", "K14", "K15", "K16",
};

/* (pia index, port b?, bit) */
static const uint8_t button_wiring[BTN_COUNT][3] = {
    { 1, 1, 5 }, { 1, 1, 1 }, { 1, 1, 0 }, { 1, 0, 7 }, { 1, 0, 6 },   /* VALITSE 1-5 */
    { 1, 0, 5 },        /* TUPLAUS */
    { 1, 0, 4 },        /* PIENI */
    { 1, 0, 3 },        /* SUURI */
    { 1, 0, 2 },        /* PANOS */
    { 1, 0, 1 },        /* VOITONMAKSU */
    { 1, 0, 0 },        /* JAKO */
    { 1, 1, 2 },        /* HUOLTO */
    { 1, 1, 3 }, { 1, 1, 4 },                                          /* SRVC1/2 */
    { 2, 0, 2 }, { 2, 0, 3 }, { 2, 0, 4 }, { 2, 0, 5 },               /* K13-K16 */
};

void ray_set_button(Button b, int pressed)
{
    if ((unsigned)b >= BTN_COUNT)
        return;
    Pia *p = &ray.pia[button_wiring[b][0]];
    pia_set_input(button_wiring[b][1] ? &p->b : &p->a, button_wiring[b][2], !pressed);
}

void ray_set_door(int open)
{
    pia_set_input(&ray.pia[1].b, 6, !open);
}

/* ------------------------------------------------- ROM routine calls */
static int call_done;
#define SENTINEL 0x000400

void ray_instr_hook(unsigned int pc)
{
    if (pc == SENTINEL) {
        call_done = 1;
        m68k_end_timeslice();
    }
}

/* Run a ROM subroutine to completion with interrupts masked, then put the
 * CPU back as it was. Returns D0, or -1 if it didn't return. */
static long rom_call(uint32_t addr, uint32_t d0, uint32_t d1)
{
    unsigned saved[16];
    for (int i = 0; i < 16; i++)
        saved[i] = m68k_get_reg(NULL, M68K_REG_D0 + i);
    unsigned pc = m68k_get_reg(NULL, M68K_REG_PC);
    unsigned sr = m68k_get_reg(NULL, M68K_REG_SR);
    m68k_set_reg(M68K_REG_SR, sr | 0x0700);
    m68k_set_reg(M68K_REG_D0, d0);
    m68k_set_reg(M68K_REG_D1, d1);
    m68k_set_reg(M68K_REG_A6, A6_BASE);
    unsigned sp = (m68k_get_reg(NULL, M68K_REG_A7) - 0x100 - 4) & 0xFFFFFF;
    m68k_write_memory_32(sp, SENTINEL);
    m68k_set_reg(M68K_REG_A7, sp);
    m68k_set_reg(M68K_REG_PC, addr);
    call_done = 0;
    ray_hook_armed = 1;
    long ran = 0;
    while (!call_done && ran < 2000000)
        ran += m68k_execute(1000);
    ray_hook_armed = 0;
    long result = m68k_get_reg(NULL, M68K_REG_D0);
    for (int i = 0; i < 16; i++)
        m68k_set_reg(M68K_REG_D0 + i, saved[i]);
    m68k_set_reg(M68K_REG_SR, sr);
    m68k_set_reg(M68K_REG_PC, pc);
    if (!call_done) {
        fprintf(stderr, "ROM call %06X did not return\n", addr);
        return -1;
    }
    return result;
}

/* ------------------------------------------------------- bookkeeping */
int ray_books_valid(void)
{
    uint32_t p = m68k_read_memory_32(BOOK_PTR);
    return p >= 0x40000 && p < 0x4FF00;
}

uint32_t ray_book_read(int offset, int size)
{
    uint32_t base = m68k_read_memory_32(BOOK_PTR);
    return size == 4 ? m68k_read_memory_32(base + offset) : m68k_read_memory_16(base + offset);
}

void ray_book_write(int offset, uint32_t value, int size)
{
    uint32_t base = m68k_read_memory_32(BOOK_PTR);
    if (size == 4)
        m68k_write_memory_32(base + offset, value);
    else
        m68k_write_memory_16(base + offset, value & 0xFFFF);
    /* recompute the CRC of the record(s) holding the field, copy to the
     * other two copies of the books */
    uint32_t f0 = base + offset, f1 = base + offset + size - 1;
    rom_call(BOOK_COMMIT, f0, f0);
    rom_call(BOOK_COMMIT, f1, f1);
}

/* Game features (books 202, TESTI 14 "PELIOMINAISUUDET"). The game tests
 * the bits straight from the books, e.g. bit 13 win music (ROM 0x1A7DE)
 * and bit 29 saver off (ROM 0x188D6, 0x1AEFA). Written only while the
 * word is 0 (a new machine), like pyray. Returns 1 if written. */
int ray_set_default_features(uint32_t value)
{
    if (!value || !ray_books_valid() || ray_book_read(FEATURES, 4) != 0)
        return 0;
    ray_book_write(FEATURES, value, 4);
    return 1;
}

void ray_set_hopper_level(int coins)
{
    int lvl = coins < 0xFFFF ? coins : 0xFFFF;
    if (ray_books_valid() && (int)ray_book_read(HOPPER1_LEVEL, 2) < lvl)
        ray_book_write(HOPPER1_LEVEL, lvl, 2);
}

int ray_books_open(void)
{
    return ray_books_valid() && (ray_book_read(CLOSURE, 2) & 0xFF) == 0xFF;
}

/* Make sure the payout float holds at least `coins`, as if an attendant
 * filled the machine, and lift an 84 (hoppers empty) / 89 (payout reserve
 * too small) closure. Returns coins added. */
int ray_ensure_float(int coins)
{
    if (!ray_books_valid())
        return 0;
    int32_t have = (int32_t)ray_book_read(BOOK_FLOAT, 4);
    int add = coins - have > 0 ? coins - have : 0;
    if (ray.coin.hopper_coins < coins)
        ray.coin.hopper_coins = coins;
    if (add) {
        static const int fields[3] = { BOOK_FLOAT, BOOK_HELD, BOOK_FILL };
        for (int i = 0; i < 3; i++)
            ray_book_write(fields[i], ray_book_read(fields[i], 4) + add, 4);
    }
    ray_set_hopper_level(coins);
    int closure = ray_book_read(CLOSURE, 2);
    if ((closure & 0xFF) == 0x84 || (closure & 0xFF) == 0x89)
        ray_book_write(CLOSURE, 0x00FF, 2);
    return add;
}

/* Mains drops: the +12V guard goes high and PIA3 CB1 fires the NMI; the
 * game saves its state and halts. Run some ticks afterwards. */
void ray_power_fail(void)
{
    pia_set_input(&ray.pia[1].b, 7, 1);
    pia_pulse_c1(&ray.pia[2].b);
}

void ray_power_cycle(void)
{
    int snd = ray.sound;
    ray.sound = 0;
    ray_power_fail();
    for (int i = 0; i < 30; i++)
        ray_tick(NULL);
    ray.sound = snd;
    pia_set_input(&ray.pia[1].b, 7, 0);
    machine_reset();
    coin_power_on(&ray.coin);
}

void ray_fault_codes(char *out, int n)
{
    int len = 0;
    out[0] = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t v = m68k_read_memory_8(FAULT_LIST + i);
        if (v && len + 4 < n)
            len += snprintf(out + len, n - len, "%02X ", v);
    }
}
