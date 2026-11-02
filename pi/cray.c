/* cray: RAY video poker for the Raspberry Pi (or any Linux console).
 *
 * Draws straight to the display with DRM/KMS (HDMI or composite, no X or
 * Wayland), reads USB keyboards (and keyboard-type arcade encoders) with
 * evdev, plays the AY-3-8910 through ALSA and drives the cabinet lamps
 * from GPIO pins. Settings come from a config file (cray.conf).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/gpio.h>
#include <linux/input.h>
#include <alsa/asoundlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "ray.h"
#include "session.h"

/* ================================================================ config */
enum {
    ACT_COIN = BTN_COUNT, ACT_DOOR, ACT_REFILL, ACT_QUIT, ACT_SCANLINES, ACT_COUNT
};
static const char *const action_names[ACT_COUNT] = {
    [BTN_HOLD1] = "HOLD1", [BTN_HOLD2] = "HOLD2", [BTN_HOLD3] = "HOLD3",
    [BTN_HOLD4] = "HOLD4", [BTN_HOLD5] = "HOLD5", [BTN_DOUBLE] = "DOUBLE",
    [BTN_SMALL] = "SMALL", [BTN_BIG] = "BIG", [BTN_BET] = "BET",
    [BTN_COLLECT] = "COLLECT", [BTN_DEAL] = "DEAL", [BTN_SERVICE] = "SERVICE",
    [BTN_SRVC1] = "SRVC1", [BTN_SRVC2] = "SRVC2", [BTN_K13] = "K13", [BTN_K14] = "K14",
    [BTN_K15] = "K15", [BTN_K16] = "K16",
    [ACT_COIN] = "COIN", [ACT_DOOR] = "DOOR", [ACT_REFILL] = "REFILL", [ACT_QUIT] = "QUIT",
    [ACT_SCANLINES] = "SCANLINES",
};

#define KEYMAX 0x300
static int keymap[KEYMAX];            /* evdev code -> action + 1 (0 = none) */

#define NLAMPS 23
static struct {
    char rom[256], romdir[512], nvram[512];
    char connector[64], mode[64];
    char audio[64];
    int volume;                         /* 0..100 */
    int latency_ms;
    char gpiochip[64];
    int lamp_gpio[NLAMPS];              /* -1 = not connected */
    int lamp_active_low;
    int float_coins, save_every, auto_refill;
    int acrtc_clock;
    int kiosk;
    int custom50;
    int bright_attract;
    int realtime;
    char event_script[512];
    char sound_payout[512], sound_coin_in[512];
    uint32_t features;
    int scanlines;                      /* % darker for the last screen line of each game line */
    char scale[16];                     /* "auto" or "WxH", e.g. "2x3" */
} cfg;

static const struct { const char *name; int code; } keynames[] = {
#define K(n) { #n, KEY_##n }
    K(ESC), K(1), K(2), K(3), K(4), K(5), K(6), K(7), K(8), K(9), K(0),
    K(MINUS), K(EQUAL), K(BACKSPACE), K(TAB), K(Q), K(W), K(E), K(R), K(T), K(Y),
    K(U), K(I), K(O), K(P), K(LEFTBRACE), K(RIGHTBRACE), K(ENTER), K(LEFTCTRL),
    K(A), K(S), K(D), K(F), K(G), K(H), K(J), K(K), K(L), K(SEMICOLON),
    K(APOSTROPHE), K(GRAVE), K(LEFTSHIFT), K(BACKSLASH), K(Z), K(X), K(C), K(V),
    K(B), K(N), K(M), K(COMMA), K(DOT), K(SLASH), K(RIGHTSHIFT), K(KPASTERISK),
    K(LEFTALT), K(SPACE), K(CAPSLOCK), K(F1), K(F2), K(F3), K(F4), K(F5), K(F6),
    K(F7), K(F8), K(F9), K(F10), K(F11), K(F12), K(NUMLOCK), K(SCROLLLOCK),
    K(KP7), K(KP8), K(KP9), K(KPMINUS), K(KP4), K(KP5), K(KP6), K(KPPLUS),
    K(KP1), K(KP2), K(KP3), K(KP0), K(KPDOT), K(KPENTER), K(RIGHTCTRL),
    K(KPSLASH), K(RIGHTALT), K(HOME), K(UP), K(PAGEUP), K(LEFT), K(RIGHT),
    K(END), K(DOWN), K(PAGEDOWN), K(INSERT), K(DELETE),
#undef K
#define B(n) { #n, BTN_##n }
    B(TRIGGER), B(THUMB), B(THUMB2), B(TOP), B(TOP2), B(PINKIE), B(BASE),
    B(BASE2), B(BASE3), B(BASE4), B(BASE5), B(BASE6),
    B(SOUTH), B(EAST), B(NORTH), B(WEST), B(TL), B(TR), B(SELECT), B(START),
#undef B
};

static int key_code(const char *name)
{
    if (!strncasecmp(name, "KEY_", 4)) name += 4;
    if (!strncasecmp(name, "BTN_", 4)) name += 4;
    if (!strncasecmp(name, "CODE", 4)) return atoi(name + 4);
    for (size_t i = 0; i < sizeof keynames / sizeof keynames[0]; i++)
        if (!strcasecmp(name, keynames[i].name))
            return keynames[i].code;
    return -1;
}

static const char *code_name(int code)
{
    for (size_t i = 0; i < sizeof keynames / sizeof keynames[0]; i++)
        if (keynames[i].code == code)
            return keynames[i].name;
    return NULL;
}

static void bind(int action, const char *keys)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", keys);
    for (int c = 0; c < KEYMAX; c++)
        if (keymap[c] == action + 1)
            keymap[c] = 0;
    for (char *t = strtok(buf, " \t,"); t; t = strtok(NULL, " \t,")) {
        int code = key_code(t);
        if (code <= 0 || code >= KEYMAX)
            fprintf(stderr, "config: unknown key %s\n", t);
        else
            keymap[code] = action + 1;
    }
}

