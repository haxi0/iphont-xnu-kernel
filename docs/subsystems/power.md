# Power management (OnePlus 8)

What the phone does with its power, from screen-off to shutdown, and where each part lives. This is the
OnePlus 8 (`board=instantnoodle`) unless it says otherwise; redwood has the CPU governor and the thermal service
but not system sleep. The screen's own power and brightness are in [display.md](display.md); the GPU block's
collapse in sleep is in [gpu.md](gpu.md); the thermal service is in [thermal.md](thermal.md).

**State as of 2026-10-26.** System sleep, the panel on/off and the governor were run on the phone in RAM-only runs
(sections marked "Verified"). The later rounds (shared-interrupt fix, volume-key interrupts, charger input limit,
off-mode charging, the empty-battery shutdown) are built and pass the QEMU checks but have **not been
observed on the phone**; the list is under "Verification record". Per the project notes, the installed bundle and
the `boot_b` loader were last updated on 2026-10-25 (power rounds 1-4); later work (GPU collapse, brightness) is
not installed. See [../HANDOFF.md](../HANDOFF.md) for the current state and [../TODO.md](../TODO.md) for what is
not done.

## Processor speed and memory bandwidth

`xnu/files/bsd/dev/hk_cpufreq.c`. There are three EPSS frequency groups per SoC: cores 0-3 (small), 4-6 (big) and
7 (prime); `domain_of()` maps a core's MPIDR to a group. Before the governor every group ran at its top row and
the memory vote at its maximum for the whole run. Now `cpufreq_gov=1` (the default with `cpufreq=1`):

- Each group's row follows its busiest core, schedutil's way: next = 1.25 x current x busy, or 2x from 90% busy.
- It goes up at once and down two rows a tick at most. Ticks are 20 ms, or 250 ms with the screen off.
- A touch or key puts every group at its top for 150 ms (`hk_cpufreq_boost()`, from `hk_touch_post()`).
- It takes over 45 s after boot (`gov_start=30` shortens that for tests), so the boot is as fast as before.
- The thermal service's rows are ceilings (`hk_cpufreq_cap()`).

RPMh's command slot set 0 of the application processor (DRV 2, TCS 0) is shared by the memory vote, the audio DSP's rails, the touch/slider/Wi-Fi/Bluetooth supplies (`instantnoodle_supply()`) and redwood's Goodix: each claims it for one request (`hk_rpmh_claim()`/`hk_rpmh_release()` in `hk_board.c`, Linux `rpmh-rsc.c`'s claim; one try with interrupts off). Before 2026-10-27 they could overwrite each other's commands.

The memory's RPMh vote, from the same thread: `ddr_vote=` (16383) while the screen is lit and something is busy
(a core over half busy or over half its rows, a touch, the GPU above its lowest level); `ddr_idle=` (1024, the
firmware's middle range) after a second idle; `ddr_dark=` (1024) with the screen off; `ddr_sleep=` (0: no request,
the memory at its floor) asleep. The GPU's DCVS timer slows to 1 s with the screen off and the GPU idle, and runs
at once when the screen lights. Governor at idle on the phone (2026-10-25): rows 0..3 of 16..19 on average.

Not done: the CPU scheduler is not heterogeneous-aware (see [cpu.md](cpu.md)); idle cores only WFI.

## System sleep

powerd asks for sleep within seconds of the screen going off when nothing holds an assertion. `IOPMrootDomain`
used to refuse it (`gSleepDisabledFlag`). With `board=instantnoodle` it now allows it; `syssleep=0` refuses it, as
on every other board. The refusal was a workaround for real bugs, all fixed:

- **The sleep hung.** `cpu_exit_wait()` waits for `cpu_sleep_token`, which `GenericARMCPU::quiesceCPU()` never
  set. `psci_smp_cpu_off()` now sets it.
- **A restarted core came back deaf.** `gic_v3_irq_cpu_init()` skipped any core it had set up before, while
  `gic_v3_irq_cpu_shutdown()` had turned off that core's IPIs, timer and Group 1. Threads handed to it were
  stranded and the system froze. Shutdown now forgets the core (`gicr_cpu_base` cleared), so the next init sets it
  up in full.
