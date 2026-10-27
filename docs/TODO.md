# TODO — what HackiOS still needs implemented or completed

The development roadmap. It lists **work**; what is *broken for a user right now* is in [KNOWN_ISSUES.md](KNOWN_ISSUES.md) (entries `K..`),
the session state is in [HANDOFF.md](HANDOFF.md). Entries here (`T..`) are cross-referenced, not duplicated. Evidence for every item is the
source tree and the subsystem pages; items built from source comments cite the file.

*Last audited: 2026-10-26 against the working tree (both git trees dirty, see HANDOFF).* Target device is the **OnePlus 8** unless an item
says otherwise; the owner asked not to run redwood checks.

**Status:** Not Started · Partial (some of it works) · In Progress (someone is actively changing it) · Blocked (cannot proceed without something named) ·
Built, Unverified (compiles/QEMU-tested, never seen on the phone) · Complete (listed only where an older document made it look unfinished).
**Priority:** **P0** safety, or without it the phone is not a usable device · **P1** a major missing iOS capability or a verification debt on shipped work ·
**P2** quality, efficiency, convenience · **P3** research or maintenance.
Order inside each section is by priority. Update an item when you change its state; when finished mark Complete and keep the diagnostic findings.

## Index

| ID | Item | Status | Pri |
|---|---|---|---|
| T01 | Verify the unverified power work on the phone | Built, Unverified | P0 |
| T02 | Wi-Fi radio through system sleep | Built, Unverified | P0 |
| T03 | Install the current build / keep installed bundle current | Partial | P1 |
| T04 | Motion, light and proximity sensors (screen rotation, auto-brightness) | Not Started | P1 |
| T05 | Bluetooth | In Progress | P1 |
| T06 | Cellular modem, telephony, SMS | Not Started | P1 |
| T07 | OnePlus 8 cameras | Not Started | P1 |
| T08 | Headphones / USB-C audio | Not Started | P1 |
| T09 | Fingerprint / biometric authentication | Not Started | P1 |
| T10 | Metal: from experimental to usable | In Progress | P1 |
| T11 | Heterogeneous CPU scheduling (Edge) | Partial (working, uncommitted) | P1 |
| T12 | Sleep power: SoC low-power states | Blocked | P1 |
| T13 | Battery drain measurement | Not Started | P1 |
| T14 | Location (GPS) | Not Started | P2 |
| T15 | App Store, Apple ID, FairPlay, push (Secure Enclave dependent) | Blocked | P2 |
| T16 | Brightness: prove the light changes | Partial | P2 |
| T17 | CPU time after wake (widgets, chronod) | Partial | P2 |
| T18 | Wi-Fi: 802.11r, TKIP pairwise, roaming, other SAE groups, persistence | Partial | P2 |
| T19 | Fast charging (Warp / PD) | Not Started | P2 |
| T20 | CPU idle power collapse | Not Started | P2 |
| T21 | GPU: IFPC and measured power | Blocked | P2 |
| T22 | CoreAnimation fidelity gaps (sb3, poster switcher, CC glyph opacity, switcher wallpaper) | Partial | P2 |
| T23 | Display: late swaps, cold-start jank | Partial | P2 |
| T24 | Speaker protection (TFADSP) and other microphones | Partial | P2 |
| T25 | Video and Live Photo recording; redwood-only camera parity | Not Started | P2 |
| T26 | Storage: writable APFS, fsck, HFS write speed | Partial | P2 |
| T27 | Boot menu usability (5 s countdown, graphics) | Partial | P2 |
| T28 | Security model: stand-ins protect nothing | Partial | P2 |
| T29 | Remove stand-ins and copies (`hk_wlfw.c`, duplicated DSP code) | Partial | P3 |
| T30 | Redwood re-verification and parity with OnePlus work | Not Started | P3 |
| T31 | Documentation and repository debts | Partial | P3 |
| T32 | Scripted gesture reach, test coverage | Partial | P3 |
| T33 | Thermal service never seen throttling | Built, Unverified | P3 |
| T34 | Sensors that are not sensors: NFC, UWB, Taptic edge cases | Not Started | P3 |
| T35 | Complete (do not reopen): items that older text made look open | Complete | — |
| T36 | iphont agent hardware validation and workflow migration | Partial / Built, Unverified | P1 |