static void config_defaults(void)
{
    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.audio, "default");
    cfg.volume = 50;
    cfg.latency_ms = 40;
    strcpy(cfg.gpiochip, "auto");
    /* lamp bit -> GPIO (BCM numbers), see cray.conf */
    static const int pins[NLAMPS] = {
        /* bit 0..7   */ 21, 4, 5, 2, 26, 18, 7, 24,
        /* bit 8..15  */ 12, 8, 6, 23, 19, 20, 22, 25,
        /* bit 16..22 */ 9, 10, 11, 13, 16, 27, 17 };
    memcpy(cfg.lamp_gpio, pins, sizeof pins);
    cfg.float_coins = 1000;
    cfg.save_every = 3600;
    cfg.auto_refill = 1;
    cfg.features = FEATURE_WIN_MUSIC;
    cfg.acrtc_clock = 3000000;
    cfg.custom50 = 1;
    cfg.bright_attract = 1;
    strcpy(cfg.event_script, "/etc/cray/event.sh");
    strcpy(cfg.romdir, "/var/lib/cray/roms");
    cfg.realtime = 1;
    bind(BTN_HOLD1, "1"); bind(BTN_HOLD2, "2"); bind(BTN_HOLD3, "3");
    bind(BTN_HOLD4, "4"); bind(BTN_HOLD5, "5");
    bind(BTN_DEAL, "ENTER KPENTER SPACE"); bind(BTN_BET, "B");
    bind(BTN_COLLECT, "P"); bind(BTN_DOUBLE, "D");
    bind(BTN_SMALL, "LEFT"); bind(BTN_BIG, "RIGHT");
    bind(BTN_SERVICE, "S"); bind(BTN_SRVC1, "F1"); bind(BTN_SRVC2, "F2");
    bind(BTN_K13, "F3"); bind(BTN_K14, "F4"); bind(BTN_K15, "F5"); bind(BTN_K16, "F6");
    bind(ACT_COIN, "I"); bind(ACT_DOOR, "F10"); bind(ACT_REFILL, "F9");
    bind(ACT_QUIT, "ESC");
    bind(ACT_SCANLINES, "F7");
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int config_set(const char *k, const char *v)
{
#define STR(name) if (!strcasecmp(k, #name)) { snprintf(cfg.name, sizeof cfg.name, "%s", v); return 0; }
#define INT(name) if (!strcasecmp(k, #name)) { cfg.name = atoi(v); return 0; }
    STR(event_script) STR(scale) STR(rom) STR(romdir) STR(nvram) STR(connector) STR(mode) STR(audio) STR(gpiochip)
    INT(volume) INT(latency_ms) INT(lamp_active_low) INT(float_coins) INT(save_every) INT(auto_refill)
    INT(acrtc_clock) INT(kiosk) INT(custom50) INT(bright_attract) INT(realtime) INT(scanlines)
#undef STR
#undef INT
    if (!strcasecmp(k, "features")) { cfg.features = strtoul(v, NULL, 16); return 0; }
    if (!strcasecmp(k, "sound.payout")) { snprintf(cfg.sound_payout, sizeof cfg.sound_payout, "%s", v); return 0; }
    if (!strcasecmp(k, "sound.coin-in")) { snprintf(cfg.sound_coin_in, sizeof cfg.sound_coin_in, "%s", v); return 0; }
    if (!strncasecmp(k, "lamp", 4) && isdigit((unsigned char)k[4])) {
        int bit = atoi(k + 4);
        if (bit < 0 || bit >= NLAMPS) return -1;
        cfg.lamp_gpio[bit] = (*v == '-' || !*v) ? -1 : atoi(v);
        return 0;
    }
    if (!strncasecmp(k, "key.", 4)) {
        for (int a = 0; a < ACT_COUNT; a++)
            if (!strcasecmp(k + 4, action_names[a])) {
                bind(a, v);
                return 0;
            }
    }
    return -1;
}

static void config_load(const char *path, int must)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (must) { perror(path); exit(1); }
        return;
    }
    fprintf(stderr, "config: %s\n", path);
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        char *s = trim(line);
        if (!*s) continue;
        if (!eq) { fprintf(stderr, "%s:%d: expected key = value\n", path, n); continue; }
        *eq = 0;
        if (config_set(trim(line), trim(eq + 1)))
            fprintf(stderr, "%s:%d: unknown setting %s\n", path, n, trim(line));
    }
    fclose(f);
}

/* ================================================================ display */
static struct {
    int fd;
    uint32_t conn_id, crtc_id;
    drmModeModeInfo mode;
    drmModeCrtc *saved;
    struct { uint32_t handle, fb, pitch; uint64_t size; uint32_t *map; uint8_t idx[SCREEN_W * SCREEN_H]; uint32_t pal[16]; int valid; int scan; } buf[2];
    int front;
    int flip_pending;
    int sx, sy, ox, oy;                 /* scale and offset of the picture */
    drmModeModeInfo fallback;           /* if the chosen mode is refused */
    int has_fallback;
    double refresh;
} drm = { .fd = -1 };

static const char *conn_type_name(uint32_t t)
{
    switch (t) {
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
    case DRM_MODE_CONNECTOR_Composite: return "Composite";
    case DRM_MODE_CONNECTOR_TV: return "TV";
    case DRM_MODE_CONNECTOR_SVIDEO: return "SVIDEO";
    case DRM_MODE_CONNECTOR_VGA: return "VGA";
    case DRM_MODE_CONNECTOR_DSI: return "DSI";
    case DRM_MODE_CONNECTOR_DPI: return "DPI";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
    case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
    default: return "Unknown";
    }
}

static double mode_hz(const drmModeModeInfo *m)
{
    double hz = (double)m->clock * 1000.0 / ((double)m->htotal * m->vtotal);
    if (m->flags & DRM_MODE_FLAG_INTERLACE) hz *= 2;
    return hz;
}

/* "WxH", "WxHi", "WxH@R" */
static int mode_matches(const drmModeModeInfo *m, const char *want)
{
    int w = 0, h = 0, r = 0;
    char *p;
    w = strtol(want, &p, 10);
    if (*p != 'x') return 0;
    h = strtol(p + 1, &p, 10);
    int inter = 0;                       /* progressive unless "i" is given */
    if (*p == 'i') { inter = 1; p++; }
    else if (*p == 'p') { p++; }
    if (*p == '@') r = atoi(p + 1);
    if (m->hdisplay != w || m->vdisplay != h) return 0;
    if (!!(m->flags & DRM_MODE_FLAG_INTERLACE) != inter) return 0;
    if (r && fabs(mode_hz(m) - r) > 1.0) return 0;
    return 1;
}

/* integer picture scale for a mode; a few lines may be cropped rather
 * than drop a whole step */
static void mode_scale(int w, int h, int *sx, int *sy)
{
    *sx = (w + 16) / SCREEN_W;
    *sy = (h + 8) / SCREEN_H;
    if (*sx < 1) *sx = 1;
    if (*sy < 1) *sy = 1;
    if (w > 720) {               /* square pixels except on SD/TV modes */
        if (*sx < *sy) *sy = *sx; else *sx = *sy;
    }
}

static int is_50hz(const drmModeModeInfo *m) { return fabs(mode_hz(m) - 50.0) < 0.5; }

/* Standard 50 Hz HDMI timings (CEA-861), for when the screen's mode list
 * leaves them out. 576p is marked 4:3 so a wide TV doesn't stretch it. */
static const struct {
    const char *name;
    uint32_t clock;
    uint16_t hd, hss, hse, ht, vd, vss, vse, vt;
    uint32_t flags;
} builtin_modes[] = {
    { "720x576", 27000, 720, 732, 796, 864, 576, 581, 586, 625,
      DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_4_3 },          /* VIC 17 */
    /* 576p's line and frame timing (31.25 kHz, 625 lines, 50 Hz) with 1216
     * pixels per line instead of 720: exactly two per game pixel, so the
     * picture has no side borders and the screen stretches it edge to edge */
    { "1216x576", 45594, 1216, 1236, 1344, 1459, 576, 581, 586, 625,
      DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC },
    { "1280x720", 74250, 1280, 1720, 1760, 1980, 720, 725, 730, 750,
      DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9 },         /* VIC 19 */
    { "1920x1080", 148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125,
      DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9 },         /* VIC 31 */
};

