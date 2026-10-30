/* Hitachi HD63484 ACRTC. Port of pyray/hd63484.py (same status model,
 * same command timing, same drawing results).
 *
 * status bits: 0 WFE write FIFO empty, 1 WFR write FIFO ready,
 *              2 RFR read FIFO ready,  3 RFF read FIFO full,
 *              5 CED command end,      7 CER command error
 * irq = status & (CCR & 0xff).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ACRTC_PROF
#include <stdio.h>
#include <time.h>
#endif
#include "ray.h"

#define ADDR_MASK (VRAM_WORDS - 1)

static const int8_t INSTR_LEN[64] = {
    0, 3, 2, 1, 0, 0, -1, 2,
    0, 3, 3, 3, 0, 0, 0, 0,
    0, 1, 2, 2, 0, 0, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5,
    3, 3, 3, 3, 3, 3, -2, -2,
    -2, -2, 2, 4, 5, 5, 7, 7,
    3, 3, 1, 1, 2, 2, 2, 2,
    5, 5, 5, 5, 5, 5, 5, 5,
};

static inline int s16(unsigned v) { return (int16_t)(v & 0xFFFF); }
static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int sgn1(int v) { return v >= 0 ? 1 : -1; }

void acrtc_reset(Acrtc *a)
{
    a->ch = 0;
    a->reg8cnt = 0;
    a->r8latch = 0;
    a->fifo_len = 0;
    a->pend_head = a->pend_len = 0;
    a->busy_until = 0;
    a->rf_head = a->rf_len = 0;
    a->error = 0;
    a->org = 0; a->org_dpd = 0;
    a->rwp = 0; a->rwp_dn = 0;
    a->cl0 = a->cl1 = a->ccmp = a->edg = 0;
    a->mask = 0xFFFF;
    a->ppy = a->pzcy = a->ppx = a->pzcx = 0;
    a->psy = a->psx = a->pey = a->pzy = a->pex = a->pzx = 0;
    a->xmin = a->ymin = a->xmax = a->ymax = 0;
    a->cpx = a->cpy = 0;
    memset(a->pattern, 0, sizeof a->pattern);
    a->raster = 0;
}

static inline int mwr1(const Acrtc *a) { return a->regs[0xCA >> 1] & 0xFFF; }
static inline uint16_t ccr(const Acrtc *a) { return a->regs[1]; }

static void consume(Acrtc *a, uint16_t word);

/* ------------------------------------------------------------ status */
static int busy(Acrtc *a)
{
    if (!a->busy_until)
        return 0;
    uint64_t now = ray_now();
    if (now < a->busy_until)
        return 1;
    a->busy_until = 0;
    /* the command finished: feed waiting words to the drawing processor */
    while (a->pend_len && !a->busy_until) {
        uint16_t w = a->pending[a->pend_head];
        a->pend_head = (a->pend_head + 1) % ACRTC_PENDING;
        a->pend_len--;
        consume(a, w);
        if (a->busy_until && ray_now() >= a->busy_until)
            a->busy_until = 0;
    }
    return a->busy_until != 0;
}

static uint8_t status(Acrtc *a)
{
    int b = busy(a);
    uint8_t s = 0;
    if (!a->pend_len) s |= 0x01;
    if (a->pend_len < 16) s |= 0x02;
    if (!b && !a->pend_len && !a->fifo_len) s |= 0x20;
    if (a->rf_len) s |= 0x04;
    if (a->rf_len >= 8) s |= 0x08;
    if (a->error) s |= 0x80;
    return s;
}

int acrtc_irq(Acrtc *a)
{
    return (status(a) & ccr(a) & 0xFF) != 0;
}

/* ------------------------------------------------------- bus interface */
static void fifo_push(Acrtc *a, uint16_t word)
{
    if (a->pend_len || (a->busy_until && busy(a))) {
        if (a->pend_len < ACRTC_PENDING) {
            a->pending[(a->pend_head + a->pend_len) % ACRTC_PENDING] = word;
            a->pend_len++;
        }
        return;
    }
    consume(a, word);
}

static void reg_written(Acrtc *a, int r)
{
    if (r == 1 && (a->regs[1] & 0x8000)) {
        /* ABT: abort the current command, flush the FIFO */
        a->fifo_len = 0;
        a->pend_len = a->pend_head = 0;
        a->busy_until = 0;
        a->error = 0;
        a->regs[1] &= 0x7FFF;
    }
}

void acrtc_write8(Acrtc *a, int offset, uint8_t data)
{
    if ((offset & 2) == 0) {
        a->ch = data;
        a->reg8cnt = 0;
        return;
    }
    int ch = a->ch;
    int r = (ch >> 1) & 0x7F;
    if (ch == 0) {
        if (a->reg8cnt == 0) {
            a->regs[0] = (a->regs[0] & 0x00FF) | (data << 8);
            a->reg8cnt = 1;
        } else {
            a->regs[0] = (a->regs[0] & 0xFF00) | data;
            a->reg8cnt = 0;
            fifo_push(a, a->regs[0]);
        }
        return;
    }
    if (ch & 1)
        a->regs[r] = (a->regs[r] & 0xFF00) | data;
    else
        a->regs[r] = (a->regs[r] & 0x00FF) | (data << 8);
    if (ch & 0x80)
        a->ch = ((ch + 1) & 0x7F) | 0x80;
    reg_written(a, r);
}

void acrtc_write16(Acrtc *a, int offset, uint16_t data)
{
    if ((offset & 2) == 0) {
        a->ch = data & 0xFF;
        a->reg8cnt = 0;
        return;
    }
    int ch = a->ch;
    int r = (ch >> 1) & 0x7F;
    if (ch & 1) {
        a->regs[r] = (a->regs[r] & 0xFF00) | (data >> 8);
        int r2 = ((ch + 1) >> 1) & 0x7F;
        a->regs[r2] = (a->regs[r2] & 0x00FF) | ((data & 0xFF) << 8);
    } else {
        a->regs[r] = data;
    }
    reg_written(a, r);
    if (a->ch & 0x80)
        a->ch = ((a->ch + 2) & 0x7F) | 0x80;
    if (a->ch == 0)
        fifo_push(a, a->regs[0]);
}

uint16_t acrtc_read16(Acrtc *a, int offset)
{
    if ((offset & 2) == 0) {
        uint8_t s = status(a);
        return (uint16_t)((s << 8) | s);
    }
    a->reg8cnt = 0;
    if (a->ch == 0) {
        if (a->rf_len) {
            uint16_t v = a->readfifo[a->rf_head];
            a->rf_head = (a->rf_head + 1) % ACRTC_READFIFO;
            a->rf_len--;
            return v;
        }
        return 0;
    }
    if (a->ch == 0x80) {
        a->raster = (a->raster + 1) & 0xFFF;
        return a->raster;
    }
    return a->regs[(a->ch >> 1) & 0x7F];
}

