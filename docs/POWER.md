# Power management on the OnePlus 8 (instantnoodle)

What the phone does with its power, from screen-off to shutdown, and where each part lives.

**Status (2026-10-25):** built; QEMU regressions pass. Most of it has run on the phone in six RAM-only runs, but
the last two rounds of fixes have not (see "Verified" at the end). Nothing is installed: the bundle and the loader
in `boot_b` are still the earlier ones.

## The screen off and on

backboardd asks the framebuffer for the display's power (`IOMobileFramebufferRequestPowerChange`, selector 12):
`0` when the screen locks or dims out, `1` when it wakes. Before this work both were ignored, and the panel, the
display engine and the 90 Hz frame clock kept running behind a black picture.

| Screen off | Screen on |
|---|---|
| panel: DCS display off (0x28) | display on (0x29) |
| pushes to the panel only remembered; the latest is sent at wake | the latest frame pushed |
| no VSync notifications (a display that is off has none) | VSync resumes for every client |
| touch chip (S6SY761) held in reset, no 100 ms watchdog | chip started as at boot (~145 ms) |
| memory vote down to `ddr_dark=` (1024) | every core group at its top row for 150 ms, memory vote at its top, GPU DCVS looked at at once |

Not sleep in (0x10), although Android's off-command has it. After sleep in, the panel lost its DSC setup and
showed coloured static. Neither sleep out plus display on, nor Android's whole on-command, brought the picture back:
Android's picture parameters aren't the ones the firmware set the display engine up with. Android resets and
repowers the panel to recover; here the firmware's setup has to stay.

**Frame pushes.** One push that doesn't finish in 100 ms no longer drops the display into the firmware's
autorefresh for the rest of the run, which means CPU-copied frames, tearing and no GPU scaling. That happens
only after 3 failures in a row (`PUSH_FAILURES`). A push whose done bit is set but whose interrupt never came is
counted as done, and logged ("a push was done but its interrupt (115) did not come"): it shouldn't happen.

Files: `iokit/Kernel/MiniIOMobileFramebuffer.cpp` (`panelPower()`, `pushWait()`), `bsd/dev/hk_power.c`,
`bsd/dev/instantnoodle_touch.c`. DCS packets go through DSI0's command DMA from a page between the two
framebuffers (`DSI_DMA_PHYS`).

## Processor speed and memory bandwidth

Before: every core group at its top EPSS row and the memory vote at its maximum, for the whole run.

Now `bsd/dev/hk_cpufreq.c` has a governor (`cpufreq_gov=1`, on by default with `cpufreq=1`):
- Each group's row follows its busiest core, schedutil's way: next = 1.25 × current × busy, or 2× from 90% busy.
- It goes up at once and down two rows a tick at most. Ticks are 20 ms, or 250 ms with the screen off.
- A touch or key puts every group at its top for 150 ms (`hk_cpufreq_boost()`, from `hk_touch_post()`).
- It takes over 45 s after boot, so the boot is as fast as before.
- The thermal service's rows are ceilings (`hk_cpufreq_cap()`).

The memory's RPMh vote, from the same thread:
- `ddr_vote=` (16383) while the screen is lit and something is busy (a core over half busy or over half its rows,
  a touch, the GPU above its lowest level);
