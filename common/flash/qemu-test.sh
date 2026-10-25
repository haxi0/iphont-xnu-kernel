#!/bin/bash
# Test the installed-device boot in QEMU, with no phone: a disk laid out like one (boot_a, boot_b with
# Qualcomm A/B attributes, Android's slot _b, a darwin partition with boot bundles and the boot state in its
# boot area), the loader started with no payload, as the UEFI in boot_a would start it.
#
# usage: common/flash/qemu-test.sh [SCENARIO...]       (none: all of them; CI runs all)
#
#   boot        the countdown: the newest bundle boots, the kernel runs, the start is counted
#   previous    down + power: "HackiOS, previous version" boots the older bundle
#   guard       the newest was started 3 times and never came up: the countdown skips it, the older boots,
#               and the newest is remembered as given up on
#   fallback    no installed generation comes up: the countdown goes to Android (the A/B bytes exchanged)
#   android     down, down + power: back to Android by hand; the GPT is checked afterwards, and the choice is
#               remembered in the boot state
#   remembered  the last choice was Android: the countdown takes Android again, like a PC's boot menu
#   not-ours    started with Android's slot active (as `fastboot boot` of the image would): Android is chosen,
#               and nothing is written -- the slots are not exchanged into the wrong state
#   tampered    the newest bundle's contents changed after it was made: its SHA-256 refuses it
#   signed      a loader with an owner key pinned: the unsigned newest is refused, the signed older boots
#   forged      a pinned loader, a bundle signed by another key: refused, nothing boots, the menu stays
#   file        darwin as a file of userdata (no partition of its own): a map of three extents out of order on
#               a disk of 512-byte sectors; the bundle boots through the map, the boot state is written through
#               it, and not one byte of userdata outside the file changes
#   file-nomark the same file without its marker (deleted and made again, say): no darwin at all, nothing written
#   no-android  a phone with no Android (the OnePlus 8): no Android in the menu, and with every generation given up
#               on the countdown tries the USB link first (none in QEMU), then HackiOS; the A/B bytes never change
#
# Each bundle runs the prebuilt kernel with userland's `hello` as its root, so the test needs no files of iOS.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.."
W=out/qemu-flash; mkdir -p $W/esp/EFI/BOOT
EFI=${QEMU_EFI:-/usr/share/AAVMF/AAVMF_CODE.fd}; [ -f "$EFI" ] || EFI=/usr/share/edk2/aarch64/QEMU_EFI.fd
VARS=${QEMU_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}; [ -f "$VARS" ] || VARS=/usr/share/edk2/aarch64/QEMU_VARS.fd
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837; [ -f $K ] || K=prebuilt/mach.development.bcm2837
HELLO=${HELLO:-userland/hello}; [ -f $HELLO ] || HELLO=prebuilt/userland/hello
[ -f $HELLO ] || HELLO=../hackios-xnu-userland/prebuilt/userland/hello
PASS=0; FAIL=0; FAILED=""

bundle() {      # bundle OUT GENERATION [mkbundle args]
	local out=$1 gen=$2; shift 2
	python3 common/flash/mkbundle.py -o $out --kernel $K --ramdisk $HELLO --cmdline "-v loader_return_after=15" \
		--generation $gen --label "test gen $gen" "$@" > /dev/null
}