uint8_t acrtc_read8(Acrtc *a, int offset)
{
    if ((offset & 2) == 0)
        return status(a);
    int ch = a->ch;
    if (ch == 0) {
        /* two byte reads of the FIFO give the high, then the low byte */
        if (a->reg8cnt == 0) {
            a->r8latch = acrtc_read16(a, offset);
            a->reg8cnt = 1;
            return a->r8latch >> 8;
        }
        a->reg8cnt = 0;
        return a->r8latch & 0xFF;
    }
    uint16_t v;
    if (ch == 0x80)
        v = acrtc_read16(a, offset);
    else
        v = a->regs[(ch >> 1) & 0x7F];
    if (ch & 0x80)
        a->ch = ((ch + 1) & 0x7F) | 0x80;
    return (ch & 1) ? (v & 0xFF) : (v >> 8);
}

/* ------------------------------------------------------ pixel access */
static inline int bpp_shift(const Acrtc *a)   /* log2 of bits per pixel */
{
    int gbm = (ccr(a) >> 8) & 7;
    return gbm <= 4 ? gbm : 2;
}

static inline int first_pixel(const Acrtc *a)
{
    int gbm = (ccr(a) >> 8) & 7;
    if (gbm >= 4)
        return 0;
    return (a->org_dpd & 0xF) >> gbm;
}

static void dot(Acrtc *a, int x, int y, int opm, unsigned color)
{
    a->dots++;
    int bs = bpp_shift(a);
    int bpp = 1 << bs;
    int pshift = 4 - bs;                   /* log2 pixels per word */
    unsigned bitmask = (bpp == 16) ? 0xFFFF : ((1u << bpp) - 1);
    x += first_pixel(a);
    int x_int = x >> pshift;               /* floor division */
    int x_mod = x & ((1 << pshift) - 1);
    int sh = x_mod * bpp;
    color &= bitmask;
    unsigned bm = bitmask << sh;
    unsigned cs = color << sh;
    int32_t dst = (a->org + x_int - y * mwr1(a)) & ADDR_MASK;
    unsigned v = a->vram[dst];
    unsigned nbm = ~bm & 0xFFFF;
    switch (opm) {
    case 0: a->vram[dst] = (uint16_t)((v & nbm) | cs); break;
    case 1: a->vram[dst] = (uint16_t)(v | cs); break;
    case 2: a->vram[dst] = (uint16_t)(v & ((v & nbm) | cs)); break;
    case 3: a->vram[dst] = (uint16_t)(v ^ cs); break;
    default: {
        unsigned cur = (v >> sh) & bitmask;
        int ok;
        if (opm == 4)      ok = cur == (a->ccmp & bitmask);
        else if (opm == 5) ok = cur != (a->ccmp & bitmask);
        else if (opm == 6) ok = cur < (a->cl0 & bitmask);
        else               ok = cur > (a->cl0 & bitmask);
        if (ok)
            a->vram[dst] = (uint16_t)((v & nbm) | cs);
    }
    }
}

static unsigned get_pixel(const Acrtc *a, int x, int y)
{
    int bs = bpp_shift(a);
    int bpp = 1 << bs;
    int pshift = 4 - bs;
    int x_int = x >> pshift;
    int x_mod = x & ((1 << pshift) - 1);
    int32_t dst = (a->org + x_int - y * mwr1(a)) & ADDR_MASK;
    unsigned bitmask = (bpp == 16) ? 0xFFFF : ((1u << bpp) - 1);
    return (a->vram[dst] >> (x_mod * bpp)) & bitmask;
}

/* ------------------------------------------------- word-level commands */
static void doclr16(Acrtc *a, int op, uint16_t fill, int ax0, int ay0)
{
    int mw = mwr1(a);
    int32_t dst = a->rwp;
    int mode = op & 3;
    int ay = ay0;
    for (;;) {
        int ax = ax0;
        for (;;) {
            int32_t ad = dst & ADDR_MASK;
            if (mode == 0)      a->vram[ad] = fill;
            else if (mode == 1) a->vram[ad] |= fill;
            else if (mode == 2) a->vram[ad] &= fill;
            else                a->vram[ad] ^= fill;
            if (ax == 0) break;
            if (ax > 0) { dst++; ax--; }
            else        { dst--; ax++; }
        }
        if (ay0 < 0) {
            dst = dst + mw - ax0;
            if (ay == 0) break;
            ay++;
        } else {
            dst = dst - mw - ax0;
            if (ay == 0) break;
            ay--;
        }
    }
    a->rwp = dst & ADDR_MASK;
}

static void docpy16(Acrtc *a, int op, int32_t src, int ax0, int ay0)
{
    int mw = mwr1(a);
    int32_t dst = a->rwp;
    int d = op & 0x700;
    int s1, s2;
    switch (d) {
    case 0x000: s1 = 1;   s2 = -mw - ax0; break;
    case 0x100: s1 = 1;   s2 = mw - ax0; break;
    case 0x200: s1 = -1;  s2 = -mw - ax0; break;
    case 0x300: s1 = -1;  s2 = mw - ax0; break;
    case 0x400: s1 = -mw; s2 = 1 - ay0 * -mw; break;
    case 0x500: s1 = mw;  s2 = 1 - ay0 * mw; break;
    case 0x600: s1 = -mw; s2 = -1 + ay0 * -mw; break;
    default:    s1 = mw;  s2 = -1 + ay0 * mw; break;
    }
    int mode = op & 7;
    unsigned cc = a->ccmp & 0xFF;
    int ax = ax0, ay = ay0;
    int vert = op & 0x800;
    uint16_t *vram = a->vram;
    for (;;) {
        for (;;) {
            int32_t ad = dst & ADDR_MASK;
            uint16_t sv = vram[src & ADDR_MASK];
            switch (mode) {
            case 0: vram[ad] = sv; break;
            case 1: vram[ad] |= sv; break;
            case 2: vram[ad] &= sv; break;
            case 3: vram[ad] ^= sv; break;
            case 4: if (vram[ad] == cc) vram[ad] = sv; break;
            case 5: if (vram[ad] != cc) vram[ad] = sv; break;
            case 6: if (vram[ad] < sv) vram[ad] = sv; break;
            default: if (vram[ad] > sv) vram[ad] = sv; break;
            }
            if (vert) {
                if (ay == 0) break;
                src += ay0 > 0 ? -mw : mw;
                dst += s1;
                ay += ay0 > 0 ? -1 : 1;
            } else {
                if (ax == 0) break;
                src += ax > 0 ? 1 : -1;
                dst += s1;
                ax += ax > 0 ? -1 : 1;
            }
        }
        if (vert) {
            ay = ay0;
            if (ax0 < 0) {
                src = src - 1 + ay * mw;
                dst += s2;
                if (ax == 0) break;
                ax++;
            } else {
                src = src + 1 - ay * mw;
                dst += s2;
                if (ax == 0) break;
                ax--;
            }
        } else {
            ax = ax0;
            if (ay0 < 0) {
                src = src + mw - ax;
                dst += s2;
                if (ay == 0) break;
                ay++;
            } else {
                src = src - mw - ax;
                dst += s2;
                if (ay == 0) break;
                ay--;
            }
        }
    }
    a->rwp = dst & ADDR_MASK;
}

