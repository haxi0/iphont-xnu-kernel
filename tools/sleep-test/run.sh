#!/bin/bash
# The platform's system sleep in QEMU (4 cores, GICv3, PSCI): the kernel puts itself to sleep sleep_test_count times
# from GenericARMPE::start (boot-arg sleep_test, iokit/Kernel/GenericARMPE.cpp), each woken by sleep_max after 2 s,
# with a driver's shared interrupt attached. Passes when every sleep came back with all cores running and that
# interrupt still on, and the kernel's timers ran after the sleeps (loader_return_after expires). No root image: the
# kernel waits for one, and the sleeps happen meanwhile.
# usage: tools/sleep-test/run.sh            (needs the kernel built, or prebuilt/, and QEMU with aarch64 UEFI)
set -u
mkdir -p out
cd "$(dirname "$(readlink -f "$0")")/../.."
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837; [ -f $K ] || K=prebuilt/mach.development.bcm2837
N=4
LOG=out/sleep-test.log
python3 xnuloader/mkpayload.py -o qemu/payload.bin kernel=$K \
	--cmdline "loader_return_after=60 cpus=4 syssleep=1 sleep_test=2 sleep_test_count=$N sleep_max=2 -v" >/dev/null || exit 1
QEMU_SMP=4 qemu/run.sh 170 > "$LOG" 2>&1
back=$(grep -a -c "sleep_test: back from sleep [0-9]*, 4 processors running, 1 shared interrupts on" "$LOG")
lost=$(grep -a -c "NOT AS BEFORE" "$LOG")
if [ "$back" -ge "$N" ] && [ "$lost" -eq 0 ] && grep -a -q -- "--- loader_return_after expired ---" "$LOG"; then
	echo "sleep-test: $N of $N sleeps came back, every core running and the driver's interrupt on"
	exit 0
fi
echo "sleep-test: FAILED ($back of $N back as they were; the log: $LOG)"
grep -a -E "sleep_test|sleep: |panic\(|psci: " "$LOG" | head -20
exit 1
