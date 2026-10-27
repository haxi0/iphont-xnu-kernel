# OnePlus 8 (instantnoodle, IN2010) — Snapdragon 865 (SM8250, "kona")

The second phone and the one under active development. Profile `common/devices/instantnoodle.conf`, UEFI
`uefi/files/Platforms/OnePlus/instantnoodlePkg` (drivers mostly from `kebab`, the OnePlus 8T), loader platform
`platform_instantnoodle`, drivers gated on `board=instantnoodle` (files `instantnoodle_*.c`, `kona` branches in shared drivers).
8 GB RAM, panel ANA6705 1080 x 2400 (command-mode DSI, 90 Hz here), touch Samsung S6SY761, battery gauge TI bq27541, Wi-Fi
QCA6390 on PCIe, speakers 2 x TFA9874, haptics AW8697, flash LED PM8150L, alert slider (two IST8801 hall sensors).

**This phone no longer has Android.** Its root is the iOS volume written once to `userdata` (`tools/store-image.py`; the loader boots
`flashboot.apfs` with `ufs=2 root_apfs=ufs_darwin`), so unlike redwood this port writes one partition when set up. Only the stock
bootloader (fastboot) and TWRP remain. Recovery: [../INSTALL.md](../INSTALL.md), [../research/boot-chain-and-recovery.md](../research/boot-chain-and-recovery.md).

## Status