static int builtin_mode(const char *want, drmModeModeInfo *out)
{
    for (size_t i = 0; i < sizeof builtin_modes / sizeof builtin_modes[0]; i++) {
        drmModeModeInfo m;
        memset(&m, 0, sizeof m);
        m.clock = builtin_modes[i].clock;
        m.hdisplay = builtin_modes[i].hd; m.hsync_start = builtin_modes[i].hss;
        m.hsync_end = builtin_modes[i].hse; m.htotal = builtin_modes[i].ht;
        m.vdisplay = builtin_modes[i].vd; m.vsync_start = builtin_modes[i].vss;
        m.vsync_end = builtin_modes[i].vse; m.vtotal = builtin_modes[i].vt;
        m.flags = builtin_modes[i].flags;
        m.vrefresh = 50;
        m.type = DRM_MODE_TYPE_USERDEF;
        snprintf(m.name, sizeof m.name, "%sp50", builtin_modes[i].name);
        if (mode_matches(&m, want)) { *out = m; return 0; }
    }
    return -1;
}

/* A progressive PAL composite mode with all 292 game lines: the driver's
 * 720x288 mode with 4 lines taken from the vertical blanking. The driver
 * accepts custom timings within limits; if it refuses this one, cray
 * falls back to 720x288 (2 lines cropped top and bottom). */
static int mode_292(drmModeConnector *c, drmModeModeInfo *out)
{
    for (int i = 0; i < c->count_modes; i++) {
        drmModeModeInfo m = c->modes[i];
        if (m.hdisplay != 720 || m.vdisplay != 288 || (m.flags & DRM_MODE_FLAG_INTERLACE))
            continue;
        int fp = m.vsync_start - m.vdisplay, bp = m.vtotal - m.vsync_end;
        int shift = fp >= 3 ? 2 : 5 - fp;        /* move the sync down by this much */
        if (bp - shift < 1) return -1;
        m.vdisplay = 292;
        m.vsync_start += shift;
        m.vsync_end += shift;
        m.type = DRM_MODE_TYPE_USERDEF;
        snprintf(m.name, sizeof m.name, "720x292");
        *out = m;
        return 0;
    }
    return -1;
}

/* The game runs at 50 Hz (PAL), so the display must too, for every frame
 * to be shown exactly once. Order of choice:
 *   1. mode = ... from the config
 *   2. a 50 Hz mode the display offers: the native size if it has one,
 *      else the one giving the biggest picture
 *   3. the native mode slowed to 50 Hz (same timing, lower pixel clock);
 *      most LCD screens accept it (custom50 = 0 turns this off)
 *   4. the native mode as it is (the motion then can't be perfectly even) */
static int pick_mode(drmModeConnector *c, drmModeModeInfo *out)
{
    int pref = 0;
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) { pref = i; break; }
    const drmModeModeInfo *p = &c->modes[pref];
    drm.has_fallback = 0;
    if (cfg.mode[0]) {
        for (int i = 0; i < c->count_modes; i++)
            if (mode_matches(&c->modes[i], cfg.mode)) { *out = c->modes[i]; return 0; }
        drmModeModeInfo m;
        if ((!strncmp(cfg.mode, "720x292", 7) && mode_292(c, &m) == 0) ||
            builtin_mode(cfg.mode, &m) == 0) {
            fprintf(stderr, "display: %s is not in the list; trying it anyway\n", cfg.mode);
            /* if it's refused, the automatic choice below */
            char saved[sizeof cfg.mode];
            memcpy(saved, cfg.mode, sizeof saved);
            cfg.mode[0] = 0;
            if (!strncmp(saved, "720x292", 7)) {
                for (int i = 0; i < c->count_modes; i++)
                    if (mode_matches(&c->modes[i], "720x288@50")) { drm.fallback = c->modes[i]; break; }
            } else {
                pick_mode(c, &drm.fallback);
            }
            memcpy(cfg.mode, saved, sizeof saved);
            drm.has_fallback = 1;
            *out = m;
            return 0;
        }
        fprintf(stderr, "display: mode %s not offered, choosing one\n", cfg.mode);
    }
    /* progressive first (one game line per screen line, no flicker), then
     * the screen's own size, then the biggest picture */
    int best = -1;
    long long best_score = -1;
    for (int i = 0; i < c->count_modes; i++) {
        const drmModeModeInfo *m = &c->modes[i];
        if (!is_50hz(m) || m->hdisplay > p->hdisplay || m->vdisplay > p->vdisplay)
            continue;
        int sx, sy;
        mode_scale(m->hdisplay, m->vdisplay, &sx, &sy);
        long long score = 0;
        if (!(m->flags & DRM_MODE_FLAG_INTERLACE)) score += 1LL << 50;
        if (m->hdisplay == p->hdisplay && m->vdisplay == p->vdisplay) score += 1LL << 45;
        score += (long long)sx * sy * 1000000000LL + m->hdisplay * m->vdisplay;
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best >= 0) {
        *out = c->modes[best];
        return 0;
    }
    *out = *p;
    if (cfg.custom50 && !is_50hz(p)) {
        double hz = mode_hz(p);
        out->clock = (uint32_t)(p->clock * 50.0 / hz + 0.5);
        out->vrefresh = 50;
        out->type = DRM_MODE_TYPE_USERDEF;
        snprintf(out->name, sizeof out->name, "%dx%d@50", p->hdisplay, p->vdisplay);
        fprintf(stderr, "display: no 50 Hz mode offered; trying the native mode slowed to 50 Hz "
                "(set custom50 = 0 if the screen goes blank)\n");
    }
    return 0;
}

static int create_buffer(int i)
{
    struct drm_mode_create_dumb cd = { .width = drm.mode.hdisplay, .height = drm.mode.vdisplay, .bpp = 32 };
    if (drmIoctl(drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) return -1;
    drm.buf[i].handle = cd.handle;
    drm.buf[i].pitch = cd.pitch;
    drm.buf[i].size = cd.size;
    if (drmModeAddFB(drm.fd, cd.width, cd.height, 24, 32, cd.pitch, cd.handle, &drm.buf[i].fb))
        return -1;
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(drm.fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) return -1;
    drm.buf[i].map = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm.fd, md.offset);
    if (drm.buf[i].map == MAP_FAILED) return -1;
    memset(drm.buf[i].map, 0, cd.size);
    drm.buf[i].valid = 0;
    return 0;
}

