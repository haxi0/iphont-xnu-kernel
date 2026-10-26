# Installing HackiOS (dual boot, or standalone on the OnePlus 8)

> Scope: written for **redwood** (dual boot next to Android); the OnePlus 8 section ("A phone without Android") is the one in daily
> use and is verified on the phone (2026-10-24). A dual-boot install on a redwood phone is not recorded as verified here; do it with
> the backup at hand. Boot internals and ABL quirks: [research/boot-chain-and-recovery.md](research/boot-chain-and-recovery.md).
> Tooling for the installed phone: `xl health|install|flashboot|flashpart|lastlog|settime` (header of `xl`).

The developer setup boots HackiOS from a computer: `fastboot boot` puts the UEFI in RAM, `xl` sends the kernel and
the system image over USB, a session lasts as long as `XL_SECS`. An **installed** phone needs no computer: you
switch it on, a menu appears, and HackiOS runs until you switch it off -- next to an Android that is never touched.

## How it is laid out

| Where | What |
|---|---|
| Android's A/B slot (e.g. `_b`) | Android, as it was. Nothing of it is written. |
| the spare slot's `boot` (e.g. `boot_a`) | the UEFI (Project Silicium port) with our loader, no payload in it |
| `darwin`, blocks 0..2042367 | the iOS volume and both cryptexes (written once, see [subsystems/storage.md](subsystems/storage.md)) |
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

The OnePlus 8's port took its whole `userdata` for the iOS volume ([devices/instantnoodle.md](devices/instantnoodle.md)), so there is no Android
to install from, to switch slots in, or to go back to. The same loader, bundles, boot guard and var store are used;
what differs is where the UEFI goes and how things are written:

| Where | What |
|---|---|
| `boot_b` (the slot the bootloader starts, `current-slot: b`) | the UEFI with our loader and no kernel -- replaces OxygenOS's kernel, which has no system left to run (backup: `ios-instantnoodle/backup/partitions/boot_b.img`) |
| `userdata`, blocks 0..2042367 | the iOS volume and both cryptexes, as now (`tools/store-image.py`) |
| `userdata`, blocks 2042368..2091519 | the boot area: two bundle slots of 96 MiB |
| `userdata`, block 2091520 | the boot state (the boot guard) |
| `userdata`, blocks 2097152..3145727 | the var store (the older archive-based persistence): two slots of 2 GiB |
| `userdata`, blocks 3145728..end | the **HFS+ data volume** (89 GB) that is `/private/var` on the installed boot (`hk_data=1 var_data=hfs`); formatted once by `init` if neither volume header is found (`var_data_format=1`) |
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
slot. **Undo**: `fastboot flash boot_b ../ios-instantnoodle/backup/partitions/boot_b.img` (that backup lives in a sibling directory **outside this
repository and is not in a fresh clone**; also `out/boot_b-restore.img` + `out/boot_b-now.sha256`, git-ignored, used by `xl flashpart`) -- fastboot is the
bootloader's (key combination at power-on), never written, and TWRP is still in `recovery_a`/`_b`.

A **timed** bundle (`loader_return_after=` on its command line: a test) does not reset when its kernel comes back:
the loader brings up its USB link, so `fastboot get_staged LOG` has the run's log and `./xl health` the boot state --
how a boot from the flash is checked with no hands on the phone. An untimed bundle (the installed one) resets.

**The stock bootloader and a cable left in.** After a reset with the USB cable in, the PMIC's charger detection
may take the computer's port for a wall charger (ABL's own log, partition `abl_log`: `usb: chgr - DCP_CHARGER`):
ABL's fastboot then never comes up on USB and the phone switches itself off; a hard reset with the cable in also
starts as off-mode charging (`startupmode=dc_charger`). Unplugging, getting into the mode wanted, then plugging in
again is the cure; it is the same on OxygenOS and not something this port changes. So nothing here needs a reset into
the stock fastboot: the loader does what it would be needed for, over a link that is already up --
`xl install` (a bundle), `xl flashboot` (the installed boot, no reset), `xl flashpart boot_b IMAGE EXPECT` (a boot
partition, only when its SHA-256 is EXPECT, read back), `xl lastlog` (what the previous start logged), `xl settime`
(the date: an offset from the PMIC's clock, which Android never set). The loader's own reboots set the stock way of
it (PS_HOLD a hard reset, the reason in the PON's soft spare register: `fastboot reboot bootloader` / `recovery`).

What an installed run keeps: since 2026-10-24 `/private/var` is a journaled HFS+ volume on the rest of `userdata`
([subsystems/storage.md](subsystems/storage.md)), written as iOS runs and synced on shutdown (what a hard reset loses is not recorded). (Before that, `/private/var` was saved into the var store archive when iOS restarted or shut down.) Shut down switches the phone off (the kernel returns
reason 3, the loader calls `ResetSystem(EfiResetShutdown)`); with the cable in, the phone then stays off and
charges until the power key or the cable comes out ([subsystems/power.md](subsystems/power.md), off-mode charging; built, not yet run on the phone). Restart starts it again
through the menu.

## Things to know

* **Android updates (OTA)** write the spare slot -- they overwrite the UEFI, and for a while after an update the
  slots must not be switched (`flashctl.py switch` refuses while a Virtual A/B merge is pending). After an update:
  `common/flash/install.sh <device>` again.
* **Do not lock the bootloader** with HackiOS installed.
* The menu is a text console for now; the loader's own graphics are a later step.
* Tested without a phone: `common/hackios test` (= `common/flash/qemu-test.sh`) runs **14 scenarios** (boot, previous version, the guard
  skipping a generation, falling back to Android, Android by hand with both GPTs checked, remembered choice, not-ours, a tampered bundle,
  a signed one, a forged one, darwin as a file of userdata with and without its marker, no-android, the off-charging flag); the loader's SHA-256 and Ed25519
  against Python's: `tools/crypto-test/run.py`. **There is no CI any more**: the workflow was removed in commit `cd4210b` (the Forgejo host is a
  Raspberry Pi with no runner), so these run only when someone runs them. The first install on a real phone should be done with the backup at hand.
* Recovery when the phone will not boot: see "Getting Android back" above (redwood) or the OnePlus 8 undo steps; ABL's USB-after-reset limit is in
  [research/boot-chain-and-recovery.md](research/boot-chain-and-recovery.md). **Never `fastboot flash userdata` without the owner's explicit approval.**