What has been **seen working on the phone itself** (the kernel's log of a run says each of these). "RAM" = RAM-only `xl run`, "installed" = boot from flash.

| Feature | State | How | Details |
|---|---|---|---|
| Boot to SpringBoard, 8 cores, prime core 2.84 GHz | works (RAM, installed) | `cpus=8 cpufreq=1 bootcpu=7` | [cpu.md](../subsystems/cpu.md) |
| Display, 90 Hz, no tearing | works | default with `board=instantnoodle` | [display.md](../subsystems/display.md); ~2% of swaps a refresh late |
| Touch (S6SY761), interrupt-driven | works; I2C on its serial engine with `i2c_geni=2` (verified at boot 2026-10-27, not yet default) | default (`touch_irq=0` polls) | [input-peripherals.md](../subsystems/input-peripherals.md) |
| Power/volume keys, alert slider | works (keys' interrupts and dark-wait not run) | default | input-peripherals.md, [power.md](../subsystems/power.md) |
| Battery level/voltage/current/charging | works | default | power.md |
| Speakers (TFA9874), iOS sounds, volume keys | works, heard 2026-10-24 | `adsp=1 sound=4 sound_amps=1 sound_below=96`, `fw-audio` | [audio.md](../subsystems/audio.md) |
| Main microphone (digital DMIC) | works (claps) | `mic=4` path via `Codec` device | audio.md |
| Haptics (AW8697) | works | `haptic=5`, `fw-haptic` | input-peripherals.md |
| Flashlight | works | `torch=3` | input-peripherals.md |
| Adreno 650 for CoreAnimation | works | `gpu=1`, `fw-gpu`, `GL=1` images | [gpu.md](../subsystems/gpu.md) |
| Wi-Fi (QCA6390) | works in RAM runs (WPA2, WPA2/TKIP mixed, WPA3 SAE) | `wifi=1`, `fw-wifi` | [wifi.md](../subsystems/wifi.md) |
| System sleep, wake by key/timer/cable | works (6 of 7 cores off); not the SoC's lowest state | `syssleep` default on | power.md |
| GPU block collapse in sleep, brightness command path | works / command path verified | `gpu_sleep` default | gpu.md, display.md |
| Boot from flash, boot menu, boot guard, `/private/var` on HFS+ | works (installed 2026-10-24) | `common/flash/install-xl.sh` | [storage.md](../subsystems/storage.md), INSTALL.md |
| Custom boot logo | works | `userland/mkbootlogo.py` | research/ios-userland.md §7 |
| Metal driver | experimental, off by default | `metal=1`/`2` | gpu.md |
| Bluetooth (QCA6390) | partial: native BLE discovery and initial GATT transfer verified; pairing/reconnection unfinished | `bt=1`, `BLUETOOTH=1`, `fw-bt` | [bluetooth.md](../subsystems/bluetooth.md) |
| Not done | headphones/USB-C audio, cameras, sensors, cellular, location, fast charging (Warp), SoC low-power states, auto-brightness | | [../TODO.md](../TODO.md), [../KNOWN_ISSUES.md](../KNOWN_ISSUES.md) |

## Setting up (RAM-only run)

**Always load the profile before `xl enter`**: a bare `./xl enter` builds redwood's UEFI, which hangs this phone on its fastboot
logo (then `fastboot reboot bootloader` clears it). `common/run.sh instantnoodle` does it. Build tools are in `nix-shell` on the
development host. Tell the owner before starting a run on the phone.

Firmware comes out of the phone's own vendor partition (here a backup of `vendor_a`; from a running Android `adb pull
/vendor/firmware/...` does the same):

```sh
7z e vendor_a.img 'firmware/aw8697_haptic*.bin' -ofw-haptic       # haptics (aw8697_haptic_170.bin is used)
7z e vendor_a.img 'firmware/a650_*' -ofw-gpu                       # GPU (gpu=1)
tools/mk-mesa.sh                                                   # Mesa, once (needs meson, ninja)
GL=1 REST=1 FLASHBOOT=1 CRYPTEXES=flash userland/build.sh          # root.apfs + flashboot.apfs
. common/lib.sh && load_device instantnoodle                       # the profile, before xl enter
./xl enter && ./xl ramdisk userland/flashboot.apfs
XL_SECS=900 XL_CMDLINE="cpus=8 cpufreq=1 bootcpu=7 ufs=2 root_apfs=ufs_darwin haptic=5 torch=3 gpu=1" ./xl run
```

Audio firmware: `adsp.*` from the modem partition's `image/` and `/vendor/firmware/tfa98xx.cnt` into `fw-audio`. Wi-Fi firmware: the
QCA6390's linux-firmware build, Android's board data, the phone's own address from `persist`:

```sh
mkdir -p fw-wifi && cp /lib/firmware/ath11k/QCA6390/hw2.0/{amss.bin,m3.bin} fw-wifi/   # (unpacked, if .zst/.xz)
7z e -ofw-wifi modem_b.img image/12wlan.b0c
7z e -so persist.img qca6390/wlan_mac.bin | sed -n 's/^Intf0MacAddress=\(..\)\(..\)\(..\)\(..\)\(..\)\(..\)$/\1:\2:\3:\4:\5:\6/p' | tr A-F a-f > fw-wifi/mac
printf '%s\n%s\n' 'network name' 'passphrase' > fw-wifi/network   # optional: joined at the start
```

(The profile's `BOOTARGS` are used by `common/run.sh`: `cpus=8 cpufreq=1 bootcpu=7 haptic=5 torch=3 gpu=1 adsp=1 sound=4 sound_amps=1
sound_below=96 slider_cal=... sb_clock=24`; add `wifi=1`.)

## Things specific to this phone

- **Never pass `touchscreen=1`** (redwood's Goodix driver; GPIO 68 → NoC CrashDump).
- Recovery from CrashDump: hold Power + Volume Up. `xl reboot` lands in the bootloader; `xl enter` goes from stock fastboot. A cold
  GPU cycle is cheap (`xl reboot`, `xl enter`, retry if "loader did not come up"; without `xl ramdisk userland/flashboot.apfs` launchd is missing).
- A reset with the USB cable in confuses the stock ABL (charger detect says DCP, fastboot never appears): unplug, enter the mode, replug.
- `slider_cal`, `sb_clock=24` (status-bar clock clear of the camera hole at x 61-137 of 1080) are in the profile.
- `/private/var` size shows ~2 GB on RAM boots (ramfs cap `max_mem/4`), 95 GB with the HFS+ volume.
- The OnePlus has no ambient light sensor wired in this port, so auto-brightness has nothing to read.
- Test with `tools/t` ([../TESTING.md](../TESTING.md)); tests must be short (the owner asked twice).

## Hardware map (more in research/hardware-qualcomm.md)

I2C by hand (engine FIFO disabled): touch SE13 gpio36/37, slider SE5 gpio4/5, gauge SE16 gpio48/49 `0x55`, speakers SE15 gpio44/45.
Display MDP at MDSS (SPI 83), GPU at `0x3d00000` (chip id `0x06050002`), PCIe 0 at `0x1c00000` (BAR0 `0x60300000`), EPSS
`board_epss`, TSENS, UFS at `0x1d84000`. Stock facts: the phone's live device tree is
`../ios-instantnoodle/research/instantnoodle-live.dts` (outside the repository).

## History
Earlier attempts on this phone: `ios-instantnoodle/` (the "sweet"-style port, still the home of the partition backups and the
live device tree) and `UniXNU-instantnoodle/`/`uefi-instantnoodle/` (UniXNU, dropped 2026-10-22: iPadOS-only and heavy plumbing; reached
`Failed to mount root device`). HackiOS is the base. Do not resume the abandoned ones unless asked.
