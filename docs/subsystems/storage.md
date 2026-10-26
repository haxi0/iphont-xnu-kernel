# Storage, filesystems and persistence

How the system image, the iOS volume and `/private/var` are stored, read and (rarely) written. The key fact is
that **UEFI never exits**: Qualcomm's own UFS driver (`UFSDxe`) has the controller, its PHY, clocks and power up and
stays resident, and the loader serves the kernel from core 0. The kernel has no UFS driver of its own in the
normal path.

## Images and RAM disks

| Image (userland repo build) | Where it lives | What |
|---|---|---|
| `userland/root.apfs` ("the small image") | travels in the payload next to the kernel, RAM disk `/dev/md1`, mounted on `/hackios` | our `init`, test programs, `etc/rc`, launchd jobs (`LaunchDaemons/`), `fw/` (firmware), injected libraries, boot logo (`/progressui`). Rebuilt every time. |
| `userland/system.apfs` | uploaded once per loader session with `xl ramdisk` into the phone's RAM (about 2.5 GB, ~80 s), the resident RAM disk `/dev/md0`, the root | the selected part of iOS 16.5 at real paths, the dyld shared cache, `stage0` as `/sbin/launchd` (the real one is `/sbin/launchd.ios`) |
| `userland/flashboot.apfs` | `FLASHBOOT=1`: the RAM part of a boot whose root is the iOS volume on flash | with `CRYPTEXES=flash`, the boot volume alone |
| iOS volume on `darwin`/`userdata` | blocks `0..2042367` of the partition | the complete iOS root plus both cryptexes, written once from the computer (`tools/darwin-write.py`, or `tools/store-image.py` for the OnePlus 8) |

`stage0` mounts the small image on `/hackios` and hands over. `init` (process 1) mounts a memory file system on
`/private/var`, creates the directories a system expects, runs the commands in `etc/rc` (usually `exec
/sbin/launchd.ios`), halts if it is still process 1. See [userland.md](userland.md).

All images are APFS containers made by `tools/apfs/mkapfs.py` (userland repo). The iOS volume is 5.6 GB (318,098
entries) which does not fit the 4.3 GB the firmware leaves usable as a RAM disk; hence the flash-root path.

## The loader serves the storage (`ufs=2`)