static int try_card(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    /* modes carry their picture aspect (4:3 for 576p), sent to the TV */
    drmSetClientCap(fd, DRM_CLIENT_CAP_ASPECT_RATIO, 1);
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { close(fd); return -1; }
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        char name[64];
        snprintf(name, sizeof name, "%s-%u", conn_type_name(c->connector_type), c->connector_type_id);
        int ok = c->connection == DRM_MODE_CONNECTED && c->count_modes > 0;
        if (cfg.connector[0] && strcasecmp(cfg.connector, name)) ok = 0;
        fprintf(stderr, "display: %s %s %s\n", path, name,
                c->connection == DRM_MODE_CONNECTED ? "connected" : "not connected");
        if (ok) conn = c; else drmModeFreeConnector(c);
    }
    if (!conn) { drmModeFreeResources(res); close(fd); return -1; }
    drm.fd = fd;
    drm.conn_id = conn->connector_id;
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *m = &conn->modes[i];
        unsigned ar = m->flags & DRM_MODE_FLAG_PIC_AR_MASK;
        fprintf(stderr, "  mode %dx%d%s@%.2f%s%s\n", m->hdisplay, m->vdisplay,
                (m->flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "", mode_hz(m),
                ar == DRM_MODE_FLAG_PIC_AR_4_3 ? " 4:3" : ar == DRM_MODE_FLAG_PIC_AR_16_9 ? " 16:9" : "",
                (m->type & DRM_MODE_TYPE_PREFERRED) ? " (preferred)" : "");
    }
    pick_mode(conn, &drm.mode);
    /* a CRTC this connector can use */
    drm.crtc_id = 0;
    for (int e = 0; e < conn->count_encoders && !drm.crtc_id; e++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc) continue;
        if (enc->crtc_id && conn->encoder_id == enc->encoder_id)
            drm.crtc_id = enc->crtc_id;
        for (int c = 0; c < res->count_crtcs && !drm.crtc_id; c++)
            if (enc->possible_crtcs & (1u << c))
                drm.crtc_id = res->crtcs[c];
        drmModeFreeEncoder(enc);
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!drm.crtc_id) { close(fd); drm.fd = -1; return -1; }
    return 0;
}

