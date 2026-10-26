# Known issues — what does not work on this device (right now)

Answers **"what currently doesn't work?"** Entries are things a user or developer will notice; work to fix them is in [TODO.md](TODO.md) (`T..`), not repeated here.
Primary device: **OnePlus 8 (instantnoodle)**; redwood and QEMU are marked where different. Audited 2026-10-26 against the source and the project's records.

**Kind:** *Unsupported* (no implementation) · *Bug* (implemented, wrong; cause known or not) · *Partial* · *Unreliable* · *Unverified* (implemented, never seen working).
**Severity:** **S1** blocks basic use or safety · **S2** a major iOS feature missing/broken · **S3** noticeable defect · **S4** minor/cosmetic.
Nothing is marked "works" merely because a driver exists: evidence is stated. For the list of what *does* work see [devices/](devices/README.md).

## Summary

| ID | Area | Issue | Kind | Sev | OnePlus 8 | Redwood |
|---|---|---|---|---|---|---|
| K01 | Graphics | Metal is experimental; most Metal features and apps untested | Partial | S2 | off by default | not applicable |
| K02 | Security | No fingerprint / biometric unlock | Unsupported | S2 | yes | yes |
| K03 | Camera | No cameras | Unsupported | S2 | yes | works (no video) |
| K04 | Audio | No headphones/USB-C audio, earpiece, call audio; one mic only | Unsupported / Partial | S2 | yes | partly |
| K05 | Connectivity | No Bluetooth; Wi-Fi gaps and unverified sleep | Unsupported / Unverified | S2 | yes | yes |
| K06 | Telephony | No cellular, calls, SMS, voicemail | Unsupported | S1 | yes | yes |
| K07 | Sensors | No accelerometer/gyro/compass/light/proximity/barometer | Unsupported | S2 | yes | yes |
| K08 | Battery | Charger/shutdown paths unverified; no drain figure; no fast charging | Unverified / Unsupported | S1 | yes | partly |
| K09 | Display | Brightness optical effect unverified; rendering gaps; ~2% late swaps | Unverified / Bug | S3 | yes | |
| K10 | Power | Sleep is shallow; drain asleep unknown; heavy wake CPU | Partial | S2 | yes | no sleep |
| K11 | Services | App Store LOADING; no Apple ID, iCloud, push, FairPlay | Unsupported | S2 | yes | yes |
| K12 | Location | No location services | Unsupported | S2 | yes | yes |
| K13 | Security | Security stand-ins accept everything; keychain protects nothing | By design | S3 | yes | yes |
| K14 | Persistence | RAM-only runs lose all data; Wi-Fi password not kept | By design | S3 | RAM runs | RAM runs |
| K15 | UI | Poster switcher black; shadows under switcher cards; CC glyph strokes | Bug | S3 | yes | |
| K16 | Boot | Stock ABL breaks with USB cable in; short menu countdown; no recovery image in a fresh clone | Limitation | S2 | yes | |
| K17 | Media | No video/Live Photo recording; portrait mode skipped | Unsupported | S3 | yes | yes |
| K18 | Thermal | Throttling never observed | Unverified | S3 | yes | yes |
| K19 | Testing | Scripted gestures do not reach system gestures; switcher not openable by script | Limitation | S4 | yes | |
| K20 | Redwood | Not re-verified | Unverified | S3 | | yes |
| K21 | Installed build | Installed bundle lags the source | Limitation | S3 | yes | |
| K22 | Agent toolkit | New USB mailbox/control path awaits owner-approved RAM loading | Unverified | S3 | yes | |
| K23 | Scheduling | Edge: rare 10-11 ms render frame under heavy DF load; BG starved when UT saturates E; energy model unvalidated | Partial | S4 | yes | untested |
| K24 | Testing | `sleep` check can fail a correct sleep (continuous clock 0.4 s short once) | Bug (test) | S4 | yes | |

---