- **Every driver's interrupt was off after a wake.** `gic_v3_irq_cpu_shutdown()` also called
  `gic_v3_irq_quiesce()`, which disables all shared interrupts in the distributor: right for the return to the
  loader, wrong for a core that goes off for a sleep. After the first wake the display's done/TE interrupt, the
  GPU's fences, the touch screen's and the audio DSP's were all off, so pushes waited out their timeout and the
  display fell back to autorefresh (tearing), GPU waits and touches were polled (slowdown), and VSync followed
  a timer instead of the panel's TE. The per-core shutdown now does only the core's own part; the loader return
  quiesces the shared ones itself. Shown in QEMU (`tools/sleep-test/run.sh`): with the old call a driver's
  interrupt is gone after one sleep. A baseline run on the phone with the fix showed no degradation across
  3x20 s + 5x5 s sleeps (81.6 -> 83.0 -> 84.7 fps).
- **The timer was not set again.** After the clock thaw `rtcPop` still equalled the deadline, so
  `timer_resync_deadlines()` never re-armed the timer. The thaw resets it.

### The sequence (`IOCPUSleepKernel()`, `xnu/files/bsd/dev/hk_power.c`)

1. **`hk_power_will_sleep()`**, every core still up: the audio path parked, the Wi-Fi radio off (network left,
   firmware stopped, PCIe link down; brought back on a thread of its own after the wake), the GPU block collapsed
   (see [gpu.md](gpu.md)), the panel dark if iOS left it lit. These need drivers' interrupts, which go to all cores.
2. Every other core switched off through PSCI (`CPU_OFF`).
3. **`hk_power_sleep()`** on the boot core (`GenericARMCPU::haltCPU()`):
   - every core group at its lowest row, the memory vote at its floor (`hk_cpufreq_sleep()`);
   - every shared interrupt but the wake's disabled at the distributor, the wake's routed to this core;
   - `mach_absolute_time()` frozen, as an iPhone's stops while it sleeps (`ml_sleep_timebase_freeze/thaw`). The
     platform clock (`GenericARMPE::getGMTTimeOfDay`) counts the hardware counter, so the wake's
     `clock_wakeup_calendar()` adds the slept time to the calendar and to `mach_continuous_time()`;
   - WFI until one of: the power key's interrupt; the wake iOS scheduled (the `"wake"` PM setting, from powerd's
     `IOPMSchedulePowerEvent`); the USB cable going in or out; every 10 minutes at 10% battery or less; or
     `sleep_max=`;
   - the loader's heartbeat bumped every 30 s (a watching loader resets a kernel silent for 50 s).
4. Every other core started again.
5. **`hk_power_did_wake()`**: the panel lit again if step 1 darkened it, the Wi-Fi radio brought back.

The wake reason goes to `IOPMrootDomain` (`claimSystemWakeEvent`): `pwrkey`, `rtc`, `usb`, `battery` or
`sleep_max`. A press that woke the phone is passed to iOS as a press even if it ended before the keys were read
again; that is what lights the screen.