- `ddr_idle=` (1024, the firmware's own middle range) after a second idle;
- `ddr_dark=` (1024) with the screen off;
- `ddr_sleep=` (0: no request, the memory at its floor) asleep.

The GPU's DCVS timer slows to 1 s with the screen off and the GPU idle, and runs at once when the screen lights.

## System sleep

powerd asks for sleep within seconds of the screen going off when nothing holds an assertion. IOPMrootDomain used
to refuse it (`gSleepDisabledFlag`). With `board=instantnoodle` it now allows it; `syssleep=0` refuses it, as on
every other board. The refusal was a workaround for real bugs, all fixed:
- **The sleep hung.** `cpu_exit_wait()` waits for `cpu_sleep_token`, which `GenericARMCPU::quiesceCPU()` never set.
  `psci_smp_cpu_off()` now sets it.
- **A restarted core came back deaf.** `gic_v3_irq_cpu_init()` skipped any core it had set up before, while
  `gic_v3_irq_cpu_shutdown()` had turned off that core's IPIs, timer and Group 1. Threads handed to it were stranded
  and the system froze. Shutdown now forgets the core, so the next init sets it up in full.
- **Every driver's interrupt was off after a wake.** `gic_v3_irq_cpu_shutdown()` also called
  `gic_v3_irq_quiesce()`, which disables all shared interrupts in the distributor: right for the return to the
  loader, wrong for a core that goes off for a sleep. After the first wake the display's done/TE interrupt, the
  GPU's fences, the touch screen's and the audio DSP's were all off:
  - pushes waited out their timeout and the display fell back to autorefresh (the tearing);
  - GPU waits and touches were polled (the slowdown);
  - VSync followed a timer instead of the panel's TE.
  The per-core shutdown now does only the core's own part; the loader return quiesces the shared ones itself, as
  before. Shown in QEMU: with the old call, a driver's interrupt is gone after one sleep.
- **The timer was not set again.** After the clock thaw `rtcPop` still equalled the deadline, so
  `timer_resync_deadlines()` never set the timer again. The thaw resets it.

The sequence, in `IOCPUSleepKernel()`:
1. **`hk_power_will_sleep()`**, every core still up: the audio path parked; the panel dark if iOS left it lit.
   These need drivers' interrupts, which go to all cores.
2. Every other core switched off through PSCI.
3. **`hk_power_sleep()`** on the boot core (`GenericARMCPU::haltCPU()`):
   - every core group at its lowest row, the memory vote at its floor;
   - every shared interrupt but the wake's disabled at the distributor, and the wake's routed to this core;
   - `mach_absolute_time()` frozen, as an iPhone's stops while it sleeps (`ml_sleep_timebase_freeze/thaw`). The
     platform clock (`GenericARMPE::getGMTTimeOfDay`) counts the hardware counter, so the wake's
     `clock_wakeup_calendar()` adds the slept time to the calendar and to `mach_continuous_time()`;
   - WFI until one of: the power key's interrupt; the wake iOS scheduled (the `"wake"` PM setting, from powerd's
     `IOPMSchedulePowerEvent`); the USB cable going in or out; every 10 minutes at 10% battery or less; or
     `sleep_max=`;
   - the loader's heartbeat bumped every 30 s (a watching loader resets a kernel that is silent for 50 s).
4. Every other core started again.
5. **`hk_power_did_wake()`**: the panel lit again if step 1 darkened it.

The power key's interrupt (`bsd/dev/hk_buttons.c`, `wake_arm()`) goes:
1. PON KPDPWR, both edges, enabled (each write read back);
2. arbiter channel 13 ACC_ENABLE; the charger's USBIN channel 33, which the firmware leaves enabled, is quieted;
3. PDC pin 1, level high;
4. GIC SPI 481.

The volume keys' pins (PM8150 GPIO6 and GPIO7, arbiter channels of their own) are set up the same way, both
edges. Awake with the screen dark and no key held, the key thread then waits for an interrupt, 250 ms at most,
instead of reading the keys every 16 ms. After an interrupt it reads every 16 ms for 500 ms. With the screen lit
it reads every 16 ms as before. If a key is seen to change after a wait that no interrupt ended, the thread logs
"a key changed with no interrupt for it" and reads every 16 ms from then on. Asleep, a volume key's interrupt is
acknowledged and does not wake the phone, as on an iPhone.

If any step fails, or the line keeps firing with no power-key bit, the sleep reads the key every 40 ms instead. A
press that woke the phone is passed to iOS as a press even if it ended before the keys were read again; that is
what lights the screen. The wake reason goes to IOPMrootDomain (`claimSystemWakeEvent`): `pwrkey`, `rtc`, `usb`,
`battery` or `sleep_max`.

`sleep_max=S` caps every sleep. Timed runs from the computer need it: `loader_return_after` is a timer, and no
timer runs during sleep.

```sh
sysctl kern.hackios.sleeps kern.hackios.sleep_seconds kern.hackios.sleep_last kern.hackios.display_on
power_test status | display off|on | sleep S [N]      # userland/src/power_test.c; RC_JOBS=rc.jobs.power
tools/sleep-test/run.sh                               # QEMU: 4 sleeps, every core back, a driver's interrupt kept
```

`tools/sleep-test/run.sh` uses the boot-arg `sleep_test=S` (`GenericARMPE::start`). It isn't in CI: CI runs the
prebuilt kernel, which predates it.

### Limitation: the SoC's own lowest state

Sleep does not reach the chip's lowest state (CX and memory collapsed by the always-on processor, "vdd-min").
That would need:
- the boot core and the loader's core 0 power-collapsed with their clusters (PSCI CPU_SUSPEND), with a resume path
  in both XNU and the UEFI-resident loader;
- RPMh sleep and wake sets;
- UEFI's resident drivers to drop their votes: the storage controller, which the loader drives for the kernel, and
  the loader's USB.

That is a change to how this port is built (UEFI never exits; core 0 serves the storage) and is not attempted.

