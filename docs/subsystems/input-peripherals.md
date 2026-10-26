# Input and peripherals: touch, keys, slider, haptics, flashlight, battery, camera

Everything that is a small device on the phone. None of Apple's drivers exist here, so each is a kernel driver that
feeds a character device (or an IOKit stand-in) and a library of ours inside a stock iOS daemon that turns it into
what iOS expects.

## Touch and buttons: `/dev/hk-touch`

iOS expects touches from Apple's multitouch driver through IOHIDFamily; neither exists. A producer (a kernel driver,
`virtio-input` under QEMU, or a script via `rwtouch`) posts `struct hk_touch` records (`xnu/files/bsd/sys/hk_input.h`:
`HK_TOUCH_DOWN/MOVE/UP`, `HK_BUTTON_DOWN/UP`, `HK_SLIDER`; coordinates 0..65535) to `/dev/hk-touch`
(`hk_input.c`, `hk_touch_post()`). `userland/src/inject.c` inside **backboardd** reads them and synthesises the
events: a "virtual" HID service (built in, one usage) turns samples into the events Apple's driver would send.
Findings behind it:

- backboardd takes for its touch screen a service that exists when its touch processing starts; one that turns up
  later is not noticed. CoreBrightness's five-second wait for an ambient light sensor
  (`-[BLControl waitForALSArrival]`) used to hide this race by accident; the wait is kept (a "brightness_fixes()"
  attempt to remove it made backboardd stop adopting the service: "still waiting for a main screen digitizer"). The
  service is therefore created repeatedly until the server has opened it (`make_service()`).
- UIKit's screen-edge recognizers (swipe up to leave an app, swipe back, pull down) accept only touches that Apple's
  controller marked as coming in over an edge, from the shape of the contact. All we have is where the finger landed,
  so a finger landing within 16/375 of the width or 16/812 of the height of an edge is marked as coming in over it.
- `TouchDetectionMode` from the window server (low byte): 1 report touches, `0xfe` wake mode (display off, a tap is to
  wake it), `0xff` off.
- Scripted touches (`rwtouch`) do not reach iOS's *system* gestures (swipe up for the app switcher, going home).
  Use `rwtouch swipe` from the bottom edge for the real gesture path and `rwtouch notify com.hackios.switcher` for
  the switcher. As of the last notes the switcher notification returns but does not change the screen when three apps run.
- Latency: `touch_lat=1` prints a line per sample (`tl: i TYPE posted US read US sent US`) and per SpringBoard
  run-loop turn; analyse with `tools/latency.py`, `tools/frames.py`, `tools/pipeline-latency.py`.

### Drivers

| Device | Chip | File | Notes |
|---|---|---|---|
| redwood | Goodix GT9916 ("Berlin-D") or FocalTech FT3680, SPI of serial engine 13 | `bsd/dev/hk_goodix.c` | `touchscreen=1`. GPIO96=1 and GPIO68=0 identify the FocalTech variant. No SPI framework: the driver does power (PM8350C LDO2 through RPMh), clocks, pins and the GENI engine in FIFO mode itself, or bit-bangs the four pins. **No interrupt**: a thread looks at the line 125 times a second (`touch_poll_us`). FocalTech firmware upload is `hk_focaltech_fw.h` (generated from `fw-touch/focaltech_ts_fw_m20.bin`, ~114 KB over SPI). |
| instantnoodle | Samsung S6SY761 ("SY771"), I2C `0x48` | `bsd/dev/instantnoodle_touch.c` | `board=instantnoodle`, `touchscreen=0` off. Pins gpio36/37 driven by hand in `instantnoodle_i2c.c` (open drain: low = output 0, high = input), because the engine's FIFO path is disabled on these phones and would need GPI DMA. Reset gpio38, report line gpio39 (active low, TLMM summary interrupt 208+32). Supplies PM8150A LDO1 (1.8 V) and PM8150 LDO13 (3 V) through RPMh. Panel coordinates 1080 x 2400 scaled to 0..65535. Configuration commands are not acknowledged on this phone yet the chip reports touches after its boot-complete event. Interrupt-driven (`touch_irq=0`: 10 ms polling); worker at priority 92. Held in reset with the screen off. |
| QEMU | virtio-input | `hk_virtio_input.c` | `virtio_input=1`; mouse with the left button is a finger |

**Never pass `touchscreen=1` on the OnePlus 8.** That argument selects redwood's Goodix driver; it touched GPIO68 and sent
the OnePlus to a NoC CrashDump. The OnePlus path is guarded now, but the profile does not pass it.

### Buttons (`hk_buttons.c`)

