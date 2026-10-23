# Porting HackiOS to another device

HackiOS was brought up on the POCO X5 Pro 5G (Snapdragon 778G), and the OnePlus 8 (`instantnoodle`, Snapdragon 865)
is the second phone: read its profile, UEFI package and `board=instantnoodle` drivers as a worked example of
everything below. A Qualcomm phone with an unlockable bootloader is the realistic next target: the boot chain
(Mu-Silicium UEFI started with `fastboot boot`) and most of the kernel's platform code carry over. Start with:

```sh
common/new-device.sh <codename> path/to/device.dtb
```

It writes `common/devices/<codename>.conf` (the profile `common/build.sh` and `common/run.sh` read) and
`common/devices/<codename>.platform.c` (the loader's description of the SoC), filling in what the device tree says.
Get the device tree from the phone's `vendor_boot` image, or from a running Android: `adb pull /sys/firmware/fdt`.

## 1. The UEFI

The phone needs a UEFI that our loader can run under. Port [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
the way `uefi/` ports redwood (it follows `lisa`, the same SoC package):

- `Platforms/<Vendor>/<codename>Pkg`: memory map and driver list (copy the closest device of the same SoC);
- `Binaries/<codename>`: the UEFI drivers of the phone's own firmware, extracted with
  [UEFIReader](https://github.com/WOA-Project/UEFIReader) from its `xbl.img`;
- apply our two changes from `uefi/mu-silicium.patch`: the boot shim copies the "XNUP" container after the UEFI
  image to `PAYLOAD_BASE`, and `BootManagerLib` starts the XNU loader from it instead of the menu.

Put the addresses in the profile: `FD_BASE`, `FD_SIZE` (where the shim puts the UEFI image) and `PAYLOAD_BASE`
(free RAM for the kernel and images). The prebuilt `BootShim.bin` is built for redwood's addresses: build the
shim from your Mu-Silicium tree (`mkxnuimg.py` does it when `Mu-Silicium/` is there). Load the profile before
`./xl enter` (`. common/lib.sh; load_device <codename>`): without it `mkxnuimg.py` builds redwood's UEFI, and on
another phone that hangs at the fastboot logo until `fastboot reboot bootloader`.

## 2. The loader

`xnuloader/loader.c` describes each device with a `struct platform` and picks it by the framebuffer base the UEFI
reports. Paste `common/devices/<codename>.platform.c` next to `platform_redwood` and add it to `platforms[]`:

| Field | Where it comes from |
|---|---|
| `io_base`, `io_size` | one window that covers the SoC's peripherals; it must not start at 0 (XNU then skips clock setup) |
| `gic_dist`, `gic_redist` | the `arm,gic-v3` node's `reg` (filled in by `new-device.sh`) |
| `cores`, `core_shift` | the `cpu@` nodes; Qualcomm numbers cores in MPIDR's second affinity field (shift 8) |
| `small_cores` | a bit per core of the little cluster (lowest `capacity-dmips-mhz`) |
| `psci_hvc` | 1 if the firmware's PSCI is called with HVC (QEMU), 0 for SMC (phones) |
| `fb_base` | the framebuffer address; the loader prints it at start (`display W x H at ...`) |

Rebuild with `xnuloader/build.sh`; QEMU must still pick `qemu-virt` (no match = ramfb).

## 3. The kernel

What is generic: GICv3, the timer, PSCI SMP, the display (the UEFI's framebuffer), APFS and ramfs, the driver
replacements, the return-to-loader path. (A framebuffer the display hardware keeps rereading on its own tears under
the processor's copy; a command-mode panel's can be pushed once per frame instead, as the OnePlus 8's is:
`fb_probe=1` shows how the firmware left the pipeline, [instantnoodle latency](instantnoodle-latency.md) the rest.)
What is redwood-specific and needs looking at:

- **touch**: `bsd/dev/redwood_goodix.c` (Goodix GT9916 over bit-banged SPI) — another panel needs its own driver
  feeding `/dev/redwood-touch` (`bsd/dev/redwood_input.c`); the OnePlus 8's `instantnoodle_touch.c` is one, on the
  bit-banged I2C of `instantnoodle_i2c.c`, which its fuel gauge (`instantnoodle_battery.c`) shares. Gate a new
  phone's drivers on a `board=` boot-arg the loader passes, so redwood's keep running unchanged;
- **buttons, CPU frequency, watchdog**: GPIO/PMIC numbers and the EPSS block in `redwood_input.c`,
  `redwood_cpufreq.c`, `osfmk/arm64/loader_return.c`;
- **flash LED, vibration motor**: `redwood_torch.c` and `redwood_haptic.c` take a table per board (PMIC block,
  pins, chip); the OnePlus 8's PM8150L and AW8697 are the second entries;
- **GPU / Wi-Fi / sound**: `redwood_adreno.c` (Adreno 642L, and the Adreno 650 as its `kona` variant: the clock
  controller's debug selectors, the SMMU stream the hypervisor accepts, and which CP registers exist differ by
  chip; the log names the bring-up step that stopped), `redwood_wifi.c` (WCN6750 + ath11k),
  `redwood_audio.c` — the same Qualcomm subsystems (SMEM, SMP2P, GLINK, QRTR, QMI) are reused, the addresses and
  firmware differ. The OnePlus 8's Wi-Fi (QCA6390) is on PCIe, which nothing here drives yet.

Start with no extras: the system boots to the lock screen with only the framebuffer, touch and the CPU.

## 4. Firmware

Vendor firmware never goes into the repository; it comes from the phone's own Android (`FIRMWARE` in the profile
lists what to pull, see README "On the phone"). Without `fw-gpu/` the kernel does not start the GPU and says so only
deep in the log (`adreno: THE GPU IS NOT STARTED`); `userland/build.sh` prints which firmware went into the image.

## 5. Run

```sh
common/build.sh <codename>
common/run.sh <codename>
```

The first useful milestone is the loader's own screen and log over USB (`./xl log`), the next one is the kernel's
verbose boot text (`-v`), then SpringBoard. Read [checks.md](checks.md) before trusting a "it is not in the log".