static int display_open(void)
{
    char path[64];
    for (int i = 0; i < 8 && drm.fd < 0; i++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        try_card(path);
    }
    if (drm.fd < 0) {
        fprintf(stderr, "display: no connected display found%s%s\n",
                cfg.connector[0] ? " named " : "", cfg.connector);
        return -1;
    }
    drm.saved = drmModeGetCrtc(drm.fd, drm.crtc_id);
    for (int attempt = 0;; attempt++) {
        drm.refresh = mode_hz(&drm.mode);
        int w = drm.mode.hdisplay, h = drm.mode.vdisplay;
        int sx, sy;
        mode_scale(w, h, &sx, &sy);
        int ux, uy;
        if (cfg.scale[0] && sscanf(cfg.scale, "%dx%d", &ux, &uy) == 2 && ux >= 1 && uy >= 1 &&
            ux <= 8 && uy <= 8) {
            sx = ux; sy = uy;          /* as set; more than fits is cropped */
        }
        drm.sx = sx; drm.sy = sy;
        drm.ox = (w - SCREEN_W * sx) / 2;
        drm.oy = (h - SCREEN_H * sy) / 2;
        fprintf(stderr, "display: %dx%d%s@%.2f, picture %dx%d (x%d, x%d)\n", w, h,
                (drm.mode.flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "", drm.refresh,
                SCREEN_W * sx, SCREEN_H * sy, sx, sy);
        if (create_buffer(0) || create_buffer(1)) {
            fprintf(stderr, "display: cannot create frame buffers: %s\n", strerror(errno));
            return -1;
        }
        if (drmModeSetCrtc(drm.fd, drm.crtc_id, drm.buf[0].fb, 0, 0, &drm.conn_id, 1, &drm.mode) == 0)
            break;
        int err = errno;
        for (int i = 0; i < 2; i++) {          /* drop this mode's buffers */
            drmModeRmFB(drm.fd, drm.buf[i].fb);
            munmap(drm.buf[i].map, drm.buf[i].size);
            struct drm_mode_destroy_dumb dd = { .handle = drm.buf[i].handle };
            drmIoctl(drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        }
        if (attempt == 0 && drm.has_fallback && err != EACCES) {
            fprintf(stderr, "display: %s refused (%s), using %dx%d%s@%.0f\n", drm.mode.name,
                    strerror(err), drm.fallback.hdisplay, drm.fallback.vdisplay,
                    (drm.fallback.flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "", mode_hz(&drm.fallback));
            drm.mode = drm.fallback;
            continue;
        }
        fprintf(stderr, "display: cannot set mode: %s%s\n", strerror(err),
                err == EACCES ? " (is something else using the screen?)" : "");
        return -1;
    }
    drm.front = 0;
    return 0;
}

static void display_close(void)
{
    if (drm.fd < 0) return;
    if (drm.saved) {
        drmModeSetCrtc(drm.fd, drm.saved->crtc_id, drm.saved->buffer_id, drm.saved->x,
                       drm.saved->y, &drm.conn_id, 1, &drm.saved->mode);
        drmModeFreeCrtc(drm.saved);
    }
    close(drm.fd);
    drm.fd = -1;
}

/* Draw the picture into buffer b, rewriting only the rows that changed
 * since this buffer last showed a picture. */
static void draw_into(int b, const uint8_t *img, const uint32_t *pal)
{
    int w = drm.mode.hdisplay, h = drm.mode.vdisplay;
    /* scanlines: with 2 or more screen lines per game line, the last one
     * is drawn darker; a change of the setting redraws everything */
    int scan = __atomic_load_n(&cfg.scanlines, __ATOMIC_RELAXED);
    if (scan < 0) scan = 0;
    if (scan > 100) scan = 100;
    if (drm.sy < 2) scan = 0;
    int palchg = !drm.buf[b].valid || memcmp(pal, drm.buf[b].pal, sizeof drm.buf[b].pal) ||
                 drm.buf[b].scan != scan;
    uint32_t dark[16];
    for (int i = 0; i < 16; i++) {
        uint32_t c = pal[i], k = 100 - scan;
        dark[i] = (((c >> 16) & 255) * k / 100) << 16 | (((c >> 8) & 255) * k / 100) << 8 |
                  ((c & 255) * k / 100);
    }
    static uint32_t dline[SCREEN_W * 8];
    uint32_t *fb = drm.buf[b].map;
    int pitch = drm.buf[b].pitch / 4;
    static uint32_t line[SCREEN_W * 8];
    int dw = SCREEN_W * drm.sx;
    int x0 = drm.ox < 0 ? -drm.ox : 0;            /* first visible column of the line */
    int dx = drm.ox < 0 ? 0 : drm.ox;
    int n = dw - x0 < w - dx ? dw - x0 : w - dx;
    for (int y = 0; y < SCREEN_H; y++) {
        /* the row as drawn is what this buffer remembers, so what's on
         * screen and the record of it can't drift apart */
        uint8_t src[SCREEN_W];
        memcpy(src, img + y * SCREEN_W, SCREEN_W);
        uint8_t *have = drm.buf[b].idx + y * SCREEN_W;
        if (!palchg && !memcmp(src, have, SCREEN_W))
            continue;
        memcpy(have, src, SCREEN_W);
        int k = 0;
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t c = pal[src[x]], d = dark[src[x]];
            for (int s = 0; s < drm.sx; s++) { line[k] = c; dline[k] = d; k++; }
        }
        for (int s = 0; s < drm.sy; s++) {
            int dy = drm.oy + y * drm.sy + s;
            if (dy < 0 || dy >= h) continue;
            const uint32_t *l = (scan && s == drm.sy - 1) ? dline : line;
            memcpy(fb + (size_t)dy * pitch + dx, l + x0, n * 4);
        }
    }
    memcpy(drm.buf[b].pal, pal, sizeof drm.buf[b].pal);
    drm.buf[b].scan = scan;
    drm.buf[b].valid = 1;
}

/* vblank counter of the last completed flip, and how many vblanks went by
 * without a new frame since the one before (0 = perfectly in step) */
static unsigned last_seq;
static int have_seq;
static int missed_vblanks;
static long missed_total, frames_total;

static void flip_handler(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
    drm.flip_pending = 0;
    if (have_seq) {
        int gap = (int)(seq - last_seq) - 1;
        if (gap > 0 && gap < 1000) {
            __atomic_add_fetch(&missed_vblanks, gap, __ATOMIC_ACQ_REL);
            missed_total += gap;
        }
    }
    last_seq = seq;
    have_seq = 1;
    frames_total++;
}

static void display_wait_flip(int timeout_ms)
{
    struct pollfd p = { .fd = drm.fd, .events = POLLIN };
    drmEventContext ev = { .version = 2, .page_flip_handler = flip_handler };
    while (drm.flip_pending) {
        if (poll(&p, 1, timeout_ms) <= 0) { drm.flip_pending = 0; break; }
        drmHandleEvent(drm.fd, &ev);
    }
}

/* ---- display thread: draws and swaps frames on its own core while the
 * game runs the next frame on another. Two picture slots: the game fills
 * one while the display thread draws the other. */
#include <pthread.h>
static pthread_mutex_t fr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fr_cond = PTHREAD_COND_INITIALIZER;
static uint8_t fr_img[2][SCREEN_W * SCREEN_H];
static uint32_t fr_pal[2][16];
static int fr_ready = -1;               /* slot holding a new frame, -1 = none */
static int fr_write;                    /* slot the game fills next */
static int fr_reading = -1;             /* slot the display thread is drawing from */
static int display_stop;
static double emu_ms;                   /* how long the game's last frame took */

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void *display_main(void *arg)
{
    double last_log = 0;
    long missed_logged = 0;
    for (;;) {
        pthread_mutex_lock(&fr_lock);
        while (fr_ready < 0 && !display_stop)
            pthread_cond_wait(&fr_cond, &fr_lock);
        if (display_stop) { pthread_mutex_unlock(&fr_lock); break; }
        int r = fr_ready;
        fr_ready = -1;
        fr_reading = r;
        pthread_cond_broadcast(&fr_cond);
        pthread_mutex_unlock(&fr_lock);

        double t0 = now_ms();
        int back = drm.front ^ 1;
        draw_into(back, fr_img[r], fr_pal[r]);
        double draw_ms = now_ms() - t0;
        if (drmModePageFlip(drm.fd, drm.crtc_id, drm.buf[back].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) {
            drm.flip_pending = 1;
            drm.front = back;
        } else {
            /* no page flips (some drivers): draw into the visible buffer */
            draw_into(drm.front, fr_img[r], fr_pal[r]);
        }
        pthread_mutex_lock(&fr_lock);
        fr_reading = -1;                        /* done with the slot */
        pthread_cond_broadcast(&fr_cond);
        pthread_mutex_unlock(&fr_lock);
        display_wait_flip(100);
        /* a missed vblank: say how long the work took (at most every 2 s) */
        if (missed_total != missed_logged && now_ms() - last_log > 2000) {
            fprintf(stderr, "display: missed %ld frame(s) (%ld in all); game %.1f ms, drawing %.1f ms "
                    "of a %.1f ms frame\n", missed_total - missed_logged, missed_total, emu_ms,
                    draw_ms, 1000.0 / drm.refresh);
            missed_logged = missed_total;
            last_log = now_ms();
        }
    }
    return NULL;
}

/* game side: hand over the finished frame (waits while the display thread
 * still has an undrawn one) */
static void publish_frame(void)
{
    /* never write into the slot the display thread is still drawing from */
    pthread_mutex_lock(&fr_lock);
    while (fr_reading == fr_write && !display_stop)
        pthread_cond_wait(&fr_cond, &fr_lock);
    pthread_mutex_unlock(&fr_lock);
    video_render(&ray.acrtc, fr_img[fr_write]);
    video_palette_ex(acrtc_reg(&ray.acrtc, 0x06), cfg.bright_attract, fr_pal[fr_write]);
    pthread_mutex_lock(&fr_lock);
    while (fr_ready >= 0 && !display_stop)
        pthread_cond_wait(&fr_cond, &fr_lock);
    fr_ready = fr_write;
    fr_write ^= 1;
    pthread_cond_broadcast(&fr_cond);
    pthread_mutex_unlock(&fr_lock);
}

/* ================================================================ input */
#define MAXDEV 16
static struct { int fd; char path[300]; } devs[MAXDEV];
static int ndevs;

static int has_bit(const unsigned long *bits, int n)
{
    return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

static int dev_open(const char *path)
{
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].path, path)) return 0;
    if (ndevs >= MAXDEV) return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    int any = 0;
    for (int c = 0; c < KEYMAX && !any; c++)
        if (keymap[c] && has_bit(keys, c)) any = 1;
    if (!any) { close(fd); return -1; }
    char name[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    /* keep the key presses away from the text console */
    ioctl(fd, EVIOCGRAB, 1);
    fprintf(stderr, "input: %s (%s)\n", path, name);
    devs[ndevs].fd = fd;
    snprintf(devs[ndevs].path, sizeof devs[ndevs].path, "%s", path);
    ndevs++;
    return 0;
}

static void input_scan(void)
{
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        dev_open(p);
    }
    closedir(d);
}

static volatile sig_atomic_t quit;

static void do_action(int a, int down)
{
    if (a < BTN_COUNT) {
        ray_set_button(a, down);
        return;
    }
    if (!down) return;
    switch (a) {
    case ACT_COIN: session_coin(); break;
    case ACT_DOOR: session_toggle_door(); break;
    case ACT_REFILL: session_refill(); break;
    case ACT_QUIT: if (!cfg.kiosk) quit = 1; break;
    case ACT_SCANLINES: {
        /* off, 25, 50, 75 % (the display thread notices and redraws) */
        int v = (__atomic_load_n(&cfg.scanlines, __ATOMIC_RELAXED) / 25 + 1) * 25;
        if (v > 75) v = 0;
        __atomic_store_n(&cfg.scanlines, v, __ATOMIC_RELAXED);
        fprintf(stderr, "scanlines %d %%\n", v);
        break;
    }
    }
}

static void input_poll(void)
{
    for (int i = 0; i < ndevs; i++) {
        struct input_event ev[32];
        ssize_t n;
        while ((n = read(devs[i].fd, ev, sizeof ev)) > 0) {
            for (size_t k = 0; k < n / sizeof ev[0]; k++) {
                if (ev[k].type != EV_KEY || ev[k].value == 2) continue;   /* skip repeats */
                int code = ev[k].code;
                int a = code < KEYMAX ? keymap[code] - 1 : -1;
                if (a >= 0)
                    do_action(a, ev[k].value);
                else if (ev[k].value == 1) {
                    const char *nm = code_name(code);
                    fprintf(stderr, "input: key %s%s (code %d) has no action\n",
                            nm ? "" : "CODE", nm ? nm : "", code);
                }
            }
        }
        if (n < 0 && errno != EAGAIN) {          /* unplugged */
            fprintf(stderr, "input: %s gone\n", devs[i].path);
            close(devs[i].fd);
            devs[i] = devs[--ndevs];
            i--;
        }
    }
}

/* ---- control channel: commands written to a FIFO while the game runs,
 * e.g. over SSH:  echo COIN | sudo tee /run/cray/control
 *   an action name (DEAL, HOLD1, COIN, DOOR, SCANLINES ...)  press it
 *   setting=value  for scanlines, volume                       change it */
#ifndef CONTROL_PATH
#define CONTROL_PATH "/run/cray/control"
#endif
static int control_fd = -1;
static int release_at[BTN_COUNT];       /* frames left until a commanded press lets go */

static void control_open(void)
{
    mkdir("/run/cray", 0755);
    if (mkfifo(CONTROL_PATH, 0620) != 0 && errno != EEXIST) return;
    /* read-write, so the FIFO never reports end of file between writers */
    control_fd = open(CONTROL_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (control_fd >= 0)
        fprintf(stderr, "control: %s\n", CONTROL_PATH);
}

static void control_command(char *cmd)
{
    char *eq = strchr(cmd, '=');
    if (eq) {
        *eq = 0;
        char *k = trim(cmd), *v = trim(eq + 1);
        if (!strcasecmp(k, "scanlines")) __atomic_store_n(&cfg.scanlines, atoi(v), __ATOMIC_RELAXED);
        else if (!strcasecmp(k, "volume")) cfg.volume = atoi(v);
        else { fprintf(stderr, "control: can't change %s while running\n", k); return; }
        fprintf(stderr, "control: %s = %s\n", k, v);
        return;
    }
    char *name = trim(cmd);
    for (int a = 0; a < ACT_COUNT; a++)
        if (!strcasecmp(name, action_names[a])) {
            do_action(a, 1);
            if (a < BTN_COUNT) release_at[a] = 10;       /* hold buttons for 0.2 s */
            return;
        }
    if (*name) fprintf(stderr, "control: unknown command %s\n", name);
}

static void control_poll(void)
{
    for (int b = 0; b < BTN_COUNT; b++)
        if (release_at[b] && --release_at[b] == 0)
            ray_set_button(b, 0);
    if (control_fd < 0) return;
    static char buf[512];
    static int len;
    ssize_t n = read(control_fd, buf + len, sizeof buf - 1 - len);
    if (n <= 0) return;
    len += n;
    buf[len] = 0;
    char *nl;
    while ((nl = strchr(buf, '\n'))) {
        *nl = 0;
        control_command(buf);
        len -= (int)(nl + 1 - buf);
        memmove(buf, nl + 1, len + 1);
    }
    if (len >= (int)sizeof buf - 1) len = 0;
}

/* ================================================================ sound */
static snd_pcm_t *pcm;
#define ABUF (AUDIO_RATE * 2)
static float aring[ABUF];
static int ahead, alen;
static double afrac;
static int aprimed;
static int atarget;

static int audio_open(void)
{
    int err = snd_pcm_open(&pcm, cfg.audio, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "sound: cannot open %s: %s (no sound)\n", cfg.audio, snd_strerror(err));
        pcm = NULL;
        return -1;
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, 1,
                             AUDIO_RATE, 1, 50000);
    if (err < 0) {
        fprintf(stderr, "sound: %s: %s (no sound)\n", cfg.audio, snd_strerror(err));
        snd_pcm_close(pcm);
        pcm = NULL;
        return -1;
    }
    atarget = AUDIO_RATE * (cfg.latency_ms > 5 ? cfg.latency_ms : 5) / 1000;
    fprintf(stderr, "sound: %s\n", cfg.audio);
    return 0;
}

/* ---- event sounds: WAV files mixed into the game's sound */
typedef struct { float *s; int n; } Clip;
static Clip clip_payout, clip_coin_in;
#define VOICES 8
static struct { const Clip *c; int pos; } voice[VOICES];

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* 8/16-bit PCM WAV, any rate and channel count -> mono 44100 Hz floats */
static int load_wav(const char *path, Clip *c)
{
    if (!path[0]) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "sound: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(size);
    if (!d || fread(d, 1, size, f) != (size_t)size) { fclose(f); free(d); return -1; }
    fclose(f);
    int ch = 0, rate = 0, bits = 0;
    const uint8_t *data = NULL;
    uint32_t dlen = 0;
    if (size < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) goto bad;
    for (long p = 12; p + 8 <= size;) {
        uint32_t len = le32(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4) && len >= 16) {
            if (le16(d + p + 8) != 1) goto bad;      /* PCM only */
            ch = le16(d + p + 10);
            rate = le32(d + p + 12);
            bits = le16(d + p + 22);
        } else if (!memcmp(d + p, "data", 4)) {
            data = d + p + 8;
            dlen = len;
            if (data + dlen > d + size) dlen = (uint32_t)(d + size - data);
        }
        p += 8 + len + (len & 1);
    }
    if (!data || ch < 1 || rate < 1000 || (bits != 8 && bits != 16)) goto bad;
    int frame = ch * bits / 8;
    long frames = dlen / frame;
    long out = (long)((double)frames * AUDIO_RATE / rate);
    c->s = malloc(sizeof(float) * (out + 1));
    c->n = (int)out;
    for (long i = 0; i < out; i++) {
        long k = (long)((double)i * rate / AUDIO_RATE);
        if (k >= frames) k = frames - 1;
        float v = 0;
        for (int j = 0; j < ch; j++) {
            const uint8_t *q = data + k * frame + j * bits / 8;
            v += bits == 16 ? (int16_t)le16(q) / 32768.0f : (q[0] - 128) / 128.0f;
        }
        /* the game's sound is about -0.5..0.5 before the volume setting */
        c->s[i] = v / ch * 0.5f;
    }
    free(d);
    fprintf(stderr, "sound: %s (%.2f s)\n", path, out / (double)AUDIO_RATE);
    return 0;
bad:
    fprintf(stderr, "sound: %s is not an 8/16-bit PCM WAV file\n", path);
    free(d);
    return -1;
}

