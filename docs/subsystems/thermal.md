# Thermal service

Device scope: written and run on **redwood** (POCO X5 Pro, `hk_thermal.c` reads the SM7325's 27 TSENS sensors). The
OnePlus 8's SM8250 has the same TSENS layout (`board_tsens0/1`, and GPU sensors tsens0 15 / tsens1 8 used by GPU
DCVS, see [gpu.md](gpu.md)), but the governor has **not been seen slowing cores on either phone**. Sensors other than
temperature (accelerometer, light, GPS) are in [../TODO.md](../TODO.md).

## Thermal service (kernel: `bsd/dev/hk_thermal.c`, userland: `thermald`)

With `cpufreq=1` every core this kernel uses starts at the top of its frequency table; the load governor
([power.md](power.md)) lowers it when idle, but nothing would hold it down under load. The thermal service sets the
ceiling the governor stays under, as Android's thermal engine would:

| What | How |
|---|---|
| Sensors | all 27 TSENS sensors of the SoC, every 250 ms (cores, GPU, DSPs, memory, modem area, camera, video) |
| Limits | from the phone's own device tree; slowing starts at 95 C (cores, GPU, modem area) or 100 C (the rest), lower than Xiaomi's 108..112 C because nothing else governs here |
| Slowing | one row of the frequency table a tick, three when 5 C past the limit; heat outside the cores slows every group |
| Speeding up | one row a second, only after a second of being 5 C under the limit (no see-saw) |
| Battery | from 45 C the pressure is at least moderate, from 50 C the cores are held at half speed, from 58 C at the lowest |
| Emergency | past 115 C everything at the lowest row at once; two seconds there and the run ends (a panic: the loader resets the phone and keeps the log) |
| Sensors silent | five seconds without any CPU reading: the cores are held at half speed |
| iOS | `thermald` posts the level as `com.apple.system.thermalpressurelevel`, so `ProcessInfo.thermalState` changes in apps |

```sh
sysctl kern.hackios.thermal            # the table: every sensor, every group of cores
sysctl kern.hackios.thermal_pressure   # 0 nominal, 1 moderate, 2 heavy, 3 trapping, 4 sleeping
thermald show                          # the same table, from the userland
```

Boot arguments: on by default with `cpufreq=1`; `thermal=0` off; `thermal=2` watch and report without `cpufreq=1`.

**OnePlus 8 (SM8250) sensor map** (`sm8250_where` in `hk_thermal.c`, 2026-10-27, built, unverified): the table is SM7325's. On SM8250 tsens0 7-10 are the tops of cpu4-7 and 11-14 their bottoms (Linux `sm8250.dtsi`, the phone's `cpu-1-N-usr` zones), so the prime core is 10 and 14: zone cpu-1-3 (group 1) reads 13 and cpu-1-6 (group 2) reads 10. tsens1 has sensors 0-8 only: mdmss-2/-3 and camera-0 read nothing, and mdmss-1 is off (it was gpuss-1's sensor 8 again). Before this, prime-core heat on sensor 10 slowed the big group instead.

The governor's decisions are tested on the computer in 11 scenarios (`tools/thermal-test/run.sh`; there is no CI any more): heat-up,
cool-down without see-saw, a single hot group, package heat, near-critical, a one-tick spike, a sustained emergency,
silent sensors, a hot battery, a configuration with one group of cores. The kernel builds with it and boots in QEMU
(where it stays off: QEMU has no TSENS). **On the phone** (POCO X5 Pro, 22 October 2026, `cpufreq=1 cpus=8`, the
cores at 1.8 / 2.4 / 2.4 GHz, one core kept busy for a minute and a half): all 27 sensors answered, 33..38 C, pressure
nominal, nothing slowed -- as it should be at that heat. The governor has not yet been seen slowing the cores on the
phone itself: that needs a load that heats it past 95 C.

## Motion/light sensors (accelerometer, gyroscope, compass, light, proximity, barometer): not available

On this phone they are not wired to the application processor: they hang off the audio DSP, behind its sensor
framework (Qualcomm's "SEE"), reached through the QMI service 400 over QRTR. What is there and what is missing:

| Step | State |
|---|---|
| the DSP started, GLINK, QRTR hello, the DSP's services listed | done (`adsp=1`, `bsd/dev/hk_adsp.c`) |
| which services it offers and which it looks for on our side | `sysctl kern.hackios.adsp_services` (new) |
| QRTR ports of our own on the DSP's channel (for any QMI client) | to do |
| the service locator and notifier the DSP looks for (its sensor domain waits for them) | to do |
| the sensor registry the DSP reads from the phone's Android partitions | to do |
| a SEE client (service 400): find the sensors, stream their samples | to do |
| the samples to iOS as the HID events CoreMotion reads | to do |

`sysctl kern.hackios.adsp_services` shows whether service 400 appears and what the DSP is waiting for; the result of that
run on a phone is not recorded.

## Location (GPS): not available

The GNSS receiver belongs to the modem (`qcom,mss@4080000`), which this port does not start yet; location comes
with the modem, through its QMI location service (16).
