/* Coin processor (PCB 5002) on the ACIA1 serial link. Port of
 * pyray/coinproc.py; see there for the protocol notes.
 *
 * Frame: 1..3 data bytes (bit 7 clear) + check byte (~sum(data)) | 0x80.
 * Control frames: 30 CF ENQ, 00 FF / 40 BF ACK0/1, 28 D7 / 68 97 NAK0/1,
 * 50 AF EOT.
 */
#include <string.h>
#include "ray.h"

enum { ENQ = 0x30, ACK0 = 0x00, ACK1 = 0x40, NAK0 = 0x28, NAK1 = 0x68, EOT = 0x50 };
enum { ST_IDLE, ST_ENQ, ST_SEND, ST_EOT, ST_SLAVE };

#define BYTE_CYCLES (CPU_HZ / 960)      /* 9600 baud, 10 bits per byte */
#define PAY_FRAMES 15                   /* ~7 coins per second */
#define PARAM1 (0x01 | 0x04)            /* coin channel 1 and hopper 1 in use */
#define PARAM2 0x00

static int is_control(uint8_t b)
{
    return b == ENQ || b == EOT || b == ACK0 || b == ACK1 || b == NAK0 || b == NAK1;
}

static uint8_t check_byte(const uint8_t *d, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) s += d[i];
    return (uint8_t)(((~s) & 0x7F) | 0x80);
}

/* fr[0..n-1] is a frame whose last byte is the correct check byte */
static int frame_valid(const uint8_t *fr, int n)
{
    return n >= 1 && check_byte(fr, n - 1) == fr[n - 1];
}

static void send_bytes(Coin *c, const uint8_t *b, int n)
{
    for (int i = 0; i < n && c->out_len < 256; i++) {
        c->out_bytes[(c->out_head + c->out_len) & 255] = b[i];
        c->out_len++;
    }
}

static void send_ctrl(Coin *c, uint8_t b)
{
    uint8_t fr[2] = { b, check_byte(&b, 1) };
    send_bytes(c, fr, 2);
}

static void outbox_push(Coin *c, const CoinMsg *m, int front)
{
    if (c->outbox_len >= COIN_OUTQ)
        return;
    if (front) {
        memmove(&c->outbox[1], &c->outbox[0], c->outbox_len * sizeof(CoinMsg));
        c->outbox[0] = *m;
    } else {
        c->outbox[c->outbox_len] = *m;
    }
    c->outbox_len++;
}

static void send_msg(Coin *c, int n, uint8_t a, uint8_t b, uint8_t d)
{
    CoinMsg m = { (uint8_t)n, { a, b, d } };
    outbox_push(c, &m, 0);
}

static void send_next(Coin *c)
{
    if (c->outbox_len) {
        CoinMsg m = c->outbox[0];
        memmove(&c->outbox[0], &c->outbox[1], (c->outbox_len - 1) * sizeof(CoinMsg));
        c->outbox_len--;
        m.d[0] = (m.d[0] & 0x3F) | (c->toggle ? 0x40 : 0);
        c->toggle ^= 1;
        memcpy(c->current, m.d, m.len);
        c->current[m.len] = check_byte(m.d, m.len);
        c->current_len = m.len + 1;
        send_bytes(c, c->current, c->current_len);
    } else {
        c->state = ST_EOT;
        send_ctrl(c, EOT);
    }
}

static void later(Coin *c, int delay, int n, uint8_t a, uint8_t b, uint8_t d)
{
    if (c->npending >= COIN_PENDING)
        return;
    CoinPending *p = &c->pending[c->npending++];
    p->delay = delay;
    p->msg.len = (uint8_t)n;
    p->msg.d[0] = a; p->msg.d[1] = b; p->msg.d[2] = d;
}

static void pay(Coin *c, int reply, int n)
{
    int start = 0;
    for (int i = 0; i < c->npending; i++)
        if (c->pending[i].msg.d[0] == reply && c->pending[i].delay > start)
            start = c->pending[i].delay;
    for (int i = 0; i < n; i++) {
        if (c->hopper_coins <= 0) {
            /* hopper empty: "voitonmaksukoneisto 1 ei toimi tai rahat lopussa" */
            later(c, start + (i + 2) * PAY_FRAMES, 3, 0x01, 0x44, 0x00);
            break;
        }
        c->paid++;
        c->hopper_coins--;
        later(c, start + (i + 1) * PAY_FRAMES, 1, (uint8_t)reply, 0, 0);
    }
}