/* ------------------------------------------------- graphic commands */
static void agcpy(Acrtc *a, int op, int sx, int sy, int dx, int dy, int ax0, int ay0)
{
    int d = op & 0x700;
    int vert = op & 0x800;
    int opm = op & 7;
    int axn = sgn1(ax0), ayn = sgn1(ay0);
    int d1x, d1y, d2x, d2y, ss1x, ss1y, ss2x, ss2y;
    if (vert) {
        switch (d) {
        case 0x000: d1x = 1;  d1y = 0;  d2x = -ayn * ay0; d2y = 1; break;
        case 0x100: d1x = 1;  d1y = 0;  d2x = -ayn * ay0; d2y = -1; break;
        case 0x200: d1x = -1; d1y = 0;  d2x = ayn * ay0;  d2y = 1; break;
        case 0x300: d1x = -1; d1y = 0;  d2x = ayn * ay0;  d2y = -1; break;
        case 0x400: d1x = 0;  d1y = 1;  d2x = 1;  d2y = -ayn * ay0; break;
        case 0x500: d1x = 0;  d1y = -1; d2x = 1;  d2y = ayn * ay0; break;
        case 0x600: d1x = 0;  d1y = 1;  d2x = -1; d2y = -ayn * ay0; break;
        default:    d1x = 0;  d1y = -1; d2x = -1; d2y = ayn * ay0; break;
        }
        ss1x = 0; ss1y = ayn; ss2x = axn; ss2y = -ay0;
    } else {
        switch (d) {
        case 0x000: d1x = 1;  d1y = 0;  d2x = -axn * ax0; d2y = 1; break;
        case 0x100: d1x = 1;  d1y = 0;  d2x = -axn * ax0; d2y = -1; break;
        case 0x200: d1x = -1; d1y = 0;  d2x = axn * ax0;  d2y = 1; break;
        case 0x300: d1x = -1; d1y = 0;  d2x = axn * ax0;  d2y = -1; break;
        case 0x400: d1x = 0;  d1y = 1;  d2x = 1;  d2y = axn * ax0; break;
        case 0x500: d1x = 0;  d1y = -1; d2x = 1;  d2y = -axn * ax0; break;
        case 0x600: d1x = 0;  d1y = 1;  d2x = -1; d2y = axn * ax0; break;
        default:    d1x = 0;  d1y = -1; d2x = -1; d2y = -axn * ax0; break;
        }
        ss1x = axn; ss1y = 0; ss2x = -ax0; ss2y = ayn;
    }
    int ax = ax0, ay = ay0;
    int xs = sx, ys = sy, xd = dx, yd = dy;
    for (;;) {
        for (;;) {
            dot(a, xd, yd, opm, get_pixel(a, xs, ys));
            if (vert) {
                if (ay == 0) break;
                ay += ay0 > 0 ? -1 : 1;
            } else {
                if (ax == 0) break;
                ax += ax > 0 ? -1 : 1;
            }
            xs += ss1x; ys += ss1y; xd += d1x; yd += d1y;
        }
        xs += ss2x; ys += ss2y; xd += d2x; yd += d2y;
        if (vert) {
            ay = ay0;
            if (ax == 0) break;
            ax += ax0 < 0 ? 1 : -1;
        } else {
            ax = ax0;
            if (ay == 0) break;
            ay += ay0 < 0 ? 1 : -1;
        }
    }
}

static void gcpy_move(Acrtc *a, int op, int ax, int ay)
{
    int d = op & 0x700;
    if (d == 0x000 || d == 0x200)      a->cpy += iabs(ay);
    else if (d == 0x100 || d == 0x300) a->cpy -= iabs(ay);
    else if (d == 0x400 || d == 0x500) a->cpx += iabs(ax);
    else                               a->cpx -= iabs(ax);
}

static void ptn(Acrtc *a, int op)
{
    int ax0 = (a->pex - a->psx) - 2;
    int ay0 = a->pey - a->psy;
    if (op & 0x800)
        return;
    int axn = sgn1(ax0);
    int d = op & 0x700;
    int d1x, d1y, d2x, d2y;
    switch (d) {
    case 0x000: d1x = 1;  d1y = 0;  d2x = -axn * ax0; d2y = 1; break;
    case 0x200: d1x = 0;  d1y = 1;  d2x = -1; d2y = -axn * ax0; break;
    case 0x400: d1x = -1; d1y = 0;  d2x = axn * ax0; d2y = -1; break;
    case 0x600: d1x = 0;  d1y = -1; d2x = 1;  d2y = axn * ax0; break;
    default: return;
    }
    int ss1x = 1, ss1y = 0, ss2x = -ax0, ss2y = 1;
    int xs = a->psx, ys = a->psy, xd = a->cpx, yd = a->cpy;
    int ax = ax0, ay = ay0;
    int mode = (op >> 3) & 3;
    int opm = op & 7;
    for (;;) {
        for (;;) {
            int bit = (xs >= 0 && xs < 16) ? (a->pattern[ys & 15] >> (xs & 15)) & 1 : 0;
            if (mode == 0)
                dot(a, xd, yd, opm, bit ? a->cl1 : a->cl0);
            else if (mode == 1) {
                if (bit) dot(a, xd, yd, opm, a->cl1);
            } else if (mode == 2) {
                if (!bit) dot(a, xd, yd, opm, a->cl0);
            }
            if (ax == 0) break;
            ax += ax > 0 ? -1 : 1;
            xs += ss1x; ys += ss1y; xd += d1x; yd += d1y;
        }
        xs += ss2x; ys += ss2y; xd += d2x; yd += d2y;
        ax = ax0;
        if (ay == 0) break;
        ay += ay0 < 0 ? 1 : -1;
    }
    int h = a->pey - a->psy;
    if (d == 0x000)      a->cpy += h > 0 ? h + 1 : -h;
    else if (d == 0x200) a->cpx += h > 0 ? h : -h;
    else if (d == 0x400) a->cpy -= h > 0 ? h : -h;
    else if (d == 0x600) a->cpx -= h > 0 ? h : -h;
}

