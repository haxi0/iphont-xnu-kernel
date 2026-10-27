# HANDOFF — start here when you begin a new session

Short, actionable, and dated. It does **not** replace [TODO.md](TODO.md) (roadmap), [KNOWN_ISSUES.md](KNOWN_ISSUES.md) (what is broken) or the subsystem pages;
it says where the work stands *today*. **Update this file after every substantial session** (rules in [../AGENTS.md](../AGENTS.md)); keep it under ~150 lines and move
anything that has aged into the right permanent page.

**Last updated: 2026-10-27 02:40 MSK** for the driver audit fixes below (before that 01:15 MSK, the Bluetooth audio transport). Earlier state: ~04:55 MSK by the scheduler session (sections 2, 3, 4, 7 for the scheduler; the rest as the documentation session left it at ~03:30). **Checked state** = what I ran/read on that date; **historical** = taken from earlier session notes and flagged as such.

## Driver audit fixes — 2026-10-27 ~02:40-02:55 MSK (built; partly verified in RAM runs on the phone)

Audit of the OnePlus drivers against OnePlus's kernel (OnePlusOSS sm8250 R11) and mainline Linux; fixes ported from them as data
(GPL code is not copied verbatim; wpa_supplicant's logic is BSD). Kernel builds clean (`out/kernel-build.log`, the usual symbolset
errors only), `tools/mk-ath11k.sh` builds, `tools/wpa-test/run.sh` 267 + 218 checks pass (new: PTK renewal case). Edited in
`xnu-src` and copied to `xnu/files` file by file (no `xnu/export.sh`). Uncommitted. Nothing flashed, no phone run yet.
- RPMh TCS 0 shared claim `hk_rpmh_claim/release` (`hk_board.c`), used by `instantnoodle_touch.c`, `hk_adsp.c`, `hk_cpufreq.c`, `hk_goodix.c` ([power.md](subsystems/power.md)).
- Wi-Fi PTK renewal + KRACK guards (`wifi/lx/src/lx_wpa.[ch]`, `lx_80211.c`, `tools/wpa-test`) ([wifi.md](subsystems/wifi.md#the-station-shared)).
- SM8250 thermal sensor map (`hk_thermal.c`) ([thermal.md](subsystems/thermal.md)).
- Touch: vendor status-event handling, chip reset, charger mode (`instantnoodle_touch.c`, hook in `instantnoodle_battery.c`).
- AW8697 factory f0 trim `haptic_trim=11` (`hk_haptic.c`, profile `common/devices/instantnoodle.conf`).
- I2C serial engines (GENI FIFO, `instantnoodle_i2c.c`), opt-in `i2c_geni=1` (read-only probe) / `2` (use). Default unchanged.
- Not done: panel sleep-in (Android's on-command already carries the PPS and was tried: [display.md](subsystems/display.md)); merging the
  8 command-database parsers / DSP stack copies (T29).
- **Phone runs (owner-approved, RAM only, 02:38-02:50 MSK):** `tools/t quick` `023834` boot PASS; `024422` PASS 7/7 (`haptic=5
  haptic_trim=11 i2c_geni=1`); `024633`/`024827` (`--keep`, the latter `i2c_geni=2`) boot PASS, `pods` FAIL only because iOS's "Low
  Battery 20%" alert covered the App Library (screenshot `t-024630-library.png`; battery 19-20 %, charging). Seen: TRIM_LRA `0x0b` read back;
  touch charger mode 2; touch/gauge/slider engines have FIFO and work in FIFO mode with 0 errors (touch read 2082 -> 368 us); no RPMh claim
  failures logged. Not exercised: audio DSP rails (`adsp=1`), Wi-Fi renewal, touch under a finger, touch error paths, thermal throttling.
- **Found (pre-existing):** a run's log no longer brings the kernel's message buffer back: the loader's `oem log` recomposes without it
  after the return (`compose_logs(NULL, 0)`), so plain kernel `printf` lines are lost (also why "IOLog lines no longer reach phone logs").
  Work-around used: touch, I2C and battery print through `say()` to the loader's log as `hk_buttons.c` does. Fix belongs in `xnuloader`
  (needs a loader reload, which the owner deferred).
- **Next ([TODO T37](TODO.md#t37-driver-audit-follow-ups-2026-10-27--drivers--partial--p2)):** the owner will run `i2c_geni=2` with the touch screen later (then make it the default); `adsp=1 sound=4` once to try the
  amplifiers' bus on its engine (quiet hours: `sound_amps=0`).

## Post-wake UI fix — 2026-10-26 23:50 MSK (Claude session that owns sleep/power; parallel to Bluetooth)

- Owner report: after sleep, Control Center empty, status bar items/bar gone, apps blank. **Fixed and verified on the phone
  (RAM runs only, nothing flashed):** `MiniIOMobileFramebuffer.cpp` `vsync()` now sends `mach_continuous_time()`; QuartzCore's
  frame clock lagged by all the time slept. Diagnosis and A/B in [power.md](subsystems/power.md#ui-missing-after-a-wake-fixed-2026-10-26).
  New diagnostic boot-arg `sleep_freeze=0` (`hk_power.c`). Both files exported to `xnu/files` by copy (not `xnu/export.sh`); uncommitted.
- Not in the installed build (T03 needs the owner's OK). Pre-existing: Control Center close after a wake is slow (KNOWN_ISSUES K10).
- Found, not fixed: `IOLog` lines (HKGPU, MiniIOSurface...) no longer reach phone logs (they did on 2026-10-25); `printf` does.
- Repro used `tools/t` through a temporary wrapper outside the repo; `tools/t` itself untouched. Phone left in the loader.

## Bluetooth audio transport — 2026-10-27 01:15 MSK (latest)

Owner-supervised RAM runs `005834`, `010727` (nothing flashed; phone back in loader). **A2DP heard** (lock sound, record chime).
**HFP/SCO UART stall fixed** in `hk_bt.c` (uncommitted, exported by copy to `xnu/files`): legacy SCO set-up sent as Enhanced with
HCI data path (`bt_sco_hci`), SCO paced to the chip's own packets (`bt_sco_pace`); `010727`: 3 voice links, 0 drops, no restart.
**Voice Memos through buds: owner-verified working in `012839`** (record + playback). Its crash in `011859` was CoreAudio's
uncaught `thread_policy_set realtime failed` (KERN_INVALID_ARGUMENT); `xnu-src/.../osfmk/kern/thread_policy.c` now cuts an
over-50-ms computation (`rt_clamp`, logged) -- not exported to `xnu/hackios.patch` yet, and not proven necessary (no cut logged in 012839).
`010727`'s other failure (buds answering after 3 s, 1809) did not recur. Details: [bluetooth.md](subsystems/bluetooth.md#bluetooth-audio-fixed-at-the-transport--2026-10-27-0115).
Also changed: `userland/src/sound_test.c` (`bluetooth-level`, max 0.1), new `userland/rc.jobs.btaudio`, `tools/bt/check.py` (SCO fixtures; 8/8 PASS).

## Recording diagnosis — 2026-10-27 00:36 MSK

Owner reports Voice Memos crash, silent lock sound, brief black screen then recovery; buds disconnected.
`002629` shows Voice Memos selects CMF HFP input/output at 00:27:33; VirtualStream reports client-format
error 'what' for 16-bit/8 kHz/mono. Standard HCI Setup Synchronous Connection completes status 0. Audio
server PID changes 40 → 239 around 00:27:56. No usable crash stack in that run; do not assert root cause.
Lock sound remains deliberately muted by `sound_amps=0`; black-screen cause is unproven. Sleep agent owns it.
Silent RAM isolation `003355-run.log.live.*` (exec 22955) enabled `exctrace=1 applog=1`, no buds. Existing
`record_test 2` selects MicrophoneBuiltIn, records 96000 non-silent samples/two seconds successfully; Voice
Memos launches without a captured exception. Test ended normally after 73 s; phone returned to loader.
No source fix justified yet. Next physical gate: re-pair buds and repeat Voice Memos Record under exception
tracing, preserving HFP/SCO and audio-server logs. Quiet-tone approval persists, but no new tone played.
Work stopped for that gate. No sleep/power edits, flash, pairing-privilege changes or persistent recording.

## Bluetooth RAM audio test — 2026-10-27 00:28 MSK (ended)

Owner authorized RAM restart and quiet playback. Shared phone lock was free; used current kernel including
Claude's sleep/display fixes without changing their files. Run `002629-run.log.live.*`, exec session 57319:
`bt=1 bt_trace=1 adsp=1 sound=4 mic=4 sound_amps=0`, native BT HAL bundle present, DSP/mic/controller up.
Settings shows CMF Buds Pro 2 Connected. Diagnostic selected BluetoothA2DPOutput/CMF Buds Pro 2, volume 0.30;
played 2 s at 1% sample peak, AudioQueueStart 0, 96000/96000 frames accepted. Native AVDTP START accepted,
OPEN → STREAMING at 00:27:20; no Bluetooth restart observed. **Heard output awaits owner confirmation.**
No microphone/bond/sleep-reconnect proof. No new driver edit, flash, loader replacement or automatic pairing confirmation.
Manual window ends after the queued 120-second post-tone hold (max run 420 seconds); preserve logs on return.

## Bluetooth audio diagnosis — 2026-10-26 22:30 MSK (historical)

- Owner paired CMF Buds Pro 2 in native Settings. `221539-run.log.live.*` + screenshot: Connected; HCI Simple
  Pairing/Authentication Complete status 0; Encryption Change 0/enabled 1; AAC selected; AVDTP CONFIG/OPEN
  accepted, media channel OI_OK/OPEN. Owner reports no heard sounds and Voice Memos recording fails.
- **Found actual audio gap:** build overlay exposes only HKAudio; original BTAudioHALPlugin.driver was excluded
  by select-system.py. bluetoothd publishes audio devices but CoreAudio has no Bluetooth device plug-in.
  Fixed selection + opt-in packaging; original unmodified bundle extracted locally from rootfs.dmg, never added to git.
- Previous test boot also omitted `adsp=1 sound=4 mic=4` and forced `sound_amps=0`; built-in silence/recording failure
  cannot diagnose Bluetooth hardware from that boot. Do not infer Voice Memos selected the buds' microphone.
- Owner authorized **quiet** audio. Added `sound_test bluetooth-tone 180`: activates Playback, waits for exactly
  CMF Buds Pro 2/BluetoothA2DPOutput, then plays 2 seconds at 1% sample peak; other routes/timeouts play nothing.
  Host gate fixtures PASS (not framework proof); HAL selection/staging/original-byte identity and H4/NVM/UUID checks PASS.
- Native userland build with HAL plug-in, diagnostic tone, timeout bounds and opt-out cleanup PASS; no new
  hardware boot yet. Final host checks PASS. Playback still unverified.
  Further transport blocker may appear: native plug-in contains AudioSkywalkPipe/AudioRingBuffer; inspect runtime logs.
- Current loader is old (`./iphont agent`: no mailbox), cannot exec into running iOS. Existing manual run ended after
  638 seconds and returned to loader; no agent tone was played. Next test needs a RAM boot/re-pair and mediaserverd logs.
- **Coordination:** owner says another Claude agent owns deep/system sleep. Leave sleep/power code to that agent;
  use the shared phone lock for every run; record Bluetooth retention/reconnect dependencies without changing its work.
  No loader replacement, flashing, private pairing privileges, key export, or parallel transport authorized.
- Next owner action is re-pairing the buds in the corrected RAM test. Stop at that physical gate; quiet-tone approval
  persists. Diagnostic recipe: `adsp=1 sound=4 mic=4 sound_amps=0 bt=1 bt_trace=1`; native HAL route is the test gate.
  Preserve ignored userland sources and uncommitted drivers; no commit/push requested. Earlier notes are historical.

Earlier implementation details: [Bluetooth subsystem](subsystems/bluetooth.md#archived-implementation-handoff-historical-2026-10-26-2142).

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
