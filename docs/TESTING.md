# Testing

Two worlds: **host/QEMU tests** (no phone) and **OnePlus 8 hardware tests** (`tools/t`, below). The rule of the project: tests are short
(the owner asked twice, 2026-10-25), a failing check is investigated not repeated, never two hardware runs at once, tell the owner before a run.
Read [research/checks.md](research/checks.md) before trusting a "not in the log".

## Without a phone

| Test | Command | What |
|---|---|---|
| installed boot, 14 scenarios | `common/hackios test [scenario...]` (= `common/flash/qemu-test.sh`) | menu, guard, A/B, signatures, file-backed darwin, off-charging flag |
| system sleep in QEMU | `tools/sleep-test/run.sh` | 4 sleeps, every core back, a driver interrupt kept (A/B vs the old per-core quiesce) |
| thermal governor | `tools/thermal-test/run.sh` | 11 scenarios |
| crypto | `tools/crypto-test/run.py` | loader SHA-256/Ed25519 against Python |
| MHI host, WPA/SAE station, QMI | `tools/mhi-test/run.sh`, `tools/wpa-test/run.sh`, `tools/qmi-test/run.sh` | against simulators / independent references / vectors |
| OnePlus input, pipeline latency | `CC=clang python3 tools/instantnoodle-input-test.py`, `python3 tools/pipeline-latency.py --self-test` | |
| AIR / Metal compiler checks | `tools/air/check.sh`, `tools/air/*check*.py` (userland repo) | see docs/air.md |
| scheduler in QEMU | `tools/sched-test/qemu.sh [edge3 edge1 clutch clutch3]` | 8 cores, no root image: Edge on the phones' three-cluster layout (`board_cpu_domains`, `board_small_cores`), Edge on one cluster, Clutch, Clutch with small cores; each boots, sleeps twice and runs the kernel's `sched_check` (topology, binding, cluster binding, RT latency under load, controller placement, spill, the report); ~65 s each |
| scheduler snapshots | `tools/sched-test/amp.py FILE [FILE2]` | what changed between `kern.hackios.amp` snapshots of a run (energy, busy, placement, steals, latency, groups); two files side by side for an A/B; `--self-test` in `tools/t host` |
| syntax | `tools/t host` | shell/Python syntax and the tools' self-tests, 2 s |

There is **no CI** (removed in `cd4210b`); nothing runs these automatically.

## The OnePlus 8 quickly

`tools/t` is the one entry point. It runs the checks of a *level* in a single boot, builds only what changed, and ends the
run the moment the last check has its evidence.

| Command | What it proves | Time (measured) |
|---|---|---|
| `tools/t host` | shell and Python syntax, the tools' self-tests | 2 s |
| `tools/t quick` | boots to a living home screen (30+ frames a second, CPU settled), no panic; the App Library's pods are lighter than their wallpaper (screenshot, numbers) | about 70 s warm, 100 s with a userland build |
| `tools/t gfx` | quick + frame pacing of the usual gestures | about 2 min |
| `tools/t power` | 2 deep sleeps asserted one by one, memory bandwidth and frame pacing the same after the wake, no display fallback | 143 s with a build |
| `tools/t display` | pacing, one sleep, no fallback to the firmware's autorefresh | about 2 min |
| `tools/t full` | everything above, a rapid series of sleeps, a sleep woken through powerd, 45 s idle | about 6 min; only for architectural changes, intermittent failures, or a final check |
| `tools/t boot darkwake` (any names) | single checks | |

Scheduler checks ([subsystems/scheduler.md](subsystems/scheduler.md)): `sched` (the kernel's checks on a settled system, then the repeatable
`sched_test` workloads: spinners per QoS, group fairness, render loops with deadlines under load, wake latency, the mixture, a lock with priority
inheritance; `kern.hackios.amp` before/after into `logs/t-<time>-amp.txt`; ~2 min with the boot), `schedframes` (render loops 5x), `schedpi`
(the lock test 4x), `amp` (a snapshot where it is put). **A/B:** the same check with `T_BOOTARGS=sched=clutch` (the previous scheduler, same kernel;
the controller then only measures, so energy figures compare); repeat a pair before believing a difference of a few percent.

Single checks: `boot pods settle frames sleep sleep1 sleep_rapid powerd darkwake brightness idle safari lockshots`; profiles (no verdict,
they print what to look at): `cpuprof` (per-process CPU before/after a wake, `kern.hackios.top`), `searchprof` (backboardd/SpringBoard
user-space samples with `T_BOOTARGS=hkprof=1`, kernel samples with `tools/kprof.py`), `widgetprof`, `wakeprof` (`hkprof=2`: sampling from
the start of each program).

Exit status 0 when every check passed. Every phase is timed (`timing:` line); the log is `logs/<time>-run.log` (its live copies
are kept when a check fails).

## What the checks assert