static void play_clip(const Clip *c)
{
    if (!c->n) return;
    for (int i = 0; i < VOICES; i++)
        if (!voice[i].c) { voice[i].c = c; voice[i].pos = 0; return; }
}

static void audio_push(const float *s, int n)
{
    float mixed[AUDIO_RATE / TICK_HZ + 8];
    if (n > (int)(sizeof mixed / sizeof mixed[0])) n = sizeof mixed / sizeof mixed[0];
    memcpy(mixed, s, n * sizeof(float));
    for (int v = 0; v < VOICES; v++) {
        if (!voice[v].c) continue;
        for (int i = 0; i < n && voice[v].pos < voice[v].c->n; i++)
            mixed[i] += voice[v].c->s[voice[v].pos++];
        if (voice[v].pos >= voice[v].c->n) voice[v].c = NULL;
    }
    s = mixed;
    for (int i = 0; i < n; i++) {
        if (alen == ABUF) { ahead = (ahead + 1) % ABUF; alen--; }
        aring[(ahead + alen) % ABUF] = s[i];
        alen++;
    }
}

/* Feed the sound card in fixed chunks. The emulator makes 44100 samples
 * per emulated second, the card plays at its own clock, so the backlog
 * drifts; each chunk is stretched or squeezed by up to 2 % to keep it
 * near the target instead of dropping sound. A display mode slowed to
 * "50 Hz" by a whole-kHz pixel clock can be 1 % off (one was 0.85 %: with
 * the old 0.5 % limit the backlog reached 1 s every 4.5 minutes and cray
 * dropped almost a second of sound). */