disk() {        # a fresh "phone": slot _a (ours) active, _b (Android's) inactive but successful
	rm -f $W/disk.img; truncate -s 9G $W/disk.img
	sgdisk -o -n 1:2048:+64M -c 1:boot_a -n 2:0:+64M -c 2:boot_b -n 3:0:0 -c 3:darwin $W/disk.img > /dev/null
	for bit in 48 49 50 52 53; do sgdisk -A 1:set:$bit $W/disk.img > /dev/null; done
	sgdisk -A 2:set:54 $W/disk.img > /dev/null
	DARWIN_START=$(sgdisk -i 3 $W/disk.img | awk '/First sector/ {print $3}')
	# boot_a holds HackiOS's image (a boot image header v1 named HACKiOS, as mkxnuimg.py makes it), boot_b an
	# Android one (header v4, no name): how the loader tells its own slot
	python3 -c "import struct, sys
h = bytearray(4096); h[:8] = b'ANDROID!'; struct.pack_into('<I', h, 40, 1); h[48:55] = b'HACKiOS'
a = bytearray(4096); a[:8] = b'ANDROID!'; struct.pack_into('<I', a, 40, 4)
open('$W/boot-ours.img', 'wb').write(h); open('$W/boot-android.img', 'wb').write(a)"
	dd if=$W/boot-ours.img of=$W/disk.img bs=512 seek=2048 conv=notrunc status=none
	dd if=$W/boot-android.img of=$W/disk.img bs=512 seek=$(sgdisk -i 2 $W/disk.img | awk '/First sector/ {print $3}') conv=notrunc status=none
}

put() {         # put FILE BLOCK: into darwin at 4 KiB block BLOCK
	dd if=$1 of=$W/disk.img bs=512 seek=$((DARWIN_START + $2 * 8)) conv=notrunc status=none
}

slot() { put $1 $((2042368 + $2 * 24576)); }

state() {       # state trying tries good bad [flags]: write a boot state
	python3 -c "import sys; sys.path.insert(0, 'common/flash'); import bundlefmt as f
open('$W/state.img', 'wb').write(f.make_state(trying=$1, tries=$2, good=$3, bad=$4, flags=${5:-0}))"
	put $W/state.img 2091520
}

read_state() {
	dd if=$W/disk.img bs=512 skip=$((DARWIN_START + 2091520 * 8)) count=8 status=none | python3 -c "
import sys; sys.path.insert(0, 'common/flash'); import bundlefmt as f
s = f.parse_state(sys.stdin.buffer.read()); print('none' if s is None else 'trying=%(trying)d tries=%(tries)d good=%(good)d bad=%(bad)d boots=%(boots)d' % s)"
}

ab() { echo "$(sgdisk -A 1:show $W/disk.img | tr -d '\n') / $(sgdisk -A 2:show $W/disk.img | tr -d '\n')"; }

# run LOADER KEYS UNTIL: boot QEMU, send KEYS once the menu is up, stop when UNTIL appears in the log
run() {
	local loader=$1 keys=$2 until=$3
	cp $loader $W/esp/EFI/BOOT/BOOTAA64.EFI; cp "$VARS" $W/vars.fd
	rm -f $W/log.txt $W/done; touch $W/log.txt
	# the keys go in once the menu is on the screen; the feeder then holds QEMU's input open until the end
	( until grep -qa "HACKiOS boot menu" $W/log.txt || [ -f $W/done ]; do sleep .5; done; sleep 1; printf '%s' "$keys"
	  until [ -f $W/done ]; do sleep .5; done ) 2> /dev/null |
		timeout ${QEMU_TIMEOUT:-300} qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 6G \
		-drive if=pflash,format=raw,readonly=on,file="$EFI" -drive if=pflash,format=raw,file=$W/vars.fd \
		-drive file=fat:rw:$W/esp,format=raw,if=virtio -drive file=$W/disk.img,format=raw,if=virtio \
		-display none -serial stdio -monitor none -no-reboot 2>&1 | stdbuf -o0 tr -d '\r' > $W/log.txt &
	local qp=$!
	for _ in $(seq 1 $(( ${QEMU_TIMEOUT:-300} * 2 ))); do
		grep -qaE "$until" $W/log.txt && sleep 2 && break
		kill -0 $qp 2>/dev/null || break
		sleep .5
	done
	touch $W/done
	pkill -f "file=$W/disk.img,format=raw" 2>/dev/null; wait $qp 2>/dev/null
}

