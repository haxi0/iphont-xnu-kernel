# HANDOFF — start here when you begin a new session

Short, actionable, and dated. It does **not** replace [TODO.md](TODO.md) (roadmap), [KNOWN_ISSUES.md](KNOWN_ISSUES.md) (what is broken) or the subsystem pages;
it says where the work stands *today*. **Update this file after every substantial session** (rules in [../AGENTS.md](../AGENTS.md)); keep it under ~150 lines and move
anything that has aged into the right permanent page.

**Last updated: 2026-10-26 ~04:55 MSK** by the scheduler session (sections 2, 3, 4, 7 for the scheduler; the rest as the documentation session left it at ~03:30). **Checked state** = what I ran/read on that date; **historical** = taken from earlier session notes and flagged as such.

## 1. State of the project (checked 2026-10-26)

- Active device: **OnePlus 8 (instantnoodle)**, no Android on it, iOS 16.5 user space running from its own flash. Installed boot verified 2026-10-24 (historical, owner-confirmed); iOS boots to SpringBoard, touch, 90 Hz display,
  GPU (OpenGL ES), speakers, main mic, haptics, flashlight, battery, Wi-Fi (RAM runs), sleep/wake, HFS+ `/private/var` work per [devices/instantnoodle.md](devices/instantnoodle.md).
- Redwood (POCO X5 Pro): not touched recently, owner asked not to run POCO checks.
- Everything is **uncommitted**: git HEAD of the kernel repo is `de24a37` (2026-10-25), of the userland repo `d2c1e87`. Work since 2026-10-25 ~15:00 (GPU collapse, brightness,
  `ca_ambient`, vmd DENY, Wi-Fi station work, rendering fixes, `tools/t`, wakeperf/look) lives in the working tree only.
- Docs: this overhaul created `docs/{README,architecture,BUILD,BOOT_ARGS,TODO,KNOWN_ISSUES,HANDOFF}.md`, `docs/{devices,subsystems,research}/`, `AGENTS.md`, `CLAUDE.md` and moved images to
  `docs/images/` — **all uncommitted**; run `git add -A docs AGENTS.md CLAUDE.md README.md` (+ `xnu/README.md`, `uefi/README.md`, `prebuilt/NOTICES.md`) in the **kernel** repo. `docs/air*.md` belong to the **userland** repo.

## 2. Uncommitted changes a new session must preserve

