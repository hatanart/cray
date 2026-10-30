#!/bin/sh
# Turn a fresh Raspberry Pi OS Lite (Bookworm or later) into a RAY machine.
# Run on the Pi, from the cray folder, as root:
#
#   sudo image/setup.sh [--hdmi | --hdmi-576p | --composite | --composite-576i]
#                       [--audio jack|hdmi] [--readonly]
#
#   --hdmi            picture on HDMI at the screen's own size, 50 Hz (default)
#   --hdmi-576p       HDMI 720x576p50 (4:3), like an Ultimate 64
#   --composite       the 3.5 mm jack's video ring, PAL progressive 288p
#                     (one game line per TV line, like an old console);
#                     tries 292 lines so nothing is cropped, else 288
#   --composite-576i  PAL interlaced 576i instead
#   --audio jack  sound on the 3.5 mm jack (default on a Pi 3)
#   --audio hdmi  sound over HDMI (default on a Zero 2 W, which has no jack)
#   --audio pwm   sound as PWM on GPIO 18 (left) / 19 (right), for boards
#                 without a jack: needs an RC filter and an amplifier; the
#                 two lamps on those pins move to GPIO 3 and 14
#   --readonly    protect the SD card against power cuts: the system runs
#                 from a read-only overlay; only the battery RAM file is
#                 written, on the boot partition
#
# The ROM chip files are not included: put them in roms/ (next to the .ray
# file there), in ../roms (next to this cray folder) or in /var/lib/cray/roms.
set -e

MODEL=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)
VIDEO=hdmi
AUDIO=jack
case "$MODEL" in *"Zero 2"*) AUDIO=hdmi ;; esac
READONLY=0
for a in "$@"; do
    case "$a" in
        --hdmi) VIDEO=hdmi ;;
        --hdmi-576p) VIDEO=hdmi576p ;;
        --composite) VIDEO=composite ;;
        --composite-576i) VIDEO=composite576i ;;
        --audio) ;;
        jack|hdmi|pwm) AUDIO=$a ;;
        --readonly) READONLY=1 ;;
        *) echo "unknown option $a"; exit 2 ;;
    esac
done
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }
echo "== $MODEL"
case "$MODEL" in
    *"Zero 2"*)
        if [ $AUDIO = jack ]; then
            echo "This board has no 3.5 mm jack: use --audio hdmi or --audio pwm"; exit 2
        fi
        case $VIDEO in composite*)
            echo "NOTE: on a Zero 2 W the composite signal is only on solder pads (no jack)" ;;
        esac
        ;;
esac
# compiling the CPU core takes ~220 MB; one file at a time on 512 MB boards
MEM_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
if [ "$MEM_KB" -lt 900000 ]; then JOBS=1; else JOBS=4; fi
cd "$(dirname "$0")/.."

# the boot partition: /boot/firmware on Bookworm and later, /boot before
BOOT=
for d in /boot/firmware /boot; do
    if [ -f $d/config.txt ]; then BOOT=$d; break; fi
done
[ -n "$BOOT" ] || { echo "cannot find config.txt in /boot/firmware or /boot"; exit 1; }
CONFIG=$BOOT/config.txt
CMDLINE=$BOOT/cmdline.txt
if ! touch $CONFIG 2>/dev/null; then
    echo "$BOOT is read-only ($(findmnt -no OPTIONS $BOOT 2>/dev/null))."
    echo "If the overlay file system is on: sudo raspi-config > Performance > Overlay, off, reboot."
    echo "Or remount it for now: sudo mount -o remount,rw $BOOT"
    exit 1
fi

echo "== packages"
apt-get update
apt-get install -y build-essential pkg-config libdrm-dev libasound2-dev

echo "== build"
make clean >/dev/null
echo "(building with $JOBS job(s); a few minutes on a small board)"
make -j$JOBS cray
make install
# the build ran as root: give the folder back to whoever ran sudo
[ -n "$SUDO_USER" ] && chown -R "$SUDO_USER": . 2>/dev/null || true

