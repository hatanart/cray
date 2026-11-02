/* Headless test run: boot a ROM set for N ticks, optionally press buttons
 * at given ticks, and write a screenshot (PPM), the RAM and the lamp log.
 *
 *   raytest [-r romdir] [-n nvram] [-t ticks] [-o out.ppm] [-m ram.bin]
 *           [-p TICK:BUTTON[:HOLD]]... [-c TICK] [-l] [-b 5501|5502]
 * -n loads battery RAM but never writes it back. -c inserts a coin at TICK.
 * -l prints every lamp change. -b picks the video board. RAY_FIXED_TIME=<unix time> pins the clock.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include "ray.h"
#include "m68k.h"

typedef struct { long tick; int button; int hold; } Press;

static long long fixed_time;
#define MAXT 200000
static double tick_ms[MAXT];
static long slice_tick = -1, cur_tick;
static void slice_hook(void)
{
    if (cur_tick == slice_tick)
        printf("S %llu %06X sr=%04X\n", (unsigned long long)ray.cycles,
               m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SR));
}
static void print_event(const char *name) { printf("tick %6ld event %s\n", cur_tick, name); }
static long long fixed_clock(void) { return fixed_time + (long long)(ray.frame / TICK_HZ); }

int main(int argc, char **argv)
{
    const char *romdir = "roms", *nvram = NULL, *out = NULL, *ramout = NULL, *rom = NULL, *idxout = NULL;
    long ticks = 1000;
    Press presses[256];
    int npress = 0;
    long coins[64];
    int ncoins = 0, lamplog = 0, trace = 0, firstrun = 0;
    double acrtc_clock = 3000000;
    int board = 5501;
    int opt;
    while ((opt = getopt(argc, argv, "r:R:n:t:o:m:p:c:la:i:TS:Fb:")) != -1) {
        switch (opt) {
        case 'r': romdir = optarg; break;
        case 'R': rom = optarg; break;
        case 'n': nvram = optarg; break;
        case 't': ticks = atol(optarg); break;
        case 'o': out = optarg; break;
        case 'm': ramout = optarg; break;
        case 'a': acrtc_clock = atof(optarg); break;
        case 'b': board = atoi(optarg); break;
        case 'l': lamplog = 1; break;
        case 'i': idxout = optarg; break;
        case 'T': trace = 1; break;
        case 'F': firstrun = 1; break;
        case 'S': slice_tick = atol(optarg); ray_slice_hook = slice_hook; break;
        case 'c': if (ncoins < 64) coins[ncoins++] = atol(optarg); break;
        case 'p': {
            char name[32];
            long t; int hold = 10;
            if (npress < 256 && sscanf(optarg, "%ld:%31[A-Z0-9]:%d", &t, name, &hold) >= 2) {
                int b = -1;
                for (int i = 0; i < BTN_COUNT; i++)
                    if (!strcmp(name, button_names[i])) b = i;
                if (b < 0) { fprintf(stderr, "unknown button %s\n", name); return 2; }
                presses[npress++] = (Press){ t, b, hold };
            }
            break;
        }
        default:
            fprintf(stderr, "usage: see the comment at the top of tools/raytest.c\n");
            return 2;
        }
    }
    static uint8_t romimg[ROM_SIZE];
    char nvp[1400], title[256], err[2048];
    if (rom_load(rom, romdir, romimg, nvp, sizeof nvp, title, sizeof title, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    static uint8_t nv[RAM_SIZE];
    int have = 0;
    if (nvram) {
        FILE *f = fopen(nvram, "rb");
        if (!f) { perror(nvram); return 1; }
        have = fread(nv, 1, RAM_SIZE, f) > 0;
        fclose(f);
    }
    ray_init(romimg, have ? nv : NULL, 1000000, acrtc_clock);
    if (ray_set_board(board)) {
        fprintf(stderr, "board must be 5501 or 5502\n");
        return 2;
    }
    if (getenv("RAY_FIXED_TIME")) {
        fixed_time = atoll(getenv("RAY_FIXED_TIME"));
        ray_wallclock = fixed_clock;
    }
    if (firstrun) {
        /* what the front end does on the first start */
        for (int i = 0; i < 1700; i++)
            ray_tick(NULL);
        fprintf(stderr, "float: added %d\n", ray_ensure_float(1000));
        ray_set_default_features(FEATURE_WIN_MUSIC | FEATURE_SAVER_OFF);
        ray_power_cycle();
    }
    ray.sound = 1;
    ray_event = print_event;
    static float audio[2048];
    double sumsq = 0;
    long nsamp = 0;
    uint32_t lamps = 0;
    double worst = 0;
    long worst_tick = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long t = 0; t < ticks; t++) {
        cur_tick = t;
        for (int i = 0; i < npress; i++) {
            if (presses[i].tick == t) ray_set_button(presses[i].button, 1);
            if (presses[i].tick + presses[i].hold == t) ray_set_button(presses[i].button, 0);
        }
        for (int i = 0; i < ncoins; i++)
            if (coins[i] == t) coin_insert(&ray.coin, 1);
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        int n = ray_tick(audio);
        clock_gettime(CLOCK_MONOTONIC, &b);
        double dt = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        if (dt > worst) { worst = dt; worst_tick = t; }
        if (t < MAXT) tick_ms[t] = dt;
        for (int i = 0; i < n; i++) sumsq += audio[i] * audio[i];
        nsamp += n;
        if (trace) {
            uint32_t h = 0;
            for (int i = 0; i < RAM_SIZE; i++) h = h * 31 + ray.ram[i];
            printf("%ld %llu %08X\n", t, (unsigned long long)ray.cycles, h);
        }
        uint32_t l = ray_lamps() & 0x7FFFFF;
        if (lamplog && l != lamps)
            printf("tick %6ld lamps %06X\n", t, l);
        lamps = l;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (getenv("RAY_TICK_TIMES")) {
        FILE *tf = fopen(getenv("RAY_TICK_TIMES"), "w");
        for (long i = 0; i < ticks && i < MAXT; i++) fprintf(tf, "%.4f\n", tick_ms[i]);
        fclose(tf);
    }
    fprintf(stderr, "slowest tick %.2f ms (tick %ld) of a 10 ms budget\n", worst, worst_tick);
    char faults[64];
    ray_fault_codes(faults, sizeof faults);
    fprintf(stderr, "%s: %ld ticks (%.1f s emulated) in %.2f s = %.0fx real time; "
            "resets %d, audio rms %.4f, faults: %s\n", title, ticks, ticks / 100.0, secs,
            ticks / 100.0 / secs, ray.resets, nsamp ? sqrt(sumsq / nsamp) : 0, faults);
    if (out) {
        static uint8_t img[SCREEN_W * SCREEN_H];
        uint32_t pal[16];
        video_render(&ray.acrtc, img);
        ray_palette(0, pal);
        FILE *f = fopen(out, "wb");
        fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
            uint32_t c = pal[img[i]];
            fputc(c >> 16, f); fputc((c >> 8) & 255, f); fputc(c & 255, f);
        }
        fclose(f);
    }
    if (idxout) {
        static uint8_t img[SCREEN_W * SCREEN_H];
        video_render(&ray.acrtc, img);
        FILE *f = fopen(idxout, "wb");
        fwrite(img, 1, sizeof img, f);
        fclose(f);
    }
    if (ramout) {
        FILE *f = fopen(ramout, "wb");
        fwrite(ray.ram, 1, RAM_SIZE, f);
        fclose(f);
    }
    return 0;
}
