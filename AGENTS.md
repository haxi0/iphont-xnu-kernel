# AGENTS.md — working on HackiOS

HackiOS runs Apple's open-source XNU (`xnu-8796.121.2`, the iOS 16.5 kernel) plus the unmodified iOS 16.5 user space on Android phones (active: **OnePlus 8 "instantnoodle"**;
also POCO X5 Pro "redwood", and QEMU). This file is how to behave here. Details live in `docs/`; do not copy them back into this file.

**Start every session by reading `docs/HANDOFF.md`** (state, uncommitted work, other sessions), then the pages for what you touch (`docs/README.md` is the index).

## Layout (workspace = this directory, `hackios/`)
`xnu/` our patch + new kernel files (applied to `xnu-src/`) · `xnuloader/` the EFI loader · `uefi/` Mu-Silicium port · `common/` build/run/install scripts and device profiles (`common/devices/*.conf`) ·
`xl` the USB driver for the phone · `qemu/` · `tools/` host tools and tests (`tools/t` = OnePlus test entry) · `wifi/lx/` compatibility layer for ath11k · `userland/` + `ios165/` (userland repo) ·
`docs/` documentation · `prebuilt/` built artefacts · `out/`, `logs/` untracked output. Architecture: `docs/architecture.md`.

## Two git repositories share this tree
- Kernel repo: `git` (`.git`). Userland repo: **`common/ugit`** (`.userland.git`; owns `userland/`, `ios165/`, `docs/air*.md`, `tools/{apfs,air,mesa,metal}`). Both use whitelist `.gitignore`s: untracked files are invisible; add by name
  (`common/ugit add -f PATH` for userland files).
- **The userland index also tracks many kernel-side files** (`xnu/`, `wifi/`, `xl`...). Never `commit -a`/`add -A` in either; commit each file in the repo that owns it (`docs/architecture.md`).
- Always check **both** `git status` and `common/ugit status` before editing; many files are modified and uncommitted. **Other agent sessions edit concurrently** (scheduler, toolkit...): do not modify a file with
  uncommitted changes you did not make unless it is your task; never `checkout`, `reset`, `stash` or `clean` to "tidy up".
- Do not open PRs to `oosexgang` or push without being asked. Commit only when asked.

## Hardware safety — hard rules
1. **Never** `fastboot flash userdata` (or any partition write) without the owner's fresh explicit approval each time. Never change slots, swap or flash ABL/XBL/TZ, or lock the bootloader. Every flash write (`xl install|flashpart`, `var_store=1 ufs_write=1`, `common/flash/install*.sh`) needs explicit OK.
2. **Tell the owner before starting any run on a phone** (they watch the screen), and when it ends. No audible output at night (`sound_amps`); never claim a hardware feature works before it was observed/reported.
3. **Never** a bare `./xl enter` for the OnePlus 8: load the profile first (`. common/lib.sh; load_device instantnoodle`, or `common/run.sh instantnoodle`) — otherwise redwood's UEFI hangs the phone. Needs `nix-shell` on the NixOS host.
4. **Never** `touchscreen=1` on the OnePlus 8. Never `wifi_again=1`/`adsp_again=1`. Never DCS sleep-in (0x10) to the panel. Read `docs/research/hardware-qualcomm.md` before touching any register: wrong accesses CrashDump the phone.
5. Do not run redwood (POCO X5 Pro) checks unless asked (owner instruction 2026-10-25).
6. One hardware run at a time: `./xl wait` first (lock `$XDG_RUNTIME_DIR/hackios-phone.run`). Never `pgrep -f`/`pkill -f` a pattern that is in your own command line (exit 144). The phone is shared between working copies.
7. Vendor firmware, keys, MAC addresses, device dumps and Apple files never go into git or into docs. Backups in `../ios-instantnoodle/backup/` are not to be altered.
8. Recovery: hold Power+VolUp from CrashDump; `xl reboot`/`xl enter`; see `docs/INSTALL.md` and `docs/research/boot-chain-and-recovery.md`. A USB cable left in breaks the stock bootloader after a reset.

## Workflow
- **RAM-only testing is the default** (nothing flashed): `common/build.sh instantnoodle` (or `SKIP="loader images boot" ...`), userland images, then `./xl enter`, `./xl ramdisk userland/flashboot.apfs`, `XL_SECS=N XL_CMDLINE="..." ./xl run`.
  Profile boot-args in `common/devices/instantnoodle.conf`. Use `nix-shell --run '...'` where clang is needed.
