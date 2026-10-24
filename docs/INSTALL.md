# Installing HackiOS as a dual boot

The developer setup boots HackiOS from a computer: `fastboot boot` puts the UEFI in RAM, `xl` sends the kernel and
the system image over USB, a session lasts as long as `XL_SECS`. An **installed** phone needs no computer: you
switch it on, a menu appears, and HackiOS runs until you switch it off -- next to an Android that is never touched.

## How it is laid out

| Where | What |
|---|---|
| Android's A/B slot (e.g. `_b`) | Android, as it was. Nothing of it is written. |
| the spare slot's `boot` (e.g. `boot_a`) | the UEFI (Project Silicium port) with our loader, no payload in it |
| `darwin`, blocks 0..2042367 | the iOS volume and both cryptexes (written once, see the userland README) |
| `darwin`, blocks 2042368.. | **boot area**: two bundle slots of 96 MiB -- kernel, boot volume, command line |
| `darwin`, block 2091520 | the **boot state**: what the loader started, how often, what came up (the boot guard) |
| `darwin`, blocks 2097152.. | the var store (what iOS writes survives a reboot), two slots |

At power-on the phone's bootloader starts the active slot. When that is the spare slot, the UEFI starts our loader
with no payload, and the loader shows its menu:

```
  HACKiOS boot menu        volume: move   power: select
   > HackiOS  (v0.2.0 2026-10-22)
     HackiOS, previous version  (v0.1.0 2026-10-01)
     Android
     Developer mode (USB, xl)
  HackiOS in 5 s
```

* **HackiOS** -- the newest good bundle from darwin's boot area runs with no time limit. Before it runs, the loader
  checks the header's own sum (a torn header is no header), the **SHA-256** of the whole bundle and -- with an
  owner key pinned -- its **Ed25519 signature**. A bundle that fails falls back to the other slot.
* **HackiOS, previous version** -- the other slot, whenever it holds a good bundle: an update you do not like is
  one key press away from being undone.
* **Android** -- the loader exchanges the A/B bytes of `boot_a` and `boot_b` in the GPT (backup table first, then
  the primary one, both read back) and resets: the phone comes up in Android.
* **Developer mode** -- the USB link `xl` uses, as after `xl enter`.

## The boot guard: a bad update rolls itself back

Every start of a bundle is counted in the boot state before the kernel runs, and iOS reports that it came up
(`var_store bootok`, a minute into the run). A generation that was started **3 times in a row without coming up**
-- a kernel panic, a hang, a userland that dies -- is skipped by the countdown: the **previous version** boots
instead, and when that does not come up either, the countdown goes to **Android**. No computer, no cable: a phone
left alone always ends up in something that works. A choice made with the keys is always honoured (and counted).

```sh
common/hackios health redwood      # what was started, how often, what came up, what was given up on
common/hackios rollback redwood    # boot the previous version from now on
common/hackios unhold redwood      # back to the newest, and forget what was given up on
```

## Verified boot held by the owner

A loader built with an **owner key** pinned boots only bundles signed by that key: a bundle written into darwin by
anything else -- a stray tool, a broken update, someone with the phone for a minute -- is refused. The key is
yours, made on your computer; no vendor holds it.

```sh
common/hackios key init            # a new Ed25519 key in ~/.hackios/owner.key (keep a copy!), pinned into the loader
xnuloader/build.sh                 # the loader, with the key in it
common/hackios install redwood     # install.sh signs the bundle by itself when the key is there
common/hackios verify redwood ~/.hackios/owner.key    # read both slots back: SHA-256 and signature
```

Without a key the loader takes unsigned bundles, as before; `common/hackios key unpin` goes back to that.

## Install

Needs: an unlocked bootloader, Android with root (Magisk) and adb, the `darwin` partition with the iOS volume and
the cryptexes on it, the workspace built (`common/setup.sh`, `ios165/fetch.sh`).

```sh
common/hackios detect              # is the phone on adb one of ours? (writes nothing)
common/hackios doctor redwood      # every precondition, one by one (writes nothing)
common/hackios install redwood
```

`doctor` and every command that writes refuse a phone whose `ro.product.device` is not one of the profile's
`CODENAMES`: a profile for one phone cannot be installed on another. `install` builds the boot volume and the bundle, shows the state (`flashctl.py status`), backs up `boot`, `vbmeta`, `dtbo`,
`vendor_boot` of both slots and the GPT of their unit into `backups/<serial>/<time>/`, writes the bundle into
darwin and the UEFI into the spare slot's `boot`, and reads every write back. Then:

```sh
python3 common/flash/flashctl.py switch hackios      # from Android: bootctl set-active-boot-slot, reboot
```

## Update

A new kernel or boot volume goes into the boot slot that does not hold the running bundle:

```sh
common/hackios update redwood
```

If that write is cut off (cable, battery), the phone still boots the previous bundle.

