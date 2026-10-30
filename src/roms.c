/* ROM sets, same format as pyray (roms/<set>.ray):
 *     title: ...
 *     00000  IC30  6457-7.7_ic30.bin  A8EF        (address socket file sum|-)
 * A single 256 KB file also works. The NVRAM goes next to it as <name>.nvram.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ray.h"

#define DEFAULT_SET "pokeri-8460-1.1"

static int is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

/* path without extension + ".nvram" */
static void nvram_for(const char *path, char *out, int n)
{
    snprintf(out, n, "%s", path);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash))
        *dot = 0;
    strncat(out, ".nvram", n - strlen(out) - 1);
}

static long read_file(const char *path, uint8_t *buf, long max)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    long n = (long)fread(buf, 1, max, f);
    if (n == max && fgetc(f) != EOF)
        n = max + 1;            /* too big */
    fclose(f);
    return n;
}

static int load_set(const char *path, uint8_t *rom, char *title, int tlen, char *err, int elen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, elen, "cannot open %s", path);
        return -1;
    }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");
    memset(rom, 0xFF, ROM_SIZE);
    char line[512];
    int lineno = 0, chips = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
        if (!*s)
            continue;
        if (strncasecmp(s, "title:", 6) == 0) {
            s += 6;
            while (isspace((unsigned char)*s)) s++;
            snprintf(title, tlen, "%s", s);
            continue;
        }
        char addr_s[32], sock[32], fn[256], sum_s[32] = "-";
        int got = sscanf(s, "%31s %31s %255s %31s", addr_s, sock, fn, sum_s);
        if (got < 3) {
            snprintf(err, elen, "%s line %d: expected 'address socket file sum'", path, lineno);
            fclose(f);
            return -1;
        }
        long addr = strtol(addr_s, NULL, 16);
        char cp[1400];
        snprintf(cp, sizeof cp, "%s/%s", dir, fn);
        static uint8_t chip[ROM_SIZE + 1];
        long n = read_file(cp, chip, ROM_SIZE);
        if (n < 0) {
            snprintf(err, elen, "ROM set %s: missing %s (%s). Put the chip files listed in "
                     "the .ray file into %s.", path, fn, sock, dir);
            fclose(f);
            return -1;
        }
        if (addr < 0 || addr + n > ROM_SIZE) {
            snprintf(err, elen, "%s: %s does not fit at %05lX", path, fn, addr);
            fclose(f);
            return -1;
        }
        unsigned sum = 0;
        for (long i = 0; i < n; i++) sum += chip[i];
        sum &= 0xFFFF;
        if (strcmp(sum_s, "-") != 0 && strtoul(sum_s, NULL, 16) != sum)
            fprintf(stderr, "warning: %s sum %04X, label says %s\n", fn, sum, sum_s);
        memcpy(rom + addr, chip, n);
        chips++;
    }
    fclose(f);
    if (!chips) {
        snprintf(err, elen, "%s: no chips listed", path);
        return -1;
    }
    return 0;
}

int rom_load(const char *arg, const char *romdir, uint8_t *rom,
             char *nvram_path, int nvram_len, char *title, int title_len,
             char *err, int errlen)
{
    char cand[3][1400];
    char found[4400] = "";
    if (!arg) {
        snprintf(cand[0], sizeof cand[0], "%s/%s.ray", romdir, DEFAULT_SET);
        if (is_file(cand[0])) {
            snprintf(found, sizeof found, "%s", cand[0]);
        } else {
            DIR *d = opendir(romdir);
            struct dirent *de;
            char best[256] = "";
            while (d && (de = readdir(d)))
                if (ends_with(de->d_name, ".ray") && (!best[0] || strcmp(de->d_name, best) < 0))
                    snprintf(best, sizeof best, "%s", de->d_name);
            if (d) closedir(d);
            if (!best[0]) {
                snprintf(err, errlen, "no ROM set (.ray file) in %s", romdir);
                return -1;
            }
            snprintf(found, sizeof found, "%s/%s", romdir, best);
        }
    } else {
        snprintf(cand[0], sizeof cand[0], "%s/%s.ray", romdir, arg);
        snprintf(cand[1], sizeof cand[1], "%s", arg);
        snprintf(cand[2], sizeof cand[2], "%s/%s", romdir, arg);
        for (int i = 0; i < 3 && !found[0]; i++)
            if (is_file(cand[i]))
                snprintf(found, sizeof found, "%s", cand[i]);
        if (!found[0]) {
            snprintf(err, errlen, "ROM %s not found (looked in %s)", arg, romdir);
            return -1;
        }
    }
    const char *base = strrchr(found, '/');
    base = base ? base + 1 : found;
    snprintf(title, title_len, "%s", base);
    nvram_for(found, nvram_path, nvram_len);
    if (ends_with(found, ".ray"))
        return load_set(found, rom, title, title_len, err, errlen);
    static uint8_t buf[ROM_SIZE + 1];
    long n = read_file(found, buf, ROM_SIZE);
    if (n != ROM_SIZE) {
        snprintf(err, errlen, "%s: expected a 256 KB image", found);
        return -1;
    }
    memcpy(rom, buf, ROM_SIZE);
    return 0;
}
