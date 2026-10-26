# Scheduler: Apple's Edge scheduler on the Snapdragons, with a performance controller of our own

Status (2026-10-26): **built, verified in QEMU (4 topologies) and on the OnePlus 8 in RAM runs**; the default on boards whose cores
differ (both phones). `sched=clutch` boots the previous scheduler from the same kernel, the regression baseline. Uncommitted;
exported to `xnu/` (`xnu/export.sh`, 2026-10-26 04:5x). Not tried on redwood (owner: no POCO runs).

## What it is

XNU on an Apple AMP SoC runs the **Edge scheduler** (`osfmk/kern/sched_clutch.c`, design in `xnu-src/.../doc/sched_clutch_edge.md`): a Clutch
scheduler per cluster (thread groups → QoS buckets → threads), clusters as nodes of a migration graph, per-QoS preferred clusters per thread
group, spill/steal/rebalance along the graph's edges, scheduling-latency load metrics. What group prefers which cluster, the graph's weights and
the clocks are decided by **CLPC**, Apple's closed performance controller, through the `sched_perfcontrol_*` interface. Open-source XNU builds
Edge only for macOS on Apple silicon. HackiOS now builds it for the generic board and supplies the missing parts:

| Apple component | Here |
|---|---|
| Edge scheduler (`sched_edge_dispatch`, migration/steal/rebalance, foreign buckets, AMP "stir the pot", shared-resource policy, load metrics) | **reused as is** |
| Clutch hierarchy, thread groups, QoS buckets, interactivity scoring, root-bucket EDF/warp, bound runqueues | **reused** |
| Processor sets per cluster, `pset_node`s for E/P, `__AMP__` code paths (`thread_bind_cluster_*`, `hw.perflevel*`, E-core binding of pageout/jetsam threads, per-type recount) | **reused**: `__ARM_AMP__` and `MAX_CPU_CLUSTERS 4` for the generic board (`pexpert/arm64/BCM2837.h`, `board_config.h`) |
| `CONFIG_SCHED_EDGE` only for `XNU_TARGET_OS_OSX` | **adapted**: also with `HK_SCHED_SELECTABLE` (`osfmk/arm64/proc_reg.h`) |
| `SCHED()` fixed to one dispatch table at compile time | **adapted**: a pointer chosen once at boot (`sched_select_dispatch()`, `osfmk/kern/sched_prim.c`): Edge or Clutch |
| Cluster topology from Apple's EDT (`cluster-type`, `cluster-id`, ACC per cluster) | **replaced**: clusters = EPSS frequency domains from the board's description (`board_cpu_domains`), E/P from the loader's `small-core`; verified at run time from each core's own MIDR_EL1 and CCSIDR (L2) |
| AppleARMCPU calling `ml_cpu_init_completed()` (Edge sets its graph there) | **added** to `GenericARMPE` (without it no thread ever left the boot cluster) |
| CLPC (thread-group recommendations, edge matrix, DVFS, energy estimates, render-server and work-interval handling) | **replaced** by `bsd/dev/hk_perfctl.c`, on the same KPI (callouts version 8) |
| Apple's per-cluster DVFS (ApplePMGR) | **replaced**: the controller drives `bsd/dev/hk_cpufreq.c`'s EPSS governor through per-domain clock floors and the touch-boost domain mask |
| Energy estimates in `perfcontrol` callouts (`energy_estimate_nj`, `CONFIG_PERVASIVE_ENERGY`) | **replaced** by a model: P = C·f·V², C from the device's own Linux DT (`dynamic-power-coefficient` 100/533/642), V read from EPSS's voltage table; `config_pervasive_energy` turned on as on iPhoneOS kernels, so threads, tasks and `ri_energy_nj` carry it |
| Deferred IPIs (ACC fast IPI) | not needed: `PE_cpu_signal_deferred` falls back to a normal GIC SGI (as Clutch already did) |
| Cluster power-down / core derecommendation (`sched_perfcontrol_update_recommended_cores`) | **not implemented**: idle cores here only WFI (no power collapse, power.md), so parking gains nothing measurable |
| Shared-resource (cluster L2/BIU) thread flags (callouts v9) | **not used**: no counters to detect contention |
| Hardware performance counters for CLPC (cycles/instructions per switch) | **not available**: MONOTONIC/kpc inert on this board |

