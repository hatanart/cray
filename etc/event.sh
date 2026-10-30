#!/bin/sh
# Called by cray for cabinet events, with the event name as $1:
#   payout    the hopper dropped one coin into the tray
#   coin-in   a coin was put in
# It runs in the background; cray doesn't wait for it. Does nothing yet.
#
# To play a sound it's simpler to let cray mix it in (sound.payout in
# cray.conf); a second program often can't open the sound card while the
# game plays. Other ideas: pulse a GPIO for a coin counter or a solenoid:
#   pinctrl set 14 op dh; sleep 0.05; pinctrl set 14 op dl

case "$1" in
    payout)  ;;
    coin-in) ;;
esac
exit 0