static void line(Acrtc *a, int x0, int y0, int x1, int y1, int opm, int nolastpix)
{
    unsigned old = get_pixel(a, x1, y1);
    int dy = y1 - y0, dx = x1 - x0, sy = 1, sx = 1;
    if (dy < 0) { dy = -dy; sy = -1; }
    if (dx < 0) { dx = -dx; sx = -1; }
    dy <<= 1;
    dx <<= 1;
    unsigned c = a->cl0;
    dot(a, x0, y0, opm, c);
    if (dx > dy) {
        int frac = dy - (dx >> 1);
        while (x0 != x1) {
            if (frac >= 0) { y0 += sy; frac -= dx; }
            x0 += sx; frac += dy;
            dot(a, x0, y0, opm, c);
        }
    } else {
        int frac = dx - (dy >> 1);
        while (y0 != y1) {
            if (frac >= 0) { x0 += sx; frac -= dy; }
            y0 += sy; frac += dx;
            dot(a, x0, y0, opm, c);
        }
    }
    if (nolastpix)
        dot(a, x1, y1, 0, old);
}

static void rect(Acrtc *a, int x0, int y0, int x1, int y1, int opm)
{
    line(a, x0, y0, x1, y0, opm, 0);
    line(a, x1, y0, x1, y1, opm, 0);
    line(a, x1, y1, x0, y1, opm, 0);
    line(a, x0, y1, x0, y0, opm, 0);
}

static void fill_rect(Acrtc *a, int x0, int y0, int x1, int y1, int opm)
{
    int stx = x1 >= x0 ? 1 : -1;
    int sty = y1 >= y0 ? 1 : -1;
    unsigned c = a->cl0;
    if (opm == 0 && bpp_shift(a) == 2 && first_pixel(a) == 0) {
        /* fast path (does not count dots; the cost doesn't use them) */
        int mw = mwr1(a);
        c &= 15;
        for (int y = y0;; y += sty) {
            int32_t row = a->org - y * mw;
            for (int x = x0;; x += stx) {
                int32_t ad = (row + (x >> 2)) & ADDR_MASK;
                int sh = (x & 3) << 2;
                a->vram[ad] = (uint16_t)((a->vram[ad] & ~(15u << sh)) | (c << sh));
                if (x == x1) break;
            }
            if (y == y1) break;
        }
        return;
    }
    for (int x = x0;; x += stx) {
        for (int y = y0;; y += sty) {
            dot(a, x, y, opm, c);
            if (y == y1) break;
        }
        if (x == x1) break;
    }
}

static void circle(Acrtc *a, int xc, int yc, int r, int opm)
{
    unsigned c = a->cl0;
    dot(a, xc, yc + r, opm, c);
    dot(a, xc, yc - r, opm, c);
    dot(a, xc + r, yc, opm, c);
    dot(a, xc - r, yc, opm, c);
    int x = 1;
    int y = (int)(sqrt((double)(r * r - 1 > 0 ? r * r - 1 : 0)) + 0.5);
    while (x < y) {
        dot(a, xc + x, yc + y, opm, c); dot(a, xc + x, yc - y, opm, c);
        dot(a, xc - x, yc + y, opm, c); dot(a, xc - x, yc - y, opm, c);
        dot(a, xc + y, yc + x, opm, c); dot(a, xc + y, yc - x, opm, c);
        dot(a, xc - y, yc + x, opm, c); dot(a, xc - y, yc - x, opm, c);
        x++;
        int q = r * r - x * x;
        y = (int)(sqrt((double)(q > 0 ? q : 0)) + 0.5);
    }
    if (x == y) {
        dot(a, xc + x, yc + y, opm, c); dot(a, xc + x, yc - y, opm, c);
        dot(a, xc - x, yc + y, opm, c); dot(a, xc - x, yc - y, opm, c);
    }
}

typedef struct { int x, y; } Pt;

static int pt_cmp(const void *p, const void *q)
{
    const Pt *u = p, *v = q;
    return u->x != v->x ? (u->x > v->x) - (u->x < v->x) : (u->y > v->y) - (u->y < v->y);
}

/* ELPS outline: integer points of B*x*x + A*y*y = B*dx*dx around (0, 0),
 * each once; returns the count, *out is malloc'd. A midpoint walk scaled
 * by four, evaluated against the true equation, so the second axis
 * dx*sqrt(B/A) is never rounded to a whole ellipse. Method from Vesuri's
 * RAY Pokeri Amiga port (github.com/vesuri/raypokeri, Hd63484::curve). */
static int ellipse_points(unsigned A, unsigned B, unsigned dxr, Pt **out)
{
    long long a = A, b = B, R = (long long)B * dxr * dxr;
    if (!a || !b) {
        *out = malloc(sizeof(Pt));
        (*out)[0] = (Pt){ 0, 0 };
        return 1;
    }
    /* y axis rounded to the nearest whole pixel; it starts the walk */
    long long qy = 0;
    for (long long bit = 1 << 15; bit; bit >>= 1)
        if (a * (qy | bit) * (qy | bit) <= R) qy |= bit;
    if (4 * R >= a * (2 * qy + 1) * (2 * qy + 1)) qy++;
    int cap = (int)(8 * (dxr + qy) + 64), n = 0;
    Pt *p = malloc(cap * sizeof(Pt));
#define SYM(X, Y) do { \
        if (n + 4 > cap) { cap *= 2; p = realloc(p, cap * sizeof(Pt)); } \
        p[n++] = (Pt){ (int)(X), (int)(Y) };  p[n++] = (Pt){ (int)-(X), (int)(Y) }; \
        p[n++] = (Pt){ (int)(X), (int)-(Y) }; p[n++] = (Pt){ (int)-(X), (int)-(Y) }; \
    } while (0)
    /* flat part: step x, sometimes y; the start includes the residual
     * a*qy*qy - R, since the rounded axis needn't lie on the curve */
    long long qx = 0, dx = 0, dy = 2 * a * qy;
    long long d = 4 * b - 4 * a * qy + a + 4 * (a * qy * qy - R);
    while (dx < dy) {
        SYM(qx, qy); qx++; dx += 2 * b;
        if (d < 0) d += 4 * dx + 4 * b;
        else { qy--; dy -= 2 * a; d += 4 * dx - 4 * dy + 4 * b; }
    }
    /* steep part: step y, sometimes x */
    d = b * (2 * qx + 1) * (2 * qx + 1) + 4 * a * (qy - 1) * (qy - 1) - 4 * R;
    while (qy >= 0) {
        SYM(qx, qy); qy--; dy -= 2 * a;
        if (d > 0) d += 4 * a - 4 * dy;
        else { qx++; dx += 2 * b; d += 4 * dx - 4 * dy + 4 * a; }
    }
