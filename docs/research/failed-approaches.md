# Failed approaches, dead ends and costly mistakes

One line each, with where the full story is. Read before repeating an experiment. Dates are 2026 unless noted.

## Hardware / kernel
| Tried | Result | Why / lesson | Detail |
|---|---|---|---|
| DCS sleep-in (0x10) to the OnePlus panel for screen off | coloured static, only a phone reset recovers | panel loses DSC setup; Android's on-command PPS does not match what UEFI set in the MDP | [display.md](../subsystems/display.md) |
| Reading apps-SMMU context bank 4 / ATOS to learn the MDP mapping | CrashDump | hypervisor-protected | [hardware-qualcomm.md](hardware-qualcomm.md) |
| `gpu=1` from a cold boot | CrashDump until the zap-shader header was handed to TZ first | PAS_SHUTDOWN before PAS_INIT_IMAGE | [gpu.md](../subsystems/gpu.md) |
| `touchscreen=1` on the OnePlus 8 | NoC CrashDump (GPIO 68) | redwood's driver | [input-peripherals.md](../subsystems/input-peripherals.md) |
| Voting the VA core clock / writing the VA GFMux for the mic | NoC CrashDump | mapped 0 = TX MCLK | [audio.md](../subsystems/audio.md) |
| `ml_io_map` of 1 MB TLMM / a 10 MB framebuffer | panic: IO submap (8 MB, permanent) exhausted | map a page, use `IOMemoryDescriptor` | hardware-qualcomm.md |
| Android's `amss20.bin` under ath11k (QCA6390) | firmware assert at first mode request | host-mode firmware needed (linux-firmware V2_TO_X86) | [wifi.md](../subsystems/wifi.md) |
| Unaligned PCI coherent DMA for the QCA6390 | firmware crash | Linux guarantees power-of-two alignment | wifi.md |
| MHI channels started on the register saying "mission" | command lost | wait for the EE event and M0 | wifi.md |
| SMMU stream `0x1c00` mask 1 for the radio | hypervisor refuses | `0x1c01` mask 0 | wifi.md |
| `wifi_again=1` / `adsp_again=1` | reset the phone twice of two | DSP shut down without acknowledging a stop | wifi.md, audio.md |
| Treating FT/AKM 4 as the cause of the mixed-network join failure | wrong | the cause was TKIP group cipher + missing capability bit | wifi.md |
| Quiescing all SPIs in the per-core shutdown | every driver interrupt dead after wake: tearing, polled GPU/touch | split per-core and global quiesce | [power.md](../subsystems/power.md) |
| Re-initialising a restarted core without clearing `gicr_cpu_base` | deaf core, system freeze | `cpu_shutdown` forgets the core | power.md |
| `CPU_SUSPEND` power-down for idle cores while awake | not tried: hang risk, QEMU cannot test it | | power.md |
| SoC vdd-min | architecturally blocked (UEFI never exits, loader core 0, resident drivers) | | power.md |
| IFPC (GMU collapsing GX between frames) | not done: needs OOB handshake on every host register access | mainline does not do it for this GPU either | gpu.md |
| Silent-start + fade for the speaker amplifiers | lost the first 150 ms of the lock sound | keep the level, start in 26-39 ms | audio.md |
| RELEASE kernel configuration | does not build; abandoned | | [cpu.md](../subsystems/cpu.md) |
| NEON SHA-256 in the kernel | not allowed (FP state) | | [kernel-standins.md](kernel-standins.md) |
| Kernel-side stub of `com_apple_driver_FairPlayIOKit` | would never be reached | fairplayd asks IOKit for nothing | [ios-userland.md](ios-userland.md) §4 |
| Swapping ABL/XBL/TZ or slots on the OnePlus 8 | blocked (anti-rollback, unverified EDL programmer); forbidden by the owner | | boot-chain-and-recovery.md |
| Resetting with the USB cable in to reach stock fastboot | ABL takes the port for a wall charger and powers off | design avoids needing it | boot-chain-and-recovery.md |
| UniXNU on the OnePlus 8 | dropped: iPadOS-only, much plumbing | HackiOS is the base | boot-chain-and-recovery.md |

## iOS user space
| Tried | Result | Lesson |
|---|---|---|
| `glyes=40` ("yes" to a shader function without source) | window server crashes in `strlen(NULL)`, restarts every 30 s | provide a source in the table, never just say yes |
| Removing the 5 s ALS wait (`brightness_fixes()`) | backboardd stopped adopting our touch service | the wait masked a startup race; switched off |
| `mix(behind, s, a)` for vibrant layers | wallpaper replaced in place: grey App Library pods | source-over |
| Multiplying the "ds8" corner colour into the sample | backdrops at the layer's opacity | CA applies opacity itself |
| One-sample "ds8", clear-and-draw variable blur | shimmer / black strip | proper chain |
| CoreAnimation's software renderer as the reference for backdrops | it draws them black | use it only for plain layers |
| Hooking `__sysctl`/`__mac_syscall` by dyld interposing | never fires | libsystem-internal calls are invisible to interposing; use the kernel's systrace |
| Reading an entitlement as behaviour (fairplayd) | wrong plan | entitlement = permission |
| Judging denied daemons one at a time | cloudd "culprit" was `nsurlsessiond` being denied | retest, use controls |
| Blaming the sleep for the slow phone after wake | it was `vmd` spinning on CommCenter lookups (40% of a core) | measure per process (`kern.hackios.top`) first |
| Reading time-ordered runs as causes | other sessions loaded the machine | control from the same afternoon |
| Triggering the app switcher via notification/two home presses | no effect | open |
| ASTC snapshots | 10 s CPU compress kills apps via the scene watchdog | JPEG/PNG |
| Fixed waits in tests (26+100 s etc.) | 11-28 min runs, wrong "kernel hung" verdicts | conditions, not waits: [TESTING.md](../TESTING.md) |
| Android's own `PS_HOLD` state left as is on reset | warm reset: ABL fastboot without USB | set hard reset explicitly |

## Process mistakes (from [checks.md](checks.md) and notes)
Counting a QEMU log's doubled lines; `pgrep -f`/`pkill -f` a string in your own command line (exit 144); `export.sh` from a stale tree
(rolls others' work back, boot crash after `Stackshot Succeeded`); an `ios165/rootfs` symlink to another copy (deleted 491 jobs from the shared
extraction on 19 October 2026); screenshots/records not announced to the owner who watches the phone; claiming the speakers work before the owner
reports.
