#!/bin/bash
# The scheduler in QEMU, no root image (the kernel waits for one; what is tested happens meanwhile): 8 cores, either
# as one cluster or described as the phones' three (board_cpu_domains, board_small_cores: cores 0-3 small), with the
# Edge scheduler or the Clutch baseline. Passes when the kernel came up on every core, the clusters are what was
# asked, the in-kernel scheduler checks passed (boot-arg sched_check, bsd/dev/hk_perfctl.c) and there was no panic.
# usage: tools/sched-test/qemu.sh [edge3|edge1|clutch|clutch3 ...]   (default: all four; ~70 s each)
#        SECS=N for a longer run; EXTRA="boot-args" adds to the command line
set -u
cd "$(dirname "$(readlink -f "$0")")/../.."
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837; [ -f $K ] || K=prebuilt/mach.development.bcm2837
mkdir -p out
# (a host without /usr/share/edk2, NixOS: QEMU's own copy of the aarch64 UEFI)
if [ -z "${QEMU_EFI:-}" ] && [ ! -f /usr/share/edk2/aarch64/QEMU_EFI.fd ] && command -v qemu-system-aarch64 >/dev/null; then
	Q=$(dirname "$(readlink -f "$(command -v qemu-system-aarch64)")")/../share/qemu
	export QEMU_EFI=$Q/edk2-aarch64-code.fd QEMU_VARS=$Q/edk2-arm-vars.fd
fi
modes=${*:-edge3 edge1 clutch clutch3}
fail=0
for mode in $modes; do
	case $mode in
	edge3)   args="sched=edge board_cpu_domains=0x21110000 board_small_cores=0x0f" want_clusters=3 want_sched=edge ;;
	clutch3) args="sched=clutch board_cpu_domains=0x21110000 board_small_cores=0x0f" want_clusters=1 want_sched=clutch ;;
	edge1)   args="sched=edge" want_clusters=1 want_sched=edge ;;
	clutch)  args="" want_clusters=1 want_sched=clutch ;;
	*) echo "sched-test: unknown mode $mode"; exit 2 ;;
	esac
	LOG=out/sched-test-$mode.log
	python3 xnuloader/mkpayload.py -o qemu/payload.bin kernel=$K \
		--cmdline "loader_return_after=${SECS:-45} cpus=8 small_cores_after=5 sched_check=1 syssleep=1 sleep_test=2 sleep_test_count=2 sleep_max=2 $args ${EXTRA:-} -v" >/dev/null || exit 1
	QEMU_SMP=8 qemu/run.sh $(( ${SECS:-45} + 60 )) > "$LOG" 2>&1
	clusters=$(grep -a -o "registered, cluster [0-9]*" "$LOG" | sort -u | wc -l)
	running=$(grep -a -o "GenericARMPE: the slower cores started too: [0-9]* processors running\|GenericARMPE: [0-9]* of 8 processors are running" "$LOG" | tail -1)
	sched=$(grep -a -o "GenericARMPE: scheduler [a-z]*" "$LOG" | head -1 | awk "{print \$3}")
	checks=$(grep -a -c "sched_check: PASS" "$LOG"); bad=$(grep -a -c "sched_check: FAIL" "$LOG")
	panic=$(grep -a -m1 -E "panic\(|Kernel panic" "$LOG")
	back=$(grep -a -c "sleep_test: back from sleep" "$LOG")
	ok=1
	[ "$clusters" = "$want_clusters" ] || ok=0
	[ "$sched" = "$want_sched" ] || ok=0
	[ -z "$panic" ] && [ "$bad" = 0 ] || ok=0
	grep -a -q -- "--- loader_return_after expired ---" "$LOG" || ok=0
	printf 'sched-test %-8s %s: scheduler %s, %s cluster(s), %s; %s sleeps back; checks %s passed %s failed%s\n' "$mode" \
		"$([ $ok = 1 ] && echo PASS || echo FAIL)" "${sched:-?}" "$clusters" "${running:-no start line}" "$back" "$checks" "$bad" "${panic:+; $panic}"
	if [ $ok = 0 ]; then
		fail=1
		grep -a -E "sched_check: FAIL|panic|GenericARMCPU|Scheduler" "$LOG" | head -12
	fi
done
exit $fail
