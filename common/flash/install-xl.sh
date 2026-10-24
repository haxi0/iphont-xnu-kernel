#!/bin/bash
# Install HackiOS on a phone that has no Android left (the OnePlus 8: its userdata is the port's store), so
# that it starts from its own flash: power on, the boot menu, HackiOS -- no computer, no time limit.
#
# usage: common/flash/install-xl.sh <device>           first install: the bundle, then the UEFI (once previewed)
#        common/flash/install-xl.sh <device> bundle    only the bundle and out/<device>-uefi.img (= update)
#        common/flash/install-xl.sh <device> update    the same
#        common/flash/install-xl.sh <device> uefi      only the UEFI with the loader, into $FLASH_BOOT_PART
#
# What is written, each after a typed YES:
#   bundle  the boot bundle (kernel, boot volume, command line) into one boot slot of the store's boot area,
#           blocks 2042368.. (the slot not holding the newest good bundle; the loader checks and reads it back:
#           xnuloader/flashboot.c, fb_install). The loader runs in RAM for it (xl enter: fastboot boot, nothing
#           flashed) or is the installed one in its menu's "Developer mode".
#   uefi    the UEFI with the loader and no kernel into $FLASH_BOOT_PART by the phone's own fastboot. Its old
#           contents are the backup $FLASH_BOOT_BACKUP: `fastboot flash $FLASH_BOOT_PART <backup>` undoes it.
#           Refused until an installed bundle has come up once (the boot guard's "last up": boot the same UEFI
#           from RAM first, `fastboot boot out/<device>-uefi.img`), and on any fastboot device that is not
#           $FASTBOOT_PRODUCT or not alone. FORCE_UEFI=1 skips the first check only.
# Nothing else: not the GPT, not recovery (TWRP), not abl/xbl, not the iOS volume, not the slots' attributes.
#   SKIP_BUILD=1   use the images already built (userland/flashboot.apfs, root.apfs, the kernel)
. "$(dirname "$(readlink -f "$0")")/../lib.sh"
load_device "$1"
MODE=${2:-install}
[ "$FLASH_VIA" = xl ] || die "$DEVICE is installed with common/flash/install.sh (FLASH_VIA is not xl)"
[ -n "$FLASH_BOOTARGS" ] && [ -n "$FASTBOOT_PRODUCT" ] || die "common/devices/$DEVICE.conf has no FLASH_BOOTARGS / FASTBOOT_PRODUCT"
case $FLASH_BOOT_PART in boot_a|boot_b) ;; *) die "FLASH_BOOT_PART must be boot_a or boot_b, not '$FLASH_BOOT_PART'" ;; esac
case $MODE in install|bundle|update|uefi) ;; *) die "usage: $0 <device> [bundle|update|uefi]" ;; esac
mkdir -p out

confirm() {
	echo
	echo "$1"
	read -r -p "type YES to go on: " answer
	[ "$answer" = YES ] || die "not confirmed: nothing written"
}

K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837; [ -f $K ] || K=prebuilt/mach.development.bcm2837
xnuloader/build.sh > /dev/null || die "the loader did not build"

if [ "$MODE" != uefi ]; then
	if [ -z "$SKIP_BUILD" ]; then
		have_userland
		[ -d ios165/rootfs ] || die "no iOS files: ios165/fetch.sh first"
		env $FLASH_USERLAND_VARS userland/build.sh
	fi
	[ -f userland/flashboot.apfs ] && [ -f userland/root.apfs ] || die "no userland/flashboot.apfs and root.apfs (FLASHBOOT=1 userland/build.sh makes them)"
	SIGN=; KEYFILE=${OWNER_KEY:-$HOME/.hackios/owner.key}; [ -f "$KEYFILE" ] && SIGN="--sign $KEYFILE"
	LABEL="$(git describe --tags --always 2>/dev/null || echo build) $(date +%Y-%m-%d)"
	python3 common/flash/mkbundle.py -o out/$DEVICE-bundle.img --kernel $K --resident userland/flashboot.apfs \
		--ramdisk userland/root.apfs --cmdline "$FLASH_BOOTARGS $BOARD_ARGS" --label "$LABEL" $SIGN || die "no bundle"
	./xl health || die "the loader did not answer (xl enter: the phone in fastboot, or the menu's Developer mode)"
	confirm "Write out/$DEVICE-bundle.img ($(( $(stat -c %s out/$DEVICE-bundle.img) >> 20 )) MB) into the boot slot of the
phone's store (blocks 2042368..2091519) that does not hold the newest good bundle (shown above)?"
	./xl install out/$DEVICE-bundle.img || die "the bundle was not installed (the other boot slot is untouched)"
	./xl settime || echo "install-xl: the date was not set (iOS starts in 1970 until ./xl settime)"
	# the UEFI with a payload of the loader alone: no kernel in it, so the loader shows the menu and boots from the
	# flash. Made here, once: `fastboot boot` of this file is the preview, and `uefi` flashes this same file
	python3 xnuloader/mkpayload.py -o out/$DEVICE-uefi-payload.bin loader.efi=xnuloader/loader.efi > /dev/null || die "no payload"
	FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE UEFI_FD=$UEFI_FD DTB=$DTB UEFI_TARGET=$UEFI_TARGET \
		python3 mkxnuimg.py -p out/$DEVICE-uefi-payload.bin -o out/$DEVICE-uefi.img > /dev/null || die "no UEFI image"
	echo "install-xl: preview, nothing flashed: fastboot boot out/$DEVICE-uefi.img"
fi

