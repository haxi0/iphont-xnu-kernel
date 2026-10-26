# Research: boot chain behaviour, UEFI, the stock bootloader and recovery

Facts about how these phones actually boot and what the stock chain does to us. User-facing install/recovery steps are in
[../INSTALL.md](../INSTALL.md); the design is in [../architecture.md](../architecture.md). Tags: **[observed]**, **[inferred]**.

## Chain

| Stage | Redwood (POCO X5 Pro) | OnePlus 8 |
|---|---|---|
| firmware | Xiaomi XBL → ABL (fastboot) | OnePlus XBL → ABL (fastboot), OxygenOS 11 bootloader, `current-slot: b` |
| what ABL boots | `fastboot boot IMAGE` (RAM-only) or the spare slot's `boot` (installed) | `fastboot boot IMAGE` (RAM-only) or `boot_b` (installed) |
| UEFI | Project Silicium / Mu-Silicium port (`uefi/`), drivers from Xiaomi firmware `OS1.0.3.0.UMSMIXM` | same project; drivers mostly from `kebab` (OnePlus 8T) except USB (OnePlus 8's own, from `xbl_b`) |
| loader | `xnuloader` runs as a UEFI application, stays resident | same |
| kernel | loaded from the "XNUP" payload or the flash bundle | same |

UEFI **never exits boot services**. The loader publishes a return stub (`/chosen/loader-return`) and the kernel returns to
it with its log; this also lets the loader serve storage, USB, the battery and the charger from core 0 while the kernel
runs on another core (see [../subsystems/storage.md](../subsystems/storage.md), [../subsystems/cpu.md](../subsystems/cpu.md)).

### The XNUP container
`mkxnuimg.py` packs the UEFI with the hook that starts the loader and an optional payload into an Android boot image for
`fastboot boot` (the UEFI image first, then the container). `xnuloader/mkpayload.py` builds the container: `u32 magic "XNUP"
(0x50554E58), u32 size`, then entries `{char name[16], u32 size, u32 reserved, data padded to 16}`, ending on an entry with an
empty name; entries seen: `kernel`, `cmdline`, root image(s). The boot shim (`BootShim.S`) copies the container to
`PAYLOAD_BASE` (0xF0000000 for both phones) and `BootManagerLib` starts the loader from it instead of the menu. The UEFI image
sits at `FD_BASE` (redwood `0x9FD00000`/`0x200000`; OnePlus 8 `0x9FC00000`/`0x300000`). `MAX_MEM_SIZE` is 6 GB (the
kernel's bootstrap page tables cover 8). The kernel is linked at `0xFFFFFFF004000000 + 0x03000000`; a root image too big for the
payload area is placed once at `RESIDENT_RD_OFFSET` (0x08000000) in the kernel's memory window and survives any number of
kernel runs (it is read-only), keeping `RESIDENT_RD_RESERVE` (0x30000000) free.

### Memory
The loader picks the largest free RAM block as XNU's `[physBase, physBase+memSize)` (about 4.2-4.3 GB usable on the phones
under the 6 GB cap; QEMU gets the same window with `QEMU_RAM_GB=7`), copies the Mach-O segments, builds an Apple-format device
tree (iPhone-derived identity properties, display, cores, GIC addresses from the profile) and `boot_args` behind the kernel,
exits to EL1 with the MMU off. The firmware's display region is `cont_splash_region@9c000000` (0x2300000 bytes) on the
OnePlus 8; `FB_BASE` 0x9C000000 there, 0xE1000000 on redwood.

## The stock ABL and a cable left in [observed 2026-10-24]
After a reset **with the USB cable in**, the PMIC's charger detection may take the computer's port for a wall charger (ABL's
own log, partition `abl_log`: `usb: chgr - DCP_CHARGER`). ABL's fastboot then never comes up on USB and the phone switches
itself off; a hard reset with the cable in also starts as off-mode charging (`startupmode=dc_charger`). It is the same on
OxygenOS. Cure: unplug, get into the mode wanted, plug in again. Consequences built into the design:
- Nothing needs a reset into the stock fastboot: the loader does what it would be needed for over a link that is already up —
  `xl install` (a bundle), `xl flashboot` (installed boot, no reset), `xl flashpart boot_a|boot_b IMAGE EXPECT` (a boot partition,
  only if its SHA-256 begins with EXPECT, read back; **`xl flashpart` compares only the first 16 hex digits of EXPECT**),
  `xl lastlog`, `xl settime`.
- The loader's own reboots set the stock way (PS_HOLD hard reset, reason in the PON's soft spare register) so
  `fastboot reboot bootloader`/`recovery` work. The OnePlus 8's PON stores the reboot reason across a hard reset; left as a
  previous stage set it, a reset may be a *warm* one after which ABL showed fastboot without USB and switched off.
- The menu entries and `fastboot reboot bootloader` wait up to 60 s for the cable to come out ("unplug the cable").
- ABL keeps a real log in the `abl_log` partition (a ring; strings; "read BootCount"). During failed reboot-bootloader experiments
  ABL once fell back to slot a; **always check slot attributes (LUN4 sde GPT, `boot_b` A/B byte `0x77` = active) before blaming boot_b.**
- **ABL swap idea** (other official ABL / slot-a stack; abl/xbl/tz a != b): blocked by anti-rollback and an unverified EDL programmer;
  the owner said not to swap ABL/XBL/TZ or change slots.

