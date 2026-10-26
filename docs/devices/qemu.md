# QEMU (`qemu-virt`, aarch64 `virt`) — no phone needed

The whole system on any Linux x86-64 computer: ~12 GB free disk, 4 GB free RAM (QEMU gets 7 GB so the kernel window matches the phone's).
Profile `common/devices/qemu-virt.conf`; scripts in `qemu/`. Drivers for virtual hardware: `hk_virtio_input.c` (mouse = finger),
`hk_virtio_net.c` (en0), `hk_virtio_sound.c`, `hk_virtio_blk.c` (read-only host image). CoreAnimation uses its software renderer (no GPU); no thermal service (no TSENS).

```sh
git clone https://github.com/oosexgang/hackios-xnu-kernel.git hackios && cd hackios   # (this fork: see docs/architecture.md for remotes)
common/setup.sh               # brings the userland repo into the same tree, checks the host
ios165/fetch.sh               # downloads 7.4 GB from Apple's CDN and extracts what is needed (5-15 min)
common/build.sh qemu-virt     # builds the APFS images (and the kernel, if the toolchain is there)
common/run.sh qemu-virt       # a 20-minute session in a QEMU window
```

The lock screen appears after 2-3 minutes; right after SpringBoard appears give it 30-40 s (apps are still registering).
**Mouse with the left button held = finger.** Unlock / leave an app: swipe up from the very bottom edge; back: swipe from the left edge.
Keys `H` home, `L` lock/power, up/down volume, `F11` (hold) five fingers. Scripted: `qemu/touch.py swipe 0.5 0.985 0.5 0.3 250`,
`qemu/touch.py tap 0.615 0.71`, `qemu/touch.py key home`; screenshot `qemu/screendump.sh shot.png`; live stacks `qemu/touch.py stacks NAME`
then `qemu/symstack.py LOG NAME` (needs [`ipsw`](https://github.com/blacktop/ipsw)). Debian/Ubuntu firmware: `QEMU_EFI=/usr/share/AAVMF/AAVMF_CODE.fd QEMU_VARS=/usr/share/AAVMF/AAVMF_VARS.fd`.
On NixOS the QEMU's own `edk2-aarch64-code.fd` is used automatically by `tools/sched-test/qemu.sh`.

## Facts and traps
- **Counting in a QEMU log:** `qemu/run-ios.sh` prints every console line twice (the second copy replayed thousands of lines later; other
  processes' output interleaves and splits one of the copies, so `sort -u` does not fix it). Count distinct events by timestamp, or by program and pid.
  The phone's logs do not do this.
- `QEMU_NET=1` adds virtio-net behind QEMU's user network (DHCP, DNS, no privileges); `QEMU_RAM_GB`; `QEMU_SMP`; `CMDLINE`; see the header of `qemu/run-ios.sh`.
- Under QEMU everything takes ~5x as long; `sb_delay` is harmful there ("for a reason not yet found the window server then never takes our touch service").
- QEMU has no cache coherence effects (`psci_smp.c`), a `CPU_SUSPEND` that keeps state, and no PMIC: bugs in those paths do not show. It does
  reproduce interrupt/sleep bugs: payload with no ramdisk (XNU waits for root) + `cpus=4 syssleep=1 sleep_test=2 sleep_test_count=5 sleep_max=2`, `QEMU_SMP=4 qemu/run.sh` (`tools/sleep-test/run.sh`).
- The installed-boot paths run in QEMU on a disk laid out like a phone: `common/hackios test` = `common/flash/qemu-test.sh` (14 scenarios) ([../TESTING.md](../TESTING.md)).