expect() {      # expect NAME PATTERN...: every pattern in the log (a leading ! : must not be there)
	local name=$1 ok=1; shift
	for p in "$@"; do
		if [ "${p:0:1}" = "!" ]; then grep -qaE "${p:1}" $W/log.txt && { echo "   unexpected: ${p:1}"; ok=0; }
		else grep -qaE "$p" $W/log.txt || { echo "   missing: $p"; ok=0; }; fi
	done
	if [ $ok = 1 ]; then PASS=$((PASS + 1)); echo "PASS $name"
	else FAIL=$((FAIL + 1)); FAILED="$FAILED $name"; echo "FAIL $name"; grep -aE "flash:|menu:|ab:" $W/log.txt | head -30 | sed 's/^/   | /'; fi
}

L=xnuloader/loader.efi; [ -f $L ] || L=prebuilt/loader.efi
bundle $W/gen1.img 1; bundle $W/gen2.img 2
DOWN=$'\e[B'; ENTER=$'\r'

scenario() {
	echo "== $1"
	case $1 in
	boot)
		disk; slot $W/gen1.img 0; slot $W/gen2.img 1
		run $L "" "Darwin Kernel|loader_return_after expired"
		expect boot "booting slot 1, generation 2" "SHA-256 checked" "start 1 of generation 2 counted" "Darwin Kernel"
		[ "$(read_state)" = "trying=2 tries=1 good=0 bad=0 boots=1" ] || { echo "   state: $(read_state)"; FAIL=$((FAIL+1)); FAILED="$FAILED boot-state"; } ;;
	previous)
		disk; slot $W/gen1.img 0; slot $W/gen2.img 1
		run $L "$DOWN$ENTER" "flash: booting slot"
		expect previous "HackiOS, previous version" "booting slot 0, generation 1" ;;
	guard)
		disk; slot $W/gen1.img 0; slot $W/gen2.img 1; state 2 3 1 0
		run $L "" "flash: booting slot"
		expect guard "generation 2 did not come up 3 times: skipped" "booting slot 0, generation 1" "!booting slot 1"
		run_state=$(read_state); echo "   state after: $run_state"
		[ "$run_state" = "trying=1 tries=1 good=1 bad=2 boots=1" ] || { FAIL=$((FAIL+1)); FAILED="$FAILED guard-state"; } ;;
	fallback)
		disk; slot $W/gen1.img 0; slot $W/gen2.img 1; state 1 3 0 2
		run $L "" "ab: slots exchanged|ab: not exactly|ab: no unit"
		expect fallback "Android is the default" "menu: Android \(countdown\)" "ab: slots exchanged" "!booting slot"
		sgdisk -v $W/disk.img | grep -q "No problems found" || { echo "   GPT not sound"; FAIL=$((FAIL+1)); FAILED="$FAILED fallback-gpt"; } ;;
	android)
		disk; slot $W/gen1.img 0; slot $W/gen2.img 1
		run $L "$DOWN$DOWN$ENTER" "ab: slots exchanged|ab: not exactly|ab: no unit"
		echo "   A/B after: $(ab)"
		expect android "menu: Android$" "ab: slots exchanged"
		sgdisk -A 2:show $W/disk.img | grep -q "^2:50:1" || { echo "   boot_b is not active"; FAIL=$((FAIL+1)); FAILED="$FAILED android-ab"; }
		dd if=$W/disk.img bs=512 skip=$((DARWIN_START + 2091520 * 8)) count=1 status=none | python3 -c "
import sys; sys.path.insert(0, 'common/flash'); import bundlefmt as f
s = f.parse_state(sys.stdin.buffer.read()); sys.exit(0 if s and s['flags'] & f.STATE_LAST_ANDROID else 1)" ||
			{ echo "   the choice was not remembered"; FAIL=$((FAIL+1)); FAILED="$FAILED android-remembered"; }
		sgdisk -v $W/disk.img | grep -q "No problems found" || { echo "   GPT not sound"; FAIL=$((FAIL+1)); FAILED="$FAILED android-gpt"; } ;;
	remembered)
		disk; slot $W/gen1.img 0; slot $W/gen2.img 1; state 2 0 2 0 2
		run $L "" "ab: slots exchanged|ab: not exactly|ab: no unit|flash: booting slot"
		expect remembered "menu: Android \(countdown\)" "ab: slots exchanged" "!booting slot" ;;
	off-charging)
		# switched off with the cable in last time; this start is not the cable's (no PMIC here): the flag is
		# cleared, the countdown boots as ever
		disk; slot $W/gen1.img 0; slot $W/gen2.img 1; state 2 0 2 0 4
		run $L "" "flash: booting slot"
		expect off-charging "switched off on the charger last time" "booting slot 1, generation 2" "!off-mode charging \("
		dd if=$W/disk.img bs=512 skip=$((DARWIN_START + 2091520 * 8)) count=1 status=none | python3 -c "
