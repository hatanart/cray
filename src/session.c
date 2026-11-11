#define _GNU_SOURCE                     /* syncfs */
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
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
    o->features = FEATURE_WIN_MUSIC;
    o->ay_clock = 1000000;
    o->acrtc_clock = 3000000;
    o->board = 5501;
    o->sound = 1;
}

const char *session_title(void) { return title; }

/* Battery RAM files. Every save is a new file with the next number,
 * e.g. pokeri-7907-2.0.000042.nvram: the 64 KB RAM followed by a trailer
 * "CRAY" + CRC-32 (pyray reads only the first 64 KB). Nothing is renamed
 * or overwritten, so a power cut can only spoil the file being written;
 * the older ones stay as they were. That matters on FAT (the boot
 * partition of a read-only system), which has no journal: there a rename
 * over another file can leave every copy damaged. After a save, older
 * files are deleted, keeping the three newest good ones. At start the
 * newest good file is used; the files of older versions (name.nvram,
 * .nvram.1, .nvram.2, or a pyray file without a trailer) are read when
 * there is no numbered one, and deleted after the first new save. */
#define NV_KEEP 3
#define NV_TRAILER 8
#define NV_MAX_FILES 256

static char nv_dir[1400], nv_stem[1400];   /* folder, set name without .nvram */
static unsigned nv_seq;                     /* highest number on the card */
static char nv_used[2900];                  /* the file loaded at start */

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

static void nv_split(void)
{
    snprintf(nv_dir, sizeof nv_dir, "%s", nvram_path);
    char *slash = strrchr(nv_dir, '/');
    const char *file;
    if (slash) {
        *slash = 0;
        if (!nv_dir[0]) strcpy(nv_dir, "/");
        file = nvram_path + (slash - nv_dir) + 1;
    } else {
        strcpy(nv_dir, ".");
        file = nvram_path;
    }
    snprintf(nv_stem, sizeof nv_stem, "%s", file);
    size_t n = strlen(nv_stem);
    if (n > 6 && !strcmp(nv_stem + n - 6, ".nvram"))
        nv_stem[n - 6] = 0;
}

static void nv_numbered(char *out, size_t n, unsigned seq)
{
    snprintf(out, n, "%s/%s.%06u.nvram", nv_dir, nv_stem, seq);
}

/* the save numbers in the folder, newest first; returns how many */
static int nv_list(unsigned *seqs)
{
    int count = 0;
    DIR *d = opendir(nv_dir);
    if (!d)
        return 0;
    size_t sl = strlen(nv_stem);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *s = e->d_name;
        if (strncmp(s, nv_stem, sl) || s[sl] != '.')
            continue;
        const char *num = s + sl + 1, *end = num;
        while (*end >= '0' && *end <= '9') end++;
        if (end == num || end - num > 9 || strcmp(end, ".nvram"))
            continue;
        if (count < NV_MAX_FILES)
            seqs[count++] = (unsigned)strtoul(num, NULL, 10);
    }
    closedir(d);
    for (int i = 1; i < count; i++)             /* newest first */
        for (int j = i; j > 0 && seqs[j] > seqs[j - 1]; j--) {
            unsigned t = seqs[j]; seqs[j] = seqs[j - 1]; seqs[j - 1] = t;
        }
    return count;
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

/* the old names: name.nvram, .nvram.1, .nvram.2 */
static void nv_legacy(char *out, size_t n, int gen)
{
    if (gen) snprintf(out, n, "%s.%d", nvram_path, gen);
    else snprintf(out, n, "%s", nvram_path);
}

