# HackiOS documentation index

HackiOS boots Apple's open-source XNU (`xnu-8796.121.2`, iOS 16.5's kernel) on Android phones and runs the unmodified iOS 16.5 user space.
Start with the root [README](../README.md) for the overview and the QEMU quick start. Coding agents: [../AGENTS.md](../AGENTS.md).

## I want to ...

| ... | read |
|---|---|
| know what works and what does not | [KNOWN_ISSUES.md](KNOWN_ISSUES.md) (broken now), [TODO.md](TODO.md) (roadmap), [devices/](devices/README.md) |
| continue the previous session's work | [HANDOFF.md](HANDOFF.md) |
| understand how it fits together | [architecture.md](architecture.md) |
| build it | [BUILD.md](BUILD.md) |
| run it (QEMU / OnePlus 8 / POCO X5 Pro) | [devices/qemu.md](devices/qemu.md), [devices/instantnoodle.md](devices/instantnoodle.md), [devices/redwood.md](devices/redwood.md) |
| install it on a phone, recover a phone | [INSTALL.md](INSTALL.md), [research/boot-chain-and-recovery.md](research/boot-chain-and-recovery.md) |
| test a change | [TESTING.md](TESTING.md) |
| inspect or control the phone from a coding agent | [TOOLKIT.md](TOOLKIT.md), `./iphont describe` |
| look up a boot argument | [BOOT_ARGS.md](BOOT_ARGS.md) |
| port to a new phone | [PORTING.md](PORTING.md) |
| learn a subsystem | below |
| learn what has been reverse-engineered | [research/README.md](research/README.md) |

## Subsystems ([subsystems/](subsystems/))

| Page | Covers |
|---|---|
| [cpu.md](subsystems/cpu.md) | cores, PSCI, GICv3, EPSS frequency, return to the loader |
| [scheduler.md](subsystems/scheduler.md) | Edge scheduler on E/P clusters, the performance controller (hk_perfctl), energy model, measurements against Clutch |
| [power.md](subsystems/power.md) | governor, system sleep, keys, charging, shutdown, off-mode charging, verification record |
| [thermal.md](subsystems/thermal.md) | temperature service; sensor and GPS status |
| [display.md](subsystems/display.md) | presentation pipeline, 90/120 Hz, screen power, brightness, measurements |
| [gpu.md](subsystems/gpu.md) | Adreno bring-up, HKGPU, DCVS, sleep collapse, OpenGL ES, Metal status |
| [audio.md](subsystems/audio.md) | DSP, speakers, guard, microphone |
| [input-peripherals.md](subsystems/input-peripherals.md) | touch, keys, slider, haptics, torch, battery, cameras |
| [wifi.md](subsystems/wifi.md) | ath11k on a compatibility layer, Apple80211, QCA6390 and WCN6750 |
| [storage.md](subsystems/storage.md) | images, UFS through the loader, partitions, `/private/var`, APFS, HFS+ |
| [userland.md](subsystems/userland.md) | iOS integration: init, jobs, injected libraries, DENY list |

## Research ([research/](research/README.md))
iOS internals [ios-userland.md](research/ios-userland.md) · kernel stand-ins [kernel-standins.md](research/kernel-standins.md) · hardware rules [hardware-qualcomm.md](research/hardware-qualcomm.md) ·
boot chain and recovery [boot-chain-and-recovery.md](research/boot-chain-and-recovery.md) · dead ends [failed-approaches.md](research/failed-approaches.md) · measurement pitfalls [checks.md](research/checks.md).
**Metal / AIR shader research** (userland repository, research not shipped functionality): [air.md](air.md), [air-report.md](air-report.md).

## Elsewhere
[../xnu/README.md](../xnu/README.md) (kernel patch layout) · [../uefi/README.md](../uefi/README.md) (UEFI port) · [../prebuilt/NOTICES.md](../prebuilt/NOTICES.md) (third-party notices) · `common/devices/_template.conf` (profile variables) ·
screenshots in [images/](images/).

## Documentation rules
Which file to update for which change is in [../AGENTS.md](../AGENTS.md#keeping-documentation-current). Status vocabulary: *verified on phone*, *built (QEMU/host-tested)*, *unverified*; tags in research pages: *observed*, *source*, *inferred*, *hypothesis*.
The sibling directories next to the workspace (`ios-instantnoodle/`, `ios-topaz/`, `unixnu/`, `UniXNU-instantnoodle/`, `uefi-instantnoodle/`, `hackios-ref/`) are other or abandoned projects and snapshots, **not** part of this documentation.