/* application layer: a message from the game (data bytes, toggle cleared) */
static void app(Coin *c, const uint8_t *data, int n)
{
    int cmd = data[0] & 0x3F;
    const uint8_t *arg = data + 1;
    int nargs = n - 1;
    switch (cmd) {
    case 0x09: {
        int k = nargs ? arg[0] : 1;
        if (k == 1)      later(c, 100, 3, 0x01, 0x79, 0x00);   /* self tests done */
        else if (k == 2) later(c, 20, 3, 0x09, 0x00, 0x60);    /* cables 1,2 in */
        else             later(c, 20, 2, 0x11, 0x40, 0);       /* game cable in */
        break;
    }
    case 0x19: later(c, 5, 3, 0x19, 0x00, 0x00); break;
    case 0x21: later(c, 5, 2, 0x21, 0x07, 0); break;           /* cables 1-3 */
    case 0x29: later(c, 5, 3, 0x29, c->controls & 0x7F, 0x00); break;
    case 0x31:
        c->configured = 1;
        later(c, 5, 3, 0x31, PARAM1, PARAM2);
        break;
    case 0x39: c->controls = nargs ? arg[0] : 0; break;
    case 0x02: case 0x0A: case 0x12: case 0x1A:                /* inhibits */
    case 0x22: case 0x2A: case 0x32: case 0x3A:
    case 0x03: case 0x33: case 0x0B: case 0x3B:                /* diverters */
        break;
    case 0x04: case 0x0C: case 0x14:
        pay(c, cmd + 1, 1);
        break;
    case 0x24: case 0x2C: case 0x34:
        if (nargs)
            pay(c, cmd == 0x24 ? 0x05 : cmd == 0x2C ? 0x0D : 0x15, arg[0]);
        break;
    case 0x05: case 0x0D:
        c->to_cashbox++;
        later(c, 30, 1, (uint8_t)(cmd + 1), 0, 0);
        break;
    case 0x25: case 0x2D:
        if (nargs) {
            for (int i = 0; i < arg[0]; i++) {
                c->to_cashbox++;
                if (cmd == 0x25)
                    c->hopper_coins--;
                later(c, (i + 1) * PAY_FRAMES, 1, cmd == 0x2D ? 0x0E : 0x06, 0, 0);
            }
        }
        break;
    case 0x06:
        for (int i = 0; i < 3; i++)
            if (nargs && (arg[0] & (0x08 << i)))
                c->counters[i]++;
        break;
    default:
        break;
    }
}

static void frame_in(Coin *c, const uint8_t *fr, int n)
{
    if (!frame_valid(fr, n)) {
        send_ctrl(c, NAK0);
        return;
    }
    int ctrl = n == 2 && is_control(fr[0]);
    int cc = (fr[0] == ACK1 || fr[0] == NAK1) ? (fr[0] & 0xBF) : fr[0];
    if (ctrl && cc == ENQ) {
        if (c->state == ST_IDLE || c->state == ST_SLAVE) {
            c->state = ST_SLAVE;
            send_ctrl(c, ACK1);
        }
        return;
    }
    if (ctrl && cc == EOT) {
        if (c->state == ST_SLAVE || c->state == ST_IDLE) {
            send_ctrl(c, EOT);
        } else if (c->state == ST_SEND && c->current_len) {
            /* session torn down mid-message: send it again later */
            CoinMsg m;
            m.len = (uint8_t)(c->current_len - 1);
            memcpy(m.d, c->current, m.len);
            m.d[0] &= 0x3F;
            outbox_push(c, &m, 1);
        }
        c->state = ST_IDLE;
        c->current_len = 0;
        c->wait = 5;
        return;
    }
    if (ctrl && cc == ACK0) {
        if (c->state == ST_ENQ) {
            c->state = ST_SEND;
            send_next(c);
        } else if (c->state == ST_SEND) {
            c->current_len = 0;
            send_next(c);
        }
        return;
    }
    if (ctrl && cc == NAK0) {
        if (c->state == ST_SEND && c->current_len)
            send_bytes(c, c->current, c->current_len);
        else if (c->state == ST_ENQ)
            send_ctrl(c, ENQ);
        return;
    }
    /* data frame */
    if (c->state != ST_SLAVE)
        return;
    send_ctrl(c, (fr[0] & 0x40) ? ACK1 : ACK0);
    uint8_t data[4];
    data[0] = fr[0] & 0xBF;
    for (int i = 1; i < n - 1; i++)
        data[i] = fr[i];
    app(c, data, n - 1 > 0 ? n - 1 : 1);
}