## Getting Android back -- three ways, from the easiest

1. The HackiOS menu: **Android**.
2. Fastboot (volume down + power): `python3 common/flash/flashctl.py switch android`, or by hand
   `fastboot --set-active=b && fastboot reboot` (the slot Android was on; the backup's `manifest.json` says which).
3. Remove the UEFI: `python3 common/flash/flashctl.py restore` puts the spare `boot` back from the backup.

Fastboot lives in the phone's own bootloader, which this install never writes: whatever happens to the UEFI or
the loader, the phone can be brought back with a cable.

## A phone without Android (OnePlus 8)

The OnePlus 8's port took its whole `userdata` for the iOS volume (README, "On the phone"), so there is no Android
to install from, to switch slots in, or to go back to. The same loader, bundles, boot guard and var store are used;
what differs is where the UEFI goes and how things are written:

| Where | What |
|---|---|
| `boot_b` (the slot the bootloader starts, `current-slot: b`) | the UEFI with our loader and no kernel -- replaces OxygenOS's kernel, which has no system left to run (backup: `ios-instantnoodle/backup/partitions/boot_b.img`) |
| `userdata`, blocks 0..2042367 | the iOS volume and both cryptexes, as now (`tools/store-image.py`) |
| `userdata`, blocks 2042368..2091519 | the boot area: two bundle slots of 96 MiB |
| `userdata`, block 2091520 | the boot state (the boot guard) |
| `userdata`, blocks 2097152..3145727 | the var store: `/private/var` between boots, two slots of 2 GiB |
| `boot_a`, `recovery_a`/`_b` (TWRP), `abl`, `xbl`, the GPT, the slots' attributes | not written |

The loader finds `userdata` by its name, both GUIDs and its first and last LBA (`struct platform` in
`xnuloader/loader.c`): a different partition table and it writes nothing. The menu has no "Android" entry
(`.no_android`), and when every installed version has failed 3 times the countdown waits in **Developer mode** for
a computer instead of starting it a fourth time (the keys can still choose HackiOS).

Writes go through the loader itself, not through Android: `xl install BUNDLE` sends a bundle over the loader's USB
link and the loader writes it into the slot that does not hold the newest good bundle, header last, everything read
back (`fb_install` in `xnuloader/flashboot.c`). The UEFI goes into `boot_b` by the phone's own fastboot.

```sh
. common/lib.sh && load_device instantnoodle
common/flash/install-xl.sh instantnoodle bundle   # build, then (YES) the bundle into a boot slot; makes out/instantnoodle-uefi.img
./xl health                                         # the boot slots and the boot guard (writes nothing)
fastboot boot out/instantnoodle-uefi.img            # the installed boot from RAM: menu, HackiOS, a minute later "came up"
common/flash/install-xl.sh instantnoodle uefi     # (YES) fastboot flash boot_b with that same file
```

The `uefi` step refuses until a bundle has come up once (the boot state's "last up", which the preview writes), on
any fastboot device but one `kona`, and for any partition but `boot_a`/`boot_b`. `common/hackios install instantnoodle`
runs the bundle step and stops there until the preview. **Update**: `common/flash/install-xl.sh instantnoodle update`
with the phone in the menu's Developer mode (or `xl enter` from fastboot); the previous version stays in the other
slot. **Undo**: `fastboot flash boot_b ../ios-instantnoodle/backup/partitions/boot_b.img` -- fastboot is the
bootloader's (key combination at power-on), never written, and TWRP is still in `recovery_a`/`_b`.

What an installed run keeps: `/private/var` (home screen, settings, app data) is saved into the var store when iOS
restarts or shuts down (launchd's SIGTERM to `var_store watch`), and every `var_save_every=` seconds (600 in the
profile) against a reset with the keys, which saves nothing. Shut down switches the phone off (the kernel returns
reason 3, the loader calls `ResetSystem(EfiResetShutdown)`); restart starts it again through the menu.

## Things to know

* **Android updates (OTA)** write the spare slot -- they overwrite the UEFI, and for a while after an update the
  slots must not be switched (`flashctl.py switch` refuses while a Virtual A/B merge is pending). After an update:
  `common/flash/install.sh <device>` again.
* **Do not lock the bootloader** with HackiOS installed.
* The menu is a text console for now; the loader's own graphics are a later step.
* Tested on every push (`.github/workflows/ci.yml`): the whole boot path in QEMU on a disk laid out like the phone,
  every path of the menu -- `common/hackios test` runs the same eight scenarios (boot, previous version, the guard
  skipping a generation, falling back to Android, Android by hand with both GPTs checked, a tampered bundle, a
  signed one, a forged one). The loader's SHA-256 and Ed25519 are checked against Python's on every push too
  (`tools/crypto-test/run.py`). The first install on a real phone should be done with the backup at hand.
