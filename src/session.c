#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "session.h"

static SessionOpts opts;
static char nvram_path[1400];
static char title[256];
static int level_ok;
static int door_open;
static uint64_t last_save_frame;
static uint64_t last_refill_frame;
static uint64_t money_frame;          /* last coin in / out, 0 = none pending */
static int money_seen = -1;

void session_defaults(SessionOpts *o)
{
    memset(o, 0, sizeof *o);
    o->romdir = "roms";
    o->float_coins = 1000;
    o->save_every = 3600;
    o->auto_refill = 1;
    o->features = FEATURE_WIN_MUSIC | FEATURE_SAVER_OFF;
    o->ay_clock = 1000000;
    o->acrtc_clock = 3000000;
    o->sound = 1;
}

const char *session_title(void) { return title; }

/* Battery RAM files. Three generations are kept: name.nvram (newest),
 * name.nvram.1 and name.nvram.2. Each is the 64 KB RAM followed by a
 * trailer "CRAY" + CRC-32 (pyray reads only the first 64 KB). At start the
 * newest file whose CRC is right is used; a file without the trailer
 * (made by pyray or an older cray) is taken as it is. */
#define NV_GENERATIONS 3
#define NV_TRAILER 8

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static void nv_name(char *out, size_t n, int gen)
{
    if (gen)
        snprintf(out, n, "%s.%d", nvram_path, gen);
    else
        snprintf(out, n, "%s", nvram_path);
}

/* 1 = good (ram filled), 0 = missing, -1 = damaged */
static int read_nv_file(const char *path, uint8_t *ram)
{
    static uint8_t buf[RAM_SIZE + NV_TRAILER + 1];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n == RAM_SIZE) {                        /* no trailer: as it is */
        memcpy(ram, buf, RAM_SIZE);
        return 1;
    }
    if (n != RAM_SIZE + NV_TRAILER || memcmp(buf + RAM_SIZE, "CRAY", 4))
        return -1;
    uint32_t want = (uint32_t)buf[RAM_SIZE + 4] << 24 | buf[RAM_SIZE + 5] << 16 |
                    buf[RAM_SIZE + 6] << 8 | buf[RAM_SIZE + 7];
    if (crc32(buf, RAM_SIZE) != want)
        return -1;
    memcpy(ram, buf, RAM_SIZE);
    return 1;
}

/* the newest good generation; returns 1 if one was found */
static int load_nvram(uint8_t *ram)
{
    char path[1500];
    for (int g = 0; g < NV_GENERATIONS; g++) {
        nv_name(path, sizeof path, g);
        int r = read_nv_file(path, ram);
        if (r > 0) {
            if (g)
                fprintf(stderr, "NVRAM: using the older copy %s\n", path);
            return 1;
        }
        if (r < 0)
            fprintf(stderr, "NVRAM: %s is damaged, trying an older copy\n", path);
    }
    return 0;
}

