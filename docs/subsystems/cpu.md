# CPU: cores, interrupts, frequency, scheduling

The kernel is XNU `xnu-8796.121.2` built from the `BCM2837` configuration (the generic ARMv8 base) plus our platform
code, because Apple's `arm64` platform pieces (interrupt controller, power, PPL, PMU) are closed. This page covers how
cores come up and go down, the interrupt controller, frequency control and scheduling. Power states are in
[power.md](power.md); heat is in [thermal.md](thermal.md).

## Topology

Both phones are 8-core Qualcomm SoCs in three clusters. Qualcomm numbers cores in the second affinity field of MPIDR
(`core_shift` 8 in the loader's `struct platform`).

| | redwood (SM7325) | instantnoodle (SM8250) |
|---|---|---|
| cores 0-3 | small cores | small cores |
| cores 4-6 | big | big |
| core 7 | the biggest core (EPSS group of its own) | the prime core |
| EPSS frequency groups ("domains") | 3: `0-3`, `4-6`, `7` (`board_cpu_domains`, default `0x21110000`; `ml_cpu_frequency_domain()`) | same |
| seen top speeds | 1.8 / 2.4 / 2.4 GHz | prime core 2.84 GHz |

`PE_parse_boot_argn` names: `cpus=N` (the kernel takes the boot core and the next N-1 of the loader's list and starts
them with PSCI `CPU_ON`), `cpumask=M` (pick by position), `bootcpu=N` (run the kernel on core N instead of the small
core the firmware starts on; the loader then watches from the first core, and a kernel that hangs still gets its log
out; the profiles pass `bootcpu=7`). The loader's list order puts cores nearest the boot core first. The loader stays on
core 0 and serves storage and USB; that core is never the kernel's.

**The slower cores start late.** `GenericARMPE.cpp` starts the boot core and the faster cores at once and the "small"
cores (an E cluster's under Edge, an L2 access penalty's under Clutch) `small_cores_after=S` seconds later (default 28; 0 = at once). With all
seven from the start the lock screen came 1.5 s later (26.5-27.0 s against 25.3-25.5 s) because start-up work shares
memory and cache with SpringBoard; the late start finishes everything earlier (34 s against 38 s).

## Multiple processors: PSCI

`osfmk/arm64/psci_smp.c`. Apple's cores are powered by closed drivers; here the firmware does it through PSCI `CPU_ON`
(the address given is the kernel's own reset vector `LowResetVectorBase`, so a new core takes Apple's path: finds its
cpu data by MPIDR, turns the MMU on, comes out in `arm_init_cpu()`) and `CPU_OFF`. The call is SMC on phones, HVC under
QEMU (the loader says which in `/chosen/psci-method`). **Cache coherence trap:** a new core starts with MMU and caches
off and reads from memory; under Qualcomm's hypervisor a line dirty in another core's cache is simply not there yet.
So before `CPU_ON` everything that core will read before its MMU is on is written back, and what it writes in that
state is read only after throwing away the reader's cached copy. QEMU has no cache and shows none of this. The other half
is the exit: the loader gets back only the one core it started, so `psci_smp_stop_others()` makes every other core switch
itself off first, wherever it is.

## Interrupts: GICv3 (`osfmk/arm64/gic_v3_irq.c`)

Open-source XNU has no interrupt controller driver. Here everything is a Group 1 IRQ (what an EL1 guest gets from a stock
hypervisor or QEMU): the CPU interface through `ICC_*_EL1`; the EL1 virtual timer PPI routed to rtclock; two SGIs carry
`cpu_signal()` and the stop order; any SPI is a driver's once attached (`gic_v3_spi_attach()`: configured, routed to the
attaching core, enabled, handler called in interrupt context); any other interrupt that fires is masked and counted. Each
core sets up its own CPU interface, redistributor frame and timer (`gic_v3_irq_cpu_init()`, from `PE_init_cpu()`). Registers
come from the device tree node `/arm-io/gic` (`reg = <dist off size redist off size>`). The EL1 *physical* timer is left
alone: the Qualcomm hypervisor traps it. **Two bugs found around sleep and fixed**: see [power.md](power.md).

## Frequency: EPSS (`hk_cpufreq.c`, `cpufreq=1`)

The boot loader leaves each group at a modest frequency. The EPSS hardware has a table (up to 40 rows, a word per row:
bits 31:30 clock source, 7:0 multiple of 19.2 MHz; source 0 = a fixed 300 MHz) and switches clock source and supply
voltage itself when told the wanted row (`EPSS_PERF_STATE`). Register layout: Linux `qcom-cpufreq-hw.c` (`epss` variant);
base address from `board_epss` (default redwood's `0x18591000`; the OnePlus's comes from the profile's `BOARD_ARGS`).
Every group with a core of ours is raised to its top row at start; the governor (schedutil-like, tick 20 ms) then follows
load. The governor, boost on touch, memory (DDR) votes and the thermal cap are described in [power.md](power.md). Boot-arg
`cpuload`, `membench` and `kern.hackios.cpuspeed` (loop speed per group and EPSS row), `memlat` (L2/L3/DRAM latency) and
`top` (per-process CPU ms since last read; `power_test top LABEL`; `tools/t cpuprof`) are the measuring probes.

## Scheduling

- **Edge (asymmetric) scheduler with our performance controller**, the default on both phones; `sched=clutch` boots the previous
  Clutch scheduler (one processor set, slower cores paired as "secondaries") from the same kernel. Clusters are the EPSS frequency
  domains (`board_cpu_domains`), E/P from the loader's `small-core`. Everything about it: [scheduler.md](scheduler.md).
- `kpc.c` reports a PMU with no counters so the kpc/kperf code stays linked but inert.
- NEON in the kernel (a SHA-256 acceleration idea) is not allowed (FP state). A RELEASE kernel configuration does not build
  (minicrypto types, `user_exception_trace.c` `proc_pid`): abandoned.

## Return to the loader

`osfmk/arm64/loader_return.c`: with no serial port there is no way to get the log out, so the loader leaves UEFI intact
(boot services never exit) and publishes the physical address of a return stub in `/chosen/loader-return`. When the
kernel has nothing more to do (panic, halt, timer, `xl stop`) it collects its message buffer, panic buffer and `kprintf`
output, maps the stub 1:1 through TTBR0 and jumps to it with the log. The stub switches the MMU off, reinstates the
saved UEFI environment and the loader writes the log to storage. Reasons: 0 orderly end, 1 early exception (ESR, ELR,
FAR, LR, SP), 2 checkpoint, 3 iOS shut down. It is valid only while the kernel does not drive hardware UEFI also uses
(UFS and USB stay UEFI's; see [storage.md](storage.md)). The Qualcomm hypervisor's watchdog is petted from here
(`hv_wdt_pet`); a watching loader resets a kernel silent for 50 s. A kernel panic therefore returns the phone to the
loader with the log instead of rebooting it. `return_panic`, `return_cpu`, `loader_return_after=S` (wall time)
control this. `ustacks*` boot-args dump user threads' stacks (`qemu/touch.py stacks NAME`, `qemu/symstack.py`).

## Files

`xnu/files/osfmk/arm64/gic_v3_irq.c`, `psci_smp.c`, `loader_return.c`, `generic_arm64_support.c`, `kpc.c`,
`user_exception_trace.c`, `tunables/`, `ppl/`; `xnu/files/iokit/Kernel/GenericARMPE.cpp`; `xnu/files/bsd/dev/hk_cpufreq.c`;
scheduler files: [scheduler.md](scheduler.md#files).