What sleep does reach:
- 6 of 8 cores off;
- the boot core in WFI and the loader's core 0 in WFE, both at their lowest speed;
- no kernel or app timers running;
- the panel dark, the touch chip in reset;
- the memory vote at its floor;
- the audio path stopped.

### Other things not done, and why

- **Power collapse of idle cores while awake** (PSCI CPU_SUSPEND with a power-down state, 0x40000004 in the stock
  device tree). Awake cores idle in WFI, which gates the clock but not the power. A power-down idle loses the
  core's registers on every idle, and the resume path would run thousands of times a second. QEMU's CPU_SUSPEND
  keeps the state, so it can't show a bug in that path, and a bug there is a random hang. Asleep it doesn't matter:
  the other cores are switched off then.
- **The thermal service's 250 ms tick** stays while dark. Its blind and emergency limits are counted in ticks (2 s
  past the stop ends the run), and a phone charging with the screen off still needs them. Its reads are register
  reads of the sensors.
- **The panel in off-mode charging** is black, not off. Sending display off from the loader would mean a second
  DSI command path in the loader, and the menu after it needs the panel on again. Black OLED pixels draw almost
  nothing; the display engine still scans out.
- **OnePlus fast charging (Warp)** needs the fast-charge MCU's protocol and firmware. Its maximum here is the
  normal charger's 2000 mA.

## Audio DSP idle (`bsd/dev/hk_sound.c`, sound=4)

Before this, 50 DSP periods a second of silence ran for the whole session, and the watcher woke 200 times a second.

Now, after 10 s of silence with the amplifiers off, the path to the speakers is parked. The feeder thread, the
ASM stream and ADM device path, and the MI2S port and its bit clock all stop. The path is also parked before a
system sleep. While parked:
- the watcher sleeps until the sink signals sound;
- the sink keeps the newest 0.68 s of what iOS writes, so the first sound is late by the restart, not lost.

The watcher clears the bring-up's step pauses (`sound_pause`, 1 s each) once the bring-up is done: on the phone,
the first unparks took 2030 ms because of them. `sound_park=0` turns parking off.

## The loader's core 0

Installed and RAM runs keep the loader on core 0 to serve the storage mailbox and USB. It used to spin flat out
for the whole run. It now waits in WFE with a 107 µs event stream (`CNTKCTL_EL1`). The kernel sends `SEV` after
every storage request (`bsd/dev/hk_ufs.c`), so a request is seen at once.

A cable taken out and put back during a run used to leave the reply the host never took queued in the
controller, and every answer after that was one behind. The loader now aborts queued transfers on a cable change,
and drops a reply it gives up on (`USB->abort_xfer`).

## Battery and charging

- **Empty battery:** no charger, and the gauge at 0% or under 3300 mV for three readings in a row (15 s), triggers
  `reboot_kernel(RB_HALT)`: every program is told to end, the filesystems are synced and unmounted, and the phone
  switches off. Android's healthd also shuts down at the gauge's 0%. `battery_shutdown=0` only logs it;
  `battery_empty_mv=N` sets another threshold, which is how it can be tried without draining the cell.
- **Charge states** use the stock smb5 numbering: 0 inhibit, 1 trickle, 2 pre, 3 full-on, 4 taper, 5 terminate,
  6 pause, 7 disable. The charging/full state reported to iOS uses it. The earlier "state 3" was fast charging.
- **Charger diagnostics:** every 10 minutes and when the cable goes in or out, one line of PM8150B registers (reads
  only): APSD result, input limit configured and in force, power path, charge enable, temperature state. On the
  computer's port it shows APSD = SDP and "USB 500" mode, so the phone gets 500 mA. That drains a busy phone and
  charges a sleeping one.
- **Input current on a wall charger** (on by default; `charge_icl=0` leaves it to the firmware): the firmware
  leaves the charger in "USB 500" mode, so a wall charger gives 500 mA too. On APSD DCP or CDP, the driver does
  what OnePlus's smb5 does (`qpnp-smb5.c`, `smb5-lib.c`: `DEFAULT_DCP_MA`, `DEFAULT_CDP_MA`, SW_OVERRIDE_HC_MODE):
  - input limit 2000 mA for a wall charger (DCP) and 1500 mA for a charging port (CDP) (`0x1370`, 50 mA a step);
  - high-current mode (`0x1366` USBIN_MODE_CHG), the command override cleared (`0x1342` bit 0);
  - the limit kept after APSD (`0x1365` ICL_OVERRIDE_AFTER_APSD).

  The charger's AICL lowers the limit if the adapter can't give that much. A computer's port (SDP) is left alone.
  The firmware's four values come back on unplug. The loader also saves them at its start and writes them back
  before every switch-off and reset, so a raised limit never meets the next cable, which may be a computer's port.