static void sync_dir(void)
{
    char dir[1400];
    snprintf(dir, sizeof dir, "%s", nvram_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

/* Write to a temporary file and flush it to the card, move the older
 * generations down one step, then rename the new file into place. A power
 * cut at any point leaves at least one good generation. */
static int write_nvram(const uint8_t *ram)
{
    static uint8_t buf[RAM_SIZE + NV_TRAILER];
    memcpy(buf, ram, RAM_SIZE);
    uint32_t c = crc32(ram, RAM_SIZE);
    memcpy(buf + RAM_SIZE, "CRAY", 4);
    buf[RAM_SIZE + 4] = c >> 24;
    buf[RAM_SIZE + 5] = c >> 16;
    buf[RAM_SIZE + 6] = c >> 8;
    buf[RAM_SIZE + 7] = c;

    char tmp[1500];
    snprintf(tmp, sizeof tmp, "%s.tmp", nvram_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "cannot write %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    ssize_t n = write(fd, buf, sizeof buf);
    if (n != (ssize_t)sizeof buf || fsync(fd) != 0) {
        fprintf(stderr, "cannot write %s: %s\n", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    char from[1500], to[1500];
    for (int g = NV_GENERATIONS - 1; g > 0; g--) {
        nv_name(from, sizeof from, g - 1);
        nv_name(to, sizeof to, g);
        rename(from, to);                       /* missing on the first saves: fine */
    }
    if (rename(tmp, nvram_path) != 0) {
        fprintf(stderr, "cannot rename %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    sync_dir();                                 /* make the renames durable */
    return 0;
}

/* Periodic saves run in a thread: flushing to an SD card can take tens of
 * milliseconds, which would cost a frame. The RAM is copied first. */
static pthread_t saver;
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t save_cond = PTHREAD_COND_INITIALIZER;
static uint8_t save_buf[RAM_SIZE];
static int save_wanted, saver_running, save_busy;

static void *saver_main(void *arg)
{
    pthread_mutex_lock(&save_lock);
    for (;;) {
        while (!save_wanted)
            pthread_cond_wait(&save_cond, &save_lock);
        save_wanted = 0;
        save_busy = 1;
        static uint8_t copy[RAM_SIZE];
        memcpy(copy, save_buf, RAM_SIZE);
        pthread_mutex_unlock(&save_lock);
        write_nvram(copy);
        pthread_mutex_lock(&save_lock);
        save_busy = 0;
        pthread_cond_broadcast(&save_cond);
    }
    return NULL;
}

static void save_in_background(void)
{
    if (!saver_running) {
        if (pthread_create(&saver, NULL, saver_main, NULL) != 0) {
            write_nvram(ray.ram);
            return;
        }
        saver_running = 1;
    }
    pthread_mutex_lock(&save_lock);
    memcpy(save_buf, ray.ram, RAM_SIZE);
    save_wanted = 1;
    pthread_cond_signal(&save_cond);
    pthread_mutex_unlock(&save_lock);
}

/* save now and wait until it's on the card */
int session_save(void)
{
    if (saver_running) {            /* let a background save finish first */
        pthread_mutex_lock(&save_lock);
        while (save_wanted || save_busy)
            pthread_cond_wait(&save_cond, &save_lock);
        pthread_mutex_unlock(&save_lock);
    }
    last_save_frame = ray.frame;
    return write_nvram(ray.ram);
}

int session_open(const SessionOpts *o, void (*progress)(int))
{
    opts = *o;
    static uint8_t rom[ROM_SIZE];
    char err[2048];
    if (rom_load(o->rom, o->romdir, rom, nvram_path, sizeof nvram_path,
                 title, sizeof title, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return -1;
    }
    if (o->nvram)
        snprintf(nvram_path, sizeof nvram_path, "%s", o->nvram);
    static uint8_t nv[RAM_SIZE];
    int have_nvram = load_nvram(nv);
    fprintf(stderr, "ROM:   %s\nNVRAM: %s%s\n", title, nvram_path, have_nvram ? "" : " (new)");
    ray_init(rom, have_nvram ? nv : NULL, o->ay_clock, o->acrtc_clock);
    ray.sound = 0;
    if (!have_nvram) {
        /* No battery RAM yet: let the game set up its books, put the payout
         * float in (an attendant filling the machine) and restart. */
        for (int i = 0; i < 1700; i++) {
            ray_tick(NULL);
            if (progress && i % 50 == 0)
                progress(i * 100 / 1700);
        }
        if (o->float_coins)
            ray_ensure_float(o->float_coins);
        ray_set_default_features(o->features);
        ray_power_cycle();
        session_save();
    }
    ray.sound = o->sound;
    last_save_frame = ray.frame;
    return 0;
}

int session_tick(float *audio)
{
    int n = ray_tick(audio);
    /* Once the game is up: machines whose hopper level was never set
     * (TESTI 1) get it now, or the game keeps moving the float to the
     * cash box. */
    if (!level_ok && ray.frame % 100 == 0 && ray.frame > 1500 && ray_books_open()) {
        if (opts.float_coins)
            ray_set_hopper_level(opts.float_coins);
        /* battery RAM made before the features were set: set them and
         * restart once, since the game picks its screen dimming at boot */
        if (ray_set_default_features(opts.features)) {
            fprintf(stderr, "game features set to %08X (TESTI 14), machine restarted\n", opts.features);
            ray_power_cycle();
        }
        level_ok = 1;
    }
    /* The game closed itself because the coins ran low: P3 84 (hoppers
     * empty) or P3 89 (payout reserve too small). Do what the attendant
     * would: fill the hopper and restart. At most every 30 s. */
    if (opts.auto_refill && ray.frame % 100 == 0 && ray.frame - last_refill_frame > 3000 &&
        ray_books_valid()) {
        /* seen on two checks a second apart, so a half-written value
         * while the game starts up doesn't count */
        static int closed_seen;
        int code = ray_book_read(126, 2) & 0xFF;
        if (code == 0x84 || code == 0x89) {
            if (++closed_seen >= 2) {
                closed_seen = 0;
                fprintf(stderr, "machine closed (P3 %02X): refilling\n", code);
                session_refill();
            }
        } else {
            closed_seen = 0;
        }
    }
    /* Saving: when money has moved (a coin in or paid out) and things
     * have been quiet for 10 s, at most once a minute; otherwise every
     * save_every seconds; and always at exit. */
    int money = ray.coin.coins_in + ray.coin.paid;
    if (money != money_seen) {
        if (money_seen >= 0)
            money_frame = ray.frame;
        money_seen = money;
    }
    int due = 0;
    if (money_frame && ray.frame - money_frame >= 10 * TICK_HZ &&
        ray.frame - last_save_frame >= 60 * TICK_HZ)
        due = 1;
    if (opts.save_every > 0 && ray.frame - last_save_frame >= (uint64_t)opts.save_every * TICK_HZ)
        due = 1;
    if (due) {
        last_save_frame = ray.frame;
        money_frame = 0;
        save_in_background();
    }
    return n;
}

void session_refill(void)
{
    last_refill_frame = ray.frame;
    int added = ray_ensure_float(opts.float_coins ? opts.float_coins : 1000);
    ray_power_cycle();
    fprintf(stderr, "payout float refilled (+%d), machine restarted\n", added);
}

void session_toggle_door(void)
{
    door_open = !door_open;
    ray_set_door(door_open);
    fprintf(stderr, "door %s\n", door_open ? "open" : "closed");
}

int session_door_open(void) { return door_open; }

void session_coin(void)
{
    coin_insert(&ray.coin, 1);
}

void session_shutdown(void)
{
    ray.sound = 0;
    ray_power_fail();
    for (int i = 0; i < 30; i++)
        ray_tick(NULL);
    session_save();
}