- **Use `tools/t`** (docs/TESTING.md): `tools/t host` (2 s) after every tool edit · `quick` (~70 s) for userland/rendering · `power` (~2 min) for sleep · `gfx`, `display` · `full` (~6 min) only for architectural change,
  intermittent failures or a final check. Name checks to share one boot: `tools/t boot sleep`. The owner wants tests short; one boot per question; a failing check is investigated from its log, not retried.
- Kernel edits happen in `xnu-src/xnu-8796.121.2`; **export with `xnu/export.sh` before committing**, but only after making sure `xnu-src` is not behind `xnu/` (the export regenerates from the whole tree; a stale tree silently rolls back others'
  work: `docs/BUILD.md`). The kernel build "ends with an error" (`-k`); judge by the binary's date.
- Wait on conditions, not sleeps; read logs in `logs/` (the loader keeps the first 256 KB and last 1 MB of a log; QEMU logs print lines twice). Read `docs/research/checks.md` before trusting "not in the log".
- Userland rc job lists run without a shell (no quoting, no variables). `iOS` binaries are never modified; hook by address with the guards in `inject*.c`; addresses are valid for iOS 16.5 (20F66) only.

## Device-specific code
- Shared drivers carry defaults of **redwood**; other boards are selected by `board=<name>` and `board_<block>=0x...` (`bsd/sys/hk_board.h`). OnePlus 8 code: `instantnoodle_*.c`, `kona` branches. Gate new board code so redwood keeps working unchanged.
- New device: `common/new-device.sh` and `docs/PORTING.md`. Bring hardware up in **steps behind a boot-arg** (read-only first, one write function with read-back, announce before touching, see hardware-qualcomm.md §9).

## Preserve what exists
Do not delete or shorten existing comments, research, screenshots, logs or abandoned-project directories you were not asked to change; do not "clean up" stand-ins (`hk_wlfw.c`, probes, `adreno=N` steps) — they are diagnostic tools.
Prefer the smallest change; reuse existing helpers; keep the style of the surrounding code (long explanatory header comments are the house style for drivers). No functional changes inside a documentation task.

## Where documentation and research live
`docs/README.md` index · `docs/architecture.md` · `docs/BUILD.md` · `docs/BOOT_ARGS.md` · `docs/TESTING.md` · `docs/INSTALL.md` · `docs/devices/` · `docs/subsystems/` · `docs/research/` (reverse-engineering findings, failed approaches) ·
`docs/TODO.md` (unfinished work) · `docs/KNOWN_ISSUES.md` (broken now) · `docs/HANDOFF.md` (session continuity) · Metal/AIR notebook `docs/air.md` (userland repo).

## Keeping documentation current
Proportionate to the change; trivial edits need none. Do these in the same session as the code:
| You did | Update |
|---|---|
| new driver / subsystem / behaviour change | the matching `docs/subsystems/*.md` (and `devices/*.md` status) |
| new boot-arg or `kern.hackios.*` sysctl | `docs/BOOT_ARGS.md` |
| learned something about iOS, firmware, Qualcomm hardware, a protocol; or a dead end | the right `docs/research/*.md` page (tag observed / source / inferred / hypothesis; no disassembly dumps, binaries, keys or assets) |
| left something unfinished | `docs/TODO.md` (status, files, blocker, next step) |
| confirmed something does not work | `docs/KNOWN_ISSUES.md` |
| fixed an issue | change its status in KNOWN_ISSUES/TODO, keep the diagnosis in the subsystem or research page |
| architectural change (boot, memory layout, repo layout, IPC) | `docs/architecture.md` |
| new build/test procedure | `docs/BUILD.md` / `docs/TESTING.md` |
| ended a substantial session or changed something the next developer must know | **refresh `docs/HANDOFF.md`** (date, uncommitted work, blockers, tests run, next actions) |
Claims need evidence: say *verified on phone*, *built*, or *unverified*; do not make a feature look working because a driver exists, nor unfinished because an old page says so. Fix a doc you find wrong.

## Caveats about names
Kernel log and userland strings still say `redwood:` or `/redwood` in places (history); the project name is HackiOS, libraries `libhackios*`, sysctls `kern.hackios.*`, devices `/dev/hk-*`.

## Agent device diagnostics

Use `./iphont describe` for the machine-readable command catalog and [docs/TOOLKIT.md](docs/TOOLKIT.md) for authorization, protocol, and validation limits. The new device agent is opt-in; normal boots do not enable control. The owner deferred a RAM loader reload on 2026-10-26. Preserve `xl`, test scenarios, and recovery/build scripts until [migration gates](docs/TOOLKIT-MIGRATION.md) pass.