/* Sound log (journalctl -u cray): each drop, underrun or dry spell, and
 * every 10 minutes the backlog range and the stretch, to see the sound
 * card's clock drifting against the 50 Hz picture. */
static double alog_t0, alog_next;
static int alog_min = 1 << 30, alog_max;

static double alog_time(void)
{
    double t = now_ms();
    if (!alog_t0) alog_t0 = t;
    return (t - alog_t0) / 1000.0;
}

static void audio_pump(void)
{
    if (!pcm) { alen = 0; return; }
    enum { CHUNK = 256 };
    double t = alog_time();
    if (alen < alog_min) alog_min = alen;
    if (alen > alog_max) alog_max = alen;
    if (t >= alog_next) {
        if (alog_next > 0) {
            double err = (double)(alen - atarget) / atarget;
            fprintf(stderr, "audio: %.0f s: backlog %d-%d ms (target %d), stretch %+.3f %%\n",
                    t, alog_min * 1000 / AUDIO_RATE, alog_max * 1000 / AUDIO_RATE,
                    atarget * 1000 / AUDIO_RATE, fmax(-2.0, fmin(2.0, err * 2.0)));
        }
        alog_next = t + 600;
        alog_min = 1 << 30;
        alog_max = 0;
    }
    if (alen > AUDIO_RATE) {                 /* stalled: start over */
        fprintf(stderr, "audio: %.0f s: backlog over 1 s, dropped %d ms of sound\n",
                t, (alen - atarget) * 1000 / AUDIO_RATE);
        ahead = (ahead + alen - atarget) % ABUF;
        alen = atarget;
    }
    if (!aprimed) {
        if (alen < atarget) return;
        aprimed = 1;
    }
    snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);
    if (avail < 0) {
        fprintf(stderr, "audio: %.0f s: sound card underrun (%s), restarting\n",
                t, snd_strerror((int)avail));
        snd_pcm_recover(pcm, (int)avail, 1);
        aprimed = 0;
        return;
    }
    float vol = cfg.volume / 100.0f;
    while (avail >= CHUNK) {
        double err = (double)(alen - atarget) / atarget;
        double ratio = 1.0 + fmax(-0.02, fmin(0.02, err * 0.02));
        int need = (int)(afrac + CHUNK * ratio) + 2;
        if (alen < need) {
            if (alen < CHUNK / 2) {               /* ran dry: wait for a backlog again */
                fprintf(stderr, "audio: %.0f s: sound ran dry, waiting for a backlog\n", t);
                aprimed = 0;
            }
            break;
        }
        int16_t out[CHUNK];
        for (int i = 0; i < CHUNK; i++) {
            double pos = afrac + i * ratio;
            int k = (int)pos;
            double f = pos - k;
            float a = aring[(ahead + k) % ABUF], b = aring[(ahead + k + 1) % ABUF];
            float v = (float)((a + (b - a) * f) * vol);
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            out[i] = (int16_t)(v * 32767);
        }
        double end = afrac + CHUNK * ratio;
        int used = (int)end;
        afrac = end - used;
        ahead = (ahead + used) % ABUF;
        alen -= used;
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, out, CHUNK);
        if (w < 0) {
            fprintf(stderr, "audio: %.0f s: write failed (%s), restarting\n",
                    t, snd_strerror((int)w));
            snd_pcm_recover(pcm, (int)w, 1);
            break;
        }
        avail -= CHUNK;
    }
}

/* ================================================================ lamps */
static int lamp_fd = -1;
static int lamp_line[NLAMPS];            /* lamp bit -> line index in the request, -1 */
static uint32_t lamp_state = 0xFFFFFFFF;

static int open_chip(const char *path, int want_label)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct gpiochip_info info;
    if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) { close(fd); return -1; }
    if (want_label && strncmp(info.label, "pinctrl-", 8)) { close(fd); return -1; }
    fprintf(stderr, "lamps: %s (%s, %u lines)\n", path, info.label, info.lines);
    return fd;
}

static int lamps_open(void)
{
    int chip = -1;
    if (strcmp(cfg.gpiochip, "auto")) {
        chip = open_chip(cfg.gpiochip, 0);
    } else {
        /* the SoC's own GPIO block: pinctrl-bcm2835 (Pi 3), -bcm2711 (Pi 4), -rp1 (Pi 5) */
        char p[64];
        for (int i = 0; i < 16 && chip < 0; i++) {
            snprintf(p, sizeof p, "/dev/gpiochip%d", i);
            chip = open_chip(p, 1);
        }
    }
    if (chip < 0) {
        fprintf(stderr, "lamps: no GPIO chip (lamps off)\n");
        return -1;
    }
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof req);
    int n = 0;
    for (int b = 0; b < NLAMPS; b++) {
        lamp_line[b] = -1;
        if (cfg.lamp_gpio[b] < 0) continue;
        lamp_line[b] = n;
        req.offsets[n++] = cfg.lamp_gpio[b];
    }
    if (!n) { close(chip); return -1; }
    req.num_lines = n;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT |
                       (cfg.lamp_active_low ? GPIO_V2_LINE_FLAG_ACTIVE_LOW : 0);
    snprintf(req.consumer, sizeof req.consumer, "cray lamps");
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        fprintf(stderr, "lamps: cannot claim the GPIO lines: %s (lamps off)\n", strerror(errno));
        close(chip);
        return -1;
    }
    close(chip);
    lamp_fd = req.fd;
    fprintf(stderr, "lamps: %d lines\n", n);
    return 0;
}