#undef SYM
    /* the mirror images repeat points on the axes; keep each once (XOR) */
    qsort(p, n, sizeof *p, pt_cmp);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (!m || p[i].x != p[m - 1].x || p[i].y != p[m - 1].y) p[m++] = p[i];
    *out = p;
    return m;
}

#ifdef ACRTC_TEST
/* Test builds only (-DACRTC_TEST, tools/elpstest.c): a command log hook,
 * and the ELPS outline used before 2026-10-30 (second axis rounded with
 * +0.33, then a whole-number ellipse) for comparison. */
void (*acrtc_test_cmd)(const Acrtc *a, const uint16_t *f, int n);

static int old_ellipse_points(long long ea, long long eb, Pt **out)
{
    if (ea <= 0 || eb <= 0) {
        *out = malloc(sizeof(Pt));
        (*out)[0].x = 0; (*out)[0].y = 0;
        return 1;
    }
    int cap = (int)(4 * (ea + eb) + 16), n = 0;
    Pt *p = malloc(cap * sizeof(Pt));
    long long a2 = ea * ea, b2 = eb * eb;
    long long x = 0, y = eb;
    long long S = a2 * (1 - 2 * eb) + 2 * b2;
    long long T = b2 - 2 * a2 * (2 * eb - 1);
    for (;;) {
        if (n + 4 > cap) { cap *= 2; p = realloc(p, cap * sizeof(Pt)); }
        p[n++] = (Pt){ (int)x, (int)y };
        if (x) p[n++] = (Pt){ (int)-x, (int)y };
        if (y) p[n++] = (Pt){ (int)x, (int)-y };
        if (x && y) p[n++] = (Pt){ (int)-x, (int)-y };
        if (y <= 0)
            break;
        if (S < 0) {
            S += 2 * b2 * (2 * x + 3);
            T += 4 * b2 * (x + 1);
            x++;
        } else if (T < 0) {
            S += 2 * b2 * (2 * x + 3) - 4 * a2 * (y - 1);
            T += 4 * b2 * (x + 1) - 2 * a2 * (2 * y - 3);
            x++;
            y--;
        } else {
            S -= 4 * a2 * (y - 1);
            T -= 2 * a2 * (2 * y - 3);
            y--;
        }
    }
    *out = p;
    return n;
}

int acrtc_elps_old_points(unsigned A, unsigned B, unsigned dxr, Pt **out)
{
    long long ry = A ? (long long)((sqrt((double)B) * dxr) / sqrt((double)A) + 0.33) : 0;
    int n = old_ellipse_points(dxr, ry, out);
    qsort(*out, n, sizeof **out, pt_cmp);
    return n;
}

int acrtc_elps_points(unsigned A, unsigned B, unsigned dxr, Pt **out)
{
    return ellipse_points(A, B, dxr, out);
}
#endif

static double pymod(double v, double m)
{
    double r = fmod(v, m);
    if (r < 0) r += m;
    return r;
}

/* ELPS: full ellipse B*x*x + A*y*y = B*dx*dx around (cx, cy) */
static void ellipse(Acrtc *a, int cx, int cy, unsigned A, unsigned B, unsigned dx, int opm)
{
    Pt *pts;
    int n = ellipse_points(A, B, dx, &pts);
    for (int i = 0; i < n; i++)
        dot(a, cx + pts[i].x, cy + pts[i].y, opm, a->cl0);
    free(pts);
}

/* ---- arcs, port of pyray's _trace_arc ---- */
typedef struct { double off; int x, y; } ArcPt;

static int arcpt_cmp(const void *p, const void *q)
{
    const ArcPt *u = p, *v = q;
    if (u->off != v->off) return u->off < v->off ? -1 : 1;
    if (u->x != v->x) return u->x < v->x ? -1 : 1;
    return (u->y > v->y) - (u->y < v->y);
}

typedef struct { Pt *p; int n, cap; } PtList;

static void pl_add(PtList *l, int x, int y)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->p = realloc(l->p, l->cap * sizeof(Pt));
    }
    l->p[l->n++] = (Pt){ x, y };
}

/* add (x, y) to the ring unless it's there already */
static void ring_add(PtList *l, int x, int y)
{
    for (int i = 0; i < l->n; i++)
        if (l->p[i].x == x && l->p[i].y == y) return;
    pl_add(l, x, y);
}

static int iabs2(int v) { return v < 0 ? -v : v; }

/* Arc of the ellipse with semi-axes rx, ry (not rounded) around (cx, cy),
 * from the start point (sdx, sdy) to the end point (edx, edy), both
 * relative to the centre. Like the chip it walks the curve from the start
 * pixel, so the arc joins the lines drawn before and after it and a
 * following PAINT stays inside the outline. The pixel path is kept
 * 8-connected and one pixel thin. Y grows upwards, so clockwise means
 * decreasing angle; end == start is a full ellipse. */