`sleep_max=S` caps every sleep. Timed runs from the computer need it: `loader_return_after` is a timer, and no
timer runs during sleep (the kernel's timed return is wall time, `loader_return_wake()` after every sleep).

```sh
sysctl kern.hackios.sleeps kern.hackios.sleep_seconds kern.hackios.sleep_last kern.hackios.display_on
power_test status | display off|on | sleep S [N]      # userland/src/power_test.c; RC_JOBS=rc.jobs.power
tools/sleep-test/run.sh                               # QEMU: 4 sleeps, every core back, a driver's interrupt kept
```

`tools/sleep-test/run.sh` uses the boot-arg `sleep_test=S` (`GenericARMPE::start`). It never ran in CI (CI no
longer exists).

### UI missing after a wake (fixed 2026-10-26)

Owner's report: after a sleep, Control Center opened empty (blur only), status bar items or the whole bar vanished,
apps opened as a blank white screen. Reproduced on the phone (RAM runs, a temporary scenario: screenshots of the
home screen, Control Center, Settings and Clock before and after 3 x 20 s sleeps, two of them dark). Bisected with one
boot each: `gpu_sleep=0` still broken; `syssleep=0` fine; `sleep_freeze=0` (new diagnostic boot-arg: mach time runs on
through the sleep) fine; `ca_vbl=0` (CoreAnimation's own timer) fine. SpringBoard's layer tree (`com.hackios.layers`)
was the same before and after: the clients had committed everything, the window server did not show it.

Cause: QuartzCore reads the framebuffer's VSync times as `mach_continuous_time()`; we sent `mach_absolute_time()`,
which stands still while the system sleeps, so after every sleep the window server's frame clock lagged its clients'
by the total time slept and every animation's start lay in its future. Fix: `MiniIOMobileFramebuffer.cpp` `vsync()`
sends continuous time. Verified: the scenario shows everything after the wake. Of the two times, only the second
("presented") matters (sending only the first as continuous time did not cure it). `tools/t power` after the fix
fails "frames after sleep" on p95 (34 ms against 17-18) in 2 of 2 runs, but a run without the fix shows the same
slow gesture (Control Center closed after the wake: 51-53 fps, 38% of steps over 16.7 ms) and passed only by margin
(p95 31.7 ms), so that slowdown predates the fix; it is not investigated.

### The power key and the other keys

The power key's interrupt (`bsd/dev/hk_buttons.c`, `wake_arm()`) goes: PON KPDPWR, both edges, enabled (each write
read back); PMIC arbiter channel 13 ACC_ENABLE (the charger's USBIN channel 33, which the firmware leaves enabled,
is quieted); PDC pin 1, level high; GIC SPI 481. Verified on the phone with a real press (2026-10-25).

The volume keys' pins (PM8150 GPIO6 and GPIO7, arbiter channels of their own) are set up the same way, both
edges (built, **not run on the phone**). Awake with the screen dark and no key held, the key thread waits for an
interrupt, 250 ms at most, instead of reading the keys every 16 ms; after an interrupt it reads every 16 ms for
500 ms. With the screen lit it reads every 16 ms. If a key is seen to change after a wait that no interrupt ended,
the thread logs "a key changed with no interrupt for it" and reads every 16 ms from then on. Asleep, a volume key's
interrupt is acknowledged and does not wake the phone, as on an iPhone. If any step fails, or the line keeps
firing with no power-key bit, the sleep reads the key every 40 ms instead. The alert slider is read every 300 ms
while dark (100 ms lit).

### What sleep reaches, and what it cannot

Reached (verified every sleep): 6 of 7 cores off; the boot core in WFI at its lowest EPSS row and the loader's
core 0 in WFE; no kernel or app timers running; the panel dark, the touch chip in reset; the memory vote at its
floor; the audio path stopped; the Wi-Fi radio off; the GPU block (CX + GX) powered off.

**Not reached: the SoC's own lowest state** (CX and memory collapsed by the always-on processor, "vdd-min"). It
needs the boot core and the loader's core 0 power-collapsed with the cluster (PSCI `CPU_SUSPEND`), RPMh sleep and
wake sets, and UEFI's resident drivers (the storage controller the loader drives for the kernel, the loader's USB)
dropping their votes. That is a change to how this port is built (UEFI never exits; core 0 serves the storage),
so it is **architecturally blocked**, not a missing driver. Separately, the boot core could be power-collapsed
but would have to come back through the PSCI resume entry with its state restored and nothing else can wake it.

### Other things deliberately not done

- **Power collapse of idle cores while awake** (PSCI `CPU_SUSPEND` with a power-down state, `0x40000004` in the
  stock device tree). Awake cores idle in WFI, which gates the clock but not the power. A power-down idle loses the
  core's registers on every idle and the resume path would run thousands of times a second; QEMU's `CPU_SUSPEND`
  keeps state, so it cannot show a bug in that path, and a bug there is a random hang.
- **The thermal service's 250 ms tick** stays while dark. Its blind and emergency limits are counted in ticks (2 s
  past the stop ends the run), and a phone charging with the screen off still needs them.
- **The panel in off-mode charging** is black, not off. Sending display-off from the loader would need a second
  DSI command path in the loader. Black OLED pixels draw almost nothing; the display engine still scans out.
- **OnePlus fast charging (Warp)** needs the fast-charge MCU's protocol and firmware. Its maximum here is the normal
  charger's 2000 mA.

## Audio path idle (`bsd/dev/hk_sound.c`, `sound=4`)

Before this, 50 DSP periods a second of silence ran for the whole session and the watcher woke 200 times a second.
Now, after 10 s of silence with the amplifiers off, the path to the speakers is parked: the feeder thread, the ASM
stream and ADM device path, and the MI2S port and its bit clock all stop. The path is also parked before a system
sleep. While parked the watcher sleeps until the sink signals sound, and the sink keeps the newest 0.68 s of what
iOS writes, so the first sound is late by the restart, not lost. The watcher clears the bring-up's step pauses
(`sound_pause`, 1 s each) once the bring-up is done (the first unparks took 2030 ms because of them). `sound_park=0`
turns parking off. Parking and unparking were verified silently on the phone; a listening test of the first sound
after an unpark has not been done.