if [ "$MODE" = install ] || [ "$MODE" = uefi ]; then
	IMG=out/$DEVICE-uefi.img
	[ -f $IMG ] || die "no $IMG: $0 $DEVICE bundle makes it"
	[ "$(head -c 8 $IMG)" = "ANDROID!" ] || die "$IMG is not a boot image"
	[ -f "$FLASH_BOOT_BACKUP" ] || die "no backup of $FLASH_BOOT_PART at $FLASH_BOOT_BACKUP: make one before writing it"
	[ "$(head -c 8 "$FLASH_BOOT_BACKUP")" = "ANDROID!" ] || die "$FLASH_BOOT_BACKUP is not a boot image"
	# an installed bundle has come up at least once: the loader's boot state says "last up G" for a G in a slot
	if [ -z "$FORCE_UEFI" ]; then
		./xl health > out/$DEVICE-health.txt || die "the loader did not answer"
		cat out/$DEVICE-health.txt
		up=$(sed -n 's/.*last up \([0-9]*\),.*/\1/p' out/$DEVICE-health.txt | tail -1)
		[ -n "$up" ] && [ "$up" != 0 ] && grep -q "generation $up \"" out/$DEVICE-health.txt ||
			die "no installed bundle has come up yet: fastboot boot $IMG first, wait for the home screen and a minute, then again"
	fi
	if [ "$(./xl state)" = loader ]; then
		# the loader writes it: no reset into the stock fastboot on the way, which does not come back on USB
		# after a reset with the cable in (2026-10-24). Only if the partition is byte for byte the backup
		# (its SHA-256, checked by the loader), then read back (xnuloader/flashboot.c, fb_flash_boot)
		echo "install-xl: backup of $FLASH_BOOT_PART: $FLASH_BOOT_BACKUP, sha256 $(sha256sum "$FLASH_BOOT_BACKUP" | cut -c1-16)..."
		confirm "Write $IMG ($(( $(stat -c %s $IMG) >> 10 )) KB, sha256 $(sha256sum $IMG | cut -c1-16)...) into $FLASH_BOOT_PART by the
loader, only if $FLASH_BOOT_PART is now exactly the backup? From then on the phone powers on into the HackiOS menu.
Undo: ./xl flashpart $FLASH_BOOT_PART <the backup's first 44437504 bytes> <this image + backup tail>, or fastboot flash $FLASH_BOOT_PART $FLASH_BOOT_BACKUP"
		./xl flashpart $FLASH_BOOT_PART $IMG "$FLASH_BOOT_BACKUP" || die "the loader did not write $FLASH_BOOT_PART (see above)"
	else
		# no loader up: the phone's own bootloader writes it (in its fastboot already: see xl state)
		[ "$(fastboot devices | wc -l)" = 1 ] || die "not exactly one device in fastboot: nothing written"
		product=$(fastboot getvar product 2>&1 | sed -n 's/^product: *//p')
		[ "$product" = "$FASTBOOT_PRODUCT" ] || die "the fastboot device is '$product', not $FASTBOOT_PRODUCT: nothing written"
		slot=$(fastboot getvar current-slot 2>&1 | sed -n 's/^current-slot: *//p')
		size=$(fastboot getvar partition-size:$FLASH_BOOT_PART 2>&1 | sed -n "s/^partition-size:$FLASH_BOOT_PART: *//p")
		[ "_$slot" = "${FLASH_BOOT_PART#boot}" ] || die "the bootloader starts slot _$slot, not ${FLASH_BOOT_PART#boot}: nothing written"
		[ -n "$size" ] && [ $(( size )) -ge $(stat -c %s $IMG) ] || die "$FLASH_BOOT_PART ($size) is smaller than $IMG"
		[ $(( size )) -eq $(stat -c %s "$FLASH_BOOT_BACKUP") ] || die "the backup is not the size of $FLASH_BOOT_PART ($size)"
		echo "install-xl: backup of $FLASH_BOOT_PART: $FLASH_BOOT_BACKUP, sha256 $(sha256sum "$FLASH_BOOT_BACKUP" | cut -c1-16)..."
		confirm "Flash $IMG ($(( $(stat -c %s $IMG) >> 10 )) KB, sha256 $(sha256sum $IMG | cut -c1-16)...) into $FLASH_BOOT_PART of
$product $(fastboot devices | cut -f1), the
partition the bootloader starts? From then on the phone powers on into the HackiOS menu.
Undo: fastboot flash $FLASH_BOOT_PART $FLASH_BOOT_BACKUP"
		fastboot flash $FLASH_BOOT_PART $IMG || die "fastboot flash failed: fastboot flash $FLASH_BOOT_PART $FLASH_BOOT_BACKUP puts the old one back"
	fi
fi

[ "$MODE" = install ] || [ "$MODE" = uefi ] || exit 0
cat <<MSG

Done. Power the phone on (or: fastboot reboot): the bootloader starts the UEFI in $FLASH_BOOT_PART, the loader shows
its menu for 5 s (volume keys move, power selects) and starts the newest good HackiOS.
  update:             common/flash/install-xl.sh $DEVICE update    (the menu's "Developer mode", or xl enter from fastboot)
  is it healthy:      ./xl health                               (the same: the phone in the loader)
  previous version:   "HackiOS, previous version" in the menu; a version that does not come up 3 times is skipped
  fastboot:           the bootloader's own key combination, as before; TWRP from its menu (recovery_a/_b untouched)
  remove the UEFI:    fastboot flash $FLASH_BOOT_PART $FLASH_BOOT_BACKUP
MSG