`xnuloader/loader.c` publishes a page (`ufs_shared_pa=` on the kernel's command line) holding the table of GPT
partitions, a request, an answer and a data buffer. `bsd/dev/hk_ufs.c` writes a request (partition, blocks) and core 0
reads them with `UFSDxe`'s `ReadBlocks` into the buffer. One request at a time (the page has one), synchronous:
Block I/O 2 exists but UEFI's driver finishes asynchronous requests from the UEFI timer, which does not tick on
core 0 while the kernel runs (measured: eight reads 61.5 ms asynchronously against 1.0 ms one by one). Each side
writes only its own cache lines. Devices: `/dev/ufs_NAME` and `/dev/rufs_NAME` for every GPT partition. Measured:
131 MB/s cold, 314 MB/s warm (2026-10-25).

**Writes** exist for exactly one place. With `ufs_write=1` only `/dev/rufs_darwin`, the port's own partition, takes
writes, and in it only whole blocks from `BLOCK_WRITE_FIRST` (2042368) on, past the iOS volume and both cryptexes.
The partition must be the one the port was set up with (name, size, place, logical unit; GUIDs too on the loader's
side), the strategy routine of block devices always refuses, and the loader reads every write back before it
answers. Three independent refusals (kernel, loader, UEFI's own bounds). Without `ufs_write=1` nothing writes.
The page's layout is versioned (`BLOCK_VERSION 3`) so an old kernel can talk to a new loader and the other way about.

### `ufs_native=1`: the kernel's own controller driver (read only)

`hk_ufshc.c` can put its own transfer-request list (32 slots) into the controller for the length of a run: with
nothing in flight the list is stopped, `UTRLBA` pointed at the new one, started again; at the end (and on every way
back from a run, `hc_take_back` in the loader) UEFI's address is written back. **Read only by construction**: only
a command that passes a whitelist is ever written into a slot (READ(10)/READ(16), TEST UNIT READY, REQUEST SENSE,
INQUIRY, READ CAPACITY; direction from-device only; never a well-known LUN; LUN 0 unless told otherwise), read back
and checked again before the doorbell is rung with interrupts off. Slot 0 holds a TEST UNIT READY used to prove the
controller reads the new list. Used on redwood's installed profile (`ufs_native=1`); not on the OnePlus 8.

`ufs_check=1` hashes the partitions that have reference images in the ROM's `firmware-update/`; `tools/ufs-survey.py`
compares them. The loader dumps the GPT and `super` metadata for `tools/ufs-super.py`.

## The `darwin` partition (redwood) / `userdata` (OnePlus 8)

Layout (blocks of 4 KiB; details and install procedure: [../INSTALL.md](../INSTALL.md)):

| Blocks | What |
|---|---|
| `0..2042367` | iOS volume and both cryptexes (written once) |
| `2042368..` | boot area: two bundle slots of 96 MiB (kernel, boot volume, command line) |
| `2091520` | boot state (what the loader started, how often, what came up: the boot guard) |
| `2091522` | OnePlus 8: offset of the PMIC RTC (`xl settime`) |
| `2097152..3145727` | var store: two slots of 2 GiB (the older, archive-based persistence) |
| `3145728..end` | OnePlus 8 only: the HFS+ data volume (89 GB) |

Redwood's `darwin` is a real 20 GiB GPT partition at the end of LUN 0, made from Android. The OnePlus 8's port takes
the whole `userdata`; `qemu-test.sh` also tests "darwin as a file of userdata" (a map of extents out of order with a
marker). The loader finds `userdata` by name, both GUIDs and first and last LBA; a different table and it writes nothing.

## `/private/var` and persistence

iOS needs a writable `/private/var`. The kernel's APFS driver reads and does not write (deliberately), so three
mechanisms exist:

1. **ramfs** (`bsd/miscfs/ramfs/ramfs.c`): a read-write file system in kernel memory, remade empty every boot, capped
   at `max_mem/4` (about 2 GB shown to iOS). Default in RAM-only runs and QEMU. Nothing survives. Consequences: icon
   layout, app data and the Wi-Fi password (Keychain) do not persist, and apps are registered anew each boot, one after
   another; SpringBoard waits `sb_delay=8` s so they land on the home screen in a stable order.
2. **The var store** (`userland/src/var_store.c`, `var_store=1 ufs_write=1`): `/private/var` packed into a plain archive and
   written into two slots of 2 GiB on `darwin` in turn. `watch` registers with the kernel
   (`kern.hackios.var_save`); on the orderly way back to the loader (time up, `xl stop`) and on iOS's SIGTERM (restart
   or shutdown) the kernel asks it to save, bounded. A header is written last over a zeroed one, the slot not holding
   the newest good archive is the one written, so a reset mid-save loses nothing. `load` reads the archive twice
   (checksum first). A save costs seconds and over a hundred MB of flash, so periodic saves (`var_save_every=`) are
   off by default. `bootok` (after `BOOTOK_AFTER` seconds of run) tells the boot guard that this generation came up.
   One saver at a time (`kern.hackios.var_busy`). Sockets and devices and the SKIP directories are left out.
3. **HFS+ data volume, OnePlus 8 installed boot** (kernel `e69840f`, userland `a7b5148`, 2026-10-24): iOS's real
   `/private/var` on a journaled HFS+ volume on the rest of `userdata`, from block 3145728 to the end, accessed as
   `/dev/hk_data` through `ufs_read/ufs_write` (`hk_data=1`; `hk_data=2` is a RAM disk of `hk_data_mb` MB for tests).
   Boot-args `var_data=hfs var_data_format=1 var_store=1 ufs_write=1`. Apple's HFS+ (`hfs-627.100.6`, the release macOS
   13.4 pairs with xnu-8796.121.2) is **fetched, not kept in the repository**, by `xnu/hfs-stage.sh` (run from
   `xnu/apply.sh`) into `bsd/hk_hfs/`; the integration follows `ios-instantnoodle`'s patch 0026; content protection is not
   built. `newfs_hfs` for iOS (`tools/mk-hfstools.sh`) runs from `init` only if neither HFS volume header
   (primary and alternate) is found *and* `var_data_format=1`, and the var store's archive is copied in once.
   Verified on the phone: format, migration of var store gen 8, persistence across boots (stamps), iOS shows 95 GB.
   Pitfalls fixed: HFS needs `DKIOCSETBLOCKSIZE 512` (counting); cluster I/O buffers need `buf_map` (a NULL data pointer
   panicked); HFS is synced (`VFS_SYNC MNT_WAIT`) on `loader_return`. Not available: `fsck_hfs`; write speed is limited by
   the loader's read-back; `hfs_corruption_panics` is a boot-arg.

## The APFS driver (`bsd/miscfs/apfs/`)

A clean-room read driver for APFS (format in `apfs_format.h`): little-endian reads through `memcpy` helpers, every
block validated (Fletcher-64, node offsets, lengths, tree depth `APFS_MAX_LEVELS` -> `EIO` rather than followed).
Supports compressed files (zlib, libdeflate 1.24 and libzbitmap, MIT, in `xnu/files/bsd/miscfs/apfs/`). It is tested
against `tools/apfs/apfs.py` and `apfsck` by hashing every entry of the volume and comparing sums
(`hk_apfs_check.c`, `apfs_check=1..8`; the whole volume's 318,098 lines are folded into an order-independent sum).

- **Write in place** (`apfs_write.c`): replaces bytes of an existing file where they lie; refuses unless the file's
  extents and the extent-reference tree both say the blocks are that file's. Transactions (full copy-on-write) are not
  implemented: a header comment documents the eight-step commit order and why (`tools/apfs/txn-probe.py` measures it).
- **Fixed-length slot store** (`apfs_store.c`, `apfs_check=8`): slot files in an APFS volume overwritten where they
  lie; the same two-slot, header-last protocol as `var_store.c`, with addressing from the volume's own records instead
  of a constant. Built and tested; not the persistence path in use.
- `hk_sparse.c` (`apfs_sparse=<offset>`): a block device over an image that holds only the blocks a given walk reads.
  Exists so the real iOS root could be read before the flash path existed.
- `hk_virtio_blk.c` (`virtio_blk=1`, QEMU only): serves a host disk image read-only.
- `hk_image.c`: reads a file out of a RAM disk image by path (`fw/NAME`) for drivers that want firmware before there is a root.

## Related

[../INSTALL.md](../INSTALL.md) for installing and the boot guard; [../BUILD.md](../BUILD.md) for the image recipes;
[../research/boot-chain-and-recovery.md](../research/boot-chain-and-recovery.md) for partition-safety history.