## The loader's core 0

Installed and RAM runs keep the loader on core 0 to serve the storage mailbox and USB. It used to spin flat out.
It now waits in WFE with a 107 us event stream (`CNTKCTL_EL1`); the kernel sends `SEV` after every storage request
(`bsd/dev/hk_ufs.c`) so a request is seen at once. A cable taken out and put back during a run used to leave the
reply the host never took queued in the controller, and every answer after was one behind; the loader now aborts
queued transfers on a cable change and drops a reply it gives up on (`USB->abort_xfer`). Verified: the new loader
from RAM boots, reads the boot slots, runs the kernel and its sleeps; storage reads 131 MB/s cold, 314 MB/s warm.
The cable-replug fix was not re-run.

## Battery and charging

- **Empty battery:** no charger, and the gauge at 0% or under 3300 mV for three readings in a row (15 s), triggers
  `reboot_kernel(RB_HALT)`: every program is told to end, filesystems are synced and unmounted, and the phone
  switches off. `battery_shutdown=0` only logs it; `battery_empty_mv=N` sets another threshold (to try it without
  draining the cell). Built; **never run on the phone**.
- **Charge states** use the stock smb5 numbering: 0 inhibit, 1 trickle, 2 pre, 3 full-on, 4 taper, 5 terminate,
  6 pause, 7 disable. The charging/full state reported to iOS uses it (the earlier "state 3" was fast charging).
- **Charger diagnostics:** every 10 minutes and when the cable goes in or out, one line of PM8150B registers (reads
  only): APSD result, input limit configured and in force, power path, charge enable, temperature state. On a
  computer's port it shows APSD = SDP and "USB 500" mode: the phone gets 500 mA, which drains a busy phone and
  charges a sleeping one.
- **Input current on a wall charger** (on by default; `charge_icl=0` leaves it to the firmware). The firmware leaves
  the charger in "USB 500" mode, so a wall charger gives 500 mA too. On APSD DCP or CDP the driver does what
  OnePlus's smb5 does (`qpnp-smb5.c`, `smb5-lib.c`: `DEFAULT_DCP_MA`, `DEFAULT_CDP_MA`, `SW_OVERRIDE_HC_MODE`):
  input limit 2000 mA for a wall charger (DCP) and 1500 mA for a charging port (CDP) (`0x1370`, 50 mA a step),
  high-current mode (`0x1366` USBIN_MODE_CHG), the command override cleared (`0x1342` bit 0), the limit kept after
  APSD (`0x1365` ICL_OVERRIDE_AFTER_APSD). The charger's AICL lowers the limit if the adapter cannot give that much.
  A computer's port (SDP) is left alone. The firmware's four values come back on unplug; the loader also saves
  them at its start and writes them back before every switch-off and reset, so a raised limit never meets the next
  cable. Built; **never run on a wall charger**.
