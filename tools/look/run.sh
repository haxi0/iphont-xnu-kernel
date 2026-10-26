#!/bin/bash
# tools/look/run.sh OUTDIR [BOOTARGS...]: one run of rc.jobs.look (the lock screen, the home screen, the App Library
# standing and scrolled: build the images with RC_JOBS=rc.jobs.look and `xl ramdisk userland/flashboot.apfs` first),
# a screenshot every 8 s into OUTDIR as NNNN-<seconds since the run began>.png. For comparing renderers:
#   tools/look/run.sh logs/look-gl gl=1 ; tools/look/run.sh logs/look-sw gl=0
# then tools/look/diff.py logs/look-sw logs/look-gl.
set -e
cd "$(dirname "$(readlink -f "$0")")/../.."
out=${1:?usage: run.sh OUTDIR [BOOTARGS...]}; shift
mkdir -p "$out"
. common/lib.sh; load_device instantnoodle > /dev/null
XL_SECS=${XL_SECS:-190} XL_FOLLOW=10 XL_CMDLINE="cpus=8 cpufreq=1 bootcpu=7 ufs=2 root_apfs=ufs_darwin gpu=1 sb_clock=24 log_launchd=0 $*" \
    ./xl run > "$out/xl.out" 2>&1 &
run=$!
began=$(date +%s); n=0
sleep 20
while kill -0 $run 2>/dev/null; do
	timeout 60 ./xl screenshot "$out/$(printf %04d $n)-$(( $(date +%s) - began )).png" > /dev/null 2>&1 || true
	n=$((n + 1)); sleep 8
done
wait $run || true