static void trace_arc(Acrtc *a, int cx, int cy, double rx, double ry,
                      int sdx, int sdy, int edx, int edy, int cw, int opm)
{
    if (rx <= 0 || ry <= 0) {
        dot(a, cx + sdx, cy + sdy, opm, a->cl0);
        return;
    }
    const double two_pi = 2 * M_PI;
    double t0 = atan2(sdy / ry, sdx / rx);
    int full = sdx == edx && sdy == edy;
    double sweep;
    if (full) {
        sweep = two_pi;
    } else {
        double t1 = atan2(edy / ry, edx / rx);
        sweep = pymod(cw ? (t0 - t1) : (t1 - t0), two_pi);
        /* When the end point is off the curve, end at the curve point
         * nearest to it (searched within 90 degrees of its direction). The
         * card fonts are drawn that way. */
        int sgn = cw ? -1 : 1;
        int have = 0;
        double best_d = 0, best_s = sweep;
        for (int k = -90; k <= 90; k++) {
            double s = sweep + k * (M_PI / 180.0);
            if (!(0 <= s && s <= two_pi))
                continue;
            double t = t0 + sgn * s;
            double ex = rx * cos(t) - edx, ey = ry * sin(t) - edy;
            double d = ex * ex + ey * ey;
            if (!have || d < best_d - 1e-9) {
                have = 1;
                best_d = d;
                best_s = s;
            }
        }
        sweep = best_s;
    }
    /* pixels nearest the true curve: one per column where the curve is
     * flat, one per row where it is steep */
    PtList ring = { 0 };
    double dd = sqrt(rx * rx + ry * ry);
    double xb = rx * rx / dd, yb = ry * ry / dd;
    int fxb = (int)floor(xb), fyb = (int)floor(yb);
    for (int x = -fxb; x <= fxb; x++) {
        double q = 1 - (double)x * x / (rx * rx);
        int y = (int)floor(ry * sqrt(q > 0 ? q : 0.0) + 0.5);
        ring_add(&ring, x, y);
        ring_add(&ring, x, -y);
    }
    for (int y = -fyb; y <= fyb; y++) {
        double q = 1 - (double)y * y / (ry * ry);
        int x = (int)floor(rx * sqrt(q > 0 ? q : 0.0) + 0.5);
        ring_add(&ring, x, y);
        ring_add(&ring, -x, y);
    }
    /* keep the ones from the start to the end, in drawing order */
    ArcPt *sel = malloc((ring.n + 1) * sizeof(ArcPt));
    int ns = 0;
    for (int i = 0; i < ring.n; i++) {
        int x = ring.p[i].x, y = ring.p[i].y;
        double t = atan2(y / ry, x / rx);
        double off = pymod(cw ? (t0 - t) : (t - t0), two_pi);
        if (full || off <= sweep + 1e-9)
            sel[ns++] = (ArcPt){ off, x, y };
    }
    qsort(sel, ns, sizeof sel[0], arcpt_cmp);
    PtList pts = { 0 };
    pl_add(&pts, sdx, sdy);
    for (int i = 0; i < ns; i++) {
        int x = sel[i].x, y = sel[i].y;
        Pt *last = &pts.p[pts.n - 1];
        if (x == last->x && y == last->y)
            continue;
        if (pts.n >= 2 && x == pts.p[pts.n - 2].x && y == pts.p[pts.n - 2].y)
            continue;
        if (iabs2(x - last->x) > 1 || iabs2(y - last->y) > 1) {
            /* bridge a gap (only near the start point) */
            int px = last->x, py = last->y;
            while (iabs2(x - px) > 1 || iabs2(y - py) > 1) {
                px += (x > px) - (x < px);
                py += (y > py) - (y < py);
                pl_add(&pts, px, py);
            }
        }
        pl_add(&pts, x, y);
    }
    if (full && pts.n > 1 && pts.p[pts.n - 1].x == pts.p[0].x && pts.p[pts.n - 1].y == pts.p[0].y)
        pts.n--;
    /* The arc ends at the end angle; the chip doesn't draw out to the end
     * point (manual, RARC: "a dot is not drawn at Pe"; only CP moves there).
     * Drop corner pixels: a pixel whose neighbours already touch. */
    PtList thin = { 0 };
    pl_add(&thin, pts.p[0].x, pts.p[0].y);
    for (int i = 1; i < pts.n - 1; i++) {
        Pt pv = thin.p[thin.n - 1], nx = pts.p[i + 1];
        if (iabs2(nx.x - pv.x) <= 1 && iabs2(nx.y - pv.y) <= 1)
            continue;
        pl_add(&thin, pts.p[i].x, pts.p[i].y);
    }
    if (pts.n > 1)
        pl_add(&thin, pts.p[pts.n - 1].x, pts.p[pts.n - 1].y);
    for (int i = 0; i < thin.n; i++)
        dot(a, cx + thin.p[i].x, cy + thin.p[i].y, opm, a->cl0);
    free(sel);
    free(ring.p);
    free(pts.p);
    free(thin.p);
}

static void rarc(Acrtc *a, int cpx, int cpy, int dxc, int dyc, int dxe, int dye, int cw, int opm)
{
    double r = sqrt((double)dxc * dxc + (double)dyc * dyc);
    trace_arc(a, cpx + dxc, cpy + dyc, r, r, -dxc, -dyc, dxe - dxc, dye - dyc, cw, opm);
}

static void rearc(Acrtc *a, int cpx, int cpy, unsigned ea, unsigned eb, int dxc, int dyc,
                  int dxe, int dye, int cw, int opm)
{
    if (!ea || !eb)
        return;
    double R = sqrt((double)dxc * dxc / ea + (double)dyc * dyc / eb);
    trace_arc(a, cpx + dxc, cpy + dyc, sqrt((double)ea) * R, sqrt((double)eb) * R,
              -dxc, -dyc, dxe - dxc, dye - dyc, cw, opm);
}

/* Flood fill bounded by the edge colour. The "seen" set is a hash table
 * allocated once; a generation number marks which entries belong to the
 * current fill, so nothing needs clearing between fills. */
#define SEEN_BITS 19                     /* 512k slots for at most 200k pixels */
typedef struct { uint64_t key; uint32_t gen; } SeenSlot;
static SeenSlot *seen;
static uint32_t seen_gen;
static Pt *pstack;
static size_t pstack_cap;

/* returns 1 if (x, y) was already seen; with add, marks it */
static int seen_test(int x, int y, int add)
{
    uint64_t k = ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
    size_t mask = ((size_t)1 << SEEN_BITS) - 1;
    size_t h = (size_t)((k * 0x9E3779B97F4A7C15ull) >> (64 - SEEN_BITS));
    while (seen[h].gen == seen_gen) {
        if (seen[h].key == k) return 1;
        h = (h + 1) & mask;
    }
    if (add) {
        seen[h].key = k;
        seen[h].gen = seen_gen;
    }
    return 0;
}

static void paint(Acrtc *a, int sx, int sy, unsigned col)
{
    int bs = bpp_shift(a);
    unsigned rep = bs == 2 ? 0x1111 : bs == 3 ? 0x0101 : 1;
    unsigned edg = a->edg;
    if (!seen) {
        seen = calloc((size_t)1 << SEEN_BITS, sizeof(SeenSlot));
        pstack_cap = 1 << 16;
        pstack = malloc(pstack_cap * sizeof(Pt));
    }
    if (++seen_gen == 0) {              /* wrapped: really clear once */
        memset(seen, 0, ((size_t)1 << SEEN_BITS) * sizeof(SeenSlot));
        seen_gen = 1;
    }
    size_t sp = 0;
    int n = 0;
    pstack[sp++] = (Pt){ sx, sy };
    static const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    while (sp && n < 200000) {
        Pt p = pstack[--sp];
        if (seen_test(p.x, p.y, 1))
            continue;
        n++;
        dot(a, p.x, p.y, 0, col);
        for (int i = 0; i < 4; i++) {
            int nx = p.x + nb[i][0], ny = p.y + nb[i][1];
            if (seen_test(nx, ny, 0))
                continue;
            unsigned pv = get_pixel(a, nx, ny) * rep;
            if (pv != col && pv != edg) {
                if (sp == pstack_cap) {
                    pstack_cap *= 2;
                    pstack = realloc(pstack, pstack_cap * sizeof(Pt));
                }
                pstack[sp++] = (Pt){ nx, ny };
            }
        }
    }
}

