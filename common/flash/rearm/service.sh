#!/system/bin/sh
# HackiOS boot menu on every power-on (a KernelSU / Magisk module; common/flash/install.sh puts it in).
#
# The phone boots whichever A/B slot is active. HackiOS lives in the spare slot (its boot partition holds
# the UEFI with HackiOS's loader); when "Android" is chosen in the menu, the loader makes Android's slot
# active and resets. Left at that, every later power-on would go straight to Android. This script undoes
# it once Android has come up: it makes the spare slot active again, so the next power-on shows the menu
# -- whose countdown then takes Android again, the last choice (xnuloader/flashboot.c).
#
# It acts only when everything checks out, and otherwise leaves the phone booting Android as it is:
#   - Android has finished starting and its slot has been marked successful (so a broken Android
#     update is never left without its fallback);
#   - no Virtual A/B update is being merged (switching slots then would cancel it);
#   - the spare slot's boot partition still holds exactly the image HackiOS installed (its SHA-256
#     is in hackios.conf): an Android update that wrote the spare slot has replaced it, and then
#     there is no menu to go back to until common/flash/install.sh is run again;
#   - the darwin partition is there.
# What it did or did not do goes into /data/adb/hackios-rearm.log.
MODDIR=${0%/*}
LOG=/data/adb/hackios-rearm.log
say() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> $LOG; }

. "$MODDIR/hackios.conf" || { say "no hackios.conf: nothing done"; exit 0; }   # SPARE=a|b, UEFI_SHA256, UEFI_BYTES

until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
sleep 60        # update_verifier and the boot control HAL mark the slot successful around boot_completed

CUR=$(getprop ro.boot.slot_suffix | tr -d _)
if [ "$CUR" = "$SPARE" ]; then
	say "running on the HackiOS slot _$SPARE itself (Android?): nothing done"; exit 0
fi
if [ -n "$(ls /metadata/ota/snapshots 2>/dev/null)" ]; then
	say "an A/B update is being merged: not switching now (next boot)"; exit 0
fi
[ -b /dev/block/by-name/darwin ] || { say "no darwin partition: nothing done"; exit 0; }
GOT=$(head -c "$UEFI_BYTES" /dev/block/by-name/boot_$SPARE | sha256sum | cut -d' ' -f1)
if [ "$GOT" != "$UEFI_SHA256" ]; then
	say "boot_$SPARE no longer holds HackiOS's image (an Android update?): the menu is gone until install.sh runs again"
	exit 0
fi
N=0; [ "$SPARE" = b ] && N=1
if bootctl set-active-boot-slot $N; then
	say "slot _$SPARE active again: the next power-on shows the HackiOS menu"
else
	say "bootctl set-active-boot-slot $N failed: the next power-on starts Android"
fi
