/* MC6821 PIA and MC6850 ACIA. */
#include <string.h>
#include "ray.h"

/* ------------------------------------------------------------ MC6821 */
static void side_reset(PiaSide *s)
{
    s->ddr = s->out = s->cr = 0;
    s->irq1 = s->irq2 = 0;
    s->c1 = s->c2 = 1;
}

void pia_reset(Pia *p)
{
    side_reset(&p->a);
    side_reset(&p->b);
}

static void set_c1(PiaSide *s, int level)
{
    level = level ? 1 : 0;
    if (level != s->c1) {
        int rising = level == 1;
        if (rising == !!(s->cr & 0x02))
            s->irq1 = 1;
        s->c1 = level;
    }
}

static void set_c2(PiaSide *s, int level)
{
    level = level ? 1 : 0;
    if (level != s->c2) {
        if (!(s->cr & 0x20)) {
            int rising = level == 1;
            if (rising == !!(s->cr & 0x10))
                s->irq2 = 1;
        }
        s->c2 = level;
    }
}

void pia_pulse_c1(PiaSide *s)
{
    if (s->cr & 0x02) { set_c1(s, 0); set_c1(s, 1); }
    else              { set_c1(s, 1); set_c1(s, 0); }
}

void pia_pulse_c2(PiaSide *s)
{
    if (s->cr & 0x10) { set_c2(s, 0); set_c2(s, 1); }
    else              { set_c2(s, 1); set_c2(s, 0); }
}

int pia_side_irq(const PiaSide *s)
{
    if (s->irq1 && (s->cr & 0x01))
        return 1;
    if (s->irq2 && (s->cr & 0x08) && !(s->cr & 0x20))
        return 1;
    return 0;
}

uint8_t pia_output(const PiaSide *s)
{
    return (uint8_t)((s->out & s->ddr) | (~s->ddr & 0xFF));
}

static uint8_t port_value(const PiaSide *s)
{
    return (uint8_t)((s->out & s->ddr) | (s->pins & ~s->ddr));
}

uint8_t pia_read(Pia *p, int reg)
{
    reg &= 3;
    PiaSide *s = reg < 2 ? &p->a : &p->b;
    if (reg & 1) {
        uint8_t v = s->cr & 0x3F;
        if (s->irq1) v |= 0x80;
        if (s->irq2) v |= 0x40;
        return v;
    }
    if (s->cr & 0x04) {
        s->irq1 = s->irq2 = 0;
        return port_value(s);
    }
    return s->ddr;
}

void pia_write(Pia *p, int reg, uint8_t val)
{
    reg &= 3;
    PiaSide *s = reg < 2 ? &p->a : &p->b;
    if (reg & 1)
        s->cr = (s->cr & 0xC0) | (val & 0x3F);
    else if (s->cr & 0x04)
        s->out = val;
    else
        s->ddr = val;
}

void pia_set_input(PiaSide *s, int bit, int level)
{
    if (level) s->pins |= (uint8_t)(1 << bit);
    else       s->pins &= (uint8_t)~(1 << bit);
}

/* ------------------------------------------------------------ MC6850 */
void acia_reset(Acia *a)
{
    a->ctrl = 0x03;
    a->rx_head = a->rx_len = 0;
    a->rdr = 0;
    a->rdrf = a->overrun = 0;
}

void acia_receive(Acia *a, uint8_t b)
{
    if (a->rx_len < ACIA_RXQ) {
        a->rx[(a->rx_head + a->rx_len) % ACIA_RXQ] = b;
        a->rx_len++;
    }
}

void acia_tick(Acia *a)
{
    if (!a->rdrf && a->rx_len) {
        a->rdr = a->rx[a->rx_head];
        a->rx_head = (a->rx_head + 1) % ACIA_RXQ;
        a->rx_len--;
        a->rdrf = 1;
    }
}

int acia_irq(const Acia *a)
{
    if ((a->ctrl & 3) == 3)
        return 0;
    if ((a->ctrl & 0x80) && (a->rdrf || a->overrun))
        return 1;
    if ((a->ctrl & 0x60) == 0x20)        /* TX irq; transmitter always ready */
        return 1;
    return 0;
}

static uint8_t acia_status(const Acia *a)
{
    if ((a->ctrl & 3) == 3)
        return 0x02;
    uint8_t v = 0x02;
    if (a->rdrf) v |= 0x01;
    if (a->overrun) v |= 0x20;
    if (acia_irq(a)) v |= 0x80;
    return v;
}

uint8_t acia_read(Acia *a, int reg)
{
    if (reg & 1) {
        a->rdrf = 0;
        a->overrun = 0;
        return a->rdr;
    }
    return acia_status(a);
}

void acia_write(Acia *a, int reg, uint8_t val)
{
    if (reg & 1) {
        if (a->has_peer)
            coin_tx(&ray.coin, val);
    } else {
        a->ctrl = val;
        if ((val & 3) == 3) {
            a->rx_head = a->rx_len = 0;
            a->rdrf = a->overrun = 0;
        }
    }
}