None of the three keys is a SoC pin: they are inputs of the PMICs on the SPMI bus, reached through the SoC's **PMIC
arbiter** (version 5), which gives each system on the chip a set of registers per PMIC peripheral ("channel").
Redwood: power = KPDPWR_N and volume down = RESIN_N of the PMK8350's PON block, volume up = GPIO6 of the PM7325
(pulled up). OnePlus 8: PM8150 PON and GPIO6/7. The driver **reads only** (the one thing written is the arbiter's
command word); the firmware reads the same keys itself (fastboot/recovery choice) and has left them configured; the
driver verifies that before it believes them and a key that reads as held before it was ever seen released is not
believed. On the OnePlus 8 the power key and volume keys have interrupts (arbiter, PDC, SPI 481; see
[power.md](power.md)). Bring-up steps `buttons_steps=N`. The phone has what the iPhone X (whose userland runs here)
has: side button (`HK_BUTTON_LOCK`) and two volume keys, no home button. `hk_spmi_lock()` serialises all PMIC
transfers (two at once and the arbiter drops one).

### Alert slider (OnePlus 8, `instantnoodle_slider.c`)

A magnet in the slider and two iSentek IST8801 hall sensors on serial engine 5 (GPIO 4/5), "up" `0x19` and "down"
`0x18`, powered by PM8150A LDO10 (3.0 V). The factory calibration (`/mnt/vendor/persist/engineermode/tri_state_hall_data`,
three pairs `down,up`; boot-arg `slider_cal=D,U,D,U,D,U`, in the profile) decides the position (nearest of three within
22); without it OxygenOS's rule (above 30 up, below -30 down). A position counts after two equal reads. Interrupts
not used: both sensors are read every 100 ms (300 ms while dark). Reported as `HK_SLIDER` records and
`kern.hackios.slider`. iOS side: backboardd keeps ring/silent (middle = silent, down and up = ring) and SpringBoard shows
its ringer HUD; "up" drives **Do Not Disturb** through an assertion of SpringBoard's HID client
(`com.apple.springboard.donotdisturb.hid`, named in its entitlements), dressing the ringer pill (`SBRingerPillView`)
with a moon and "Do Not Disturb". The first DND call is deferred 20 s or SpringBoard SIGSEGVs. Verified on the phone by
the owner.

### Haptics (`hk_haptic.c`, `haptic=N`)

