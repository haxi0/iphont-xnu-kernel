# HackiOS architecture

How the whole system fits together, as implemented today. Detail per subsystem is in [subsystems/](subsystems/); what is
unfinished is in [TODO.md](TODO.md); intended-but-absent pieces are marked *not implemented*.

**One sentence:** an EFI application (our loader, running under a Mu-Silicium UEFI) loads Apple's open-source XNU
(`xnu-8796.121.2`, iOS 16.5's kernel, built from the generic-ARMv8 `BCM2837` configuration with our platform code) on a
Snapdragon phone, with kernel stand-ins for Apple's closed drivers, and the **unmodified iOS 16.5 user space** runs on top.

## Boot chain

```
phone ROM → XBL → ABL (stock fastboot)
      │  fastboot boot IMAGE            (RAM-only developer run)      or   boot_b / spare-slot boot partition (installed)
      ▼
UEFI (Project Silicium / Mu-Silicium port, uefi/) ── BootShim copies the "XNUP" container to PAYLOAD_BASE ──┐
      │ BootManagerLib starts the loader                                                                    │
      ▼                                                                                                      │
xnuloader (EFI application, stays resident; core 0)  ◄───────────── kernel image + root RAM disk + cmdline ◄─┘
   │  - picks free RAM → XNU's physical window, builds an Apple-format device tree + boot_args
   │  - serves: USB link (fastboot protocol, `xl`), storage (UFS) mailbox, battery/charger, watchdog, log
   │  - installed mode: boot menu, bundles in the flash, boot guard, signature checks
   ▼  jump to kernel on another core (bootcpu=7), MMU off → kernel sets up its own
XNU (xnu/ patch + files) ──► init (process 1) ──► iOS launchd ──► backboardd, SpringBoard, apps
   ▲                                                  injected libraries of ours (libhackios*.dylib)
   └── on end/panic/xl stop: returns to the loader stub with the log (osfmk/arm64/loader_return.c)
```

Details: [research/boot-chain-and-recovery.md](research/boot-chain-and-recovery.md), [subsystems/cpu.md](subsystems/cpu.md).

### The loader (`xnuloader/`)
`loader.c` (about 3,800 lines): `struct platform` per device (picked by the framebuffer base the UEFI reports; QEMU's
`qemu-virt` must still match, no match = ramfb), device-tree building, kernel run on another core (`run_on_other_cpu`, watched
with a heartbeat), UEFI-environment save/restore for the return, USB device mode, storage service, log writing, PMIC reset
control. `flashboot.c`: boot menu, bundle slots, boot state, boot guard, `fb_install`, off-mode charging. `crypto.c`: SHA-256
and Ed25519 (checked against Python in `tools/crypto-test`). `darwinfile.c`: finding the port's partition/file. `mkpayload.py`:
the XNUP container. `identity.h`: the iPhone identity.

### Memory layout (see also research/boot-chain-and-recovery.md)
- UEFI image at `FD_BASE`/`FD_SIZE` (profile); payload container at `PAYLOAD_BASE` = `0xF0000000` (both phones).
- Kernel window: the largest free block, up to 6 GB (bootstrap tables cover 8); kernel text 48 MB above the base; the resident
  root RAM disk at a fixed offset (`0x08000000`) in the window, with 0x30000000 left for the kernel itself.
- Framebuffer: redwood `0xE1000000`, OnePlus 8 `0x9C000000` (+ second buffer `0x9CA00000` inside the firmware's
  `cont_splash_region`); GPU/IO maps are limited by the 8 MB arm IO submap.
- QEMU: `virt` with up to 7 GB, the same 4.2 GB kernel window.

## Kernel (`xnu/`, applied to Apple's tree)

`xnu/hackios.patch` changes Apple's files; `xnu/files/` are new files at their XNU paths; `apply.sh`/`export.sh` move between
this repo and `xnu-src/`. Subsystems, in the order they appear in the source tree:

| Layer | Files | Doc |
|---|---|---|
| Platform: GICv3, timer, PSCI SMP, loader return, user-exception trace, stubs for files Apple left out | `osfmk/arm64/*` | [cpu.md](subsystems/cpu.md) |
| Platform expert + stand-ins for closed drivers | `iokit/Kernel/*`, `libkern/amfi/*`, `bsd/kern/kern_minimac.c`, `osfmk/minicrypto/*` | [research/kernel-standins.md](research/kernel-standins.md) |
| Filesystems | `bsd/miscfs/apfs/` (read, in-place write), `bsd/miscfs/ramfs/`, HFS+ (fetched) | [storage.md](subsystems/storage.md) |
| Storage through the loader | `bsd/dev/hk_ufs.c`, `hk_ufshc.c` | storage.md |
| Display / presentation / backlight | `MiniIOMobileFramebuffer.cpp`, `hk_display.c`, `MiniAppleARMBacklight.cpp` | [display.md](subsystems/display.md) |
| GPU | `hk_adreno.c`, `HKGPU.cpp`, `hk_a650_hwcg.h` | [gpu.md](subsystems/gpu.md) |
| Power / CPU frequency / thermal | `hk_power.c`, `hk_cpufreq.c`, `hk_thermal.c`, `hk_buttons.c` | [power.md](subsystems/power.md), [thermal.md](subsystems/thermal.md) |
| Wi-Fi | `hk_wifi.c`, `instantnoodle_wifi.c`, `hk_lx.c`, `hk_qmi.c`, `hk_sta.c`, `hk_apple80211.c`, `hk_net.c`, `HKWLAN.cpp`, `wifi/lx/` | [wifi.md](subsystems/wifi.md) |
| Audio | `hk_adsp.c`, `hk_sound.c`, `hk_audio.c`, `hk_guard.c`, `hk_speaker.c`, `instantnoodle_speaker.c`, `hk_mic.c` | [audio.md](subsystems/audio.md) |
| Input and small peripherals | `hk_input.c`, `hk_goodix.c`, `instantnoodle_touch.c`, `instantnoodle_i2c.c`, `instantnoodle_slider.c`, `hk_haptic.c`, `hk_torch.c`, `hk_battery.c`, `instantnoodle_battery.c`, `HKBattery.cpp`, `hk_camera.c` | [input-peripherals.md](subsystems/input-peripherals.md) |
| Board layer | `hk_board.c`, `bsd/sys/hk_board.h`, `instantnoodle.h` | below |
| QEMU devices | `hk_virtio_{blk,input,net,sound}.c` | [devices/qemu.md](devices/qemu.md) |

### Device-specific code
A kernel binary serves every phone whose SoC has the same blocks: addresses are looked up with `hk_board_addr("name", default)`
and overridden on the command line (`board_<block>=0x...`, listed in `bsd/sys/hk_board.h`; `tools/board-from-dt.py` reads them from a
device tree into the profile's `BOARD_ARGS`). **Defaults are the POCO X5 Pro's** (SM7325). Drivers specific to a board are gated on
`board=<name>` (the loader passes it); OnePlus 8 files are named `instantnoodle_*`. This keeps redwood's drivers running unchanged.
Several drivers are a pair: a shared engine (`hk_cpufreq.c`, `hk_adreno.c`, `hk_wifi.c`, `hk_sound.c`, `hk_speaker.c`) with
`kona ? a : b` branches or a per-board table (`hk_torch.c`, `hk_haptic.c`).

## Inter-process communication and services
- iOS's own: Mach messages, XPC, `notifyd`/Darwin notifications, launchd services. Our additions ride on those: Darwin
  notifications `com.hackios.*` as remote controls (`unlock`, `layers`, `switcher`, `brightness.N`, `slider`, `prof`), state-carrying
  notifications (`com.hackios.slider`, the lock-button state), and `com.apple.system.thermalpressurelevel` (thermald).
- Kernel ↔ user: character devices (`/dev/hk-touch`, `hk-audio`, `hk-mic`, `hk-haptic`, `hk-torch`, `hk-camera`, `hk_data`,
  `ufs_*`), sysctls `kern.hackios.*`, IOKit user clients (`HKGPU`, framebuffer, IOSurface, backlight, keystore), ioctls on sockets
  (Apple80211), `kern.exec_inject` (library injection), `kern.bootargs`.
- Kernel ↔ loader: the shared pages (storage mailbox, battery/charger page, log), `SEV` after storage requests, the return stub,
  the heartbeat (a loader resets a kernel silent for 50 s).
- Kernel ↔ DSPs: SMEM / SMP2P / GLINK / QRTR / APR ([research/hardware-qualcomm.md](research/hardware-qualcomm.md)).

## Userland integration
See [subsystems/userland.md](subsystems/userland.md). iOS binaries are not modified; libraries are injected into backboardd,
SpringBoard, mediaserverd (and others by `INJECT`); `init`/`stage0` bring the system up; images are built by `userland/build.sh`.

## Rendering
CoreAnimation (backboardd) → OpenGL ES → Mesa (freedreno, Darwin patches) → `HKGPU` (msm-DRM-shaped user client) → Adreno via the GMU
and its ring; frames are scaled by the GPU into scanout surfaces and pushed by the MDP on TE (OnePlus 8) or copied on the panel's
scan (redwood). Without `gpu=1`, CoreAnimation's software renderer draws (QEMU). Metal is a separate, experimental road to the same
GPU (`hkmetal.m` → `libhackiosair.dylib` → Mesa/ir3 → `HKGPU`). See [gpu.md](subsystems/gpu.md), [display.md](subsystems/display.md).

## Power
Governed CPU/memory votes → screen off → powerd-driven system sleep implemented as cores off + WFI (not SoC vdd-min) → off-mode
charging in the loader. [power.md](subsystems/power.md). *Not implemented:* SoC low-power states, IFPC, CPU idle power collapse.

## Persistence
RAM-only runs persist nothing (ramfs `/private/var`). Installed boots keep `/private/var` on flash (var store, or HFS+ on the
OnePlus 8). [storage.md](subsystems/storage.md).

## Testing and recovery architecture
- `xl` drives the resident loader over USB (fastboot protocol); one phone, one run lock (`$XDG_RUNTIME_DIR/hackios-phone.run`).
- `tools/t` is the OnePlus 8 test entry point (levels host/quick/gfx/power/display/full) running checks in one boot and ending at
  `t-mark: done` via `XL_DONE`; QEMU scenario tests (`common/flash/qemu-test.sh`, `tools/sleep-test`, sched tests when they land) run with no phone;
  host unit tests for crypto, MHI, QMI, WPA, thermal ([TESTING.md](TESTING.md)).
- Recovery: fastboot is the stock bootloader's and never written; `boot_b`/spare slot restore, A/B switch, boot guard
  ([INSTALL.md](INSTALL.md), [research/boot-chain-and-recovery.md](research/boot-chain-and-recovery.md)).

## Two repositories, one tree

| | git dir | remotes | Owns |
|---|---|---|---|
| kernel repo (`hackios-xnu-kernel` fork `iphont-xnu-kernel`) | `.git` | origin `haxi0/iphont-xnu-kernel` (GitHub), forgejo, upstream `oosexgang/hackios-xnu-kernel` | `README.md`, `LICENSE`, `.gitignore`, `common/`, `docs/` (all but `air*.md`), `xl`, `mkxnuimg.py`, `xnu-*`, `xnuloader/`, `qemu/`, `tools/` (except the userland's), `xnu/`, `xnu-deps/`, `uefi/`, `prebuilt/` (not `userland/`), `wifi/` |
| userland repo (`hackios-xnu-userland` fork `iphont-xnu-userland`) | `.userland.git` (`common/ugit`) | origin `haxi0/iphont-xnu-userland`, forgejo, upstream `oosexgang/hackios-xnu-userland` | `userland/`, `ios165/`, `prebuilt/userland/`, `docs/air.md`, `docs/air-report.md`, `tools/{apfs,air,mesa,metal}`, `tools/{mk-mesa.sh,mk-jb.sh,jb-preinstall.py,font_trim.py,metallib.py}` |

Both use whitelist `.gitignore`s: **nothing is tracked unless named**. `common/ugit status` shows the userland side (untracked files hidden;
add new files with `common/ugit add -f PATH`). `README.md`, `.gitignore`, `LICENSE` at the top are the kernel repo's: the userland's are
`skip-worktree` (read them with `common/ugit show HEAD:README.md`). `common/setup.sh` makes the userland checkout. Plain `git status` or a
`git grep` **misses the Metal/Mesa/userland code**. Both working trees were dirty on 2026-10-26 (see [HANDOFF.md](HANDOFF.md)); several
agent sessions may edit at once, so check before touching a file. **Caveat (checked 2026-10-26):** the userland repo's *index* also tracks files that belong to the kernel repo (1,189 files:
about 618 under `uefi/`, 124 under `xnu/`, `xnuloader/`, `wifi/`, `common/`, `xl`, ...), because history merged upstream's
re-rooted trees; the exclude whitelist only hides *untracked* files. So `common/ugit status` lists the same modified `xnu/`, `wifi/`
and `xl` files as `git status`, and a careless `common/ugit commit -a` would commit kernel files into the userland repo (commit
`f8fec25` of the userland repo did touch `xnu/files/bsd/dev/hk_adreno.c`). Add files by name (`common/ugit add PATH`), and commit
each file in the repo that owns it per the table.

## Agent debugging interface: iphont

The Python standard-library `iphont` CLI shares `xl`'s fastboot endpoint, USB lock, and run/resident-image coordination. `XNULIVE4` extends the existing loader-to-XNU interface with a bounded request/reply mailbox and cumulative log counter. The opt-in native kernel agent queries real proc/task/IOService state and exposes a root-only `/dev/iphont` bridge to `iphontd`, whose launchd job follows the device-node path. Normal installed boot profiles do not enable it. No network listener, GUI, Rust host, or parallel USB implementation is added. [TOOLKIT.md](TOOLKIT.md) records the command contract and current validation limits.