## K23 Scheduling (Edge) — OnePlus 8 · **Partial** · S4
- **Observed (2026-10-26, RAM runs, `sched_test`):** a 120 Hz render loop with 7 busy default-QoS threads misses about 1 frame in 1400 with a
  10-11 ms frame under Edge (Clutch: 1 in 1440, ≤ 8.7 ms), while Edge's p50/p99 are better; background-QoS threads get ~5% of the E cluster when
  utility threads saturate it (by design: BG does not go up to P, `sched_edge_bg_up=1` restores Apple's code); the controller's energy figures are
  a C·f·V² model, not measured power. Frames after 5 rapid sleeps drop to ~66 fps on **both** schedulers (pre-existing, see K10).
- **Workaround:** `sched=clutch` boots the previous scheduler. **Work:** T11. **Evidence:** [scheduler.md](subsystems/scheduler.md#measured-oneplus-8-ram-runs-same-kernel-schedclutch-vs-default-edge).

## K24 `sleep` check false failure — OnePlus 8 · **Bug (test)** · S4
- **Observed once (2026-10-26 04:4x, Edge):** `t-sleep: FAIL n=1 want=3 cont=2.60 ... the continuous clock did not run the time asked for`, the
  other sleep of the run passing; other runs gave 3.3-4.1 s. The continuous clock after a sleep is corrected from the PMIC RTC (1 s resolution),
  so a short reading of up to ~1 s is possible; the check allows -0.3 s. **Work:** widen the lower bound or correct with sub-second time. Not a scheduler effect.

## K01 Metal graphics — OnePlus 8 · Graphics · **Partial** (experimental) · S2
- **Expected:** apps and the system draw with Metal like an iPhone. **Actual:** OpenGL ES (Mesa) draws the whole UI and is solid. Metal is an experimental driver, off by default (`metal=1` test programs;
  `metal=2 metal_profile=1` hands it the whole system and reaches the home screen). See the exact breakdown in [gpu.md](subsystems/gpu.md#metal-work-in-progress-instantnoodle-only).
- **Works (measured):** blits, clears, 4x MSAA resolve, fences/events/shared events (in-process), heaps (no suballocation), 263 of 271 system fragment shaders draw (258 compared agree with softpipe), compute kernels; Apple Maps' pipelines build and its command buffers complete (no picture checked).
- **Broken / missing:** mesh and object shaders, tessellation, indirect command buffers, tile shading/imageblocks, function tables, indirect draw/dispatch, multiple viewports, vertex amplification, counters, texture views, wide lines; cross-process shared events; chronod widgets still have 2 pipelines refused under `metal=2`;
  135 AIR library modules are not entry points; no third-party Metal app has been tried; Weather, Photos and Measure never opened a Metal device. Siri's orb once aborted SpringBoard on Metal (48 times in the 2026-10-21 boot); current behaviour under `metal=2` not recorded.
- **Cause:** work in progress, not a defect of one component. **Workaround:** leave Metal off. **Evidence:** `docs/air.md`, `docs/air-report.md`, `metal_sync_test` 10/10. **Work:** T10. **Not completed functionality:** do not describe this as "Metal works".

## K02 Fingerprint / Touch ID-equivalent — all · Security · **Unsupported** · S2
No driver, `biometrickitd`/`pearld` denied, no Secure Enclave. Passcode is not supported either (the keybag models a never-locked device; K13). Workaround: none; the lock screen is swipe-to-unlock. Work: T09.

## K03 Cameras — OnePlus 8 **Unsupported**; redwood **Partial** · Camera · S2
- OnePlus 8: no camera driver; Camera.app has no capture device (flashlight works). Work: T07.
- Redwood (per README, unverified recently): front/rear preview, focus, HEIC photos; no video or Live Photo recording; processing is CPU-only (grey-world white balance, no noise reduction). Work: T25.

## K04 Audio — OnePlus 8 · **Partial**
- **Works:** speakers (heard), UI sounds/ringtones/alerts/notification sounds, volume keys, main microphone (clap test).
- **Unsupported:** USB-C analog headphones (no detection, no RX path), earpiece, call audio, Bluetooth audio, secondary microphones, noise suppression.
- **Caveats:** amplifiers default off in the kernel (`sound_amps=0`; the profile passes 1); speaker protection is a software guard, not the DSP algorithm; level is 12 dB under stock maximum (`sound_below=96`); the first sound after an unpark has not been listened to;
  the amp health polarity is only checked via MANALARM/MANSTATE. Work: T08, T24. Evidence: [audio.md](subsystems/audio.md).

## K05 Wi-Fi and Bluetooth — OnePlus 8
- **Bluetooth: Unsupported** (shares the chip; `bluetooth`/`BTServer` denied). T05.
- **Wi-Fi: Partial, verified in RAM runs only** (scan, join from Settings, DHCP, Safari; WPA2, WPA2/TKIP-mixed, WPA3 SAE).
  - Unsupported: 802.11r (iOS is told FT is absent and joins with PSK), TKIP pairwise, roaming, WPA3 other groups/password identifiers. T18.
  - **Unverified:** radio through system sleep and wake (T02), per-network private MAC address, WPA3 transition mode, firmware-crash recovery.
  - A RAM run starts with a new `/private/var`: networks are not remembered (`fw/network` joins one at start). Installed boot keeps `/private/var` but auto-join across reboots has not been checked.
- **Cause of the TKIP/-3903 failure** was found and fixed (wifi.md). Evidence level high for the listed joins.

## K06 Cellular, calls, SMS, voicemail — all · Telephony · **Unsupported** · S1 for a phone
No modem start, no baseband IOKit drivers; CommCenter (non-cellular variant) is denied after it crashes on missing `AppleIPAppender`/`AppleBasebandPCI`; `vmd` denied (spun a core). Phone/Messages apps cannot place or receive. Work: T06. Also NFC (`nfcd` denied): T34.

## K07 Sensors — all · **Unsupported** · S2
No screen rotation, motion, compass, proximity, light, barometer; no auto-brightness (`BLControl` waits 5 s for an ALS at window-server start and gives up). The sensors sit behind the audio DSP (SEE, QMI 400) and the path is not built. Work: T04.

## K08 Battery and charging — OnePlus 8 · **Unverified / Unsupported** · S1 (safety paths)
- **Works (seen):** level/voltage/current/charging reported to iOS; status bar percentage; charger type via diagnostics lines.
- **Unverified (built, never run on the phone):** raising the charger input limit on a wall charger (2000 mA) / charging port (1500 mA); **off-mode charging** (a phone switched off with the cable in should stay off and charge); **empty-battery orderly shutdown**; the loader's charger-limit save/restore.
  Until verified: on a computer's USB port (SDP, 500 mA) a busy phone drains while plugged in; the firmware's limit is what you get.
- **Unsupported:** OnePlus Warp fast charging, PD/PPS. No drain figure (asleep or awake). Work: T01, T13, T19. Evidence: [power.md](subsystems/power.md).

## K09 Display and brightness — OnePlus 8 · **Unverified / Bug (minor)**
- **Brightness:** the Settings/Control Center slider reaches the panel as DCS `0x51` commands (verified at command level); **that the light actually changes, and the byte order, have not been verified** (no light measurement, no DSI read). There is no ambient sensor (K07). T16.
- **Late swaps:** ~2% of swaps come a refresh late (one push in flight); the first gesture set after boot is ~7% late; the unlock-after-wake blur costs 14 ms of GPU per frame. T23.
- **Never send DCS 0x10** (panel static until reset): this is a developer hazard, not a user setting.
- 60 Hz fallbacks exist (`fb_hz=60`, `fb_push=0`) if 90 Hz misbehaves. Resolved: tearing, shadows "too large" (owner).

## K10 Sleep, wake and power — OnePlus 8 · **Partial** · S2
- **Works:** screen off/on, system sleep (6 of 7 cores off, clocks frozen), wake by power key, timer, cable, `sleep_max`; GPU block and audio path and touch chip powered down; iOS's own idle sleep with maintenance wakes.
- **Limits:** the SoC's lowest state is **not reached** (architecturally blocked: T12), so sleep draw is higher than an iPhone's and **unmeasured** (T13). Awake idle cores sit in WFI, not power-collapsed (T20).
- **After a wake** widget extensions and chronod burn ~6 s CPU in 14 s (T17). (The 40%-of-a-core `vmd` spin that made the phone slow after wake is fixed.)
- **Unverified:** Wi-Fi radio across sleep (T02); volume-key interrupts; slider wake behaviour. The thermal service ticks every 250 ms even asleep by design.

## K11 App Store, Apple ID, iCloud, push, FairPlay — all · **Unsupported (blocked)** · S2
App Store shows LOADING for ever (the bag request is built but never resolved; an AMS thread waits on `fairplayd.H2`, which exits 0 immediately every 10 s). No Apple ID sign-in, no `apsd` push courier (identity needs the Secure Enclave), `mobileactivationd` answers "Device is not activated".
Safari, Settings, Sileo/Procursus (apt, tweaks) work. `nsurlsessiond` must run or Sileo downloads sit "Queued". Work: T15.

## K12 Location — all · **Unsupported** · S2
No GNSS (modem not started); `locationd` denied; Maps has no position. Work: T14 (blocked by T06).

## K13 Security properties — all · **By design (stand-ins)** · S3
AMFI and Sandbox answer "allowed" to everything; entitlements come from the signature; no trust cache. `AppleKeyStore` models a device with no passcode and never locked: keychain items are "wrapped" with a reversible transformation that protects nothing, and the keychain lives in `/private/var`.
Do not use this device for secrets. Work: T28. Evidence: [kernel-standins.md](research/kernel-standins.md).

## K14 Persistence — RAM-only runs · **By design** · S3
`/private/var` is a memory file system remade empty each boot (capped at ~2 GB): home-screen layout, app data, Settings, networks and the Keychain do not survive; apps are registered anew and may appear on a different page unless `sb_delay` is set.
The installed boot keeps `/private/var` on flash (HFS+ on the OnePlus 8). Starting a persistent RAM run needs `var_store=1 ufs_write=1` and the owner's permission (it writes flash).

## K15 UI rendering gaps — OnePlus 8 · **Bug** · S3
- Long-press on the lock-screen wallpaper (poster switcher) would show black; it is disabled instead (`poster_switcher_allowed()`); cause unknown.
- Shadows under cards of the app switcher are missing (function `sb3` refused). The switcher's wallpaper wash uses a placeholder image.
- Control Center glyph strokes appear only after the fade settles (`cc_fade` workaround exists, off).
- Notification banner material has not been compared with a real iOS.
- The app switcher cannot be opened by script (a test limitation, K19). Work: T22. Evidence: research/ios-userland.md.

## K16 Boot and recovery limits — OnePlus 8 · **Limitation** · S2
- A reset with the **USB cable in** makes the stock ABL take the port for a wall charger: fastboot never appears on USB and the phone switches off. Unplug, enter the mode, replug. Menu entries wait up to 60 s for the cable to come out.
- The 5 s menu countdown is too short to pick Developer mode by hand.
- There is no Android; recovery is the stock bootloader + TWRP. **The stock `boot_b` backup is outside this repository** (`../ios-instantnoodle/backup/partitions/boot_b.img`, ignored) and `out/boot_b-restore.img` is ignored by git: a fresh clone cannot restore.
- A bare `./xl enter` hangs the phone (loads redwood's UEFI); `touchscreen=1` and `wifi_again=1`/`adsp_again=1` crash or reset it.
- Details: [research/boot-chain-and-recovery.md](research/boot-chain-and-recovery.md). Work: T27, T31.

## K17 Media — all · **Unsupported** · S3
No video or Live Photo recording; portrait-mode Core Image prewarm is skipped (it crashed mediaserverd); video stabilisation refused; HEIC decoding is software (VideoToolbox's `VCPHEVC.videocodec`), performance not measured. T25.

## K18 Thermal — all · **Unverified** · S3
27 TSENS sensors read (33-38 C idle on redwood); the governor has never been seen slowing cores. Emergency path ends the run with a panic past 115 C. T33.

## K19 Test-harness limitations — developers · S4
Scripted touches do not reach iOS system gestures (swipe-up home/switcher) except from the bottom edge path; `com.hackios.switcher` returns but changes nothing with three apps running. QEMU logs print every line twice. T32.

## K20 Redwood (POCO X5 Pro) — **Unverified recently** · S3
Documented capabilities come from the upstream README and earlier sessions. The owner asked not to run POCO checks. Lacks GPU page tables, DCVS, fence IRQ, sleep, interrupt touch. T30.

## K21 Installed build lags the source — OnePlus 8 · **Limitation** · S3
The installed bundle/loader date from 2026-10-25 14:20; GPU collapse, brightness, Wi-Fi, `ca_ambient`, vmd fix and later work are only in RAM runs. T03.

---

### How to add an entry
Verify against code and evidence, state Kind, Severity, device, expected vs actual, cause if established, evidence, workaround only if known, the TODO id. Fixed? Change the kind to *Fixed (date)*, keep the diagnosis, and update the summary table.

## K22 Agent toolkit activation — developers · Unverified · S3
The physical phone still runs loader 0.5 without the new `getvar:iphont` extension. New agent commands correctly return `old_loader`; deep/control features cannot be considered working there yet. The owner deferred a RAM-only reload on 2026-10-26. No flash operation is needed to validate the extension. See [TOOLKIT.md](TOOLKIT.md), TODO T36.