/* ------------------------------------------------------------ execute */
static void wpr(Acrtc *a, int r, uint16_t v)
{
    switch (r) {
    case 0: a->cl0 = v; break;
    case 1: a->cl1 = v; break;
    case 2: a->ccmp = v; break;
    case 3: a->edg = v; break;
    case 4: a->mask = v; break;
    case 5: a->ppy = (v >> 12) & 15; a->pzcy = (v >> 8) & 15;
            a->ppx = (v >> 4) & 15; a->pzcx = v & 15; break;
    case 6: a->psy = (v >> 12) & 15; a->psx = (v >> 4) & 15; break;
    case 7: a->pey = (v >> 12) & 15; a->pzy = (v >> 8) & 15;
            a->pex = (v >> 4) & 15; a->pzx = v & 15; break;
    case 8: a->xmin = v; break;
    case 9: a->ymin = v; break;
    case 10: a->xmax = v; break;
    case 11: a->ymax = v; break;
    case 12: a->rwp = ((a->rwp & 0xFFF) | ((v & 0xFF) << 12)) & ADDR_MASK;
             a->rwp_dn = (v & 0xC000) >> 14; break;
    case 13: a->rwp = ((a->rwp & 0xFF000) | ((v & 0xFFF0) >> 4)) & ADDR_MASK; break;
    default: break;
    }
}

static void execute(Acrtc *a, const uint16_t *f)
{
    int op = f[0];
    if (op == 0x0400) {                                  /* ORG */
        a->org = (((f[1] & 0xFF) << 12) | ((f[2] & 0xFFF0) >> 4)) & ADDR_MASK;
        a->org_dpd = f[2] & 0xF;
    } else if ((op & 0xFFE0) == 0x0800) {                /* WPR */
        wpr(a, op & 0x1F, f[1]);
    } else if ((op & 0xFFF0) == 0x1800) {                /* WPTN */
        int start = op & 0xF, n = f[1] < 16 ? f[1] : 16;
        for (int i = 0; i < n; i++)
            a->pattern[(start + i) & 15] = f[2 + i];
    } else if (op == 0x4400) {                           /* RD */
        if (a->rf_len < ACRTC_READFIFO) {
            a->readfifo[(a->rf_head + a->rf_len) % ACRTC_READFIFO] = a->vram[a->rwp];
            a->rf_len++;
        }
        a->rwp = (a->rwp + 1) & ADDR_MASK;
    } else if (op == 0x4800) {                           /* WT */
        a->vram[a->rwp] = f[1];
        a->rwp = (a->rwp + 1) & ADDR_MASK;
    } else if (op == 0x5800 || (op & 0xFFFC) == 0x5C00) { /* CLR / SCLR */
        doclr16(a, op, f[1], s16(f[2]), s16(f[3]));
    } else if ((op & 0xF0FF) == 0x6000 || (op & 0xF0FC) == 0x7000) {  /* CPY / SCPY */
        int32_t src = ((f[1] & 0xFF) << 12) | ((f[2] & 0xFFF0) >> 4);
        docpy16(a, op, src, s16(f[3]), s16(f[4]));
    } else if (op == 0x8000) {                           /* AMOVE */
        a->cpx = s16(f[1]); a->cpy = s16(f[2]);
    } else if (op == 0x8400) {                           /* RMOVE */
        a->cpx += s16(f[1]); a->cpy += s16(f[2]);
    } else if ((op & 0xFF00) == 0x8800) {                /* ALINE */
        line(a, a->cpx, a->cpy, s16(f[1]), s16(f[2]), op & 7, 1);
        a->cpx = s16(f[1]); a->cpy = s16(f[2]);
    } else if ((op & 0xFF00) == 0x8C00) {                /* RLINE */
        int ex = a->cpx + s16(f[1]), ey = a->cpy + s16(f[2]);
        line(a, a->cpx, a->cpy, ex, ey, op & 7, 1);
        a->cpx = ex; a->cpy = ey;
    } else if ((op & 0xFFF8) == 0x9000) {                /* ARCT */
        int x1 = s16(f[1]), y1 = s16(f[2]);
        rect(a, a->cpx, a->cpy, x1, y1, op & 7);
        a->cpx = x1; a->cpy = y1;
    } else if ((op & 0xFFF8) == 0x9400) {                /* RRCT */
        int x1 = a->cpx + s16(f[1]), y1 = a->cpy + s16(f[2]);
        rect(a, a->cpx, a->cpy, x1, y1, op & 7);
        a->cpx = x1; a->cpy = y1;
    } else if ((op & 0xFFF8) == 0x9800 || (op & 0xFFF8) == 0x9C00) {  /* APLL / RPLL */
        int rel = (op & 0xFFF8) == 0x9C00;
        int sx = a->cpx, sy = a->cpy;
        for (int n = 0; n < f[1]; n++) {
            int ex = s16(f[2 + n * 2]), ey = s16(f[3 + n * 2]);
            if (rel) { ex += sx; ey += sy; }
            line(a, sx, sy, ex, ey, op & 7, 1);
            sx = ex; sy = ey;
        }
        a->cpx = sx; a->cpy = sy;
    } else if ((op & 0xFFF8) == 0xA000 || (op & 0xFFF8) == 0xA400) {  /* APLG / RPLG */
        int rel = (op & 0xFFF8) == 0xA400;
        int sx = a->cpx, sy = a->cpy;
        for (int n = 0; n < f[1]; n++) {
            int ex = s16(f[2 + n * 2]), ey = s16(f[3 + n * 2]);
            if (rel) { ex += sx; ey += sy; }
            line(a, sx, sy, ex, ey, op & 7, 0);
            sx = ex; sy = ey;
        }
        line(a, sx, sy, a->cpx, a->cpy, op & 7, 0);
    } else if ((op & 0xFE00) == 0xA800) {                /* CRCL */
        circle(a, a->cpx, a->cpy, f[1] & 0x1FFF, op & 7);
    } else if ((op & 0xFE00) == 0xAC00) {                /* ELPS */
        ellipse(a, a->cpx, a->cpy, f[1], f[2], f[3], op & 7);
    } else if ((op & 0xFE00) == 0xB400) {                /* RARC */
        rarc(a, a->cpx, a->cpy, s16(f[1]), s16(f[2]), s16(f[3]), s16(f[4]),
             (op & 0x100) ? 1 : 0, op & 7);
        a->cpx += s16(f[3]); a->cpy += s16(f[4]);
    } else if ((op & 0xFE00) == 0xBC00) {                /* REARC */
        rearc(a, a->cpx, a->cpy, f[1], f[2], s16(f[3]), s16(f[4]), s16(f[5]), s16(f[6]),
              (op & 0x100) ? 1 : 0, op & 7);
        a->cpx += s16(f[5]); a->cpy += s16(f[6]);
    } else if ((op & 0xFFF8) == 0xC000) {                /* AFRCT */
        fill_rect(a, a->cpx, a->cpy, s16(f[1]), s16(f[2]), op & 7);
    } else if ((op & 0xFFF8) == 0xC400) {                /* RFRCT */
        int dx = s16(f[1]), dy = s16(f[2]);
        fill_rect(a, a->cpx, a->cpy, a->cpx + dx, a->cpy + dy, op & 7);
        a->cpy += dy < 0 ? dy - 1 : dy + 1;
    } else if (op == 0xC800) {                           /* PAINT */
        paint(a, a->cpx, a->cpy, a->cl0);
    } else if ((op & 0xFFF8) == 0xCC00) {                /* DOT */
        dot(a, a->cpx, a->cpy, op & 7, a->cl0);
    } else if ((op & 0xF000) == 0xD000) {                /* PTN */
        ptn(a, op);
    } else if ((op & 0xF018) == 0xE000) {                /* AGCPY */
        agcpy(a, op, s16(f[1]), s16(f[2]), a->cpx, a->cpy, s16(f[3]), s16(f[4]));
        gcpy_move(a, op, s16(f[3]), s16(f[4]));
    } else if ((op & 0xF018) == 0xF000) {                /* RGCPY */
        agcpy(a, op, a->cpx + s16(f[1]), a->cpy + s16(f[2]), a->cpx, a->cpy,
              s16(f[3]), s16(f[4]));
        gcpy_move(a, op, s16(f[3]), s16(f[4]));
    }
}