static void lamps_set(uint32_t lamps)
{
    if (lamp_fd < 0 || lamps == lamp_state) return;
    struct gpio_v2_line_values v = { 0, 0 };
    for (int b = 0; b < NLAMPS; b++) {
        if (lamp_line[b] < 0) continue;
        v.mask |= 1ull << lamp_line[b];
        if (lamps & (1u << b)) v.bits |= 1ull << lamp_line[b];
    }
    if (ioctl(lamp_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) == 0)
        lamp_state = lamps;
}

/* ================================================================ events */
extern char **environ;

/* Start the event script (e.g. to play a coin sound) and don't wait for
 * it. It runs at normal priority, so it can't delay a frame. */
static void on_event(const char *name)
{
    if (!strcmp(name, "payout")) play_clip(&clip_payout);
    if (!strcmp(name, "coin-in")) play_clip(&clip_coin_in);
    if (!cfg.event_script[0] || access(cfg.event_script, X_OK) != 0)
        return;
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    struct sched_param sp = { .sched_priority = 0 };
    posix_spawnattr_setschedpolicy(&at, SCHED_OTHER);
    posix_spawnattr_setschedparam(&at, &sp);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGINT); sigaddset(&def, SIGTERM); sigaddset(&def, SIGHUP);
    sigaddset(&def, SIGCHLD);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_SETSIGMASK |
                                  POSIX_SPAWN_SETSIGDEF);
    char *argv[] = { cfg.event_script, (char *)name, NULL };
    pid_t pid;
    int err = posix_spawn(&pid, cfg.event_script, NULL, &at, argv, environ);
    if (err)
        fprintf(stderr, "event: cannot run %s: %s\n", cfg.event_script, strerror(err));
    posix_spawnattr_destroy(&at);
}

/* ================================================================ main */
static void on_signal(int s) { quit = 1; }

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void progress(int pct)
{
    if (pct % 20 == 0) fprintf(stderr, "first start: setting up battery RAM %d%%\n", pct);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: cray [-c cray.conf] [setting=value ...]\n"
        "  Reads /etc/cray/cray.conf, then ./cray.conf, then -c FILE, then settings\n"
        "  given on the command line (e.g. mode=1280x720@50 kiosk=1).\n");
}

int main(int argc, char **argv)
{
    config_defaults();
    config_load("/etc/cray/cray.conf", 0);
    config_load("cray.conf", 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            config_load(argv[++i], 1);
        } else if (strchr(argv[i], '=')) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s", argv[i]);
            char *eq = strchr(buf, '=');
            *eq = 0;
            if (config_set(buf, eq + 1)) { fprintf(stderr, "unknown setting %s\n", buf); return 2; }
        } else {
            usage();
            return 2;
        }
    }

    SessionOpts o;
    session_defaults(&o);
    o.rom = cfg.rom[0] ? cfg.rom : NULL;
    o.romdir = cfg.romdir;
    o.nvram = cfg.nvram[0] ? cfg.nvram : NULL;
    o.float_coins = cfg.float_coins;
    o.save_every = cfg.save_every;
    o.auto_refill = cfg.auto_refill;
    o.features = cfg.features;
    o.acrtc_clock = cfg.acrtc_clock;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGCHLD, SIG_IGN);           /* event scripts: no zombies */
    ray_event = on_event;

    if (display_open()) return 1;
    audio_open();
    o.sound = pcm != NULL;
    if (pcm) {
        load_wav(cfg.sound_payout, &clip_payout);
        load_wav(cfg.sound_coin_in, &clip_coin_in);
    }
    lamps_open();
    input_scan();
    control_open();
    if (session_open(&o, progress)) { display_close(); return 1; }

    /* At 50 (or 100) Hz the emulation follows the display: 2 ticks per
     * frame, smooth motion; the sound resampler absorbs the difference.
     * Other rates follow the wall clock. */
    int per_flip = fabs(drm.refresh - 50) < 0.5 ? 2 : fabs(drm.refresh - 100) < 1 ? 1 : 0;
    fprintf(stderr, "running (%s)\n", per_flip ? "locked to the display, 1 frame per vblank"
                    : "display is not 50 Hz: following the wall clock, motion will not be even");
    if (cfg.realtime) {
        /* keep other programs from delaying a frame */
        struct sched_param sp = { .sched_priority = 10 };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            fprintf(stderr, "note: no real-time priority (%s)\n", strerror(errno));
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
            fprintf(stderr, "note: cannot lock memory (%s)\n", strerror(errno));
    }
    double last_report = now_s();
    double start = now_s(), last_scan = start;
    long long done = 0;
    static float audio[AUDIO_RATE / TICK_HZ + 8];

    pthread_t disp;
    if (pthread_create(&disp, NULL, display_main, NULL) != 0) {
        fprintf(stderr, "cannot start the display thread\n");
        return 1;
    }
    while (!quit) {
        input_poll();
        control_poll();
        double e0 = now_ms();
        if (per_flip) {
            /* a vblank went by without a new frame: run its ticks too, so
             * game time stays equal to display time */
            int mv = __atomic_exchange_n(&missed_vblanks, 0, __ATOMIC_ACQ_REL);
            int vb = 1 + (mv < 4 ? mv : 4);
            for (int i = 0; i < per_flip * vb; i++) {
                int n = session_tick(audio);
                audio_push(audio, n);
            }
        } else {
            long long target = (long long)((now_s() - start) * TICK_HZ);
            if (target - done > 20) done = target - 2;   /* too slow: don't catch up */
            for (int i = 0; i < 4 && done < target; i++, done++) {
                int n = session_tick(audio);
                audio_push(audio, n);
            }
        }
        lamps_set(ray_lamps());
        audio_pump();
        emu_ms = now_ms() - e0;
        publish_frame();
        audio_pump();
        if (now_s() - last_report > 600.0) {
            fprintf(stderr, "display: %ld frames, %ld missed\n", frames_total, missed_total);
            last_report = now_s();
        }
        if (now_s() - last_scan > 2.0) {             /* keyboards plugged in later */
            input_scan();
            last_scan = now_s();
        }
    }
    pthread_mutex_lock(&fr_lock);
    display_stop = 1;
    pthread_cond_broadcast(&fr_cond);
    pthread_mutex_unlock(&fr_lock);
    pthread_join(disp, NULL);
    fprintf(stderr, "display: %ld frames, %ld missed\n", frames_total, missed_total);
    fprintf(stderr, "power down: saving battery RAM\n");
    session_shutdown();
    lamps_set(0);
    if (pcm) snd_pcm_close(pcm);
    display_close();
    return 0;
}
