/* AY-3-8910 sound, port of pyray/ay8910.py. Register writes are stamped
 * with their CPU cycle in the tick; ay_render() splits the tick at every
 * write so note changes and envelope restarts land where they happened. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ray.h"

#define NOISE_LEN ((1 << 17) - 1)

static const double LEVELS[16] = {
    0.0, 0.0137, 0.0205, 0.0291, 0.0423, 0.0618, 0.0847, 0.1369,
    0.1691, 0.2647, 0.3527, 0.4499, 0.5704, 0.6873, 0.8482, 1.0 };

static uint8_t *noise_table;

void ay_init(Ay *ay, double clock)
{
    if (!noise_table) {
        /* 17-bit LFSR (taps 0 and 3), one entry per noise step */
        noise_table = malloc(NOISE_LEN);
        unsigned r = 1;
        for (int i = 0; i < NOISE_LEN; i++) {
            noise_table[i] = r & 1;
            unsigned bit = (r ^ (r >> 3)) & 1;
            r = (r >> 1) | (bit << 16);
        }
    }
    ay->clock = clock;
    ay_reset(ay);
}

void ay_reset(Ay *ay)
{
    memset(ay->regs, 0, sizeof ay->regs);
    ay->tone_phase[0] = ay->tone_phase[1] = ay->tone_phase[2] = 0;
    ay->noise_pos = ay->env_pos = ay->carry = ay->dc = 0;
    ay->nev = 0;
}

void ay_write(Ay *ay, int reg, int val, uint32_t cycle_offset)
{
    if (ay->nev < AY_EVENTS) {
        ay->ev[ay->nev].cyc = cycle_offset;
        ay->ev[ay->nev].reg = reg & 15;
        ay->ev[ay->nev].val = val & 0xFF;
        ay->nev++;
    }
}

static int envelope(long long e, int shape)
{
    long long cyc = e / 16;
    int s = (int)(e % 16);
    int att = shape & 4, cont = shape & 8, alt = shape & 2, hold = shape & 1;
    int first = att ? s : 15 - s;
    if (cyc == 0)
        return first;
    if (!cont)
        return 0;
    if (hold) {
        int end = att ? 15 : 0;
        return alt ? 15 - end : end;
    }
    if (alt) {
        int up = ((cyc % 2) == 0) == !!att;
        return up ? s : 15 - s;
    }
    return first;
}

static void segment(Ay *ay, int n, double *out)
{
    if (n <= 0)
        return;
    const uint8_t *r = ay->regs;
    double rate = AUDIO_RATE;
    int mixer = r[7];
    int np = (r[6] & 0x1F) ? (r[6] & 0x1F) : 1;
    double nstep = ay->clock / (16.0 * np * rate);
    int ep = (r[11] | (r[12] << 8)) ? (r[11] | (r[12] << 8)) : 1;
    double estep = ay->clock / (16.0 * ep * rate);
    double step[3];
    int tone_ok[3];
    for (int ch = 0; ch < 3; ch++) {
        int tp = r[ch * 2] | ((r[ch * 2 + 1] & 0x0F) << 8);
        if (!tp) tp = 1;
        step[ch] = ay->clock / (16.0 * tp * rate);
        tone_ok[ch] = step[ch] <= 0.5;      /* above Nyquist the square averages out */
    }
    double npos = 0, epos = 0, ph[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        double t = i + 1;
        npos = ay->noise_pos + t * nstep;
        int noise = noise_table[(long long)npos % NOISE_LEN];
        epos = ay->env_pos + t * estep;
        int env = envelope((long long)epos, r[13]);
        double acc = 0;
        for (int ch = 0; ch < 3; ch++) {
            ph[ch] = ay->tone_phase[ch] + t * step[ch];
            int tone = tone_ok[ch] ? (ph[ch] - floor(ph[ch])) < 0.5 : 1;
            int tone_on = tone || (mixer & (1 << ch));
            int noise_on = noise || (mixer & (8 << ch));
            if (tone_on && noise_on) {
                int vol = r[8 + ch];
                acc += (vol & 0x10) ? LEVELS[env] : LEVELS[vol & 15];
            }
        }
        out[i] = acc;
    }
    ay->noise_pos = fmod(npos, NOISE_LEN);
    ay->env_pos = epos < 1e9 ? epos : 1e9;
    for (int ch = 0; ch < 3; ch++)
        ay->tone_phase[ch] = ph[ch] - floor(ph[ch]);
}

int ay_render(Ay *ay, uint32_t frame_cycles, float *outf)
{
    double exact = (double)frame_cycles * AUDIO_RATE / CPU_HZ + ay->carry;
    int total = (int)exact;
    ay->carry = exact - total;
    double buf[AUDIO_RATE / TICK_HZ * 4 + 8];
    if (total > (int)(sizeof buf / sizeof buf[0]))
        total = sizeof buf / sizeof buf[0];
    /* events are in cycle order: offsets only grow within a tick */
    int done = 0;
    for (int i = 0; i < ay->nev; i++) {
        int at = (int)((double)ay->ev[i].cyc * AUDIO_RATE / CPU_HZ);
        if (at > total) at = total;
        if (at > done) {
            segment(ay, at - done, buf + done);
            done = at;
        }
        ay->regs[ay->ev[i].reg] = ay->ev[i].val;
        if (ay->ev[i].reg == 13)
            ay->env_pos = 0;
    }
    ay->nev = 0;
    if (total > done)
        segment(ay, total - done, buf + done);
    if (total <= 0)
        return 0;
    double mean = 0;
    for (int i = 0; i < total; i++) {
        buf[i] /= 3.0;
        mean += buf[i];
    }
    mean /= total;
    /* slow DC removal so silence sits at zero */
    ay->dc = 0.97 * ay->dc + 0.03 * mean;
    for (int i = 0; i < total; i++)
        outf[i] = (float)(buf[i] - ay->dc);
    return total;
}