---


## P0

### T01 Verify the unverified power work on the phone — Power · Built, Unverified · P0
- **Works:** sleep/wake with timer and power key, panel off/on, governor, touch reset, audio park, GPU collapse (verified; [power.md](subsystems/power.md)).
- **Missing evidence:** shared-interrupt fix in a long session; 3-strikes push fallback; volume-key interrupts and the key thread's dark wait; slider at 300 ms dark;
  USB reconnect fix (`abort_xfer`); charge-state numbering; charger input limit (2000 mA DCP / 1500 CDP) and its save/restore; **off-mode charging** (loader);
  menu entries "Recovery"/"Bootloader"/"Power off" and the cable-out wait; **empty-battery shutdown**; first sound after an unpark.
- **Why P0:** these paths write charger registers and decide whether a flat phone shuts down safely.
- **Files:** `bsd/dev/hk_power.c`, `hk_buttons.c`, `instantnoodle_battery.c`, `xnuloader/flashboot.c`, `loader.c`.
- **Next:** run one `tools/t power` level for the sleep parts; with the owner present: plug a wall charger and read `charger` diagnostics, try off-mode charging,
  `battery_empty_mv=N` above the cell voltage to test the shutdown. Record in power.md and HANDOFF.
- **Refs:** [power.md](subsystems/power.md) "Verification record"; K08.

### T02 Wi-Fi radio through system sleep — Wi-Fi/Power · Built, Unverified · P0
- **Works:** radio up, scan, join, DHCP, Safari; `hk_power_will_sleep()` takes the radio down (network left, firmware stopped, link off) and a thread brings it back.
- **Missing:** a real sleep with the radio on and a network joined, then a wake and rejoin; firmware crash recovery by reset.
- **Files:** `bsd/dev/hk_wifi.c` (wifi-power thread), `instantnoodle_wifi.c`, `hk_power.c`.
- **Blockers:** needs RAM run with `wifi=1` + `sleep_max`; `/private/var` RAM-only so the password must be in `fw/network`.
- **Next:** `tools/t`-style run: join, `power_test sleep 5 2`, check `en0` and Settings.
- **Refs:** [wifi.md](subsystems/wifi.md); K05.

## P1

### T03 Keep the installed bundle current — Boot/Installed · Partial · P1
- **State:** the installed bundle and `boot_b` loader were updated 2026-10-25 14:20 (power rounds 1-4). Later work (GPU collapse, brightness, `ca_ambient`, vmd DENY,
  Wi-Fi) is not installed. The latest userland images were built for tests, not the installed recipe.
- **Next:** `common/flash/install-xl.sh instantnoodle update` after T01 evidence; **requires the owner's explicit OK** (writes the flash). Rebuild with `FLASH_USERLAND_VARS`.
- **Refs:** [INSTALL.md](INSTALL.md), research/boot-chain-and-recovery.md.