- **Off-mode charging** (the loader, `xnuloader/flashboot.c`, `fb_offmode()`). A shutdown with the cable in
  (iOS's, or the menu's "Power off") sets a flag in the boot state. The PMIC then starts the phone again by the
  cable. That start, with the flag set, no power-key press in the PMIC's power-on reason and the cable in, doesn't
  go to the countdown. Instead:
  - the screen shows the charge state for 5 s, then goes black;
  - the PMIC charges on its own hardware limits (JEITA), at the wall charger's input limit;
  - the loader checks the keys and the cable every 250 ms, in WFE between checks;
  - the power key starts the menu; a volume key shows the state again; the cable out switches the phone off.

  The flag counts once. A cable put into a phone switched off without one still starts it, as on an iPhone. The
  panel is black but stays on, with the display engine scanning out (see the limitations).
- Hardware JEITA limits come from the UEFI's `QcomChargerCfg.cfg` (hard hot 55 C). The OnePlus fast-charge MCU
  ("Warp") is left alone.

## Shutdown, restart, the stock modes

- **Shut down:** the kernel returns reason 3. The loader sets PS_HOLD to "shutdown", then calls
  `ResetSystem(EfiResetShutdown)`. This was confirmed earlier to power the phone off. With the cable in, see
  off-mode charging above.
- **Restart:** PS_HOLD set to hard reset, a cold reset, back through ABL to the menu (unchanged).
- **Boot menu:** new entries on the OnePlus 8: "Recovery (TWRP)" (PMIC reason 1), "Bootloader (fastboot)" (reason 2)
  and "Power off". The countdown never chooses them.
- **Bootloader with the cable in:** the stock ABL never comes up on USB after a reset with the cable in. It
  detects a wall charger (`usb: chgr - DCP_CHARGER` in abl_log), shows fastboot, switches the phone off, and the
  cable then starts it again through the menu: that is the round of reboots. ABL is not ours to change. So the menu
  entry and `fastboot reboot bootloader` now say "unplug the cable" and wait, 60 s at most, until the PMIC sees no
  cable before resetting. ABL then starts without the cable, and its fastboot comes up on USB once the cable goes
  back in.

## Verified

On the computer:

| What | How | Result |
|---|---|---|
| kernel builds (instantnoodle) | `common/build.sh instantnoodle` | no errors, no warnings in the changed files |
| loader builds as CI builds it | `OUT=loader-ci.efi xnuloader/build.sh` | no warnings |
| installed-boot paths (menu, guard, fallback, signatures, file maps, the off-charging flag) | `common/flash/qemu-test.sh` | 14/14 |
| system sleep in QEMU, a driver's interrupt kept | `tools/sleep-test/run.sh` | 4/4; with the old per-core quiesce put back: the interrupt lost after the first sleep |
| crypto, thermal governor, Python, shell scripts | `tools/crypto-test/run.py`, `tools/thermal-test/run.sh`, `py_compile`, `shellcheck` | all pass |

On the phone (RAM runs, earlier builds):

| What | Result |
|---|---|
| panel dark/lit with 0x28/0x29, picture after wake | right (you looked) |
| system sleep with a scheduled wake | 3/3 in each of runs 3, 4 and 5. Calendar and continuous time +~20 s, `mach_absolute_time` +0.1 s, all 6 other cores back |
| iOS's own sleep while locked, with its maintenance wakes | yes |
| power key wake through PON → arbiter → PDC → SPI 481 | yes (your press) |
| governor at idle | rows 0..3 of 16..19 on average |
| touch chip off/on with the screen | 145 ms |
| new loader from RAM (`fastboot boot` from the stock ABL) | boots, reads the boot slots, runs the kernel and its sleeps; storage reads 131 MB/s cold, 314 MB/s warm |
| audio path parked and unparked | yes, silently |

**Not yet on the phone:** everything from the last two rounds:
- the shared-interrupt fix (the post-wake slowdown and tearing);
- the 3-strikes push fallback;
- the will-sleep/did-wake split;
- the GPU DCVS kick;
- the quick unpark;
- the USB reconnect fix;
- the charge-state numbering;
- the volume keys' interrupts and the key thread's dark wait;
- the slider read every 300 ms while dark (100 ms lit);
- the loader's charger-limit save and restore.

**Never run on the phone:**
- the drain asleep with the cable out (no number);
- the empty-battery shutdown;
- the charger limit on a wall charger (2000 mA) or a charging port (1500 mA);
- shutdown with the cable in, and off-mode charging (QEMU covers only the flag: no PMIC there);
- the new menu entries and the cable-out wait;
- a listening test of the first sound after an unpark.