void coin_tx(Coin *c, uint8_t byte)
{
    /* keep the last 4 bytes: a frame is at most 3 data bytes + check */
    if (c->rxlen == 4) {
        memmove(c->rxbuf, c->rxbuf + 1, 3);
        c->rxlen = 3;
    }
    c->rxbuf[c->rxlen++] = byte;
    if (!(byte & 0x80))
        return;
    uint8_t fr[4];
    int n = c->rxlen;
    memcpy(fr, c->rxbuf, n);
    c->rxlen = 0;
    /* resynchronise on stray bytes: use the longest valid tail */
    for (int k = n; k > 1; k--) {
        if (frame_valid(fr + n - k, k)) {
            memmove(fr, fr + n - k, k);
            n = k;
            break;
        }
    }
    frame_in(c, fr, n);
}

void coin_poll(Coin *c, uint64_t cycles)
{
    if (c->out_len && cycles >= c->next_byte_at) {
        acia_receive(&ray.acia[0], c->out_bytes[c->out_head]);
        c->out_head = (c->out_head + 1) & 255;
        c->out_len--;
        c->next_byte_at = cycles + BYTE_CYCLES;
    }
}

void coin_reset(Coin *c)
{
    memset(c, 0, sizeof *c);
    c->state = ST_IDLE;
    c->hopper_coins = 100;
    c->configured = 1;
}

void coin_power_on(Coin *c)
{
    /* The coin unit reports a cold start (07), repeated until the game
     * asks for the parameters. */
    c->configured = 0;
    c->announce_timer = 150;
}

void coin_insert(Coin *c, int channel)
{
    static const uint8_t cmd[5] = { 0, 0x03, 0x0B, 0x14, 0x1C };
    if (channel < 1 || channel > 4)
        return;
    if (channel <= 2)
        c->hopper_coins++;
    c->coins_in++;
    if (ray_event)
        ray_event("coin-in");
    send_msg(c, 1, cmd[channel], 0, 0);
}

static void link_tick(Coin *c)
{
    if (c->wait) {
        c->wait--;
        return;
    }
    if (c->state == ST_IDLE && c->outbox_len) {
        c->toggle = 0;          /* first message of a session has toggle 0 */
        c->state = ST_ENQ;
        send_ctrl(c, ENQ);
        c->wait = 20;
    } else if ((c->state == ST_ENQ || c->state == ST_SEND || c->state == ST_EOT)
               && !c->out_len) {
        if (++c->wait_timeout > 100) {
            c->wait_timeout = 0;
            c->state = ST_IDLE;
        }
    }
}

void coin_tick_100hz(Coin *c)
{
    if (!c->configured) {
        if (--c->announce_timer <= 0) {
            c->announce_timer = 300;
            if (c->state == ST_IDLE && !c->outbox_len)
                send_msg(c, 3, 0x01, 0x07, 0x00);
        }
    }
    for (int i = 0; i < c->npending; i++)
        c->pending[i].delay--;
    int keep = 0;
    CoinMsg due[COIN_PENDING];
    int ndue = 0;
    for (int i = 0; i < c->npending; i++) {
        if (c->pending[i].delay <= 0)
            due[ndue++] = c->pending[i].msg;
        else
            c->pending[keep++] = c->pending[i];
    }
    c->npending = keep;
    for (int i = 0; i < ndue; i++) {
        uint8_t m = due[i].d[0];
        /* VOITTO replies go out as each coin leaves the hopper */
        if (ray_event && (m == 0x05 || m == 0x0D || m == 0x15) && due[i].len == 1)
            ray_event("payout");
        outbox_push(c, &due[i], 0);
    }
    link_tick(c);
}