import sys; sys.path.insert(0, 'common/flash'); import bundlefmt as f
s = f.parse_state(sys.stdin.buffer.read()); sys.exit(0 if s and not s['flags'] & f.STATE_OFF_CHARGING else 1)" ||
			{ echo "   the flag was not cleared"; FAIL=$((FAIL+1)); FAILED="$FAILED off-charging-flag"; } ;;
	not-ours)
		disk; slot $W/gen1.img 0
		# Android's slot _b active, ours _a inactive
		for bit in 50; do sgdisk -A 1:clear:$bit $W/disk.img > /dev/null; sgdisk -A 2:set:$bit $W/disk.img > /dev/null; done
		before=$(ab)
		run $L "$DOWN$ENTER" "Android's slot is the active one|ab: slots exchanged|ab: not exactly|ab: no unit|nothing written"
		expect not-ours "Android's slot is the active one already" "!ab: slots exchanged"
		[ "$(ab)" = "$before" ] || { echo "   the A/B bytes changed: $(ab)"; FAIL=$((FAIL+1)); FAILED="$FAILED not-ours-ab"; } ;;
	file|file-nomark)
		# a "phone" whose darwin is a file: boot_a, boot_b, userdata (9 GiB); the file's map, out of order
		rm -f $W/disk.img; truncate -s 10G $W/disk.img
		sgdisk -o -n 1:2048:+64M -c 1:boot_a -n 2:0:+64M -c 2:boot_b -n 3:0:0 -c 3:userdata $W/disk.img > /dev/null
		for bit in 48 49 50 52 53; do sgdisk -A 1:set:$bit $W/disk.img > /dev/null; done
		sgdisk -A 2:set:54 $W/disk.img > /dev/null
		dd if=$W/boot-ours.img of=$W/disk.img bs=512 seek=2048 conv=notrunc status=none 2>/dev/null || true
		UD=$(( $(sgdisk -i 3 $W/disk.img | awk '/First sector/ {print $3}') * 512 ))
		cat > $W/file.darwin <<JSON
{"kind": "file", "file": "/data/hackios/darwin.img", "host": "userdata", "blocks": 2097152, "mark": "0123456789abcdef",
 "extents": [[1300000, 800000], [10, 697152], [697200, 600000]]}
