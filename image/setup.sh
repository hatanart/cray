#!/bin/sh
# Turn a fresh Raspberry Pi OS Lite (Bookworm or later) into a RAY machine.
# Run on the Pi, from the cray folder, as root:
#
#   sudo image/setup.sh [--hdmi | --hdmi-576p | --hdmi-1216 | --composite | --composite-576i]
#                       [--audio jack|hdmi] [--readonly]
#
#   --hdmi            picture on HDMI at the screen's own size, 50 Hz (default)
#   --hdmi-576p       HDMI 720x576p50 (4:3), like an Ultimate 64
#   --hdmi-1216       HDMI 1216x576@50: 576p timing with 1216 pixels a line,
#                     the game doubled both ways with no side borders
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
        --hdmi-1216) VIDEO=hdmi1216 ;;
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
# HDMI audio off unless the sound goes there: many cheap screens flicker
# or lose the picture when they get audio data they don't expect
KMS=vc4-kms-v3d
[ $AUDIO = hdmi ] || KMS=$KMS,noaudio
case $VIDEO in
    composite*)
        echo "dtoverlay=$KMS,composite" >> $CONFIG
        set_conf connector Composite-1
        if [ $VIDEO = composite ]; then set_conf mode 720x292@50; else set_conf mode 720x576i@50; fi
        ;;
    hdmi576p)
        echo "dtoverlay=$KMS" >> $CONFIG
        set_conf connector HDMI-A-1
        set_conf mode 720x576@50
        ;;
    hdmi1216)
        echo "dtoverlay=$KMS" >> $CONFIG
        set_conf connector HDMI-A-1
        set_conf mode 1216x576@50
        ;;
    *)
        echo "dtoverlay=$KMS" >> $CONFIG
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
# the screen's details are read once at start, not again every 10 s
# (a re-read can make some screens blink)
add_cmd drm_kms_helper.poll=0
if [ $VIDEO = composite ] || [ $VIDEO = composite576i ]; then
    # the boot console in 576i; cray switches to its own mode
    add_cmd vc4.tv_norm=PAL
    add_cmd video=Composite-1:720x576@50ie
fi

echo "== logs in RAM"
# The system log is kept in memory only (lost at a reboot), so logging
# never writes to the SD card. journalctl -u cray still works.
mkdir -p /etc/systemd/journald.conf.d
cat > /etc/systemd/journald.conf.d/cray.conf <<'EOF2'
[Journal]
Storage=volatile
RuntimeMaxUse=16M
EOF2
systemctl restart systemd-journald
# older systems also copy everything to /var/log/syslog
if systemctl list-unit-files rsyslog.service >/dev/null 2>&1; then
    systemctl disable --now rsyslog.service 2>/dev/null || true
fi

echo "== quiet system: no automatic updates, no waiting for the network"
# A toy cabinet: updates in the background would use the CPU and the card
# (and can't be kept on a read-only system); update by hand with apt.
# Booting doesn't wait for a network, and cloud-init (the first-boot setup
# of Raspberry Pi Imager) has done its job and isn't run again.
for u in apt-daily.timer apt-daily-upgrade.timer man-db.timer dpkg-db-backup.timer \
         e2scrub_all.timer e2scrub_reap.service fstrim.timer \
         NetworkManager-wait-online.service systemd-networkd-wait-online.service; do
    systemctl disable --now $u 2>/dev/null || true
done
systemctl mask apt-daily.service apt-daily-upgrade.service 2>/dev/null || true
if [ -d /etc/cloud ]; then
    touch /etc/cloud/cloud-init.disabled
fi

echo "== settings on the boot partition"
# Read after /etc/cray/cray.conf: stays writable on a read-only system, and
# a PC can edit it with the card in a reader.
mkdir -p $BOOT/cray
[ -f $BOOT/cray/cray.conf ] || cat > $BOOT/cray/cray.conf <<'EOF2'
# Sound over HDMI (the TV's speakers) instead of the 3.5 mm jack, e.g. on
# a Zero 2 W, which has no jack: remove the # from the next line, and in
# config.txt (next to this folder) change "dtoverlay=vc4-kms-v3d,noaudio"
# to "dtoverlay=vc4-kms-v3d".
#audio = default:CARD=vc4hdmi

# cray settings that override /etc/cray/cray.conf (all settings are
# listed and explained there). Lines are "setting = value"; # starts a
# comment. This file is on the boot partition, so it can be changed even
# on a read-only system, or from a PC with the card in a reader. Restart
# the game after a change: sudo systemctl restart cray, or reboot.
#
#rom = pokeri-7907-2.0          # or pokeri-8460-1.1
#scanlines = 25                 # darker line under each game line, 0..100 %
#volume = 100                   # 0..100
#board = 5502                   # the later video board (colour palette chip)
EOF2

echo "== service"
install -m 644 image/cray.service /etc/systemd/system/cray.service
systemctl daemon-reload
systemctl enable cray.service

if [ $READONLY = 1 ]; then
    echo "== read-only system"
    mkdir -p $BOOT/cray
    for f in /var/lib/cray/roms/*.nvram; do [ -f "$f" ] && cp -n "$f" $BOOT/cray/; done
    # a folder: each set keeps its own save there, so rom can be changed
    set_conf nvram $BOOT/cray/
    raspi-config nonint do_overlayfs 0
    # raspi-config may also write-protect the boot partition, but the
    # battery RAM is saved there: keep it writable
    sed -i -E '/[[:space:]]\/boot(\/firmware)?[[:space:]]/ s/,ro([,[:space:]])/\1/' /etc/fstab
    echo "The root file system becomes read-only after the reboot."
    echo "To change settings later: sudo raspi-config (Performance > Overlay file system), off, reboot."
fi

echo "== done. Reboot to start the game: sudo reboot"