## OnePlus 8 recovery facts
- The phone has **no Android any more** (since 2026-10-23): only the stock bootloader (fastboot) and TWRP (`recovery_a`/`_b`).
  Never wait for adb/Android. `userdata` holds the iOS volume. The UEFI+loader is in `boot_b`.
- Back-ups: the stock `boot_b` is at `ios-instantnoodle/backup/partitions/boot_b.img` (sibling directory **outside this repository**,
  `.gitignore`d) and the live `boot_b` of 2026-10-24 at `out/boot_b-restore.img` with its hash in `out/boot_b-now.sha256`
  (both ignored by git). **A fresh clone has no recovery image.** Restore:
  `./xl flashpart boot_b out/boot_b-restore.img $(cat out/boot_b-now.sha256)` (hash-checked) or ABL
  `fastboot flash boot_b ../ios-instantnoodle/backup/partitions/boot_b.img`. Rollback of the 2026-10-25 loader: the old
  `boot_b` image was not kept; rebuild the loader from git HEAD or restore the stock backup via ABL fastboot.
- Entering the loader: `xl enter` from stock fastboot; `xl reboot` lands in the bootloader. After a hang `xl run` resets the
  phone; a reset with the cable in makes ABL power off, the user powers on and picks "Developer mode" in the menu. Back-to-
  loader recipe that worked: `xl stop` during an installed run, reset, pick Developer mode, loader idle in ~20 s. The
  OnePlus 8's bootloader often needs a physical USB replug after reboot.
- **Never `fastboot flash userdata` without the owner's fresh explicit approval** (rejected once, 2026-10-22; a wipe for iOS
  was approved 2026-10-21 only). Do not touch other partitions; do not swap ABL/XBL/TZ or change slots.
- Bare `./xl enter` builds **redwood's** UEFI (the `mkxnuimg.py` defaults) and hangs the OnePlus at its fastboot logo; ABL then
  refuses every boot image ("Load Error") until `fastboot reboot bootloader`. Always `. common/lib.sh; load_device
  instantnoodle` first. `xl enter` needs `nix-shell` (clang) on the NixOS host; retry if "loader did not come up".

## Installed boot (dual boot / standalone)
Bundle slots, boot state, boot guard (3 starts without "came up" → previous version → Android or Developer mode), SHA-256 and
Ed25519 verified boot with an owner key, A/B switch with GPT read-back: see [../INSTALL.md](../INSTALL.md). A timed bundle
(`loader_return_after=`) ends in the loader's USB link instead of a reset, so a boot from flash can be checked with no hands
(`fastboot get_staged LOG`, `xl health`). The menu countdown (5 s) is too short to pick Developer mode by hand (open).
The installed boot is **verified on the OnePlus 8**: power on → ABL → `boot_b` (our UEFI, reproducible: `mkxnuimg` mtime 0) →
menu → newest bundle; iOS starts, date right, iOS power-off switches the phone off, changes kept after power-on (2026-10-24).
The OnePlus 8's loader identifies `userdata` by name, both GUIDs and first/last LBA, and refuses otherwise. Dual-boot
and the Android-switching paths are tested in QEMU (`common/flash/qemu-test.sh`, 14 scenarios); a dual-boot install on a real redwood
phone is **not recorded as verified** in this repository (the README says the first install should be done with the backup at hand).

## UEFI port notes (`uefi/`)
- `uefi/BASE` records the Mu-Silicium commit; `uefi/mu-silicium.patch` changes existing files (`BootShim/BootShim.S`,
  `BootManagerLib`); `uefi/files/` holds new files (`Platforms/Xiaomi/redwoodPkg`, `Platforms/OnePlus/instantnoodlePkg`,
  `XnuLoader.[ch]`, config, DTBs, `Binaries/*`). `uefi/apply.sh` clones Mu-Silicium (~3 GB with submodules) and applies the port;
  `uefi/export.sh` saves changes back.
- Firmware drivers are extracted with UEFIReader from `xbl.img`. Silicium's five usual patches apply (ButtonsDxe maps the power
  key to ENTER, etc.: `uefi/files/Binaries/*/README.md`). The OnePlus 8's USB drivers (`UsbPwrCtrlDxe`, `UsbfnDwc3Dxe`,
  `UsbDeviceDxe`, `UsbConfigDxe`, `UsbInitDxe`) are its own: with kebab's the loader's USB link did not start (`start_ex`
  returned `EFI_NOT_FOUND`). `UsbConfigDxe` is unpatched (kebab's removes the USB de-init at ExitBootServices).
- A pitfall: `mu_nasm` is installed through NuGet and needs Mono; point `Mu_Basecore/BaseTools/Bin/mu_nasm_extdep` at the system
  nasm (see `uefi/README.md`).
- **A build from scratch by these instructions has not been re-run** since the tree was built along the way.
- Abandoned, kept on disk: `uefi-instantnoodle/`, `UniXNU-instantnoodle/`, `unixnu/` (a different project that reached kernel
  entry + a UFS register probe; its UFS patch adopted the firmware SMMU entry SID `0x4e0` → context bank 2). Do not resume unless asked.

## Boot logo
The Apple logo seen at boot comes from iOS, not from UEFI: see [ios-userland.md](ios-userland.md) §7.