/* Execution time in 2CLK cycles (HD63484 user's manual p. 173).
 * dots = pixels the command drew. cpx/cpy are the values after execute,
 * as in pyray. */
static long cost(const Acrtc *a, const uint16_t *f, int n, long dots)
{
    int op = f[0];
    int P = (op & 4) ? 6 : 4;
    int top = op >> 12;
    if (top < 4) {
        if ((op & 0xFFF0) == 0x1800)
            return 4L * f[1] + 8;                       /* WPTN */
        return 8;
    }
    if (top < 8) {
        if (op == 0x4400 || op == 0x4800 || (op & 0xFFFC) == 0x4C00)
            return 12;
        long x = iabs(s16(f[n - 2])) + 1, y = iabs(s16(f[n - 1])) + 1;
        if (op == 0x5800)
            return (2 * x + 8) * y + 12;                /* CLR */
        if ((op & 0xFFFC) == 0x5C00)
            return (4 * x + 6) * y + 12;                /* SCLR */
        return (6 * x + 10) * y + 12;                   /* CPY / SCPY */
    }
    if ((op & 0xF800) == 0x8000)
        return 56;
    if ((op & 0xF800) == 0x8800 || (op & 0xF800) == 0x9000)
        return P * dots + 54;
    if ((op & 0xF000) == 0x9000 || (op & 0xF800) == 0xA000)
        return P * dots + 16L * f[1] + 20;
    if ((op & 0xFE00) == 0xA800 || (op & 0xFE00) == 0xB000 || (op & 0xFE00) == 0xB400)
        return 8 * dots + 66;
    if ((op & 0xF800) == 0xA800 || (op & 0xF800) == 0xB800)
        return 10 * dots + 96;
    if ((op & 0xFFF0) == 0xC000 || (op & 0xFFF0) == 0xC400) {
        long ra, rb;
        if ((op & 0xFFF8) == 0xC400) {
            ra = iabs(s16(f[1])) + 1;
            rb = iabs(s16(f[2])) + 1;
        } else {
            ra = iabs(s16(f[1]) - a->cpx) + 1;
            rb = iabs(s16(f[2]) - a->cpy) + 1;
        }
        return (P * ra + 8) * rb + 18;
    }
    if ((op & 0xFE00) == 0xC800)
        return 20 * dots + 100;
    if ((op & 0xFFF8) == 0xCC00)
        return 8;
    if ((op & 0xF000) == 0xD000)
        return P * dots + 10L * (((f[1] >> 8) & 15) + 1) + 20;
    if (top >= 14) {
        long ra = iabs(s16(f[3])) + 1, rb = iabs(s16(f[4])) + 1;
        return ((P + 2) * ra + 10) * rb + 70;
    }
    return 20;
}

static void consume(Acrtc *a, uint16_t word)
{
    if (a->fifo_len < ACRTC_FIFO_MAX)
        a->fifo[a->fifo_len++] = word;
    uint16_t *f = a->fifo;
    int ln = INSTR_LEN[f[0] >> 10];
    if (ln == -1) {
        if (a->fifo_len < 2) return;
        ln = f[1] + 2;
    } else if (ln == -2) {
        if (a->fifo_len < 2) return;
        ln = 2 * f[1] + 2;
    }
    if (ln == 0) {
        a->error = 1;
        a->fifo_len = 0;
        return;
    }
    if (a->fifo_len >= ln || a->fifo_len >= ACRTC_FIFO_MAX) {
        int n = a->fifo_len;
        a->fifo_len = 0;
#ifdef ACRTC_TEST
        if (acrtc_test_cmd)
            acrtc_test_cmd(a, f, n);
#endif
#ifdef ACRTC_PROF
        struct timespec pt0, pt1;
        clock_gettime(CLOCK_MONOTONIC, &pt0);
#endif
        if (a->cpu_per_cycle > 0) {
            uint64_t d0 = a->dots;
            execute(a, f);
            long c = cost(a, f, n, (long)(a->dots - d0));
            a->busy_until = ray_now() + (uint64_t)(c * a->cpu_per_cycle) + 1;
        } else {
            execute(a, f);
        }
#ifdef ACRTC_PROF
        clock_gettime(CLOCK_MONOTONIC, &pt1);
        double ms = (pt1.tv_sec - pt0.tv_sec) * 1e3 + (pt1.tv_nsec - pt0.tv_nsec) / 1e6;
        if (ms > 1.0)
            fprintf(stderr, "acrtc %04X %04X %04X %04X %04X took %.2f ms\n", f[0], f[1], f[2],
                    n > 3 ? f[3] : 0, n > 4 ? f[4] : 0, ms);
#endif
    }
}