## Topology discovery (`osfmk/arm64/machine_routines.c`)

`ml_choose_scheduler()` runs before the processors are counted. The loader lists cores 7, 6..4, 3..1 (core 0 is its own, never the kernel's)
and marks the A55s `small-core`. A cluster is an EPSS frequency domain: `board_cpu_domains=` gives the domain of each core number (4 bits a
core; default `0x21110000`, both phones: cores 0-3 domain 0, 4-6 domain 1, 7 domain 2, as Linux's `qcom,freq-domain`). Domain 0 is shared
with the loader's core 0, which idles in UEFI. More than one domain and not `sched=clutch` → **AMP topology, Edge**: on the OnePlus 8

| cluster | cores (kernel numbers) | type | measured at boot (`perfctl:` lines) |
|---|---|---|---|
| 0 | cpu 0 (core 7) | P | Cortex-A77 r1p0, L2 512 KB, 2841 MHz, capacity 1024 |
| 1 | cpus 1-3 (cores 6-4) | P | Cortex-A77 r1p0, L2 256 KB, 2419 MHz, capacity ~870 |
| 2 | cpus 4-6 (cores 3-1) | E | Kryo Silver (MIDR 0x51..805, Cortex-A55), L2 128 KB, 1804 MHz, capacity ~175 |

Capacity is measured: one bound thread per cluster runs an integer/branch/table workload at the cluster's top speed (`pc_bench()`), the
fastest = 1024. The A55's 0.17 matches single-core benchmarks of the chip, not Qualcomm's DMIPS figure (0.34). A core whose MIDR or L2 differs
from its cluster's is reported (the board description would be wrong). The E cores still start 28 s late (`small_cores_after`): until then
Edge counts their cluster unavailable and places E work on P. `boot-arg small_cores=` and the old "slow secondaries" pairing are used only
under `sched=clutch`.

## The performance controller (`bsd/dev/hk_perfctl.c`)

Started from `GenericARMPE::start()` once the clusters exist; a kernel thread in the perf-controller thread group ticks every 20 ms (250 ms with
the screen off) and wakes at once on a late frame or a touch.

- **Accounting** (callouts, interrupts off, atomics only): every context switch and quantum expiry charges the run time to the thread's group
  by class (FG = above-UI/UI/RT/above-UI kernel; DF; UT; BG), weighted by the core's capacity at its current speed — demand in one unit
  wherever it ran (1024 = the prime core flat out). Also: cores used (width), busy fraction, moves between clusters (the thread's last cluster
  in its perfcontrol state), scheduling latency histograms by class and cluster (on-core callout), work-interval frames, misses and the share
  of its period a frame needs, render-server waits, the energy model's nJ (returned to XNU as `energy_estimate_nj`).
- **Placement levels per group** (E, P = the P cluster with most cores, prime = the fastest P cluster), applied with
  `sched_perfcontrol_thread_group_preferred_clusters_set()` per class (ABOVEUI/UI → FG level, NONUI → DF level, UT and BG → E):
  - *interactive* work (screen lit; UI-class demand, CA frames, UI-app groups with demand) → P at once, held 2 s; a touch restores
    recently interactive groups at once;
  - *deadline* work (work intervals, screen on or off): E only if a frame needs < 30% of its period at E's top speed, prime if > 60% on P
    or frames are missed on P; a missed frame raises the clock floor to the top for 100 ms (written at once);
  - other work by demand: up to P when one thread would need > 90% of an E core (or the group > 90% of the E cluster, or threads never wait
    on an E cluster already at its top speed) for 0.1 s; down when it fits (< 60%) for 0.5 s;
  - new groups (a launch) start on P with the screen lit (or with UI work), on E otherwise;
  - `THREAD_GROUP_FLAGS_EFFICIENT` groups stay on E; UT and BG always prefer E (Apple's `sched_edge_restrict_ut/bg`);
  - the prime cluster takes the busiest interactive/deadline group(s), one per prime core, re-chosen at most every 100 ms;
  - moving up migrates running and runnable threads at once (`MIGRATE_RUNNING|RUNNABLE`), moving down lets them drain.
- **Migration graph** (`sched_perfcontrol_edge_matrix_set()`): P↔P free (weight 0); P→E allowed, weight 64 µs (16 dark, 192 while more
  than 3000 E↔P moves a second: ping-pong costs caches); E→P opened only while the E cluster is saturated (default-QoS expected latency
  ≥ 2 ms for two ticks, or the scheduler's max-runnable-latency callout reports a normal-urgency thread waiting > 20 ms), closed after 0.5 s calm.
- **Clocks**: per-domain floors in `hk_cpufreq.c` (interactive groups: their busiest thread's demand + 25%; deadline groups: a frame in 70%
  of its period); the governor's own schedutil-like rule still follows each group's busiest core; a touch raises only the P domains.

Two Apple-code additions in `sched_clutch.c`, both with a boot-arg to restore Apple's behaviour:

- **BG stays on E** (`sched_edge_bg_up=1` to allow): background-bucket threads never migrate or are stolen E→P. Without it, once E saturated
  and its edge opened, idle P cores stole exactly the BG threads (E runs the higher QoS; BG is what waits): measured 93% of BG time on P.
- **Priority inheritance across core types** (`sched_edge_promote=0` to disable): a thread pushed by a turnstile above its base into the UI
  range (`BASEPRI_FOREGROUND`..RT) prefers the P cluster while queued or woken if its group would keep it on E. A *running* holder is not
  preempted for it (an idle P cluster at its lowest clock is no faster than the busy E core).

`perfctl=0`: Edge with its static defaults, no controller. Under `sched=clutch` the controller only **observes** (same accounting, capacities and
energy model per frequency domain) so the baseline is measured with the same meter; it recommends nothing.

## Observing it

- `kern.hackios.amp` (text): clusters (type, cores, MIDR, L2, capacity, MHz, utilisation, Edge's load metric and average execution time per
  bucket), cores, the energy model (mJ per cluster since boot, mW now), the graph, Edge's counts (placed, placed away by preferred→chosen,
  steals, pulls, rebalance IPIs, AMP rebalances, thread-group migrations), the controller's (recommendation changes, prime, E→P openings,
  frames/missed, render-server blocks, floor boosts), latency histograms, the 24 busiest groups with level, demand, width, moves, frames, mJ.
- `kern.hackios.sched_check` (write 1: run the kernel's checks now) and boot-arg `sched_check=1`: `bsd/dev/hk_sched_check.c`.
- `perfctl_log=1`: every recommendation change in the log.
- User side: `sched_test` (userland `src/sched_test.c`): `amp LABEL`, `check`, `spin`, `fair`, `frames`, `lat`, `mix`, `pi`.
- Host side: `tools/sched-test/qemu.sh` (QEMU, 4 topologies), `tools/sched-test/amp.py` (snapshot deltas, A/B side by side), `tools/t sched`,
  `schedframes`, `schedpi`, `amp` (see [../TESTING.md](../TESTING.md)).

## Measured (OnePlus 8, RAM runs, same kernel, `sched=clutch` vs default Edge)

Energy is the model's estimate of the cores' dynamic power (not idle/leakage, not the rest of the SoC), whole system during the workload.

| workload (`sched_test`) | Clutch (baseline) | Edge + controller |
|---|---|---|
| 3 BG spinners, 3 s | 63.0k it/s, 12.6 J (15 it/mJ), 0% on E | 38.0k it/s, 1.1 J (100 it/mJ), 100% on E |
| 2 DF spinners, 3 s | 42.6k it/s, 8.9 J | 38.3k it/s, 7.1 J (gold cluster; daemon groups do not get the prime core) |
| mix 6 s: 60 Hz render (4 ms) + 2 DF + 3 UT + 2 BG | render p50/p99 7.0/7.7 ms; DF 31.5k (49% on E); UT 44.0k; BG 17.2k; 33.8 J | 6.3/6.9 ms; DF 37.3k (3% E); UT 38.1k; BG 1.8k (starved by UT on E, Clutch's BG deadline share); 20.9 J |
| 60 Hz render + 6 UT, ×2 | p50 7.07/7.08 ms, p99 7.76/7.67, 0 late | 6.54/6.53, p99 10.95/7.14, 0 late |
| 120 Hz render (3 ms) + 7 DF, ×3 | p50 3.1/4.4/4.5 ms, p99 3.5/4.6/4.7, late 0/0/1, max ≤ 8.7 | p50 2.4/4.1/4.1, p99 2.8/4.2/4.2, late 0/1/1, max 5.8/10.7/11.1 |
| wake latency, 4 UI threads + 7 DF load | p50 557 µs, p99 1004 | 562, 1004 |
| 2 vs 14 busy threads in two groups | group A 24% of CPU | 23% (Clutch's group share is the same under both) |
| SpringBoard gestures (`tools/t full`), 3 phases | 78.5 / 75.7 / 65.7 fps; 6.3 / 8.1 / 14.5% late | 78.1 / 76.0 / 66.7 fps; 5.8 / 7.6 / 13.2% late |
| sleep + wake + one gesture set (one pair) | 76.8 J | 59.7 J (prime core 9.6 J vs 26 J) |

Checks: `sched_check` 8/8 on the phone (topology, bind, cluster bind, RT under full load: 100 wakes, latest 0.63 ms, placement of DF and BG
load, spill to every core, the report), 24 lines in QEMU per Edge topology; `tools/t full` on Edge 16/17 (the failing check, frames after
5 rapid sleeps, fails the same way on the baseline: pre-existing), sleeps 10/9, GPU collapse, brightness, dark wake all pass.

Not shown by these runs: the priority-inheritance promotion (`sched_test pi`: holder's location changed, wait times within run-to-run
variance either way); app launch time (only Safari's launch was exercised, not timed); behaviour on redwood; thermal throttling interplay.

## Known limits and next steps

- One late 120 Hz frame in ~1400 under 7 busy DF threads (max ~11 ms) where Clutch had one in 1440 (max 8.7 ms): not traced yet.
- BG work is starved by UT work on a saturated E cluster (QoS-correct; Clutch's root-bucket deadline gives BG ~5%).
- CPU-bound daemon groups get the gold cluster, not the prime core (prime is for interactive/deadline work): −10% throughput for such a
  workload, less energy.
- Render-server inheritance (`thread_group_blocked` with `PERFCONTROL_CALLOUT_BLOCKING_TG_RENDER_SERVER`) is implemented but iOS 16.5 here
  never raised it (`rs_blocks 0`): the port flag may need CoreAnimation features we do not have.
- The energy model ignores idle and leakage; with WFI-only idle and a USB-powered phone the battery gauge cannot validate it.
- `board_cpu_dpc<d>` defaults are SM8250's; redwood needs its own DT values.

## Files

`xnu/files/bsd/dev/hk_perfctl.c`, `hk_sched_check.c`, `hk_cpufreq.c` (floors, voltage table, membench fix), `iokit/Kernel/GenericARMPE.cpp`;
patch hunks in `osfmk/kern/sched_clutch.c` (counters, info accessor, BG guard, promotion), `sched_prim.c/.h` (dispatch selection),
`thread_group.c/.h` (machine data 256 B, iterator), `osfmk/arm64/machine_routines.c` (topology), `osfmk/arm64/proc_reg.h`,
`osfmk/arm/machine_routines.h`, `pexpert/pexpert/arm64/BCM2837.h`, `board_config.h`, `config/MASTER.arm64.bcm2837`; userland
`src/sched_test.c`; `tools/sched-test/`, `tools/t`.
