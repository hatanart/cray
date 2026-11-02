/* Front-end logic shared by the Pi and desktop builds: ROM and battery RAM
 * files, first start, payout float, periodic saving, power down. */
#ifndef SESSION_H
#define SESSION_H

#include "ray.h"

typedef struct {
    const char *rom;            /* set name or file, NULL = default */
    const char *romdir;
    const char *nvram;          /* NULL = next to the ROM set */
    int float_coins;            /* payout float on first start / refill (0 = off) */
    int save_every;             /* seconds between battery RAM saves (0 = only at exit) */
    int auto_refill;            /* refill the float when the game closes for lack of coins */
    uint32_t features;          /* game features to set while the books hold 0 (0 = leave) */
    double ay_clock, acrtc_clock;
    int sound;
    int board;                  /* video board 5501 (default) or 5502 */
} SessionOpts;

void session_defaults(SessionOpts *o);
/* progress(percent) is called during the first-start boot (may be NULL) */
int session_open(const SessionOpts *o, void (*progress)(int percent));
int session_tick(float *audio);     /* one 100 Hz tick; returns samples */
void session_refill(void);
void session_toggle_door(void);
int session_door_open(void);
void session_coin(void);
int session_save(void);
void session_shutdown(void);
const char *session_title(void);

#endif
