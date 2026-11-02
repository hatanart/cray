# cray: RAY video poker for the Raspberry Pi (C)

A C port of `pyray`, made for playing on a real cabinet:

- The picture goes straight to HDMI or composite with no X, Wayland or
  SDL, scaled by whole numbers (608x292 becomes 1216x584 on 720p).
- The game is locked to the display. The screen runs at exactly 50 Hz,
  each vertical blank gets two 100 Hz game ticks, and every game frame
  is shown exactly once.
- Buttons come from a USB keyboard, or from an arcade encoder that acts
  as a keyboard or joystick.
- The game's 23 lamp lines go to GPIO pins.
- Sound goes through ALSA. Optional WAV sounds and a script run when
  coins are paid out or put in.

## Quick install

On a Raspberry Pi 3, 4 or Zero 2 W with Raspberry Pi OS Lite
(Bullseye or later), connected to the network:

    sudo apt install git
    git clone https://github.com/hatanart/cray.git
    cd cray
    # copy the four ROM chip files (6457-7.7_ic30.bin, 6458-7.7_ic38.bin,
    # 6458-7.7_ic34.bin, 8460-1.1_ic43.bin) into roms/
    sudo image/setup.sh          # or --hdmi-576p, --composite ... (see below)
    sudo reboot

The game starts at boot. Plug in a USB keyboard: I puts in a coin,
B bets, Enter deals, 1-5 hold (a Zero 2 W needs a USB OTG adapter). With
no keyboard, the game can also be played over SSH (see "Control over
SSH"). To update later:

    cd cray && git pull && make && sudo make install && sudo systemctl restart cray

The ROMs are not included: they are RAY's (now Veikkaus) property.
`roms/pokeri-8460-1.1.ray` lists the chips the game needs and their
checksums; cray says which file is missing or wrong.

pyray stays the tool for studying the game (video memory view, payout
statistics). cray only plays it.

It runs the same CPU core (Musashi) and the same chip models as pyray,
and gives the same results byte for byte. From the same battery RAM,
after 2 minutes of scripted play (coins, bet, deal, hold, double,
collect), the RAM and the screen match pyray exactly. So do the
first-start float fill and a boot from blank RAM.
It runs about 40 times faster than real time on a PC core; a frame (2 ticks)
takes 0.45 ms (1.2 ms at worst), so a Pi 3 has plenty of room.

## Folders

    src/       the emulator core: plain C, no OS calls except the clock
    musashi/   68000 core (Musashi 3.32 from machine68k), with ray_conf.h
    pi/        cray, the Raspberry Pi / Linux console program
    sdl/       cray-sdl, a desktop window for trying it on a PC
    tools/     raytest, headless runs for testing and comparing with pyray
    etc/       cray.conf (all settings, lamp pins, keys) and event.sh
    image/     setup.sh and the systemd service for a Pi

## On a Raspberry Pi

Start from Raspberry Pi OS Lite (Bullseye or later). Put the ROM chip
files in `cray/roms/` (or in a `roms` folder next to `cray`), then run
`sudo image/setup.sh` with the options for your picture and sound, and
reboot. To change them later, run setup again with other options and
reboot; it undoes the previous choice. The build needs a few minutes on
512 MB boards (one file at a time).

    sudo image/setup.sh --hdmi-576p --audio hdmi
    sudo reboot

### Picture

| Option | Output | Where |
|---|---|---|
| `--hdmi` (default) | the screen's own resolution at 50 Hz | HDMI |
| `--hdmi-576p` | 720x576p50, 4:3, like an Ultimate 64 | HDMI |
| `--hdmi-1216` | 1216x576@50: 576p timing, game doubled both ways, no side borders | HDMI |
| `--composite` | PAL 288p (292 lines), one game line per TV line | composite |
| `--composite-576i` | PAL 576i, interlaced | composite |

- **HDMI:** a DVI monitor works through an HDMI-DVI cable or adapter, but
  gets no sound. If a screen shows nothing, try `--hdmi-576p`, or set
  `mode = 1280x720@50` in `/etc/cray/cray.conf`. The Zero 2 W has a mini
  HDMI socket.
- **Composite on a Pi 3 or 4:** the 3.5 mm jack is a 4-pole (TRRS) socket
  carrying sound and video: tip = left, ring 1 = right, ring 2 = ground,
  sleeve = video. AV cables are wired in different orders; if the yellow
  plug gives no picture, try the red one in the TV's video input. With
  `--composite` the sound stays on the same jack.
- **Composite on a Zero 2 W:** there is no socket. The signal is on a
  solder test pad (see the official Zero 2 W documentation for where):
  solder the centre of an RCA cable to it and the shield to ground
  (e.g. header pin 6).

### Sound

| Option | Output | Default on |
|---|---|---|
| `--audio jack` | 3.5 mm jack (tip left, ring 1 right) | Pi 3, Pi 4 |
| `--audio hdmi` | over HDMI to the TV | Zero 2 W (no jack) |
| `--audio pwm` | GPIO 18 / 19 as PWM, needs a filter | |

The game sound is mono and goes to both channels. The level is
`volume = 0..100` in `cray.conf` (also `echo volume=70 | sudo tee
/run/cray/control` while it runs).

**PWM pins** (`--audio pwm`, for boards without a jack, e.g. a Zero 2 W
on a monitor without speakers). The Pi's analog sound block drives two
GPIO pins:

    GPIO 18 (header pin 12)  left
    GPIO 19 (header pin 35)  right
    ground  (header pin 6, 14, 20, 30, 34 or 39)

The pins carry a fast 3.3 V square wave, not sound yet. Per channel:

    GPIO ──[ 1 kΩ ]──┬──[ 10 µF + ]── amplifier input
                     │
                  [ 10 nF ]
                     │
    ground ──────────┴─────────────── amplifier ground

The resistor and the 10 nF capacitor filter out the switching (about
16 kHz cut-off); the 10 µF capacitor blocks the DC. Use a small amplifier
board (PAM8403 or similar) with a speaker, or powered PC speakers; one
channel is enough. Never connect a speaker straight to a pin. Setup moves
the two lamp lines that used GPIO 18 and 19 (lamp 5, never lit, to
GPIO 3; lamp 12, always on, to GPIO 14).

On the first start, like pyray, cray puts 1000 coins in the payout
float and turns on win music in TESTI 14 (a tune for wins of 50 or
more). See `features` in `cray.conf`. A battery RAM file made before
this gets the same setting at the next start (the game restarts once,
about 15 seconds after it comes up).

The game's attract-mode saver is on: after a while the screen turns
yellow, then dark blue. On the Pi the dark step is kept as bright as
play, only light blue (`bright_attract = 1`; 0 = dark as on the
machine, or set bit 29 in `features` / TESTI 14 for no saver at all).
TESTI 14 settings live in the battery RAM, so a save made with the old
default (20002000) keeps the saver off: change it in TESTI 14 and
restart, or delete the save (all three copies, see below).

The game then starts at boot, runs as a service and restarts if it
stops. Settings are in `/etc/cray/cray.conf`, and the ROMs and battery
RAM are in `/var/lib/cray/roms`. To see its messages (display modes,
keyboards found, missed frames):

    journalctl -u cray -f

To stop it: `sudo systemctl stop cray`. The game is told the mains is
failing and saves its battery RAM first.

### Power cuts and the SD card

Switching the Pi off at the wall can still damage the SD card's file
system, mostly while something is writing. cray writes only the 64 KB
battery RAM file: 10 seconds after coins go in or out (at most once a
minute), once an hour otherwise, and at exit. It writes a new file,
flushes it to the card and renames it into place. Like the game's own
books, which it keeps in three copies inside the RAM, cray keeps three
generations of the file: `pokeri-8460-1.1.nvram` (newest), `.nvram.1`
and `.nvram.2`, each ending in a checksum. At start it takes the newest
one whose checksum is right and logs it if it had to fall back to an
older one. The write runs in a background thread, so it never delays a
frame. (pyray reads the same files: it ignores the checksum at the end.)

Setup also keeps the system log in RAM only (lost at a reboot), so
logging never writes to the card. `journalctl -u cray` still shows the
game's messages since the last start.

**Read-only system.** Add `--readonly` to the setup options:

    sudo image/setup.sh --hdmi-1216 --audio jack --readonly
    sudo reboot

The system then runs from a read-only overlay (Raspberry Pi OS's own
"overlay file system"): every write, including logs and anything you
change, goes to RAM and is gone after a reboot. The only file written to
the card is the battery RAM, kept on the boot partition
(`/boot/cray/` or `/boot/firmware/cray/`). A power cut can lose at most
the last minute of bookkeeping.

To change settings or update cray later, turn the overlay off first:

    sudo raspi-config nonint do_overlayfs 1     # or: raspi-config > Performance > Overlay, off
    sudo reboot
    # edit /etc/cray/cray.conf, git pull, make && sudo make install ...
    sudo image/setup.sh <your options> --readonly
    sudo reboot

### Display

At start, cray lists the connectors and modes it found. It picks the
screen's own resolution at 50 Hz. Failing that, it takes the best 50 Hz
mode on offer. If the screen offers no 50 Hz mode at all, it runs the
native mode slowed to 50 Hz, which most LCD screens accept. If yours
doesn't, set `custom50 = 0`; the game then follows the wall clock, and
motion can't be perfectly even at 60 Hz. `mode = 1280x720@50` and
`connector = HDMI-A-1` choose one directly.

The game and the drawing run on separate cores: while one frame is drawn
and swapped in, the game already runs the next one. Each job gets a full
20 ms, and a single slow frame is absorbed by the frame waiting in
between. The cost is one extra frame (20 ms) of delay. Missed frames
(a vertical blank with no new picture) are logged as they happen, with
the time the game and the drawing took, and counted every 10 minutes
and at exit. Game time still follows the display
after a miss, so the sound doesn't drift.

Progressive modes come first: one game line per screen line, no
interlace flicker.

- **Composite 288p.** PAL progressive, 312 lines, 50.08 Hz like an old
  console. The Raspberry Pi kernel has had 720x288 and 720x240 composite
  modes since 2021, with the same timing as the old firmware's
  `sdtv_mode=18`. The game has 292 lines, so `--composite` asks for a
  720x292 mode: the driver's 288p mode with 4 lines taken from the
  vertical blanking. If the driver refuses it, cray uses 720x288 and
  crops 2 lines at the top and bottom. The display lock still applies at
  50.08 Hz, and the game runs 0.16 % fast, as a console on such a TV would.
- **HDMI 576p50.** 720x576, 4:3, 27 MHz, the standard CEA-861 timing
  (VIC 17). It is built in, so it works even if the TV doesn't list it.
  The picture is 608x584 in 720x576: doubled lines, 2 game lines cropped
  at the top and bottom, side borders.
- **720p50 and 1080p50** are built in the same way.

Not tested on hardware yet: composite under the KMS driver (288p, and
whether 292 lines are accepted), 576p on a TV, and HDMI and PWM sound.

### Lamps

The game drives 23 lamp lines (three 8-bit latches). By default each
goes to its own GPIO pin. The table in `etc/cray.conf` shows which line
is which lamp (the five HOLD buttons, DEAL, BET, ...), worked out by
watching the game. About 13 of the lines are used.

A Pi pin gives 3.3 V and a few mA. Drive the lamps through transistors
or ULN2803 chips (8 lamps each), never straight from a pin. GPIO 2–8
are pulled high at power-on, so the default map puts only unused or
always-on lamps on those pins, and nothing flashes during boot.

### Keys

Same as pyray: 1-5 hold, Enter deal, B bet, P collect, D double,
Left/Right small/big, I coin, S service, F1-F6, F9 refill the payout
float, F10 door. Change them in `cray.conf` (`key.DEAL = ENTER SPACE`). Any key
without an action is logged with its name, which helps when wiring an
arcade encoder.

### Control over SSH

While the game runs, commands can be written to `/run/cray/control`,
for example from an SSH session, with no keyboard on the Pi:

    echo COIN | sudo tee /run/cray/control           # insert a coin
    echo DEAL | sudo tee /run/cray/control           # press a button (any key.NAME action)
    echo scanlines=25 | sudo tee /run/cray/control   # also volume=N

Settings changed this way last until the game restarts; put them in
`cray.conf` to keep them.

To play from the SSH session with the same keys as on the Pi's keyboard
(1-5, Enter, B, P, D, arrows, I, F1-F10; q quits):

    tools/craykeys

### Coin events

For each coin the hopper drops into the tray, cray plays
`sound.payout` (a WAV file mixed into the game sound), if set, and runs
`/etc/cray/event.sh payout`. Coins put in give `coin-in`. The script
runs in the background at normal priority and does nothing yet.
Use the WAV setting for sounds: a second program often can't open the
sound card while the game is playing.

## On a PC

    sudo apt install libsdl2-dev                # Debian / Ubuntu
    make sdl && ./cray-sdl -r roms              # window, same keys; lamps under the picture
    make raytest && ./raytest -r roms -t 3000 -o shot.ppm      # headless

`raytest` options: `-n NVRAM` (loaded, never written), `-t TICKS`,
`-c TICK` coin, `-p TICK:DEAL[:HOLD]` button press, `-F` first-start
float fill, `-l` lamp log, `-T` per-tick RAM checksum trace, `-o` PPM screenshot,
`-m` RAM dump, `-i` raw screen dump. `RAY_FIXED_TIME=<unix time>` pins the
clock chip so runs repeat exactly.

Building the Pi program on a PC also works (`make`, needs libdrm and
ALSA headers), but it can't take the screen while a desktop runs.

## Not done

- Tested on a Pi Zero 2 W (Bullseye) with HDMI and a DVI monitor at
  1216x576@50. Not tested yet: composite (288p/292p), 576p on a TV,
  HDMI and PWM audio.
- Bare metal: the core in `src/` has no OS calls except `time()`, so a
  bare-metal build (e.g. with the Circle library) would only need a new
  front end like `pi/cray.c`.