### T04 Motion, light and proximity sensors — Sensors · Not Started · P1
- **Why:** no accelerometer means no screen rotation, no CoreMotion, no auto-brightness (and CoreBrightness's 5 s ALS wait stays).
- **Facts:** sensors hang off the audio DSP behind its sensor framework ("SEE") via QMI service 400 over QRTR ([thermal.md](subsystems/thermal.md) has the step table:
  DSP started / GLINK / QRTR / services listed = done (`adsp=1`, `hk_adsp.c`; `sysctl kern.hackios.adsp_services`); QRTR ports of our own on the DSP channel, the service locator and notifier
  the DSP's sensor domain waits for, the sensor registry the DSP reads from Android's partitions, a SEE client, delivery as HID events for CoreMotion = **to do**).
- **Files:** `bsd/dev/hk_adsp.c`, `hk_qmi.c`, `sys/hk_qrtr.h`; new driver; iOS side: a virtual HID accelerometer service in `inject.c` (pattern: the touch service).
- **Blockers:** sensor registry (Android partitions: `persist`/`vendor`) unavailable without Android; OnePlus 8 firmware needs checking.
- **Next:** run `adsp=1` and read `kern.hackios.adsp_services` for service 400 (the previous plan's first question; result not recorded).
- **Refs:** K07.

### T05 Bluetooth — Connectivity · In Progress · P1
- **Verified on phone:** QCA6390 UART, firmware/NVM and HCI; native CoreBluetooth service-filtered discovery before/after sleep;
  initial native BLE connection and 22-byte GATT write/readback. Later supervised `221539` verifies CMF Buds Pro 2
  classic pairing, authentication/encryption and AAC A2DP channel negotiation through native Settings. Playback unverified.
- **Fixed:** optional unsupported init step, Broadcom capabilities/multi-advertising, and mixed legacy/extended LE commands.
  HCI capability masks and replies remain truthful. Malformed NVM rejection is host-tested; latest kernel also brings up the controller on the phone.
- **Files:** `instantnoodle_bt.c`, `hk_bt.c`, `HKBluetooth.cpp`; userland `btboot.c`, `inject_bt.c`, `bt_scan.m`; `tools/bt/check.py`, `tools/t`.
- **Audio gap:** original BTAudioHALPlugin.driver was excluded/hidden by HKAudio-only overlay; selection/packaging fixed and built.
  Corrected image routes the quiet tone to A2DP and enters STREAMING (`002629`); heard output unconfirmed.
  Voice Memos selects HFP, encounters format error, and audio server restarts; exact fatal stack still needed.
  No-buds `003355` records two seconds through the built-in mic and opens Voice Memos without a captured exception.
- **Owner step:** supervised Settings pairing succeeded. Quiet test ran with approval; next required action is re-pairing/tapping Record under exception tracing.
  Earlier CLI post-wake MITM failure (4815/705) is distinct from the now-verified native classic pairing flow.
- **Remaining:** BLE pairing-session validation, general foreground BLE app discovery,
  reliable reconnect, audio, bond persistence, coexistence. Audio needs a headset and an approved audible test.
- **Refs:** [subsystems/bluetooth.md](subsystems/bluetooth.md), [research/bluetooth-ios.md](research/bluetooth-ios.md), K05.
  Opt-in `bt=1` / `BLUETOOTH=1`; nothing flashed. `213836` run: 10/11 (post-wake GATT failed); host parsers/guards PASS.

### T06 Cellular modem, telephony and SMS — Connectivity · Not Started · P1
- **Facts:** the modem (MSS) is not started (`qcom,mss@4080000`); CommCenter (`-noncellular` variant) answers but dies on missing `AppleIPAppender` (SIGSEGV) and
  `AppleBasebandPCI` (SIGABRT) — stubbing one leaves the other ([research/ios-userland.md](research/ios-userland.md) §4). `vmd` denied.
- **Next:** start the modem (PAS), QMI services, then IOKit drivers for the baseband path; scope is research-heavy.
- **Refs:** K06.

### T07 OnePlus 8 cameras — Camera · Not Started · P1
- **State:** redwood has `hk_camera.c` (four sensors, ISP in userland). The OnePlus 8 has no camera driver.
- **Next:** same method as redwood (`cam=1..5` ladder): sensors, CSIPHY/IFE, MCLK, regulators from the live device tree; `camera_device.c` formats are iPhone X names.
- **Files:** new `instantnoodle_camera.c` (or `board` table in `hk_camera.c`), `userland/src/camera_*.c`.
- **Refs:** K03; [input-peripherals.md](subsystems/input-peripherals.md).

### T08 Headphones / USB-C audio, earpiece — Audio · Not Started · P1
- **Facts:** USB-C analog headset: FSA4480 switch (on the speakers' I2C bus, deliberately not addressed), WCD938x RX path, MBHC detection. TFA9874 "receiver" profile exists for the earpiece.
- **Next:** MBHC/FSA4480 detection, RX path in `hk_mic.c`-style DSP bring-up; route change in `HKAudio.driver`.
- **Refs:** [audio.md](subsystems/audio.md); K04.

### T09 Fingerprint / biometric authentication — Security · Not Started · P1
- **Facts:** `biometrickitd` and `pearld` are denied; no driver; `MiniAppleKeyStore` models a device with no passcode. The phone has an under-display
  fingerprint sensor per its product specification; its interface has not been investigated in this repository. Real Touch ID semantics (Secure Enclave, `AppleBiometricSensor`)
  cannot be reproduced; a stand-in would have to satisfy `biometrickitd`/LocalAuthentication without real security.
- **Next:** identify the sensor and its bus from the live device tree; decide whether a *functional* (unlock only) emulation is wanted. Design discussion first.
- **Refs:** K02; research/kernel-standins.md (AppleKeyStore).

### T10 Metal: from experimental to usable — GPU/Graphics · In Progress · P1
- **Works:** see [gpu.md](subsystems/gpu.md) "Metal" (blits, MSAA resolve, fences/events/shared events in-process, heaps, 263/271 system fragment shaders draw, Apple Maps pipelines build).
- **Missing:** whole-system Metal is off by default (chronod widgets: 2 pipelines refused); no third-party Metal app tried; no picture compared for Maps; not implemented:
  mesh/object shaders, tessellation, ICBs, tile shading/imageblocks, function tables, indirect draw/dispatch, multiple viewports, depth-clip clamp, vertex amplification, counters,
  texture views, wide lines, per-binding LOD clamps, wrapping caller memory, heap suballocation, cross-process shared events. 135 `air.visible` modules are library functions, not
  entry points. Siri orb abort history.
- **Files:** `userland/src/hkmetal.m`, `hkdrawable.m`, `tools/mesa/air/*`, `docs/air.md`.
- **Next:** make `metal=2` the default only after: chronod pipelines; a Metal app test (e.g. a triangle app exists: `triangle_app.m`); compare Maps visually. Record numbers in air-report.
- **Refs:** K01.

### T11 Heterogeneous CPU scheduling — CPU · Partial (working, uncommitted) · P1
- **Done (2026-10-26, verified on the phone in RAM runs and in QEMU):** Edge scheduler on three clusters (prime, gold, silver), boot-time choice
  `sched=edge|clutch`, performance controller `bsd/dev/hk_perfctl.c` (group placement, deadline/interactive policy, migration graph, clock
  floors, energy model), BG-stays-on-E guard, cross-core priority inheritance, `sched_check`, `kern.hackios.amp`, `sched_test`, `tools/sched-test/`.
  Details and numbers: [scheduler.md](subsystems/scheduler.md). Exported to `xnu/`; **not committed**.
- **Open:** trace the rare 10-11 ms render frame under 7 busy DF threads; measure app launch time (Clutch vs Edge); try the render-server
  inheritance path (never raised by iOS here); redwood's `board_cpu_dpc*` values and a redwood run (only when the owner asks);
  thermal interplay; validate the energy model against a real power measurement (needs battery-only runs, T13).
- **Refs:** [scheduler.md](subsystems/scheduler.md), [cpu.md](subsystems/cpu.md).

### T12 Sleep power: SoC low-power states — Power · Blocked · P1
- **Blocker (architectural):** UEFI never exits; loader core 0 and resident UEFI drivers (storage, USB) hold votes; vdd-min needs the boot core and core 0 power-collapsed,
  RPMh sleep/wake sets. Boot core power-collapse additionally needs a PSCI resume path.
- **Next options:** (a) give storage/USB to the kernel (large); (b) accept WFI-based sleep; decide with measured drain (T13).
- **Refs:** [power.md](subsystems/power.md).

### T13 Battery drain measurement — Power · Not Started · P1
- **Facts:** no number exists for drain asleep or idle awake; `bq27541` current reads exist; runs 6's log covered only the plugged stretch; the owner said stop drain testing on 2026-10-25.
- **Next:** with the owner's consent: a timed sleep with the cable out, read gauge before/after through `power_test status`/`battery_test`. Needed to judge T12, T20, T21.

## P2

### T14 Location (GPS) — Connectivity · Not Started · P2
GNSS belongs to the modem (T06); location via its QMI service 16. `locationd` denied. **Blocked by T06.** Refs: K12.

### T15 App Store, Apple ID, FairPlay, push — Services · Blocked · P2
- **Facts:** App Store LOADING for ever: AMS builds a bag request but the promise never resolves; an AMS thread waits on `fairplayd.H2`, which exits 0 immediately every 10 s
  without touching IOKit (decision made with inlined syscalls). `apsd` runs but has no push courier; `mobileactivationd`: "Device is not activated". No Secure Enclave.
- **Next:** find what `fairplayd.H2` decides on (Ghidra path in `inject_fairplay.c`; kernel `systrace` extension to print sysctl MIB / mac-syscall policy). Probably unachievable without an SEP stand-in.
- **Refs:** research/ios-userland.md §4; K11.

### T16 Brightness: prove the light changes — Display · Partial · P2
- **Works:** the path from `BKSDisplayBrightnessSet` to DCS `51 hi lo` (20/90/5/100% → 0x52/0x67a/0x6/0x7ff), re-sent after wake, default restored on loader return.
- **Missing:** optical confirmation, byte order (no DSI read), auto-brightness (T04).
- **Next:** owner watches the screen during `tools/t brightness`; or implement a DSI read of the panel's brightness register. Record.
- **Refs:** [display.md](subsystems/display.md); K09.

### T17 CPU after wake: widgets and chronod — Performance · Partial · P2
After each wake Weather/Calendar/Stocks widget extensions and chronod burn ~6 s CPU in 14 s (SwiftUI timeline entries). `vmd` (40% of a core) is fixed. Next: profile (`hkprof=2`, `tools/t wakeprof`), consider
denying chronod's widget extensions at wake or throttling. Refs: K10.

### T18 Wi-Fi gaps — Wi-Fi · Partial · P2
Not done: 802.11r (iOS is told FT is absent), TKIP pairwise, roaming, WPA3 other groups and password identifiers, Bluetooth coexistence. Built, not seen on the phone: private Wi-Fi address,
AKM 6, RSNE/RSNXE downgrade check, WPA3 transition mode. Auto-join across reboots needs a persistent `/private/var` run (`var_store=1 ufs_write=1`, or installed boot) — the
Keychain itself works. Files: `wifi/lx/src/lx_wpa.c`, `lx_sae.c`, `lx_80211.c`, `hk_sta.c`. Refs: [wifi.md](subsystems/wifi.md); K05.

### T37 Driver audit follow-ups (2026-10-27) — Drivers · Partial · P2
From the audit against OnePlus's sm8250 kernel and mainline (HANDOFF "Driver audit fixes"). Open:
- `i2c_geni=2` (I2C serial engines, FIFO mode; `instantnoodle_i2c.c`) verified at boot on touch/gauge/slider; **next:** a manual run with a finger
  on the screen (`i2c_geni=2 touch_lat=1`), then make it the default; try the amplifiers' bus once with `adsp=1 sound=4 sound_amps=0`.
- Not yet seen on the phone: touch error paths (ESD, queue full, own restart), the audio DSP's RPMh claim (`adsp=1`), Wi-Fi PTK renewal (needs an
  AP with `wpa_ptk_rekey`), thermal throttling with the SM8250 map (T33), whether the haptic trim feels better.
- Merging the duplicated command-database parsers / bit-banged I2C / DSP stack: T29. Kernel log loss after a run: K25 (loader).

### T19 Fast charging (Warp / USB PD/PPS) — Power · Not Started · P2
OnePlus's fast-charge MCU (I2C `0x26`) protocol and firmware unknown; max here is the normal charger's 2000 mA (when T01 verifies the ICL). Redwood reads the charger but does not drive PD/PPS. Refs: K08.

### T20 CPU idle power collapse — Power · Not Started · P2
Awake idle is WFI (clock gated, power on). `CPU_SUSPEND` power-down (`0x40000004`) needs a resume path that would run thousands of times a second; QEMU cannot test it; risk = random hang. Do only with a measured benefit (T13).

### T21 GPU IFPC and measured GPU power — GPU/Power · Blocked · P2
IFPC needs the GMU power config and an OOB handshake around every host register access; mainline does not do it for this GPU either. hwcg is on but the power saved is unmeasured (blocked by T13).

### T22 CoreAnimation fidelity gaps — Graphics · Partial · P2
- `sb3` (function 40, single-pass blur: shadows under cards of the app switcher) is refused by the OpenGL ES renderer; `glsb3=1` hands a shader but still "refused x1".
- Poster switcher (long press on lock wallpaper) draws black; `poster_switcher_allowed()` hides it; cause unknown.
- Control Center glyph strokes at opacity < 1 come in only at the end (`cc_fade` is a check-only workaround, off).
- Switcher wallpaper wash uses a placeholder image.
- Notification banner material (`MTMaterialLayer`: `luminanceCurveMap`, `colorSaturate 2.4`, `gaussianBlur 29.5`) not compared with a real iOS.
- Mesa shader cache disabled at build.
Files: `userland/src/eagl.c`, `inject.c`, `inject_sb.c`. Refs: research/ios-userland.md §1-2; K09.

### T23 Display late swaps and cold-start jank — Display · Partial · P2
~2% of swaps a refresh late (one push in flight); first gesture set after boot ~7% late (cold); unlock-after-wake blur transition 14 ms GPU/frame; idle home screen still ~30 fps drawn. Files: `MiniIOMobileFramebuffer.cpp`, `eagl.c`. Refs: display.md.

### T24 Speaker protection (TFADSP) and other microphones — Audio · Partial · P2
TFA9874 protection runs in Android's audio DSP (AFE module `0x1000B911` on `TERT_MI2S_RX`, VI on `TERT_MI2S_TX` `0x1000B912` topology `0x1000B909`, container messages via
`AFE_PARAM_ID_TFADSP_RX_CFG`); here a software guard stands in. Only the main mic is set up. Amp health polarity only via MANALARM/MANSTATE. Refs: audio.md; K04.

### T25 Video / Live Photo recording; camera parity — Camera · Not Started · P2
README lists video and Live Photo recording as not working (redwood). No encoder path is built (software HEVC decode only is known: `VCPHEVC.videocodec`). Depends on T07 for OnePlus.

### T26 Storage — Storage · Partial · P2
Transactions for a writable APFS (the in-place write and the slot store exist; `tools/apfs/txn-probe.py` measures why transactions are hard); no `fsck_hfs`; HFS write speed limited by the loader's read-back;
`ufs_native=1` driver is read-only and used on redwood only; Block I/O 2 gives no asynchrony. Files: `apfs_write.c`, `hk_ufs.c`, `hk_ufshc.c`. Refs: [storage.md](subsystems/storage.md).

### T27 Boot menu usability — Boot · Partial · P2
5 s countdown too short to choose Developer mode by hand; text console only ("the loader's own graphics are a later step"). Files: `xnuloader/flashboot.c`. Refs: INSTALL.md.

### T28 Security model — Security · Partial · P2
AMFI and Sandbox stand-ins accept everything; `MiniAppleKeyStore` models a never-locked no-passcode device and protects nothing; no Secure Enclave; no passcode support; kernel runs whatever
it is given. Documented honestly as stand-ins ([kernel-standins.md](research/kernel-standins.md)). Not a roadmap item to "finish" unless a security goal is declared.

## P3

### T29 Remove stand-ins and copies — Maintenance · Partial · P3
`hk_wlfw.c` ("A STAND-IN, TO BE THROWN AWAY when ath11k's own qmi.c runs here" — it does now with `wifi_lx`); `hk_adsp.c` is a copy of `hk_wifi.c`'s approach ("a copy, not a shared library, for now");
`hk_haptic_pretend()` test stand-in; the RPMh command-database parser is copied in 8 files (`hk_adsp`, `hk_cpufreq`, `hk_speaker`, `hk_adreno`, `instantnoodle_touch`, `hk_camera`, `hk_goodix`, `hk_wifi`) and bit-banged I2C in 4 (`instantnoodle_i2c`, `hk_haptic`, `hk_speaker`, `hk_camera`): merge into one each (the TCS 0 race they caused is fixed by `hk_rpmh_claim()`, 2026-10-27); `hk_sparse.c`/`apfs_check` bring-up aids; `hk_hfs` fetched at build time.

### T30 Redwood re-verification and parity — Redwood · Not Started · P3
The owner said not to run POCO checks. Redwood lacks `gpu_pt`, DCVS, fence IRQ, system sleep, interrupt-driven touch; dual boot is verified only in QEMU. Needs an explicit request.

### T31 Documentation and repository debts — Docs · Partial · P3
`docs/metal.md` is cited 7x by `air.md` and exists nowhere (write it from the source comments or fix the cites); 43 `kern.hackios.*` sysctls undocumented individually; 49 boot-args only described by
file headers; no CI (removed); `uefi` rebuild-from-scratch unverified; `userland/build.sh:8` and `init.c`/`var_store.c` still say `/redwood`; kernel log lines still prefixed `redwood:` in userland;
`ios-instantnoodle/` copy-pasted READMEs (sweet/guacamole); two-repo index overlap (see architecture.md). Also: `kern.hackios.*` and `rc.jobs.*` indexes.

### T32 Scripted gesture reach and test coverage — Testing · Partial · P3
Scripted touches do not reach iOS system gestures (swipe-up for home/switcher) except from the real bottom edge path; `com.hackios.switcher` returns but changes nothing with 3 apps. `tools/t` covers boot/pods/frames/sleep/powerd/darkwake/brightness/idle;
no automated test for audio, Wi-Fi join, GPU hang recovery (manual `rc.jobs.*`).

### T33 Thermal service never seen throttling — Thermal · Built, Unverified · P3
27 sensors answered at 33-38 C on redwood; the governor has not slowed cores on either phone (needs >95 C load). Next: a controlled heat test with the owner. Refs: thermal.md.

### T34 NFC, UWB, other hardware — Hardware · Not Started · P3
`nfcd` denied; Wallet UI exists without NFC. Nothing investigated.

### T36 iphont agent hardware validation and workflow migration — Tooling · Partial / Built, Unverified · P1
The Python CLI, native loader/kernel mailbox, event logging, userspace bridge, diagnostics, input scheduling and capture interface are implemented and compile. Host/C tests and real XNU QEMU queries pass; physical state/log/framebuffer capture works. The owner explicitly deferred the required RAM loader reload, so USB RPC, daemon/framework/entitlement behavior, touch/keyboard delivery, and full deployment/recovery parity remain unverified. Normal boot profiles are unchanged. Next: use the supervised RAM sequence in [TOOLKIT.md](TOOLKIT.md), then validate one operation at a time. Still unfinished: GPU queue-level introspection, delivered keyboard guarantees, orientation/sensor support, KDP/LLDB integration, high-rate compressed mirroring, userspace function symbols, service-specific regressions and complete legacy replacement. Keep all unique recovery/test/build scripts; [TOOLKIT-MIGRATION.md](TOOLKIT-MIGRATION.md) records the static inventory and deletion gates.

## Complete — do not reopen (older documents made these look unfinished)
- Speakers on the OnePlus 8 (heard by the owner 2026-10-24); the alert slider; the main microphone (owner's claps); App Library grey pods (fixed 2026-10-25); iOS shadows "too large" (owner: resolved);
  `ca_ambient` idle cost; `vmd` spin; shared-interrupt-after-wake bug; GPU block collapse in sleep (verified 2026-10-26); per-program GPU page tables; hang recovery; cold `gpu=1`; installed boot of the OnePlus 8 (verified 2026-10-24);
  Wi-Fi join incl. WPA3 and TKIP-mixed on the OnePlus 8 (verified in RAM runs); HFS+ `/private/var` (verified); Metal fences/events/heaps (metal_sync_test 10/10).
- The README's old "alert slider: no driver" and "microphones: not yet" were stale; "Wi-Fi: a network cannot be joined from Settings yet" in `userland/build.sh`'s WIFID comment is stale too.

## Source-comment markers that are real work (inventory)
`hk_wlfw.c` stand-in; `hk_adsp.c` copy; `apfs_write.c` ("no caller in the kernel yet"); `instantnoodle_slider.c` interrupts not used (polled); `instantnoodle_speaker.c`/`hk_speaker.c` "what is not done compared with Android"
(DSP algorithm, boost current limit by battery); `mc_aes.c` no XTS/GCM; `MiniIOSurface` child surfaces/wrapping/purgeability/compression/notifications; `hkmetal.m` ~40 features; `GenericARMPE.cpp` no RTC driver (time from the loader / `xl settime`). The code has essentially no `TODO`/`FIXME` markers: unfinished work is written as prose in headers.
