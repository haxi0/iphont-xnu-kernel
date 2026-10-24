#!/bin/bash
# Install HackiOS on a phone as a dual boot next to Android, or update an installed one.
#
# usage: common/flash/install.sh <device>            first install: build, back up, write bundle + UEFI
#        common/flash/install.sh <device> update     a new kernel/boot volume: only the bundle (into the other boot slot)
#
# Needs: the phone in Android with adb and root (su), an unlocked bootloader, the darwin partition with the
# iOS volume and the cryptexes already on it (README of the userland repository: "iOS with its root on the
# flash"). Nothing of Android is written; see common/flash/flashctl.py for every check made.
#   SKIP_BUILD=1   use the images already built (userland/flashboot.apfs, the kernel)
. "$(dirname "$(readlink -f "$0")")/../lib.sh"
load_device "$1"
# a phone with no Android left (the OnePlus 8): the loader's USB link and fastboot instead of adb
[ "$FLASH_VIA" = xl ] && exec "$(dirname "$(readlink -f "$0")")/install-xl.sh" "$@"
MODE=${2:-install}
[ "$KIND" = phone ] || die "$DEVICE is not a phone"
[ -n "$FLASH_BOOTARGS" ] || die "common/devices/$DEVICE.conf has no FLASH_BOOTARGS: this device has no installed mode yet"
mkdir -p out

K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837; [ -f $K ] || K=prebuilt/mach.development.bcm2837
# this phone's darwin (made when it was cut: tools/userdata-carve.py, common/flash/darwinid.py): the loader is
# built with it, and the kernel is told it on its command line
SERIAL=$(adb get-serialno 2>/dev/null | tr -d '\r')
export DARWIN_ID=phones/$SERIAL.darwin
[ -f "$DARWIN_ID" ] || die "no $DARWIN_ID: this phone's darwin has not been cut yet (tools/userdata-carve.py, then darwinid.py from-gpt)"
FLASH_BOOTARGS="$FLASH_BOOTARGS $BOARD_ARGS $(python3 common/flash/darwinid.py bootargs $DARWIN_ID)"
# darwin as a file: the kernel's own UFS driver does not read along its map yet -- the loader serves it
python3 -c "import json,sys; sys.exit(json.load(open('$DARWIN_ID')).get('kind') != 'file')" &&
	FLASH_BOOTARGS=$(echo "$FLASH_BOOTARGS" | sed 's/ ufs_native=1//')
xnuloader/build.sh > /dev/null || die "the loader did not build"
if [ -z "$SKIP_BUILD" ]; then
	have_userland
	[ -d ios165/rootfs ] || die "no iOS files: ios165/fetch.sh first"
	env $FLASH_USERLAND_VARS userland/build.sh
fi
[ -f userland/flashboot.apfs ] && [ -f userland/root.apfs ] || die "no userland/flashboot.apfs and root.apfs (FLASHBOOT=1 userland/build.sh makes them)"
# signed with the owner key when there is one (common/flash/ownerkey.py); OWNER_KEY=file names another
SIGN=; KEYFILE=${OWNER_KEY:-$HOME/.hackios/owner.key}; [ -f "$KEYFILE" ] && SIGN="--sign $KEYFILE"
LABEL="$(git describe --tags --always 2>/dev/null || echo build) $(date +%Y-%m-%d)"
# flashboot.apfs is the root the kernel starts from (md0, resident), root.apfs the small image on /hackios (md1)
python3 common/flash/mkbundle.py -o out/$DEVICE-bundle.img --kernel $K --resident userland/flashboot.apfs --ramdisk userland/root.apfs \
	--cmdline "$FLASH_BOOTARGS" --label "$LABEL" $SIGN

F=common/flash/flashctl.py
export HACKIOS_CODENAMES="${CODENAMES:-$DEVICE}"
python3 $F doctor || die "the phone is not ready (see above)"
python3 $F status || die "the phone is not ready (see above)"
if [ "$MODE" = install ]; then
	# the UEFI with a payload of the loader alone (no kernel): the UEFI's hook starts the loader from the
	# payload, and the loader, finding no kernel there, shows the boot menu and boots from darwin
	python3 xnuloader/mkpayload.py -o out/$DEVICE-uefi-payload.bin loader.efi=xnuloader/loader.efi > /dev/null ||
		die "the loader's payload was not made"
	FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE UEFI_FD=$UEFI_FD DTB=$DTB UEFI_TARGET=$UEFI_TARGET \
		python3 mkxnuimg.py -p out/$DEVICE-uefi-payload.bin -o out/$DEVICE-uefi.img
	python3 $F backup || die "backup failed: nothing was written"
	python3 $F write-bundle out/$DEVICE-bundle.img || die "the bundle was not written"
	python3 $F write-uefi out/$DEVICE-uefi.img || die "the UEFI was not written"
	# the menu on every power-on, as on a PC: Android hands the next start back to HackiOS's slot
	python3 $F menu-on out/$DEVICE-uefi.img || echo "install.sh: no boot menu module (no KernelSU/Magisk?): switch with flashctl.py switch"
else
	python3 $F write-bundle out/$DEVICE-bundle.img || die "the bundle was not written (the installed one still boots)"
fi
cat <<MSG

Installed. Android is untouched and still the active slot.
  start HackiOS:        python3 $F switch hackios        (from Android; or fastboot --set-active=<spare slot>)
  back to Android:      choose "Android" in the HackiOS boot menu (volume keys, power)
                        or, from fastboot: python3 $F switch android
  remove the UEFI:      python3 $F restore
  is it healthy:        python3 $F health          (the boot guard: a version that does not come up 3 times
                                                    is skipped by itself; then the previous one, then Android)
  previous version:     python3 $F rollback        (or "HackiOS, previous version" in the menu)
(each flashctl.py command needs HACKIOS_CODENAMES=$HACKIOS_CODENAMES, or use common/hackios)
MSG