Redwood: Awinic AW86927 (ID 0x9270, Xiaomi's `aw8692x` 1.7.0), I2C of serial engine 9 (GPIO 36/37), address `0x5a`,
reset GPIO 35; the motor's f0 is 175.5 Hz. OnePlus 8: Awinic AW8697 with the waveforms of the phone's
`/vendor/firmware/aw8697_haptic.bin` (`aw8697_haptic_170.bin` is used; 11 waveforms: 1..10 short, 11 looped). `haptic=1..4`
progress from reading to playing an effect; **`haptic=5` is the mode for iOS**: what iOS plays on its "Actuator" device
(`audio_plugin.c`) is written to `/dev/hk-haptic` as one-channel samples and each waveform becomes a tap (waveform 1) or,
if longer than a tap, the looped waveform until it ends (at most `HAPTIC_MAX_MS`, then rest for as long). Gain follows the
loudness (Android's own 0x80 from a third of full scale; five eighths for the softest; never above). Limits: Android's
own values only; any fault flag, failed read-back or unacknowledged byte puts the chip in reset for the run. Verified
on the OnePlus 8 (system feedback, CoreHaptics).

### Flashlight (`hk_torch.c`, `torch=N`)

Redwood: the PM8350C flash LED module (SPMI slave 2, `0xee00`), channel 0 at Android's torch current 125 mA
(12.5 mA resolution). OnePlus 8: PM8150L, both LEDs. `torch=1` looks, `2` lights for `torch_secs`, **`3` is the mode for
iOS**: `/dev/hk-torch` takes a level 0..1000 (thousandths of Android's current, never above it). Limits: the light on at
most `torch_max_s` (default 30, max 120) at a time; the module's fault and over-temperature flags, the PMIC's
temperature alarm and the battery's temperature (45 C) are checked ten times a second and any trouble switches it off for
the run. Every write goes through one function that takes only this module's registers, with read-back. iOS side:
the loader tells MobileGestalt the camera has a flash (`/product/camera "flash"`) and `libhackios-media.dylib` in
mediaserverd gives `FigFlashlight` (which finds no camera) a device of ours whose torch level is written to
`/dev/hk-torch` (Control Center's button and AVFlashlight). Verified on both.

### Battery and charger (`hk_battery.c`, `instantnoodle_battery.c`, `HKBattery.cpp`)

iOS finds every `IOPMPowerSource` (the first is "InternalBattery-0"). `HKBattery.cpp` is that service; values reach it
through `hk_battery_report()` with a `struct hk_battery_state` (percent in hundredths, voltage, current, temperature,
present, external, status, charges in uAh, cycles, USB type). `CurrentCapacity` is the percentage and `MaxCapacity`
100 (newer-battery convention); mAh values are `AppleRawCurrentCapacity` etc. Everything else (status bar, "Charging"
on the lock screen, Batteries widget, Settings > Battery) follows from powerd. The status-bar percentage switch is
turned on by the image (`SBShowBatteryPercentage`).

- **Redwood:** values from the audio DSP's battery manager (`hk_adsp.c`, `adsp=1`, `battmgr=0` off). Charger limits and
  JEITA are firmware-owned. Without `adsp=1` the loader owns the firmware charger context on core 0 (`battery=1`).
  Verified charging with `adsp=1` (+0.4 A from a computer's port); `sysctl kern.hackios.charger` has health and type.
- **OnePlus 8:** a TI **bq27541** fuel gauge (bq27411 variant if the type register says so) at I2C `0x55` on serial
  engine 16 (gpio48/49, pull-up and clock stretching needed), OnePlus's driver `bq27541_fuelgauger.c` as the register map.
  Charger presence from the PM8150B (SPMI slave 2, USBIN `0x1310` bit 4, state `0x1006` [2:0]). Reads every 5 s. The Warp
  fast-charge MCU (`0x26`) is left alone. Details of charge limits, off-mode charging and the empty-battery shutdown:
  [power.md](power.md).
- A UIDevice fallback in SpringBoard (`inject_sb.c`) still reads `kern.hackios.battery_soc/charging` when no power source
  exists; once `HKBattery` is there the hooks hand everything to iOS's own code.

### Cameras (redwood only: `hk_camera.c`, `cam=N`)

Redwood has four sensors: macro OmniVision OV02B10 (CCI 1 master 0, `0x3c`), wide Samsung S5KHM2SD 108 MP (CCI 0
master 1, `0x20`, chip ID `0xfad2`), front OmniVision OV16A1Q 16 MP (CCI 0 master 0, `0x10`, ID `0x1641`), ultra-wide
Samsung S5K4H7 8 MP (CCI 1 master 1, `0x2d`, ID `0x487b`), with the PM8008i supplying the LDOs. The driver was brought
up step by step: `cam=1` look (reads only), `2` power the sensor as Android does and read its ID, `3` the CSID test
pattern through RDI 0 / IFE-lite 0 into memory (`xl camera` fetches it), `4` the sensor's own frames through its CSIPHY,
**`5` the wide camera for iOS** (`/dev/hk-camera`; `cam_sensor=wide` selects it in the early modes). Limits: only Android's
voltages; every write read back; anything wrong switches all off.

iOS side (userland repo): `inject_media.c` gives mediaserverd's capture-device vendor the create function of our
capture device (`camera_device.c`), a `FigCaptureDevice` with two `FigCaptureStream`s (wide as the back camera, front
as the front), laid out like H10ISP's in iOS 16.5, formats named as the iPhone X's in CMCapture's
`D22/AVCaptureSession.plist`. Frames: RAW10 read from `/dev/hk-camera`, made into pictures by the CPU
(`camera_isp.c`: black level, lens shading, grey-world white balance and exposure gains, the camera's own colour matrix,
sRGB gamma, zoom; no noise reduction). Camera.app shows preview, focus (by contrast, continuous, and on a tap), and
takes HEIC photos (README claim). The portrait-mode Core Image prewarm that crashed mediaserverd is skipped.
Not on the OnePlus 8 (no driver), and no video or Live Photo recording. See [../KNOWN_ISSUES.md](../KNOWN_ISSUES.md).

### Sensors, ambient light, location

Not present. The accelerometer, gyroscope, compass, light, proximity and barometer hang off the audio DSP behind its
sensor framework ("SEE"), reached through QMI service 400 over QRTR; the steps and their state are in
[../TODO.md](../TODO.md). GPS belongs to the modem, which is not started.

## Files

`xnu/files/bsd/dev/hk_input.c`, `hk_goodix.c`, `hk_virtio_input.c`, `instantnoodle_touch.c`, `instantnoodle_i2c.c`,
`hk_buttons.c`, `instantnoodle_slider.c`, `hk_haptic.c`, `hk_torch.c`, `hk_battery.c`, `instantnoodle_battery.c`,
`hk_camera.c`; `xnu/files/iokit/Kernel/HKBattery.cpp`; `xnu/files/bsd/sys/hk_input.h`, `instantnoodle.h`. Userland repo:
`userland/src/inject.c`, `inject_sb.c`, `inject_media.c`, `camera_device.c`, `camera_isp.c`, `rwtouch.c`, `battery_test.c`,
`torch_test.c`.