- **Off-mode charging** (`xnuloader/flashboot.c`, `fb_offmode()`). A shutdown with the cable in (iOS's, or the
  menu's "Power off") sets a flag (`FB_OFF_CHARGING`, bit 2) in the boot state. The PMIC then starts the phone
  again by the cable. That start, with the flag set, no power-key press in the PMIC's power-on reason and the cable
  in, does not go to the countdown: the screen shows the charge state for 5 s then goes black; the PMIC charges on
  its own hardware limits (JEITA) at the wall charger's input limit; the loader checks keys and cable every
  250 ms, in WFE between checks; the power key starts the menu, a volume key shows the state again, the cable out
  switches the phone off. The flag counts once. A cable put into a phone switched off without one still starts it,
  as on an iPhone. QEMU scenario `off-charging` covers only the flag (no PMIC there); the behaviour was **never run
  on the phone**.
- Hardware JEITA limits come from the UEFI's `QcomChargerCfg.cfg` (hard hot 55 C).

## Shutdown, restart, the stock modes

- **Shut down:** the kernel returns reason 3. The loader sets PS_HOLD to "shutdown", then calls
  `ResetSystem(EfiResetShutdown)`; this was confirmed earlier to power the phone off. With the cable in, see
  off-mode charging.
- **Restart:** PS_HOLD set to hard reset, a cold reset, back through ABL to the menu. The OnePlus 8's PON stores
  the reboot reason across a hard reset (`qcom,store-hard-reset-reason`), so a reboot, also one into fastboot, is
  a hard reset there; the loader sets the type itself before every reset (`xnuloader/loader.c`).
- **Boot menu:** entries on the OnePlus 8: "Recovery (TWRP)" (PMIC reason 1), "Bootloader (fastboot)" (reason 2)
  and "Power off". The countdown never chooses them.
- **Bootloader with the cable in:** the stock ABL never comes up on USB after a reset with the cable in. It
  detects a wall charger (`usb: chgr - DCP_CHARGER` in `abl_log`), shows fastboot, switches the phone off, and the
  cable then starts it again through the menu: the "round of reboots". ABL is not ours to change, so the menu entry
  and `fastboot reboot bootloader` now say "unplug the cable" and wait, 60 s at most (`CABLE_OUT_SECONDS`), until
  the PMIC sees no cable before resetting. See [../research/boot-chain-and-recovery.md](../research/boot-chain-and-recovery.md).

## Verification record

On the computer (2026-10-25/26):

| What | How | Result |
|---|---|---|
| kernel builds (instantnoodle) | `common/build.sh instantnoodle` | no errors, no warnings in the changed files |
| installed-boot paths (menu, guard, fallback, signatures, file maps, the off-charging flag) | `common/flash/qemu-test.sh` | 14/14 scenarios |
| system sleep in QEMU, a driver's interrupt kept | `tools/sleep-test/run.sh` | 4/4; with the old per-core quiesce put back the interrupt is lost after the first sleep |
| crypto, thermal governor, Python, shell | `tools/crypto-test/run.py`, `tools/thermal-test/run.sh`, `py_compile`, `shellcheck` | all pass |

On the phone (RAM-only runs; logs `logs/023717-run.log`, `024950-run.log` and later):

| What | Result |
|---|---|
| panel dark/lit with 0x28/0x29, picture after wake | right |
| system sleep with a scheduled wake | 3/3 in each of runs 3, 4 and 5; calendar and continuous time +about 20 s, `mach_absolute_time` +0.1 s, all 6 other cores back |
| iOS's own sleep while locked, with its maintenance wakes | yes |
| power-key wake through PON, arbiter, PDC, SPI 481 | yes (a real press) |
| 10 sleeps in one `tools/t power` run (3 s, 5 x 2 s, 15 s through powerd) | pass, GPU CX and GX off in each |
| touch chip off/on with the screen | 145 ms |
| audio path parked and unparked | yes, silently |

**Not run on the phone** (built, QEMU-checked where possible): the shared-interrupt fix in a long session, the
3-strikes push fallback in the field, the will-sleep/did-wake split under stress, the quick unpark, the USB
reconnect fix, the charge-state numbering, the volume keys' interrupts and the key thread's dark wait, the
slider read every 300 ms while dark, the loader's charger-limit save and restore.

**Never run on the phone:** the drain asleep with the cable out (no battery-drain figure exists; run 6's log
covered only the plugged stretch), the empty-battery shutdown, the charger limit on a wall charger (2000 mA) or
charging port (1500 mA), shutdown with the cable in and off-mode charging, the menu entries and the cable-out
wait, a listening test of the first sound after an unpark.

## What the SoC sleeps in, and what it cannot yet

| State | Status |
|---|---|
| 6 of 7 cores off (PSCI CPU_OFF), back through CPU_ON | implemented, verified every sleep |
| boot core in WFI, clock-gated, lowest EPSS row, memory vote at its floor | implemented, verified |
| display off (DCS 0x28), touch chip in reset, audio path parked, Wi-Fi radio off | implemented; display/touch/audio verified, Wi-Fi sleep **not tried on the phone** |
| GPU block (CX + GX) powered off | implemented 2026-10-26, verified |
| boot core power-collapsed (PSCI CPU_SUSPEND power-down) | supported by the SoC; not implemented |
| cluster / L3 collapse, CX and memory vdd-min with RPMh sleep sets | architecturally blocked (loader core 0 and resident UEFI drivers) |

## Files

`xnu/files/bsd/dev/hk_power.c`, `hk_cpufreq.c`, `hk_buttons.c`, `instantnoodle_battery.c`, `instantnoodle_touch.c`,
`hk_sound.c`, `hk_ufs.c`; `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`, `GenericARMPE.cpp`;
`xnu/files/osfmk/arm64/gic_v3_irq.c`, `psci_smp.c`, `loader_return.c`; `xnuloader/flashboot.c`, `loader.c`;
`userland/src/power_test.c` (userland repo). Hardware facts and failure causes behind these:
[../research/hardware-qualcomm.md](../research/hardware-qualcomm.md).
