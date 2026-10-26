# POCO X5 Pro 5G (redwood) — Snapdragon 778G (SM7325, "Kodiak")

The phone HackiOS was brought up on. Profile `common/devices/redwood.conf` (`CODENAMES="redwood"`), UEFI
`uefi/files/Platforms/Xiaomi/redwoodPkg` (follows Mu-Silicium's `lisa`, same SoC package `KodiakPkg`), loader `platform_redwood`.
The kernel's driver defaults (`board_*` addresses) are this phone's. Display: 1080 x 2400 DSI **video** panel at 120 Hz. Radio WCN6750.
Touch Goodix GT9916 or FocalTech FT3680; speakers 2 x Awinic AW88261; haptics AW86927; four camera sensors.

**Nothing is flashed in a RAM run**: the UEFI is started with `fastboot boot`, the system lives in RAM and a reboot returns to Android.
The dual-boot install ([../INSTALL.md](../INSTALL.md)) uses the spare A/B slot and never writes Android.
**Project rule from the owner (2026-10-25): do not run redwood (POCO) checks** during OnePlus work; redwood statements below are
from the upstream README and earlier sessions and have **not been re-verified in this checkout's recent history**.

## Status (as claimed by the project; unverified recently)

| Feature | Notes |
|---|---|
| Lock screen, home screen, system gestures, Settings, Clock, Calendar, Photos, Notes, Safari | works; widgets (Weather, Calendar) on the phone |
| Touchscreen, multitouch, wake on touch, power and volume keys | Goodix polled 125 Hz, no interrupt |
| 8 CPU cores, big core 2.4 GHz, ~20 s to the lock screen | `cpus=8 cpufreq=1 bootcpu=7` |
| Adreno 642L GPU (`gpu=1`) | no DCVS, no per-program page tables, polled fences |
| Wi-Fi WCN6750 (ath11k) | join from Settings, Safari, 55-90 Mbit/s |
| Sound (speakers, main microphone), camera in Camera.app (front/rear, preview, focus, HEIC) | |
| Persistence (`var_store=1 ufs_write=1`), iOS root on the phone's flash | |
| Charging with `adsp=1` (+0.4 A from a computer), battery health in `sysctl kern.hackios.charger` | |
| 120 Hz, no tearing (`fb_scan=1`) | |
| Sileo + Procursus, tweaks, SSH | |
| Not working | cellular, Bluetooth, fast charging (PD/PPS read, not driven), video/Live Photo recording, App Store content and Apple ID sign-in (no Secure Enclave; FairPlay daemon exits), Metal for apps |

## Running

You need an unlocked bootloader, `adb`, `fastboot`, the firmware pulled from the phone ([../BUILD.md](../BUILD.md)) and the images
(`ios165/fetch.sh`, `common/build.sh redwood GL=1`):

```sh
common/run.sh redwood 900            # loader into RAM, system image into RAM, a 15-minute session
# or step by step
./xl enter            # Android -> fastboot -> our loader in RAM (fastboot boot); it brings up its own USB link
./xl ramdisk          # upload the system image into the phone's RAM (2.5 GB, ~80 s), once per loader session
XL_SECS=900 XL_CMDLINE="touchscreen=1 cpufreq=1 bootcpu=7 cpus=8 gpu=1 wifi=1 adsp=1" ./xl run
./xl screenshot shot.png
./xl reboot           # back to Android
```

Wi-Fi also needs `fw-wifi/mac` and `fw-wifi/network`. Prebuilt boot image: every upstream release carries `hackios-redwood-boot.img`
(UEFI with the loader inside, no Apple files).

## Architecture notes
Panel scan: [../subsystems/display.md](../subsystems/display.md). GPU/Wi-Fi/audio/camera drivers: the subsystem pages (the camera driver
`hk_camera.c` exists only for redwood). Thermal service: [../subsystems/thermal.md](../subsystems/thermal.md) (27 sensors answered, 33-38 C, 2026-10-22; never seen slowing cores).
Dual boot: [../INSTALL.md](../INSTALL.md). Boot-time facts: [../research/boot-chain-and-recovery.md](../research/boot-chain-and-recovery.md).
