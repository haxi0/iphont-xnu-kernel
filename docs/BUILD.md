# Building HackiOS

Everything needed to build the kernel, loader, UEFI and images, and what `prebuilt/` is. Running is in
[devices/](devices/README.md); installing in [INSTALL.md](INSTALL.md); tests in [TESTING.md](TESTING.md).

`common/build.sh <device>` runs every step this machine can run and uses `prebuilt/` for the rest:

1. **kernel** `xnu/apply.sh`, `xnu-deps/setup.sh`, `xnu-build.sh` — only with theos + `iPhoneOS16.5.sdk`, else `prebuilt/mach.development.bcm2837`
2. **loader** `xnuloader/build.sh` — only with `clang` + `lld-link` (an EFI application without EDK2)
3. **images** `userland/build.sh` (`REST=1` and the profile's `USERLAND_VARS`) — needs `ios165/fetch.sh` to have run
4. **boot** `mkxnuimg.py` → `out/<device>-boot.img` (the UEFI image for `fastboot boot`)

`SKIP="kernel loader images boot"` leaves steps out. Profiles: `common/devices/{redwood,instantnoodle,qemu-virt}.conf`
(`_template.conf` documents every variable). `common/hackios build <device>` is the same through the one-command front end.

## Host requirements

Linux x86-64, ~12 GB free disk for the QEMU path (the whole working tree on the author's machine is far larger), 4 GB RAM.

```sh
# Arch
sudo pacman -S --needed git python 7zip lz4 gcc qemu-system-aarch64 qemu-ui-gtk edk2-aarch64 \
     base-devel curl clang lld perl xz
# Debian/Ubuntu: git python3 7zip liblz4-dev gcc qemu-system-arm qemu-efi-aarch64 build-essential curl clang lld perl xz-utils
```

7-Zip must be **23 or newer** (it reads APFS). `common/setup.sh` checks the host and prints what is missing for which step.
On the NixOS development host there is no `clang`/`cc` outside `nix-shell` (`shell.nix`): use
`nix-shell --run '...'` for kernel, Wi-Fi driver and `xl enter` (mkxnuimg needs clang) steps; `nix-shell -p gcc` for host tests;
Pillow/numpy via `nix-shell -p python3Packages.pillow python3Packages.numpy` for `tools/look` and `mkbootlogo.py`.

## Workspace

HackiOS is **two git repositories in one directory** — see [architecture.md](architecture.md#two-repositories-one-tree). `common/setup.sh`
checks `hackios-xnu-userland` out into the same tree as `.userland.git` (`common/ugit` is its git).
`tools/new-workspace.sh` makes another working copy with the iOS extractions as hard links. **Never make `ios165/rootfs`,
`cryptex`, `cryptex-app` symlinks to another copy's tree**: `build.sh` refuses, because a link made `rm -rf` delete 491
LaunchDaemons etc. from the main extraction on 19 October 2026.

`ios165/fetch.sh` downloads the iOS 16.5 IPSW (7.4 GB) from Apple's CDN and extracts what is needed (5-15 min). No Apple file
is in either repository.

## Kernel

```sh
xnu/apply.sh             # ~6 s: tag xnu-8796.121.2 of apple-oss-distributions/xnu + our changes -> xnu-src/xnu-8796.121.2 (branch redwood, changes uncommitted); also xnu/hfs-stage.sh
xnu-deps/setup.sh        # ~30 s: dependency sources, mig/unifdef/tcsh, the SDK overlay, two libraries
tools/mk-ath11k.sh       # optional once: Linux 7.0.14 + FreeBSD 15.1 sources at pinned hashes, builds ath11k (phones' Wi-Fi)
tools/mk-wlfw.sh         #   ... and its firmware-service message tables
./xnu-build.sh -j8 -k    # ~45 s on 12 threads
```

Needs [theos](https://theos.dev/docs/installation-linux) in `~/theos` (or `$THEOS`) for cctools (ld64, nm, otool, strip) and
`iPhoneOS16.5.sdk`. Result: `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`; `xl run` and
`qemu/run-ios.sh` take it from there (from `prebuilt/` only when absent).

- **`-k` is required.** The build "errors" (`check_all_exports`, three symbolsets, 217 `exported name not in import list` about
  DriverKit RPC names from the `iig` stubs) all happen after linking; make exits 2; judge by the file's date.
- The path to the repository must not contain spaces. `xnu-deps/host/bin/xnu-clang` calls `/usr/bin/clang` (tested with 22.1).
- After changing kernel headers that outside libraries see (pthread kext, firehose): `xnu-deps/setup.sh --rebuild-libs`.
- If the `iig` undefined-symbol set changes: `xnu-deps/gen-iig-stubs.py xnu-deps/iig-impl/undefined-all.txt xnu-deps/iig-impl qemu/kernel.syms`, then `rm -rf BUILD`.
- For `qemu/sym.py`: `nm -n .../mach.development.bcm2837.unstripped > qemu/kernel.syms`.
- The RELEASE configuration does not build (abandoned). `DEVELOPMENT_ARM64_BCM2837` is the one.
- `tools/t` rebuilds the kernel only if sources are newer, and reports a build failure when the binary is older than sources.

### Editing the kernel: the export trap

You edit `xnu-src/xnu-8796.121.2` (Apple's tree, branch `redwood`). `xnu/export.sh` regenerates `xnu/hackios.patch` and `xnu/files/`
**from the whole tree**, not from your changes. If the tree fell behind the committed patch (somebody changed the kernel after
you set yours up) the export silently rolls their work back, and the sign is a kernel that **builds and crashes at boot right
after `Stackshot Succeeded`**. Before exporting, bring the tree to the current patch (`git checkout -- .` in `xnu-src`, then
`git apply xnu/hackios.patch && cp -a xnu/files/. .`), or paste a single diff into the patch by hand and say so. Several
sessions can share one tree: check `git -C xnu-src status` first. Hosts: `xnu/hfs-stage.sh` fetches Apple's HFS+ (`bsd/hk_hfs`)
and `xnu/export.sh` leaves it out.

## Loader

`xnuloader/build.sh` (clang + lld-link) → `xnuloader/loader.efi`. `OUT=loader-ci.efi xnuloader/build.sh` builds without touching it.
`mkidentity.py` makes `identity.h` from an iPhone's device tree. `common/flash/ownerkey.py`/`hackios key init` pins an owner key
into the loader (see INSTALL.md). A new SoC needs a `struct platform` in `loader.c` ([PORTING.md](PORTING.md)).

## UEFI

Not needed to run: `prebuilt/uefi/` has it and `mkxnuimg.py` takes it. Rebuilding: [../uefi/README.md](../uefi/README.md)
(`uefi/apply.sh` clones Mu-Silicium ~3 GB and applies the port; `python3 build_uefi.py -d redwood|instantnoodle -r RELEASE`).
A from-scratch rebuild by those instructions has not been re-run.

## Images (userland repository)

```sh
ios165/fetch.sh                       # once
REST=1 userland/build.sh              # root.apfs + system.apfs
GL=1 REST=1 userland/build.sh         # + Mesa OpenGL ES for the window server (needs tools/mk-mesa.sh; phone with gpu=1)
GL=1 REST=1 FLASHBOOT=1 CRYPTEXES=flash userland/build.sh     # OnePlus 8: flashboot.apfs for the flash-root boot
```

Variables are documented at the top of `userland/build.sh` (`RC`, `JOBS`, `REST`, `WIFID`, `OSLOG`, `INJECT`, `STOREFRONT`, `EXTRA`, `EXTRA_SHARE`,
`RC_JOBS`, `GL`, `ADSP`, `FLASHBOOT`, `CRYPTEXES`, `PREBUILT`, `CHECK`, `SYSTEM`, `SHARE`). Without the cross toolchain the build takes our programs
from `prebuilt/userland`. `tools/mk-mesa.sh` fetches Mesa 25.0.7 and applies `tools/mesa/*.patch`. `ADSP=1` puts the 20 MB audio DSP
firmware in the small image, not beside `GL=1` (they do not fit the 64 MB the loader takes with the kernel). Files of the userland
repo need `common/ugit add -f` (shared whitelist `.gitignore`). `tools/t` caches userland builds in `out/tcache/<hash>`.

## Vendor firmware (never in the repository)

Taken from the phone itself; `userland/build.sh` prints which went into the image. `FIRMWARE` in the profile lists the directories
(`fw-gpu`, `fw-wifi`, `fw-audio`, `fw-haptic`, place them next to the workspace).

```sh
# POCO X5 Pro, from a running Android:
adb pull /vendor/firmware/a660_{gmu.bin,sqe.fw,zap.mdt,zap.b00,zap.b01,zap.b02} fw-gpu/
adb pull /vendor/firmware_mnt/image/wpss.{mdt,b02,b03,b04,b05,b06,b07} fw-wifi/
adb pull /vendor/firmware_mnt/image/adsp.{mdt,b02,...,b36} fw-audio/
adb pull /vendor/firmware/aw8697_haptic.bin fw-haptic/
```

OnePlus 8: from a backup of its vendor partition and `persist`: see [devices/instantnoodle.md](devices/instantnoodle.md).
Wi-Fi also needs `fw-wifi/mac` (the radio's address, one line) and optionally `fw-wifi/network`. **Never print the MAC or commit any firmware.**

## prebuilt/

`prebuilt/` holds the kernel, loader, UEFI pieces and our programs built from these repositories so the QEMU path needs no
toolchain. After rebuilding: `tools/update-prebuilt.sh`. `prebuilt/SHA256SUMS` lists them; `prebuilt/NOTICES.md` has third-party notices.
Prebuilt files lag the source: `prebuilt/mach.development.bcm2837` predates `tools/sleep-test`'s `sleep_test` boot-arg.

## Licensing of what is built

`xnu/` is a derivative of XNU (APSL 2.0); the kernel also contains ath11k (BSD-3-Clause-Clear) and FreeBSD LinuxKPI code (BSD-2-Clause);
libzbitmap (Corellium, MIT), libdeflate 1.24 (MIT). Apple's HFS+ is fetched at build time. See [../prebuilt/NOTICES.md](../prebuilt/NOTICES.md).
