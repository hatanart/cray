/* Screen composition: HD63484 display registers + VRAM -> 608x292 index
 * image. Port of pyray/display.py.
 *   R8C/R8A/R8E   heights of upper/base/lower screen
 *   RC2/RCA/RD2   memory width, RC4-6/RCC-E/RD4-6 start address
 *   R90-R96       window position and size, RDA-RDE window memory
 *   DCR (R06)     bits 15-8 enable the screens, bits 5-0 colour attenuation
 */
#include <string.h>
#include "ray.h"

#define MASK (a->addr_mask)

static const uint8_t PALETTE[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0x2D, 0x2D, 0x2D }, { 0xAA, 0x00, 0x00 }, { 0xFF, 0x00, 0x00 },
    { 0x00, 0xAA, 0x00 }, { 0x00, 0xFF, 0x00 }, { 0xAA, 0xAA, 0x00 }, { 0xFF, 0xFF, 0x00 },
    { 0x00, 0x00, 0xAA }, { 0x00, 0x00, 0xFF }, { 0xAA, 0x00, 0xAA }, { 0xFF, 0x00, 0xFF },
    { 0x00, 0xAA, 0xAA }, { 0x00, 0xFF, 0xFF }, { 0xAA, 0xAA, 0xAA }, { 0xFF, 0xFF, 0xFF },
};

void video_palette(uint16_t dcr, uint32_t pal[16])
{
    video_palette_ex(dcr, 0, pal);
}

/* bright = 1: when the game dims all three colours (the last step of the
 * attract-mode saver, made to spare a CRT), take out the part they have
 * in common, so the screen stays as bright as in play and keeps only the
 * tint (light blue). A step that leaves a colour undimmed (the yellow
 * tint before it) doesn't change. */
void video_palette_ex(uint16_t dcr, int bright, uint32_t pal[16])
{
    int attr = (dcr & 0x3F) ^ 0xFF;
    int dim[3] = { 1, 1, 1 };
    if (attr & 0x01) dim[0] += 2;
    if (attr & 0x02) dim[1] += 2;
    if (attr & 0x04) dim[2] += 2;
    if (attr & 0x08) dim[0] += 3;
    if (attr & 0x10) dim[1] += 3;
    if (attr & 0x20) dim[2] += 3;
    int common = 1;
    if (bright) {
        common = dim[0] < dim[1] ? dim[0] : dim[1];
        if (dim[2] < common) common = dim[2];
    }
    for (int i = 0; i < 16; i++) {
        uint32_t r = PALETTE[i][0] * common / dim[0], g = PALETTE[i][1] * common / dim[1],
                 b = PALETTE[i][2] * common / dim[2];
        pal[i] = (r << 16) | (g << 8) | b;
    }
}

static uint32_t sar(const Acrtc *a, int base)
{
    return ((uint32_t)(acrtc_reg(a, base) & 0x0F) << 16) | acrtc_reg(a, base + 2);
}

static inline void unpack(uint16_t w, uint8_t *p)
{
    p[0] = w & 15;
    p[1] = (w >> 4) & 15;
    p[2] = (w >> 8) & 15;
    p[3] = (w >> 12) & 15;
}

void video_render(const Acrtc *a, uint8_t *img)
{
    static const int screens[3][4] = {
        { 0x8C, 0xC2, 0xC4, 0x3000 },      /* upper */
        { 0x8A, 0xCA, 0xCC, 0xC000 },      /* base */
        { 0x8E, 0xD2, 0xD4, 0x0C00 },      /* lower */
    };
    memset(img, 0, SCREEN_W * SCREEN_H);
    uint16_t dcr = acrtc_reg(a, 0x06);
    int y = 0;
    for (int s = 0; s < 3; s++) {
        int h = acrtc_reg(a, screens[s][0]) & 0xFFF;
        int mw = acrtc_reg(a, screens[s][1]) & 0xFFF;
        if (!mw) mw = 152;
        if (h > SCREEN_H - y) h = SCREEN_H - y;
        if (h <= 0)
            continue;
        int en = screens[s][3];
        if ((dcr & en) == en) {
            uint32_t start = sar(a, screens[s][2]);
            int ww = mw < SCREEN_W / 4 ? mw : SCREEN_W / 4;
            for (int r = 0; r < h; r++) {
                uint8_t *row = img + (y + r) * SCREEN_W;
                uint32_t base = start + (uint32_t)r * mw;
                for (int c = 0; c < ww; c++)
                    unpack(a->vram[(base + c) & MASK], row + c * 4);
            }
        }
        y += h;
    }

    /* window */
    if ((dcr & 0x0300) == 0x0300) {
        uint16_t r92 = acrtc_reg(a, 0x92);
        int wx = (r92 >> 8) * 8 - ((acrtc_reg(a, 0x84) >> 8) + 3) * 4;
        int wy = (acrtc_reg(a, 0x94) & 0xFFF) - (acrtc_reg(a, 0x88) >> 8);
        int ww = (r92 & 0xFF) * 2 + 2;
        int wh = acrtc_reg(a, 0x96) & 0xFF;
        int mw = acrtc_reg(a, 0xDA) & 0xFFF;
        if (!mw) mw = 152;
        uint32_t start = sar(a, 0xDC) + 2;
        if (wh > 0 && ww > 0) {
            int x0 = wx > 0 ? wx : 0, y0 = wy > 0 ? wy : 0;
            int x1 = wx + ww * 4 < SCREEN_W ? wx + ww * 4 : SCREEN_W;
            int y1 = wy + wh < SCREEN_H ? wy + wh : SCREEN_H;
            for (int yy = y0; yy < y1; yy++) {
                uint32_t base = start + (uint32_t)(yy - wy) * mw;
                uint8_t *row = img + yy * SCREEN_W;
                for (int xx = x0; xx < x1; xx++) {
                    int px = xx - wx;
                    uint16_t w = a->vram[(base + (px >> 2)) & MASK];
                    row[xx] = (w >> ((px & 3) * 4)) & 15;
                }
            }
        }
    }
}