echo "== files"
mkdir -p /etc/cray /var/lib/cray/roms
[ -f /etc/cray/cray.conf ] || cp etc/cray.conf /etc/cray/cray.conf
[ -f /etc/cray/event.sh ] || install -m 755 etc/event.sh /etc/cray/event.sh
for d in roms ../roms; do
    cp -n $d/*.ray $d/*.bin /var/lib/cray/roms/ 2>/dev/null || true
done
ls /var/lib/cray/roms/*.ray >/dev/null 2>&1 || echo "NOTE: no ROM set in /var/lib/cray/roms yet"
ls /var/lib/cray/roms/*.bin >/dev/null 2>&1 || echo "NOTE: no ROM chip files (.bin) in /var/lib/cray/roms yet"

set_conf() {     # set_conf key value: set or add "key = value" in cray.conf
    if grep -q "^#*$1 *=" /etc/cray/cray.conf; then
        sed -i "s|^#*$1 *=.*|$1 = $2|" /etc/cray/cray.conf
    else
        echo "$1 = $2" >> /etc/cray/cray.conf
    fi
}

unset_conf() {   # unset_conf key: comment out "key = ..." (back to automatic)
    sed -i "s|^$1 *=|#$1 =|" /etc/cray/cray.conf
}

echo "== display: $VIDEO, sound: $AUDIO"
# start from a clean slate, so running setup again with other options works
unset_conf connector
unset_conf mode
sed -i '/^dtoverlay=audremap/d' $CONFIG
for c in vc4.tv_norm=PAL video=Composite-1:720x576@50ie; do sed -i "1 s| $c||" $CMDLINE; done
# KMS driver (the default on Bookworm); composite needs its parameter
sed -i 's/^dtoverlay=vc4-kms-v3d.*/#&/' $CONFIG
sed -i 's/^dtoverlay=vc4-fkms-v3d.*/#&/' $CONFIG
case $VIDEO in
    composite*)
        echo "dtoverlay=vc4-kms-v3d,composite" >> $CONFIG
        set_conf connector Composite-1
        if [ $VIDEO = composite ]; then set_conf mode 720x292@50; else set_conf mode 720x576i@50; fi
        ;;
    hdmi576p)
        echo "dtoverlay=vc4-kms-v3d" >> $CONFIG
        set_conf connector HDMI-A-1
        set_conf mode 720x576@50
        ;;
    *)
        echo "dtoverlay=vc4-kms-v3d" >> $CONFIG
        ;;
esac
set_conf lamp5 18                 # the default pins (pwm moves them below)
set_conf lamp12 19
grep -q "^disable_splash=1" $CONFIG || echo "disable_splash=1" >> $CONFIG
grep -q "^dtparam=audio=on" $CONFIG || echo "dtparam=audio=on" >> $CONFIG
case $AUDIO in
    hdmi)
        set_conf audio "default:CARD=vc4hdmi" ;;
    pwm)
        # the analog audio block, routed to GPIO 18/19 instead of a jack
        grep -q "^dtoverlay=audremap" $CONFIG || echo "dtoverlay=audremap,pins_18_19" >> $CONFIG
        set_conf audio "default:CARD=Headphones"
        set_conf lamp5 3          # unused lamp line, was GPIO 18
        set_conf lamp12 14        # always-on lamp, was GPIO 19
        ;;
    *)
        set_conf audio "default:CARD=Headphones" ;;
esac

# quiet boot, no blinking cursor, no screen blanking; PAL for composite
add_cmd() { grep -q "$1" $CMDLINE || sed -i "1 s|\$| $1|" $CMDLINE; }
add_cmd quiet
add_cmd loglevel=3
add_cmd logo.nologo
add_cmd consoleblank=0
add_cmd vt.global_cursor_default=0
if [ $VIDEO = composite ] || [ $VIDEO = composite576i ]; then
    # the boot console in 576i; cray switches to its own mode
    add_cmd vc4.tv_norm=PAL
    add_cmd video=Composite-1:720x576@50ie
fi

echo "== service"
install -m 644 image/cray.service /etc/systemd/system/cray.service
systemctl daemon-reload
systemctl enable cray.service

if [ $READONLY = 1 ]; then
    echo "== read-only system"
    mkdir -p $BOOT/cray
    for f in /var/lib/cray/roms/*.nvram; do [ -f "$f" ] && cp -n "$f" $BOOT/cray/; done
    SET=$(ls /var/lib/cray/roms/*.ray 2>/dev/null | head -1)
    NAME=$(basename "${SET:-pokeri-8460-1.1.ray}" .ray)
    set_conf nvram $BOOT/cray/$NAME.nvram
    raspi-config nonint do_overlayfs 0
    echo "The root file system becomes read-only after the reboot."
    echo "To change settings later: sudo raspi-config (Performance > Overlay file system), off, reboot."
fi

echo "== done. Reboot to start the game: sudo reboot"
