# HackiOS — XNU kernel

**HackiOS** boots Apple's open-source **XNU kernel** (`xnu-8796.121.2`, the kernel of iOS 16.5) on Android phone
hardware and runs the **unmodified iOS 16.5 user space** on top of it: launchd → backboardd → SpringBoard →
apps. Our own EFI loader, our own replacements for Apple's closed drivers, a UEFI port, and a Linux Wi-Fi driver
on a compatibility layer make that possible.

It runs in two places:

- **POCO X5 Pro 5G (redwood, Snapdragon 778G)** — nothing is flashed: the UEFI is started with `fastboot boot`,
  the whole system lives in RAM, and after a reboot the phone is back in Android;
- **QEMU** (`virt`, aarch64) — the same thing on any Linux computer, no phone needed.

This repository is half of the project:

| Repository | What |
|---|---|
| **hackios-xnu-kernel** (this one) | XNU patches, the EFI loader, the UEFI port, Wi-Fi, QEMU scripts, `xl`, prebuilt kernel/loader/UEFI, `common/` scripts |
| [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | our init, injected libraries, launchd jobs, the APFS image builder, the scripts that take iOS out of Apple's IPSW, Metal/Mesa work |

> **No Apple files are in either repository.** `ios165/fetch.sh` downloads the iOS 16.5 IPSW from Apple's own CDN
> to *your* machine and extracts what is needed there. HackiOS is an independent research project by OOSEX GANG and
> is not affiliated with, authorised or endorsed by Apple Inc. iOS, iPhone and Apple are trademarks of Apple Inc.

<p>
<img src="docs/lockscreen.jpg" width="190" alt="Lock screen">
<img src="docs/homescreen.jpg" width="190" alt="Home screen">
<img src="docs/control-center.jpg" width="190" alt="Control Center">
<img src="docs/settings.jpg" width="190" alt="Settings">
</p>

Photographed on the phone, the Adreno GPU drawing (`gpu=1`). More in [Screenshots](#screenshots).

## What works

| Works | Does not work (yet) |
|---|---|
| Lock screen, swipe to unlock, home screen with all icons (two pages), stock iOS 16 wallpapers | Cellular, Bluetooth |
| System gestures: swipe up (home, app switcher), edge swipe (back), Control Center | Fast charging (PD/PPS); the charger is read, not yet driven |
| Apps: Settings, Clock, Calendar, Photos, Notes, Safari and more; widgets (Weather, Calendar) on the phone | Metal for apps (in progress: 293 of 456 system shaders already run on the Adreno, see `docs/air.md` in hackios-xnu-userland) |
| Phone touchscreen (Goodix GT9916), multitouch, wake on touch; real power and volume keys | App Store content and Apple ID sign-in (no Secure Enclave; FairPlay daemon exits) |
| Up to 8 CPU cores (`cpus=8`), big core at 2.4 GHz, ~20 s to the lock screen | Video and Live Photo recording |
| Adreno 642L GPU through Mesa/freedreno (`gpu=1`, OpenGL ES for CoreAnimation) | |
| Wi-Fi (WCN6750, ath11k): join from Settings, Safari over Wi-Fi, 55–90 Mbit/s | |
| Sound (speakers, main microphone), camera in Camera.app (front/rear, preview, focus, HEIC photos) | |
| Data that iOS writes survives a reboot (`var_store=1 ufs_write=1`); iOS root volume on the phone's flash | |
| Charging with `adsp=1`: the audio DSP's charger takes current from USB (seen: +0.4 A from a computer's port), battery health and charger type in `sysctl kern.hackios.charger` | |
| Frames follow the panel's own scan (`fb_scan=1`): 120 Hz, each frame lands whole in one refresh -- no tearing | |
| Sileo + Procursus (dpkg, apt), tweaks, SSH | |

It is a research bench, not a ROM: a session lasts a given number of seconds, then the kernel returns to the
loader and hands over its log.

## Quick start: QEMU, no kernel build

`prebuilt/` holds the kernel, loader and our programs built from these repositories, so all you need is the system
image, which is made on your machine from Apple's IPSW.

Needs: Linux x86-64, ~12 GB free disk, 4 GB free RAM. Packages (Arch):

```sh
sudo pacman -S --needed git python 7zip lz4 gcc qemu-system-aarch64 qemu-ui-gtk edk2-aarch64
```

(Debian/Ubuntu: `git python3 7zip liblz4-dev gcc qemu-system-arm qemu-efi-aarch64`; 7-Zip must be 23 or newer — it
reads APFS.)

```sh
git clone https://github.com/oosexgang/hackios-xnu-kernel.git hackios
cd hackios
common/setup.sh               # brings hackios-xnu-userland into the same tree, checks the host
ios165/fetch.sh               # downloads 7.4 GB from Apple's CDN and extracts what is needed (5-15 min)
common/build.sh qemu-virt     # builds the APFS images (and the kernel, if the toolchain is there)
common/run.sh qemu-virt       # a 20-minute session in a QEMU window
```

The lock screen appears after 2–3 minutes.

- **Mouse with the left button held = finger.** Unlock / leave an app: swipe up from the very bottom edge; back:
  swipe from the left edge.
- Keys: `H` home, `L` lock/power, `↑`/`↓` volume, `F11` (hold) five fingers.
- From a script: `qemu/touch.py swipe 0.5 0.985 0.5 0.3 250`, `qemu/touch.py tap 0.615 0.71`,
  `qemu/touch.py key home`; screenshot: `qemu/screendump.sh shot.png`.
- Right after SpringBoard appears the system is still registering apps: give it 30–40 s.
- Debian/Ubuntu UEFI firmware: `QEMU_EFI=/usr/share/AAVMF/AAVMF_CODE.fd QEMU_VARS=/usr/share/AAVMF/AAVMF_VARS.fd`.

## On the phone (POCO X5 Pro 5G)

You need a **POCO X5 Pro 5G (redwood)** with an unlocked bootloader, `adb` and `fastboot`, and the images from the
section above. Nothing is written to the phone's storage.

**The phone's own firmware** is not in the repository and cannot be: it is the vendor's and is taken from the very
phone it will run on, from its Android:

```sh
adb pull /vendor/firmware/a660_{gmu.bin,sqe.fw,zap.mdt,zap.b00,zap.b01,zap.b02} fw-gpu/      # GPU (gpu=1)
adb pull /vendor/firmware_mnt/image/wpss.{mdt,b02,b03,b04,b05,b06,b07} fw-wifi/              # Wi-Fi (wifi=1)
adb pull /vendor/firmware_mnt/image/adsp.{mdt,b02,...,b36} fw-audio/                         # sound (adsp=1)
adb pull /vendor/firmware/aw8697_haptic.bin fw-haptic/                                       # haptics
```

(Put the `fw-*` directories next to the workspace. Wi-Fi also needs `fw-wifi/mac` — the radio's address on one
line — and `fw-wifi/network` — network name and password. `userland/build.sh` prints which firmware went in.)

```sh
common/build.sh redwood GL=1          # images + out/redwood-boot.img
common/run.sh redwood 900             # loader into RAM, system image into RAM, a 15-minute session
```

or step by step with `xl`:

```sh
./xl enter            # Android -> fastboot -> our loader in RAM (fastboot boot); it brings up its own USB link
./xl ramdisk          # upload the system image into the phone's RAM (2.5 GB, ~80 s), once per loader session
XL_SECS=900 XL_CMDLINE="touchscreen=1 cpufreq=1 bootcpu=7 cpus=8 gpu=1 wifi=1 adsp=1" ./xl run
./xl screenshot shot.png
./xl reboot           # back to Android
```

`./xl` with no arguments lists every command (logs, screen and sound recording, camera frames, stop, state...).

## On the phone (OnePlus 8)

The OnePlus 8 (`instantnoodle`, Snapdragon 865 / SM8250) is the second phone: profile `common/devices/instantnoodle.conf`,
UEFI `uefi/files/Platforms/OnePlus/instantnoodlePkg`, loader platform `platform_instantnoodle`. Its root is the iOS
volume written to its `userdata` (`tools/store-image.py`; the loader boots `flashboot.apfs` with
`ufs=2 root_apfs=ufs_darwin`), so unlike redwood this port does write that one partition, once, when it is set up.

What has been seen working on the phone itself (the kernel's log of a run says each of these):

| Works | How | Not yet |
|---|---|---|
| Boot to SpringBoard, 8 cores, prime core at 2.84 GHz | `cpus=8 cpufreq=1 bootcpu=7` | Metal for the whole system is not on by default (`metal=2 metal_profile=1` reaches the home screen; chronod's widgets still have 2 pipelines refused) |
| Display: 90 Hz, no tearing; iOS at up to 90 fps (the MDP pushes double-buffered frames on the panel's TE; the GPU scales them into the framebuffers, `fb_gpu=0`: the processor) | default with `board=instantnoodle` (`fb_hz=60`, `fb_push=1`/`0` fall back) | About 2% of swaps still come a refresh late (one push in flight at a time) |
| Touch screen (Samsung S6SY761, by its interrupt) and the keys | `board=instantnoodle` (the loader passes it; `touch_irq=0`: polling) | Wi-Fi (QCA6390 on PCIe, not redwood's WCN6750) |
| Battery: level, voltage, current, charging (TI bq27541 gauge, PM8150B USB input) | on with `board=instantnoodle` | Speakers by ear: the TFA9874s play with `sound_amps=1` (the profile passes it, with `sound_below=96`: 12 dB under Android's maximum; 0 is stock's (no stock attenuation, per its volume tables and ACDB)) but have not been heard yet; headphones and microphones (WCD938x) |
| Sound, up to the amplifiers: the audio DSP (SM8250 image, relocated) up with APR; tertiary MI2S at 48 kHz; iOS's UI sounds, ringtones and alert tones (ToneLibrary), local notifications with their sound and banner, volume keys -- all reach the DSP, as recordings show (`rec_sound=N rec_sound_fed=1`, `RC_JOBS=rc.jobs.speakers`/`alerts`/`volume`) | `adsp=1 sound=4`, `fw-audio` (`adsp.*` from the modem partition's `image/`, `/vendor/firmware/tfa98xx.cnt`); `speaker_probe=3` sets the amplifiers up alone | the alert slider (ring/silent: two hall sensors, no driver) |
| Haptics from iOS (Awinic AW8697): system feedback, CoreHaptics | `haptic=5`, `fw-haptic` | Cameras, sensors, Bluetooth |
| Flashlight from iOS (PM8150L flash LED, both LEDs) | `torch=3` | Charging control (it charges as the boot chain set it up) |
| Adreno 650 drawing for CoreAnimation (Mesa/freedreno, OpenGL ES); its speed follows its load (305-525 MHz), a hang is recovered; every program has GPU page tables of its own, and its commands cannot reach another's memory, the kernel's ring or the GPU's protected registers (`RC_JOBS=rc.jobs.gpuiso`) | `gpu=1`, `fw-gpu`, images with `GL=1` (from a cold start too); `gpu_dcvs=0` / `gpu_recover=0` / `gpu_pt=0` turn those off | third-party Metal apps (none tried yet) |
| Metal driver on the Adreno 650: blits, clears, 4x MSAA resolve, command-buffer completion, fences, events and shared events, heaps (`metal_sync_test`); 263 of Apple's 271 fragment shaders draw, the 258 compared agree with softpipe; Apple Maps runs on it: its pipelines build and its command buffers complete without error (`RC_JOBS=rc.jobs.maps`, offline, no picture checked) | `metal=1` (test programs), `metal=2` (the system) | shared events across processes; heaps do not suballocate |

Run `xl enter` with the profile loaded (`common/run.sh instantnoodle`, or `. common/lib.sh; load_device
instantnoodle` first): a bare `./xl enter` builds redwood's UEFI, which hangs this phone on its fastboot logo
(then `fastboot reboot bootloader` clears it).

The firmware comes out of the phone's own vendor partition (here a backup of `vendor_a`; from a running Android
`adb pull /vendor/firmware/...` does the same):

```sh
7z e vendor_a.img 'firmware/aw8697_haptic*.bin' -ofw-haptic       # haptics (aw8697_haptic_170.bin is used)
7z e vendor_a.img 'firmware/a650_*' -ofw-gpu                       # GPU (gpu=1)
tools/mk-mesa.sh                                                   # Mesa, once (needs meson, ninja)
GL=1 REST=1 FLASHBOOT=1 CRYPTEXES=flash userland/build.sh          # root.apfs + flashboot.apfs
. common/lib.sh && load_device instantnoodle                       # the profile, before xl enter
./xl enter && ./xl ramdisk userland/flashboot.apfs
XL_SECS=900 XL_CMDLINE="cpus=8 cpufreq=1 bootcpu=7 ufs=2 root_apfs=ufs_darwin haptic=5 torch=3 gpu=1" ./xl run
```

Boot-args for the GPU and Metal work: `metal=1` lets the Metal driver serve test programs (`RC_JOBS=rc.jobs.metaldefer`
runs the blits, every fragment shader and the timings); `metal=2 metal_profile=1` hands it the whole system;
by default a Metal command buffer is one GPU submission and one fence (8 full-screen draws block `-commit` for
107 us, against 7086 us with a fence per draw); `metal_sync=1` makes every draw wait for its own fence again
(slower, but a GPU failure names the draw);
`log_launchd=0` keeps launchd's lines out of the log that comes back, so long test output survives.

The panel runs at 90 Hz without tearing: frames are double-buffered and pushed by the MDP on the panel's TE,
and CoreAnimation's frame clock follows that TE (`fb_hz=60`, `fb_push=1` or `fb_push=0` fall back to 60 Hz, one
buffer, or the firmware's autorefresh). Touch is interrupt-driven (`touch_irq=0`: 10 ms polling). Measurements,
fallbacks and the RAM-only procedure: [instantnoodle latency](docs/instantnoodle-latency.md).

## Install as a dual boot (no computer needed afterwards)

HackiOS can live on the phone next to Android: the UEFI with our loader in the spare A/B slot, the kernel and the
boot volume in darwin, a boot menu at power-on, sessions with no time limit. Android is never written, and fastboot
always brings it back. See [docs/INSTALL.md](docs/INSTALL.md).

| | |
|---|---|
| **Boots from the flash** | no `fastboot boot`, no computer, no RAM-only session: power on, menu, HackiOS |
| **Two versions installed** | updates go into the other slot; "HackiOS, previous version" is always in the menu |
| **Boot guard** | a version that does not come up 3 times is skipped by itself -- the previous one boots, then Android |
| **Verified boot, your key** | SHA-256 of every bundle; with an owner key pinned, Ed25519-signed bundles only |
| **A/B switch in the loader** | back to Android from the menu: backup GPT first, CRCs, both tables read back |
| **Device identity checks** | nothing is written to a phone that is not the profile's `CODENAMES` |
| **Tested on every push** | eight QEMU scenarios of the boot menu and the guard, crypto checked against Python |

```sh
common/hackios detect                # which profile is the phone on adb? (writes nothing)
common/hackios doctor redwood        # every precondition, checked (writes nothing)
common/hackios install redwood
common/hackios switch redwood hackios
common/hackios health redwood        # the boot guard's view
common/hackios test                  # the whole dual boot in QEMU, no phone
```

## Prebuilt boot image

Every release has `hackios-redwood-boot.img` attached: the redwood UEFI with our loader inside, ready for
`fastboot boot`. It contains no Apple files; the system image is still made on your machine (above).

## Building from source

`common/build.sh <device>` runs every step that this machine can run and uses `prebuilt/` for the rest.

- **Kernel** — needs [theos](https://theos.dev/docs/installation-linux) in `~/theos` (cctools and
  `iPhoneOS16.5.sdk`) and `clang lld python perl`. Details: [xnu/README.md](xnu/README.md).
  ```sh
  xnu/apply.sh && xnu-deps/setup.sh && ./xnu-build.sh -j8 -k
  ```
- **Loader** — `xnuloader/build.sh` (clang + lld-link, an EFI application without EDK2).
- **Images** — `REST=1 userland/build.sh` (variables documented at the top of that file).
- **UEFI** — [uefi/README.md](uefi/README.md) (Project Silicium port); not needed to run, `prebuilt/uefi` has it.
- After rebuilding: `tools/update-prebuilt.sh` refreshes `prebuilt/`.

## Porting to another device

```sh
common/new-device.sh <codename> path/to/device.dtb   # profile + loader platform from the phone's device tree
```

Then follow [docs/PORTING.md](docs/PORTING.md): the UEFI port, the loader's platform description, the kernel's
drivers, the firmware. Device profiles live in `common/devices/` (`redwood.conf`, `qemu-virt.conf`, `_template.conf`).

## How it works

```
fastboot boot ──► UEFI (Mu-Silicium) ──► xnuloader (EFI application)
                                           │  stays resident: USB link (fastboot protocol),
                                           │  watchdog, receives the kernel's log
                                           ▼
            XNU (BCM2837 config as the base + our platform)
              ├─ GICv3, timer, video console, return to the loader
              ├─ APFS (images in RAM, iOS volume on flash), ramfs (/private/var, /tmp)
              ├─ minicrypto instead of corecrypto, AMFI and Sandbox stand-ins (MAC policies)
              ├─ driver replacements: IOSurface, IOMobileFramebuffer, AppleKeyStore, network stack
              ├─ input: /dev/hk-touch ◄─ Goodix GT9916 (phone) / virtio-input (QEMU)
              ├─ GPU: Adreno 642L, Wi-Fi: WCN6750 (ath11k via LinuxKPI), sound: ADSP / virtio-sound
              ▼
   iOS 16.5 launchd → logd, notifyd, cfprefsd, runningboardd, backboardd, SpringBoard, apps
              ├─ our libraries are injected into backboardd, SpringBoard and mediaserverd
              │  (virtual HID services, a few ObjC method swaps — in memory only)
              └─ mediaserverd gets our CoreAudio output device (HKAudio.driver)
```

The iOS programs, frameworks and dyld shared cache are not changed by a single byte: what they miss is done by the
kernel (driver replacements) or by a library the kernel injects with `DYLD_INSERT_LIBRARIES` (`kern.exec_inject`).

## Layout

| Path | What |
|---|---|
| `common/` | `setup.sh` (workspace), `build.sh` / `run.sh` (per device), `new-device.sh`, `devices/*.conf`, `ugit` |
| `xl` | drives the phone over USB: loader, kernel runs, logs, screenshots, screen and sound recording |
| `xnuloader/` | the loader: `loader.c` (UEFI application), `mkpayload.py` (kernel + images + arguments container) |
| `xnu/` | our XNU changes: patch, new files, `apply.sh` / `export.sh` |
| `xnu-deps/` | everything for building XNU on Linux: `setup.sh`, tool wrappers, stubs |
| `xnu-build.sh`, `xnu-env.sh` | kernel build |
| `uefi/`, `mkxnuimg.py` | the UEFI port and packing the boot image for `fastboot boot` |
| `wifi/lx/` | the layer on which Linux's Wi-Fi driver builds for our kernel |
| `qemu/` | QEMU runs, touch input, screenshots, stack symbolication |
| `tools/` | recording (`recording.py`, `sound.py`), UFS/flash tools, `mk-ath11k.sh`, `new-workspace.sh`, host tests |
| `prebuilt/` | built kernel, loader and UEFI pieces (`NOTICES.md` for the third-party code in the kernel) |
| `docs/` | screenshots, [checks.md](docs/checks.md), [PORTING.md](docs/PORTING.md) |

## Debugging

- Session log: QEMU's stdout or `logs/HHMMSS-run.log` from the phone.
- `RC_JOBS=rc.jobs.tour REST=1 userland/build.sh` — an image that unlocks and opens every app of the first page.
- `userabort:` in the log is the stack of a process that called `abort()`; `usertrap:` (with `systrace=1`) — crashes.
- Live stacks (QEMU): `qemu/touch.py stacks NAME`, then `qemu/symstack.py LOG NAME`
  (needs [`ipsw`](https://github.com/blacktop/ipsw)).
- How a check can lie to you: [docs/checks.md](docs/checks.md).

## Screenshots

From the phone's screen, one session (`GL=1 REST=1`, `gpu=1`):

<p>
<img src="docs/homescreen-2.jpg" width="160" alt="Home screen, page 2">
<img src="docs/search.jpg" width="160" alt="Search">
<img src="docs/settings-general.jpg" width="160" alt="Settings > General">
<img src="docs/calendar.jpg" width="160" alt="Calendar">
<img src="docs/clock.jpg" width="160" alt="Clock">
</p>
<p>
<img src="docs/wallet.jpg" width="160" alt="Wallet">
<img src="docs/messages.jpg" width="160" alt="Messages">
<img src="docs/music.jpg" width="160" alt="Music">
<img src="docs/maps.jpg" width="160" alt="Maps">
<img src="docs/appstore-tabs.png" width="160" alt="App Store">
</p>

## Licenses

- No iOS files are in the repositories: `ios165/fetch.sh` downloads them from Apple's CDN to your machine.
- `xnu/` is a derivative work of XNU and is distributed under APSL 2.0, like XNU itself; `prebuilt/mach.*` is the
  kernel built from it. It also contains ath11k (BSD-3-Clause-Clear) and FreeBSD LinuxKPI code (BSD-2-Clause):
  see [prebuilt/NOTICES.md](prebuilt/NOTICES.md).
- `xnu/files/bsd/miscfs/apfs/libzbitmap/` and `xnu-deps/build/lzbitmap/` — libzbitmap (Corellium LLC, MIT here);
  `xnu/files/bsd/miscfs/apfs/libdeflate/` — libdeflate 1.24 (Eric Biggers, MIT, `COPYING` there).
- ath11k, Linux and FreeBSD sources are not in the repository: `tools/mk-ath11k.sh` fetches pinned releases.
  `wifi/lx/patches/*.patch` are our changes to FreeBSD headers; the rest of `wifi/lx/` is ours.
- `uefi/files/Binaries/redwood` and `prebuilt/uefi/SILICIUM_UEFI.fd` contain UEFI drivers from the phone's
  Xiaomi firmware, distributed the way Project Silicium distributes them for other devices;
  `prebuilt/uefi/mkbootimg.py` — AOSP, Apache-2.0.
- This repository is licensed under the [Apple Public Source License 2.0](LICENSE): our changes to XNU are a derivative of XNU and must be, and our other code here (loader, scripts) is released under the same license. Third-party code keeps its own license, as listed above.

## Team

OOSEX GANG — [@oodze](https://t.me/oodze) (founder), [@netusd](https://t.me/netusd), [@wannqn](https://t.me/wannqn),
[@hxfuxyyXD](https://t.me/hxfuxyyXD). News: [t.me/oosexgangdevs](https://t.me/oosexgangdevs).
