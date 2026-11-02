/* ELPS / PAINT test: scripted play, log curve and PAINT commands, compare
 * each ELPS with the outline used before 2026-10-30, dump VRAM and screen.
 *   elpstest [-r romdir] [-t ticks] -o prefix
 * Writes prefix.log (every CRCL, ELPS, arc and PAINT; each ELPS lists the
 * pixels the current method removes (-) and adds (+) against the old one),
 * prefix.vram, .pal and .ppm.
 * Build with "make elpstest" (core built with -DACRTC_TEST in its own folder).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ray.h"

typedef struct { int x, y; } Pt;
extern void (*acrtc_test_cmd)(const Acrtc *a, const uint16_t *f, int n);
int acrtc_elps_points(unsigned A, unsigned B, unsigned dx, Pt **out);
int acrtc_elps_old_points(unsigned A, unsigned B, unsigned dx, Pt **out);

static long tick;
static FILE *logf;

static int has(const Pt *p, int n, Pt q)
{
    for (int i = 0; i < n; i++) if (p[i].x == q.x && p[i].y == q.y) return 1;
    return 0;
}

static void cmd(const Acrtc *a, const uint16_t *f, int n)
{
    int op = f[0], top = op & 0xFC00;
    const char *name = NULL;
    switch (top) {
    case 0xA800: name = "CRCL"; break;
    case 0xAC00: name = "ELPS"; break;
    case 0xB000: name = "AARC"; break;
    case 0xB400: name = "RARC"; break;
    case 0xB800: name = "AEARC"; break;
    case 0xBC00: name = "REARC"; break;
    case 0xC800: name = "PAINT"; break;
    }
    if (!name) return;
    fprintf(logf, "tick %6ld %-5s op=%04X cp=(%d,%d) cl0=%04X cl1=%04X edg=%04X ptn=%04X..%04X",
            tick, name, op, a->cpx, a->cpy, a->cl0, a->cl1, a->edg, a->pattern[0], a->pattern[15]);
    for (int i = 1; i < n; i++) fprintf(logf, " %04X", f[i]);
    if (top == 0xAC00) {
        Pt *o, *e;
        int no = acrtc_elps_old_points(f[1], f[2], f[3], &o);
        int ne = acrtc_elps_points(f[1], f[2], f[3], &e);
        fprintf(logf, " | old %d px, now %d px;", no, ne);
        for (int i = 0; i < no; i++) if (!has(e, ne, o[i])) fprintf(logf, " -(%d,%d)", o[i].x, o[i].y);
        for (int i = 0; i < ne; i++) if (!has(o, no, e[i])) fprintf(logf, " +(%d,%d)", e[i].x, e[i].y);
        free(o); free(e);
    }
    fputc('\n', logf);
}

int main(int argc, char **argv)
{
    long ticks = 9000;
    const char *prefix = "out", *romdir = "roms";
    int opt;
    while ((opt = getopt(argc, argv, "r:t:o:")) != -1) {
        if (opt == 't') ticks = atol(optarg);
        else if (opt == 'o') prefix = optarg;
        else if (opt == 'r') romdir = optarg;
    }
    static uint8_t romimg[ROM_SIZE];
    char nvp[1400], title[256], err[2048], path[512];
    if (rom_load(NULL, romdir, romimg, nvp, sizeof nvp, title, sizeof title, err, sizeof err)) {
        fprintf(stderr, "%s\n", err); return 1;
    }
    snprintf(path, sizeof path, "%s.log", prefix);
    logf = fopen(path, "w");
    ray_init(romimg, NULL, 1000000, 3000000);
    /* first start, as the front end does it */
    for (int i = 0; i < 1700; i++) ray_tick(NULL);
    ray_ensure_float(1000);
    ray_set_default_features(FEATURE_WIN_MUSIC | FEATURE_SAVER_OFF);
    ray_power_cycle();
    acrtc_test_cmd = cmd;
    /* scripted play: coins, then hands; after each deal try double + big */
    for (tick = 0; tick < ticks; tick++) {
        if (tick >= 1500 && tick < 1800 && tick % 30 == 0) coin_insert(&ray.coin, 1);
        long t = tick - 2000;
        if (t >= 0) {
            long c = t % 700;
            int b = -1;
            if (c == 0) b = BTN_DEAL;
            else if (c == 150) b = BTN_HOLD1;
            else if (c == 170) b = BTN_HOLD2;
            else if (c == 250) b = BTN_DEAL;
            else if (c == 450) b = BTN_DOUBLE;
            else if (c == 550) b = BTN_BIG;
            else if (c == 650) b = BTN_COLLECT;
            if (b >= 0) ray_set_button(b, 1);
            if (c == 10 || c == 160 || c == 180 || c == 260 || c == 460 || c == 560 || c == 660)
                for (int i = 0; i < BTN_COUNT; i++) ray_set_button(i, 0);
        }
        ray_tick(NULL);
    }
    fclose(logf);
    snprintf(path, sizeof path, "%s.vram", prefix);
    FILE *f = fopen(path, "wb");
    fwrite(ray.acrtc.vram, 2, ray.acrtc.addr_mask + 1, f);
    fclose(f);
    static uint8_t img[SCREEN_W * SCREEN_H];
    uint32_t pal[16];
    video_render(&ray.acrtc, img);
    video_palette(acrtc_reg(&ray.acrtc, 0x06), pal);
    snprintf(path, sizeof path, "%s.pal", prefix);
    f = fopen(path, "wb");
    for (int i = 0; i < 16; i++) { fputc(pal[i] >> 16, f); fputc((pal[i] >> 8) & 255, f); fputc(pal[i] & 255, f); }
    fclose(f);
    fprintf(stderr, "org=%05X mwr1=%d\n", ray.acrtc.org, acrtc_reg(&ray.acrtc, 0xCA) & 0xFFF);
    snprintf(path, sizeof path, "%s.ppm", prefix);
    f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint32_t c = pal[img[i]];
        fputc(c >> 16, f); fputc((c >> 8) & 255, f); fputc(c & 255, f);
    }
    fclose(f);
    fprintf(stderr, "%s: %ld ticks, resets %d\n", prefix, ticks, ray.resets);
    return 0;
}