- **boot**: `rwtouch frames 30 90` (the window server draws 30 frames a second for two seconds) then `rwtouch idle 60 20`.
  Not a fixed wait. A boot that has not got there in 100 s with a log that does not grow is stopped and its last lines are
  printed; past 160 s even a growing log is called too slow. It is not retried: look at the log.
- **sleep**: `power_test sleep S N` puts the system to sleep through `IOPMSleepSystem` with the wake taken from the kernel's own
  timer (`kern.hackios.sleep_timer`, once), counted as a press of the power key (`kern.hackios.sleep_timer_key`) so that iOS
  wakes fully and lights the screen as it does for a user. Each sleep prints one `t-sleep: PASS|FAIL` line, PASS only if:
  the wake came from what was asked for; `mach_continuous_time()` ran S seconds (-0.3 s, +1.5 s) while `mach_absolute_time()`
  stood still (under 2 s); and all but two cores were off when the sleep began (`hk_power.c` counts them). That is the proof
  that the sleep that the kernel implements was entered. **It is not the SoC's lowest state** (CX and memory collapsed):
  that is not implemented, see [subsystems/power.md](subsystems/power.md).
- **memory after wake**: `kern.hackios.membench` (all cores writing, about a second) before and after: the vote and the
  restarted cores. Not under 80% of before.
- **frames after sleep**: the same gestures before and after; not under 90% of the frame rate, not 5 points more late
  frames, not 25% + 2 ms more at p95.
- **display after wake**: no "autorefresh back" and no lost done interrupt in the kernel's log.
- **pods**: `tools/look/check.py pods` on the screenshot taken at `t-mark: shot-library`.
- **darkwake**: after a timer's dark wake, the lock button must light the screen.

## Why the old tests took so long, and what was done

| Cause (found in the logs) | Fix |
|---|---|
| `loader_return_after` counted mach time, which stands still during system sleep. `xl run` waited XL_SECS + 100 s of wall time, so a run with 190 s of sleeps in it was called "the kernel hung" after 11 minutes, twice, while it was still running | the kernel's timed return is wall time (`loader_return_wake()`, called after every sleep) |
| `xl run` never checked that `oem run` was accepted, and looped on an empty answer until the deadline | refusal is read at once; "never started" after 20 s; 5 empty answers in a row = no answer, with the loader's log saved |
| no way to stop when the test was done | `XL_DONE=regex`: the log is read every 2 s, the run is stopped at `t-mark: done` |
| `pgrep -f "xl run"` / `pkill -f` matched the waiting command itself (exit 144, loops that never end) | `xl wait [SECS]` looks at the run's lock; never a process list |
| `power_test sleep 5` took 20 to 40 s: powerd writes its own wake schedule (the log line: "iOS asked for a wake at calendar second N") and the test's 5 s was not in it, so the sleep ran to `sleep_max` | the test's wake comes from the kernel's timer; `power_test sleep S N powerd` keeps the powerd path as one explicit check (15 s) |
| `power_test` waited `seconds + 120` s to say "not asleep" | 12 s: entering takes a second when iOS allows it |
| fixed waits: 26 + 100 s after boot, 5 s between sleeps, 8 s after unlock | conditions: `rwtouch frames`, `idle`, `uptime`; `power_test waitdisplay`; no pauses between sleeps |
| the governor takes over 45 s after boot, so a measurement needed an 45 s wait | boot-arg `gov_start=30` for tests only (one settle step before the first measurement) |
| 5 of the 9 minutes of the "regression" run were 3 identical gesture sets | one set per phase: ~14 s each, about 800 frames, enough for p95 and the late share |
| the userland image was rebuilt (30 s) for every run | cached by a hash of its sources and its job in `out/tcache/` |

Before: a planned 28 min baseline (stopped at 14), a planned 9 min regression that took 11 min and was wrongly called a hang
(twice). After: 70 s to 2.5 min, with a verdict.

## Rules that keep it short

- Run the smallest level that covers the change; `tools/t host` after every edit to a tool.
- One boot per question: put the checks in one `tools/t a b c` instead of one run each.
- A failing check is investigated, not repeated. The evidence is in the log (and `logs/*-noanswer.loader.log`).
- Never two hardware runs at once: `./xl wait` first. The lock is `$XDG_RUNTIME_DIR/hackios-phone.run`.

## iphont agent checks

`nix-shell --run './iphont test host'` runs Python tests and a compiled C fixture using the actual loader command implementation; it never uses the phone. `iphont kernel-test KERNEL` runs a private QEMU instance with a sleeping process-1 fixture and `iphont=1 iphont_selftest=1`; it checks seven native XNU probes, event records, denied control, and orderly timed return. It tests neither USB nor the iOS daemon. `iphont test device` runs read-only live smoke checks, without booting or flashing. The owner deferred loading the new RAM loader on 2026-10-26; agent hardware/control tests remain pending. See [TOOLKIT.md](TOOLKIT.md).