JSON
		DARWIN_ID=../$W/file.darwin OUT=../$W/loader-file.efi xnuloader/build.sh > /dev/null || { echo "   no file loader"; FAIL=$((FAIL+1)); return; }
		python3 common/flash/darwinid.py put $W/file.darwin $W/gen1.img $W/disk.img $UD 2042368
		if [ $1 = file ]; then
			python3 common/flash/darwinid.py mark $W/file.darwin $W/mark.img
			python3 common/flash/darwinid.py put $W/file.darwin $W/mark.img $W/disk.img $UD 2091521
		fi
		# the userdata blocks just outside every extent, and its first and last: none may change
		outside() { for b in 0 9 697162 697199 1297200 1299999 2100000 2300000; do
			dd if=$W/disk.img bs=4096 skip=$(( UD / 4096 + b )) count=1 status=none | sha256sum | cut -c1-12; done | tr '\n' ' '; }
		before=$(outside)
		if [ $1 = file ]; then until_re="Darwin Kernel|loader_return_after expired"; else until_re="Developer mode \(USB, xl\) in 1 s|nothing is written"; fi
		run $W/loader-file.efi "" "$until_re"
		if [ $1 = file ]; then
			expect file "darwin: a file of userdata, 2097152 blocks in 3 extents, marker right" "booting slot 0, generation 1" \
				"SHA-256 checked" "Darwin Kernel"
			st=$(python3 common/flash/darwinid.py get $W/file.darwin $W/disk.img $UD 2091520 1 | python3 -c "
import sys; sys.path.insert(0, 'common/flash'); import bundlefmt as f
s = f.parse_state(sys.stdin.buffer.read()); print('none' if s is None else 'trying=%(trying)d tries=%(tries)d boots=%(boots)d' % s)")
			[ "$st" = "trying=1 tries=1 boots=1" ] || { echo "   state through the map: $st"; FAIL=$((FAIL+1)); FAILED="$FAILED file-state"; }
		else
			expect file-nomark "no longer carries its marker" "!flash: booting slot" "!a file of userdata, 2097152"
		fi
		[ "$(outside)" = "$before" ] || { echo "   userdata outside the file CHANGED"; FAIL=$((FAIL+1)); FAILED="$FAILED $1-outside"; } ;;
	tampered)
		disk; cp $W/gen2.img $W/bad.img
		printf 'X' | dd of=$W/bad.img bs=1 seek=$((4096 + 5000)) conv=notrunc status=none
		slot $W/gen1.img 0; slot $W/bad.img 1
		run $L "" "flash: booting slot 0|Darwin Kernel"
		expect tampered "boot slot 1: contents do not match their SHA-256" "booting slot 0, generation 1" ;;
	signed|forged)
		export OWNER_KEY_H=$PWD/$W/owner_key.h
		rm -f $W/owner.key $W/other.key
		python3 common/flash/ownerkey.py init $W/owner.key > /dev/null
		rm -f $W/loader-pinned.efi
		OUT=../$W/loader-pinned.efi xnuloader/build.sh > /dev/null || { echo "   the pinned loader did not build"; FAIL=$((FAIL+1)); return; }
		unset OWNER_KEY_H
		if [ $1 = signed ]; then
			bundle $W/gen1s.img 1 --sign $W/owner.key
			disk; slot $W/gen1s.img 0; slot $W/gen2.img 1
			run $W/loader-pinned.efi "" "flash: booting slot|Darwin Kernel"
			expect signed "owner key pinned" "boot slot 1: not signed" "booting slot 0, generation 1 .*signature checked"
		else
			OWNER_KEY_H=$W/other.h python3 common/flash/ownerkey.py init $W/other.key > /dev/null
			bundle $W/gen3x.img 3 --sign $W/other.key
			disk; slot $W/gen3x.img 0
			run $W/loader-pinned.efi "" "menu: no USB link|resetting|Developer mode \(USB, xl\) in 1 s"
			expect forged "boot slot 0: the signature is not by key .*REFUSED" "!flash: booting slot"
		fi ;;
	no-android)
		rm -f $W/loader-noandroid.efi
		LOADER_DEFS=-DQEMU_NO_ANDROID OUT=../$W/loader-noandroid.efi xnuloader/build.sh > /dev/null ||
			{ echo "   the loader did not build"; FAIL=$((FAIL+1)); return; }
		disk; slot $W/gen1.img 0; slot $W/gen2.img 1; state 2 3 0 1
		before=$(ab)
		run $W/loader-noandroid.efi "" "flash: booting slot|ab: "
		expect no-android "developer mode is the default" "menu: no USB link here" "HackiOS is the default" "booting slot 1, generation 2" \
			"!(>| ) Android" "!ab: "
		[ "$(ab)" = "$before" ] || { echo "   the A/B bytes changed: $(ab)"; FAIL=$((FAIL+1)); FAILED="$FAILED no-android-ab"; } ;;
	*) echo "unknown scenario $1"; exit 2 ;;
	esac
}

ALL="boot previous guard fallback android remembered off-charging not-ours tampered signed forged file file-nomark no-android"
[ $# -gt 0 ] || set -- $ALL
for s in "$@"; do scenario "$s"; done
echo "qemu-test: $PASS passed, $FAIL failed${FAILED:+:$FAILED}"
[ $FAIL = 0 ]