Check with `git status --short` **and** `common/ugit status --short` (many paths appear in both; see [architecture.md](architecture.md#two-repositories-one-tree)). 2026-10-26 03:25: 68 modified/deleted entries in the kernel
repo, 39 modified in the userland repo. Do not `git checkout`/`reset`/`stash` them. Groups:

| Group | Paths (representative) | Owner/meaning |
|---|---|---|
| Wi-Fi station and QCA6390 (verified in RAM runs, 2026-10-25) | `wifi/lx/src/*`, `wifi/lx/include/lx_more.h`, `xnu/files/bsd/dev/hk_wifi.c`, `hk_lx.c`, `hk_sta.c`, `hk_apple80211.c`, `hk_net.c`, `sys/hk_{lx,net,sta}.h`, `tools/mk-ath11k.sh`, `tools/wpa-test/run.sh` | WPA3 H2E, TKIP, private MAC etc. |
| Power / GPU / display (GPU collapse and brightness verified at command level 2026-10-26) | `hk_power.c`, `hk_adreno.c`, `HKGPU.cpp`, `hk_cpufreq.c`, `MiniIOMobileFramebuffer.cpp`, `loader_return.c`, `GenericARMPE.cpp`, `MiniDrivers.h`, `instantnoodle.h`, `hk_input.c/.h`, `instantnoodle_{i2c,touch}.c` | `xnu/export.sh` was run for round 3 addenda; **re-check that `xnu/` matches `xnu-src/`** before exporting again |
| Tools | `xl`, `xnu-build.sh`, `tools/frames.py`, `tools/t`, `tools/look/`, `tools/wakeperf/`, `tools/sleep-test/` | test harness (short tests) |
| Profile | `common/devices/instantnoodle.conf` | sound/sb_clock args |
| Userland repo | `userland/build.sh`, `mkjobs.py`, `src/eagl.c`, `inject.c`, `inject_sb.c`, `rwtouch.c`, `power_test.c`, `hkprof.h`, ... | rendering fixes, vmd DENY, ca_ambient, profilers; new files need `common/ugit add -f` |
| Docs | all of `docs/`, `AGENTS.md`, `CLAUDE.md`, `README.md` | this overhaul |

**Scheduler work (2026-10-26 02:20-04:55, done for now, uncommitted):** Edge scheduler + performance controller ([subsystems/scheduler.md](subsystems/scheduler.md)).
`xnu/export.sh` was run at 04:48 after checking `xnu/` was not ahead of `xnu-src/` (only `hk_cpufreq.c` and `GenericARMPE.cpp` differed, both newer in `xnu-src`); the export
also picked up other sessions' `xnu-src` edits then present (`bsd_init.c`, `kern_exit.c`, `IOService.cpp`, `exception.c`, `osfmk/conf/files.arm64`, `iokit/conf/files`, `bsd/sys/Makefile`).
New files to add when committing: kernel repo `xnu/files/bsd/dev/hk_perfctl.c`, `hk_sched_check.c`, `tools/sched-test/{qemu.sh,amp.py}`, `docs/subsystems/scheduler.md`;
userland repo `userland/src/sched_test.c` (`common/ugit add -f`). Baseline kernel binary from before the work: `out/sched-baseline/mach.development.bcm2837` (`XL_KERNEL=`).
A second session is building a **developer toolkit** (`tools/hkdev/`, `docs/TOOLKIT.md`, edits to TESTING/README/INSTALL command references). Ask before editing a file that is modified and not yours.

## 3. Active priorities (see TODO.md for all)

1. **T01/T02** verify power and Wi-Fi-through-sleep on the phone (safety paths unverified): [TODO.md](TODO.md#t01-verify-the-unverified-power-work-on-the-phone--power--built-unverified--p0).
2. **T11** scheduler — working (Edge default, `sched=clutch` baseline); open points in [TODO.md](TODO.md#t11-heterogeneous-cpu-scheduling--cpu--partial-working-uncommitted--p1).
3. **T03** install the current build (needs the owner's explicit approval; writes flash).
4. **T04/T05/T07/T08** missing hardware: sensors, Bluetooth, OnePlus cameras, headphones.
5. **T10** Metal usability; research notes in `docs/air.md` (userland repo).

## 4. Recent changes worth knowing (2026-10-24 .. 26, historical summary, details in the pages)

- 10-24: speakers, alert slider, main mic, HFS+ `/private/var`, installed boot on the OnePlus 8 ([audio.md](subsystems/audio.md), [storage.md](subsystems/storage.md)).
- 10-25: sleep/wake rounds 1-4 and the GIC shared-interrupt fix ([power.md](subsystems/power.md)); Wi-Fi QCA6390 ([wifi.md](subsystems/wifi.md)); App Library vibrancy fix; custom boot logo; `tools/t` ([TESTING.md](TESTING.md)).
- 10-26: **Edge scheduler on three clusters with our CLPC replacement** (`hk_perfctl.c`; boot-arg `sched=`; `config_pervasive_energy` on; `kern.hackios.amp`);
  membench threads bound and self-starting (a caller-starvation hang under `sched=clutch`).
- 10-26: GPU block collapse in sleep (133 ms wake), hwcg, touch boost, brightness service, `ca_ambient`, vmd denied (post-wake CPU), kernel probes (`top`, `cpuspeed`, `memlat`, `membench`, `kprof`, `hkprof`). The installed bundle predates these (T03).

## 5. Blockers
- Owner approval for anything that writes flash (`userdata`, `boot_b`), swaps slots/ABL/XBL/TZ (forbidden) or runs audibly.
- SoC low-power states are architecturally blocked ([power.md](subsystems/power.md)). Sensors need the DSP's SEE client; App Store/FairPlay need Secure Enclave behaviour.
- No Android on the OnePlus 8: its vendor/persist/modem firmware only exists in backups outside the repo (`../ios-instantnoodle/`).

## 6. Verified findings to rely on (details in pages)
CrashDump triggers ([research/hardware-qualcomm.md](research/hardware-qualcomm.md)); never DCS 0x10; never `touchscreen=1` on the OnePlus; Android's QCA6390 firmware asserts under ath11k (use linux-firmware);
GIC per-core shutdown must not quiesce shared SPIs; `vmd` was the post-wake CPU hog; sleep timers use wall time; tests: `tools/t host` 2 s, `quick` ~70 s, `power` ~2 min.

## 7. Tests performed and results (checked unless marked)
- 2026-10-26 02:40-04:50 (scheduler session): `tools/sched-test/qemu.sh` 4/4 modes PASS; phone `tools/t boot sched` Edge 8/8 kernel checks, A/B workloads (scheduler.md);
  `tools/t full` Edge 16/17 and Clutch 16/17 (both fail only "frames after sleep" phase 2 after 5 rapid sleeps, ~66 fps: pre-existing); one false `sleep` FAIL (K24).
- 2026-10-26 (historical, from session notes): `tools/t power` 10 sleeps with GPU CX/GX off, wake 133 ms; `tools/t brightness`: 20/90/5/100% reach the panel as 0x52/0x67a/0x6/0x7ff; `tools/t full` passed except a noisy frames-after-wake phase (verdict loosened).
- 2026-10-25 (historical): `common/flash/qemu-test.sh` 14/14; `tools/sleep-test/run.sh` 4/4; phone: 3/3 sleeps, power-key wake, WPA2/TKIP-mixed/WPA3 joins.
- This documentation session ran **no hardware tests** and made no functional changes. It checked links and referenced paths mechanically with a throw-away script (not kept in the repository).

## 8. Where things are
Index [README.md](README.md) · architecture [architecture.md](architecture.md) · build [BUILD.md](BUILD.md) · boot-args [BOOT_ARGS.md](BOOT_ARGS.md) · tests [TESTING.md](TESTING.md) ·
research [research/README.md](research/README.md) · agent rules [../AGENTS.md](../AGENTS.md) · phone logs `logs/` (`t-<time>.txt`, `<time>-run.log`, `.live` copies) · build log `out/kernel-build.log`.

## 9. Recommended immediate next actions
1. Read `git status` of both trees; commit the docs (kernel repo) without touching the others' files.
2. Ask the owner for a short supervised phone session: `tools/t power`, then T01 charger/off-mode checks.
3. When the scheduler session finishes, export (`xnu/export.sh` after syncing `xnu-src` to the patch), update [subsystems/cpu.md](subsystems/cpu.md), TODO T11.
4. Pick one missing-hardware item (T04 sensors is the most valuable for iOS behaviour) and start with its first read-only probe.

## iphont continuation — 2026-10-26

- New agent CLI is **`./iphont`**, Python standard library, not `tools/hkdev`/Rust. Start with `./iphont describe` and [TOOLKIT.md](TOOLKIT.md). Host modules, `xnuloader/iphont.c`, `xnu/files/{bsd/dev/iphont.c,bsd/sys/iphont.h,iokit/Kernel/IPhont.cpp,osfmk/arm64/iphont_threads.c}`, kernel hooks, and the userland daemon/job are uncommitted. New userland files need explicit `common/ugit add -f` when a commit is requested.
- Preserved scheduler/Wi-Fi work. Added only a narrow `hk_touch_inject` API to the scheduler-owned input files: same queue/boost, no physical recovery-chord counting. Apple-file patch sections were regenerated selectively; **no whole-tree export from a stale private tree**. `xl` log/capture staging and fetches now hold one USB lock. No legacy script was deleted; [TOOLKIT-MIGRATION.md](TOOLKIT-MIGRATION.md) inventories 170 owned scripts/modules and the parity gates.
- Verified: 32 host checks including the real loader C protocol fixture; existing `tools/t host` 4/4; native loader/daemon builds; clean private XNU compile/link (known export checks still fail); APFS verified by both readers; seven real XNU QEMU probes and a timed return. Physical phone: state/log snapshots/screenshots and five PNG captures (~1.6 fps), without changing the run. This is **not** mailbox, daemon, touch, or keyboard hardware validation.
- **Owner deferral:** “Continue host/build checks; leave phone running.” No reboot, slot change, partition write, or new physical boot was performed. The next physical test needs that restriction lifted and coordination with the running phone session, including the known cable replug requirement.
- Ignored reviewable artifacts are in `out/iphont/`: `loader-ram.img`, new `loader.efi`, `iphontd`, diagnostic `root.apfs`, private `xnu-clean/.../mach.development.bcm2837`, host/QEMU/build/image reports, and capture files. `prepare` packages a RAM-only instantnoodle image and never touches the phone. The new root is md1 diagnostic content; preserve the ordinary resident md0 flashboot workflow.
- **Build trap found:** copied `BUILD/` embeds absolute `SOURCE_DIR` paths into the original checkout. A copy can otherwise compile concurrent scheduler edits against stale private headers. The clean private build discarded only its own generated `BUILD/`; shared builds/source were not reset. Use `out/iphont/xnu-clean`, not the older mixed-path `xnu-integrated` artifact, for the next RAM test.
- Remaining scope is T36/K22: supervised USB/agent/control tests, framework/entitlement behavior, deeper GPU/service tests, recovery/deployment parity, function symbolication, and legacy removal after evidence. No claim of comprehensive completion.

Validation note: source whitespace checks pass. Full `git diff --check` flags unified-diff context whitespace inside the stored `xnu/hackios.patch`; raw source checks are separate. The original concurrent scheduler/Wi-Fi/docs changes remain uncommitted and were not reset.