/* the newest good save; returns 1 if one was found */
static int load_nvram(uint8_t *ram)
{
    static unsigned seqs[NV_MAX_FILES];
    char path[2900];
    nv_split();
    int count = nv_list(seqs);
    nv_seq = count ? seqs[0] : 0;
    for (int i = 0; i < count; i++) {
        nv_numbered(path, sizeof path, seqs[i]);
        int r = read_nv_file(path, ram);
        if (r > 0) {
            snprintf(nv_used, sizeof nv_used, "%s", path);
            return 1;
        }
        if (r < 0)
            fprintf(stderr, "NVRAM: %s is damaged, trying an older one\n", path);
    }
    for (int g = 0; g < 3; g++) {
        nv_legacy(path, sizeof path, g);
        int r = read_nv_file(path, ram);
        if (r > 0) {
            snprintf(nv_used, sizeof nv_used, "%s", path);
            return 1;
        }
        if (r < 0)
            fprintf(stderr, "NVRAM: %s is damaged, trying an older one\n", path);
    }
    return 0;
}

static void sync_dir(void)
{
    int dfd = open(nv_dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

/* Write the next numbered file and flush it to the card, then delete what
 * is no longer needed: older files beyond the three newest good ones,
 * damaged ones and the old names. */
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

    char path[2900];
    unsigned seq = nv_seq + 1;
    nv_numbered(path, sizeof path, seq);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    ssize_t n = write(fd, buf, sizeof buf);
    if (n != (ssize_t)sizeof buf || fsync(fd) != 0) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    sync_dir();                                 /* the new name, on ext4 */
    nv_seq = seq;

    /* the new file is on the card: tidy up */
    static unsigned seqs[NV_MAX_FILES];
    static uint8_t check[RAM_SIZE];
    int count = nv_list(seqs), kept = 0;
    for (int i = 0; i < count; i++) {
        nv_numbered(path, sizeof path, seqs[i]);
        if (seqs[i] == seq || (seqs[i] < seq && kept < NV_KEEP && read_nv_file(path, check) > 0))
            kept++;
        else if (seqs[i] < seq)
            unlink(path);
    }
    for (int g = 0; g < 3; g++) {
        nv_legacy(path, sizeof path, g);
        unlink(path);
    }
    snprintf(path, sizeof path, "%s.tmp", nvram_path);
    unlink(path);
    /* the deletions too, so a later file can't reuse their space while
     * the card still lists them */
    int dfd = open(nv_dir, O_RDONLY);
    if (dfd >= 0) {
        if (syncfs(dfd) != 0)
            fsync(dfd);
        close(dfd);
    }
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
    if (o->nvram) {
        struct stat st;
        size_t n = strlen(o->nvram);
        if ((n && o->nvram[n - 1] == '/') || (stat(o->nvram, &st) == 0 && S_ISDIR(st.st_mode))) {
            /* a folder: the save is named after the set, so changing rom
               never loads another set's books */
            const char *base = strrchr(nvram_path, '/');
            base = base ? base + 1 : nvram_path;
            char name[sizeof nvram_path];
            snprintf(name, sizeof name, "%s", base);
            if (snprintf(nvram_path, sizeof nvram_path, "%s%s%s", o->nvram,
                         n && o->nvram[n - 1] == '/' ? "" : "/", name) >= (int)sizeof nvram_path) {
                fprintf(stderr, "nvram path too long\n");
                return -1;
            }
        } else {
            snprintf(nvram_path, sizeof nvram_path, "%s", o->nvram);
        }
    }
    static uint8_t nv[RAM_SIZE];
    int have_nvram = load_nvram(nv);
    if (have_nvram)
        fprintf(stderr, "ROM:   %s\nNVRAM: %s\n", title, nv_used);
    else
        fprintf(stderr, "ROM:   %s\nNVRAM: %s/%s.*.nvram (new)\n", title, nv_dir, nv_stem);
    ray_init(rom, have_nvram ? nv : NULL, o->ay_clock, o->acrtc_clock);
    if (ray_set_board(o->board)) {
        fprintf(stderr, "board %d: must be 5501 or 5502\n", o->board);
        return -1;
    }
    if (o->board != 5501)
        fprintf(stderr, "video board %d\n", o->board);
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
