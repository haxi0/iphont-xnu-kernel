# Research: iOS 16.5 userland behaviour, private interfaces and the hooks built on them

What was learned about how iOS's own daemons and frameworks behave on this kernel, and what was done about it.
Build investigated everywhere unless noted: **iOS 16.5 (20F66), iPhone10,3 (iPhone X), arm64 (not arm64e)**, from the
IPSW `iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw` / 16.5 files fetched by `ios165/fetch.sh`; shared-cache
addresses below are *unslid* and valid for that build only. Where a finding came from iOS 26 sources it says so.

**Evidence tags:** **[observed]** seen directly on the phone/QEMU (log, trace, measurement); **[inferred]** concluded from
disassembly/reading or from several observations; **[hypothesis]** not established. Source of each finding: code comment
in the named file unless a log is cited. Method summary at the end. No Apple code, disassembly dumps, keys or assets are
reproduced here: names, selectors, offsets and behaviour only.

Related: [../subsystems/userland.md](../subsystems/userland.md), [../subsystems/gpu.md](../subsystems/gpu.md),
[../subsystems/display.md](../subsystems/display.md), [kernel-standins.md](kernel-standins.md),
[../KNOWN_ISSUES.md](../KNOWN_ISSUES.md).

## 1. backboardd and CoreAnimation (QuartzCore)

### 1.1 Renderer selection
- CoreAnimation's render server (`CA::WindowServer::EAGLServer`) draws with Metal, else OpenGL ES, else
  `CA::WindowServer::Server::sw_renderer()` (software). Every frame it asks for its renderer and, while it has none, tries
  again to make a Metal and then an OpenGL ES context, which costs a lot and cannot succeed without a GPU: one try is
  enough (`my_renderer()` in `inject.c`). [observed]
- With `metal=1` `MTLCreateSystemDefaultDevice()` answers in every process, and the window server is the first to ask.
  The road has no fork: `EAGLServer`'s constructor (`0x1883d1544`) calls `renderer()` (`0x1882e2548`) with a direct branch
  (past the vtable, so a vtable hook never sees the first call), `renderer()` calls `CAMetalContextCreate()`
  (`0x18837b718`, a `dispatch_once` around `MTLCreateSystemDefaultDevice`), a non-nil device goes into
  `CA::OGL::new_metal_context()` (`0x18847efcc`), which loads `default.metallib` from the QuartzCore bundle
  (`com.apple.QuartzCore`; assertions `ogl-metal.mm:3305/3308`). The hook point chosen is `-[CAWindowServer _detectDisplays]`
  (`0x1884da264`), the last Objective-C method on the path (`my_detect_displays`). [inferred from disassembly + observed boots]
- The static `NSMutableArray` of Metal devices is at `0x1e1efe9a0` (`MTL_DEVICE_ARRAY`) in the cache. [inferred]

### 1.2 The ninety shader "functions"
CoreAnimation draws a layer's pixels with one of 90 numbered functions (a fragment shader each). The OpenGL ES renderer
keeps their source in a table at unslid `0x1d5b54790` (`CA_SHADER_TABLE`) indexed by function number; functions it has no
shader for are null entries. `function_supported` answers from the same table. Context vtable: `CA::OGL::GLESContext`
at `0x1d5b3b540`. `GLContext::function_uniform()` hands a function its numbers.
- **Missing functions that matter:** "cst" (0x59, last; drawn with `glUseProgram(0)`: the lock screen wallpaper and the clock
  seen through its digits), "vcm" 45 (vibrantColorMatrix), "ds8" 28 (8x downsample, first step of a backdrop blur), "sb3" 40
  (single-pass blur, shadows under switcher cards), "vrd" 38 / "vrb" 39 (variable blur, the strip under the status bar in
  Search), "homeAffordanceBase" (the home indicator). `eagl.c` supplies programs of its own for these (see 1.3). [observed]
- **Pitfall (cost an afternoon, `glyes=40`):** telling the renderer "yes" for a function without a source makes
  `GLContext::create_shader` hand NULL to `strdup`, and the window server dies in `strlen`, then restarts every half
  minute with SpringBoard after it. Handing a source needs an entry in the shader table (the way "sb3" is given the
  "ds4" shader without weights), via `CA::OGL::Context::compile_shader(const char *)` (table slot `0x060`; the shader text
  is assembled from `GLESContext::shader_preamble`, slot `0x400`, plus pieces). [observed crash; mechanism inferred]
- The window server's crash reports never arrive; `say_the_fault()` in `inject.c` prints the fault address and a
  frame-pointer walk, unslid, for `ipsw dyld a2s`.

### 1.3 What eagl.c had to emulate (all [observed] on the phone's GPU path)
- `EXT_shader_framebuffer_fetch`: when offered, CoreAnimation draws label shadows and colour-matrix-over-picture as one draw;
  without it it needs four times the submissions. (A Mesa/freedreno bug drew the first triangle of such a quad with pixels of
  another tile until a flush was added before the first fetching draw after a non-fetching one:
  `tools/mesa/freedreno-darwin.patch`, `fd6_draw.cc`.) `glfetch=0` turns it off.
- **Vibrancy ("vcm", function 45).** The lock screen has two: a gradient over the screen (opaque red at the top, clear at the
  bottom) under which the wallpaper is toned down for the clock, and the clock itself whose digits are a mask through which
  the wallpaper comes out lighter. Drawn as pictures they were a red veil. The filter's arithmetic is taken from the
  software renderer's `tex_vibrant_color_matrix`. **The App Library grey pod bug (fixed 2026-10-25):** a pod's background
  is a plain grey CALayer plus a `vibrantColorMatrix` filter whose alpha row reads the backdrop (about 0.44 over a mid
  wallpaper); `vibrant_fragment` and `group_over_fragment` computed `mix(behind, s, layer.a)`, i.e. wrote the tint *in place of*
  the wallpaper, correct only when `s.a == 1` (so Control Center looked right). Now `s*a + behind*(1 - s.a*a)`.
- **Vibrant layer inside a group** (Control Center fading in: opacity below 1 makes CA draw the layer into a group picture,
  then lay it at the layer's opacity): the group's picture is cleared to nothing, "colour matrix of nothing is nothing",
  so buttons and titles vanished until the fade ended. A vibrant draw into an empty group now keeps what it covers and the
  matrix with it (`glgroup=0` restores the old behaviour). `cc_fade=MS` (off) replaces CC's opacity springs by an eased fade
  as a check-only workaround.
- **"ds8" downsample:** sixteen samples of four pixels (the four places mirrored in both directions) make the 64 pixels that
  become one; drawn with one centre sample the later blur shimmered when moving. The corners' colour is the layer's
  opacity, which CA applies when laying the layer, so it must not be multiplied in (it made captured backdrops come out at
  0.45 for the translucent minus on a jiggling icon).
- **Variable blur ("vrd"/"vrb")** — the strip under the status bar in Search: the Metal renderer uses mip levels of one
  texture; the OpenGL ES renderer clears the texture and draws into it with itself as source, which leaves whatever the
  driver decides (black strip after one route to Search, right after another). `eagl.c` keeps a chain of own textures
  (each half the previous), reads a radius `r` at level `log2(r)` between two levels, holds mask coordinates to its ends
  (otherwise the last row read the first), and divides by opacity when `inputNormalizeEdges`. `vblur=0` is the old
  stand-in, `vblur=2` paints the mask.
- **Home indicator ("homeAffordanceBase")**: a table (one row of 256 pixels: brightness, opacity) on unit 2; arithmetic:
  table entry for the backdrop's brightness plus `inputAddWhite`, times `inputAmount`, over the backdrop, then laid over
  itself in overlay mode at `inputOverlayOpacity` — the same as the software renderer.
- **IOSurface with no pixel format** (memory CA made a surface around, mostly tintable signs): byte order is opacity, blue,
  green, red; read blue-first they lose their opacity and vanish.
- Texture byte order BGRA labelled `GL_RGBA`; shaders reading textures at coordinates in pixels; draws made with no program
  (`plain_draw_begin()`); the OpenGL ES library is reached through a table at the offsets of iOS's own OpenGLES
  (`src/eagl_slots.txt`).
- **Not working / open:** the "sb3" refusal (a shadow under switcher cards is not drawn), `glsb3=1` hands a shader but still
  reports refused x1; the poster switcher (below). **Shadows "too large": the owner declared resolved on 2026-10-26 (tied to
  the vibrancy fix); do not investigate further.**

### 1.4 Input into backboardd
See [../subsystems/input-peripherals.md](../subsystems/input-peripherals.md): the virtual HID service, the race with
the event server, edge marking, `TouchDetectionMode`.

### 1.5 CoreBrightness / backlight
- `-[BLControl waitForALSArrival]` blocks the window server's start for 5 s waiting for an ambient light sensor HID service.
  There is none and none will come. [observed]
- backboardd sets brightness through a registry service (`AppleARMBacklight`, registry name `backlight`) that did not exist;
  UserEventAgent logged "Error calling IOServiceAddInterestNotification for backlight service". The properties backboardd
  sets, found by logging every property and user-client open: `backlight-level` and `brightness` as 16.16 fractions (0x8000 =
  50%), `FadePeriod`, `DisplayBrightness`, `DisplayBrightnessSliderPosition`; nits fields are 0. At shutdown it sets level 0.
  [observed; see display.md for how it reaches the panel]

### 1.6 Frame rate
`ca_rates=1` lists CA's display classes' rate methods: `idealRefreshRate` was 60, `vblDelta` 16.7 ms. The ideal rate is set by
`ca_ideal_rate()` (boot-arg `ca_fps`). QuartzCore takes VSync notifications only on A7-A10 phones or with `CA_FORCE_TIMER`
in the environment (`CADeviceUseVBL()`); on an iPhone X it runs its own `CADisplayTimer`, so `ca_vbl` makes it use ours.
`IOMFBDisplayRefresh` in the framebuffer properties is what gives iOS the panel's period. UIKit/animation display
links ask for no more than the screen says unless given a range and a reason as ProMotion iPhones are
(`CAHighFPS` by PoomSmart lifts the same cap for apps); `inject_sb.c` gives every animation and display link
30..N with a reason. The Clock icon's links (`SBUIPreciseClockTimer`, `SBHClockApplicationIconImageView`) are the only thing
animating on an idle home screen and are held to `ca_ambient=30`. [observed numbers in display.md]

## 2. SpringBoard

All in `userland/src/inject_sb.c` (replacements located by class, selector and expected address; each checked).
- **Activation.** SpringBoard asks `mobileactivationd` (not running, and could not talk to Apple) and treats the device as
  bricked, showing first-boot "hello" setup instead of the lock screen; the phone is made "activated" and the setup
  assistant never offered. [observed]
- **Snapshots.** `+[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:]` chooses the file format of the picture
  taken of an app leaving the screen: 0 PNG, 1 and 2 ATX (ASTC texture), 3 JPEG. A wide-colour device with ASTC gets ATX,
  compressed on the CPU; that took >10 s per picture under QEMU, the app waits and the 10 s scene-update watchdog (no
  setting) kills it, so no app survived going to the background. JPEG takes a fraction of a second; pictures needing
  transparency fall back to PNG as on devices without ASTC. [observed]
- **Vibrancy with the software renderer.** `BSUIVibrancyEffectView` (BaseBoardUI) draws digits as a mask. The user default
  `BSUIVibrancyEffectViewDisabled` shows content plain; set for the run only (nothing written) where the software renderer
  draws (`plain_vibrancy()`). Why the software renderer shows the digits red was not found. [observed]
- **Long press.** `-[SBLockHardwareButton longPress:]` is the side button's long-press recognizer (Siri); it is a timer started
  when the main thread *sees* the press. Waking the display from inside handling of that press takes longer than a long
  press (0.1 s phone, 1 s QEMU), so every wake ended with Siri's sheet; a long press of a button that is no longer held
  (state published by backboardd as the Darwin notification state, 2 held / 1 released) is dropped. [observed]
- **Poster switcher** (long press on the lock screen wallpaper): PosterBoard (`CSPosterSwitcherViewController`, a `CALayerHost`
  of PosterBoard's window context over an opaque black layer) lays out its window correctly, but the window server draws
  nothing of that context with either renderer; black remains. Not caused by secure mode or authentication. **Why: unknown.**
  `poster_switcher_allowed()` reports the capability as absent so the long press does nothing. [observed; cause hypothesis-free]
- **Wallpaper in the app switcher.** `-[SBAppSwitcherPageView _createOrDestroyWallpaperOverlayView]` makes a wash at
  ~15% opacity (layer tree `SBWallpaperEffectView` 0.1497, `PBUIFakeBlurView`, `PBUIWallpaperView`,
  `PBUISnapshotReplicaView`, `UIImageView`). The wallpaper is drawn live by PosterBoard in another process; SpringBoard has
  no bitmap, nothing bakes one (after 3.5 min `/var/mobile/Library` has 60 directories, none PaperBoard/PosterBoard/Wallpaper),
  so the provider returned a one-colour placeholder 846 x 1827 (179, 77, 179) which CA compresses to ASTC and draws purple.
  The provider is replaced. [observed]
- **Alert slider as Do Not Disturb.** Through the DoNotDisturb service as an assertion of the HID client
  `com.apple.springboard.donotdisturb.hid` (named in SpringBoard's entitlements), not Control Center's; a Focus the user
  turned on by hand is untouched. The ringer pill is dressed (moon, "Do Not Disturb", "On", no volume). First call deferred
  20 s or SpringBoard SIGSEGVs. [observed]
- **Status bar.** On the iPhone X identity the left region holds only the clock; `sb_clock=N` centres it N points of 375
  further right to clear the OnePlus camera hole (UIKit lays it out so it animates normally).
- **Battery.** `UIDevice batteryLevel/batteryState` hooks and `_UIBatteryView` `chargePercent`/`setChargePercent:`; the
  battery view fill sat at about 1% when BatteryCenter / IOPowerSources were empty. Once `HKBattery` (an `IOPMPowerSource`)
  exists, iOS's own code does it. Monitoring is switched on one second after run-loop start. [observed]
- **Siri's orb.** `-[SpringBoard applicationDidFinishLaunching:]` builds the banner manager → `SBAssistantController` →
  Siri's presentation controller whose `-prewarmOrbViewShaders` makes a `SUICOrbView` whose `-_updateMetalTextureRasterSize:`
  asks Metal for something Metal refuses: `_MTLReportFailure`, abort; 48 times in the boot of 2026-10-21, every 8.3 s. Our
  Metal driver prints with `printf` and SpringBoard's stdout went nowhere, so `dup2(stderr, 1)` plus unbuffered stdout was added
  (otherwise the sentence naming the refusal is lost with the process). `siri_orb=0` skips the prewarm as a diagnostic
  only. Current behaviour under `metal=2` unrecorded. [observed historically]
- **Switcher from a script.** `SBMainSwitcherControllerCoordinator` "activate the main switcher not interactively, from this
  source" (0 = none) answers that it took the request and the screen does not change with three apps running. Untried:
  the transition with a window scene. [observed; open]
- **Layer dump** (`com.hackios.layers`): class/name, place, opacity, contents, filters, mask: the tool for "wrong on one
  renderer, right on another". PosterBoard has 0 UIWindows so the dump cannot see it.

## 3. mediaserverd
- Prewarms portrait-mode Core Image filters at start, which needs an OpenGL ES context; with no GPU it jumped to address 0
  every time launchd restarted it. Skipped. [observed]
- mediaserverd is where VideoToolbox decodes for everybody (`com.apple.coremedia.decompressionsession`); a HEIC is HEVC inside;
  without it ImageIO has "no way to decode a HEIC" and the iOS 16 stock wallpaper (four HEIC layers) is its gradient alone.
  Software decoder `VCPHEVC.videocodec`. [observed]
- Flashlight: `FigFlashlight` asks `+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]` for a device and the back stream's
  `TorchLevel` (0..1); with no camera there is no vendor (nil), every message does nothing and FigFlashlight claims success.
  A vendor of ours serves only FigFlashlight (by caller). [inferred, working on phone]
- Camera (redwood): our create function replaces the one H10ISP.mediacapture would give for the iPhone's ISP. Capture formats
  are named as D22's in `D22/AVCaptureSession.plist`. No ISP processing session, so video stabilisation is refused.

## 4. Daemons that were denied, and why
(`userland/mkjobs.py` is the record; counts are from logs of the time.)
- **CommCenter** (cellular): the `-noncellular` variant is chosen by launchd when the device tree has no `baseband` node and it
  answers `com.apple.commcenter.xpc`/`.coretelephony.xpc`, serves geod its policy, loads network-extension configurations — and
  then dies every time on one of two missing IOKit drivers: 11 of 20 crashes SIGSEGV right after "Could not find
  AppleIPAppender service" (user client `ApplePDPHelperInterface`: PDP contexts, flows, dedicated bearers) and 9 SIGABRT right
  after "Unable to communicate with AppleBaseband driver" (`com.apple.driver.AppleBasebandPCI`). Stubbing one would leave the
  other. It stays denied. [observed]
- **vmd** (voicemail): asked launchd for CommCenter's service in a loop with no delay — 1363 failed lookups in 70 s, 40% of a
  core for ever — which was the root cause of "laggy after wake / CPU 95% busy" (gesture cores 95% busy after wake against
  55-60% before). Found with `kern.hackios.top` per-process CPU. Denied: 40-55% after. Not the sleep. [observed]
- **cloudd** looked like a daemon that cost the home screen; it retried `nsurlsessiond` thousands of times *because that was
  denied*. Lesson in mkjobs.py: denials interact; retest.
- **apsd** (push): was denied from day one untried; about a third of all "failed lookup" lines were 22 daemons asking for it.
  Running, it serves clients but never reaches a push courier (identity rooted in the Secure Enclave).
- **mobileactivationd**: running gives `Code=-8 "Device is not activated: Unactivated"` instead of IPC errors; "Failed to bump
  UIK: 0xe00002c7" because activation wants a Secure Enclave. Does not unblock the store.
- **fairplayd.H2:** starts, exits 0 at once, launchd restarts it every 10 s (34 times in 6 min; 22 in a session). Watching it
  from inside (`inject_fairplay.c`, interposing `IOServiceMatching`, `IOServiceGetMatchingServices`, `IOServiceOpen`,
  `IOConnectCallMethod`, `exit`, plus a sampler and `systrace=2`): argv as Apple's plist; opens only the CoreFoundation /
  containermanagerd feature-flag files and its own bundle (all that exist succeed; the missing ones are missing on a stock iPhone
  too); **not one IOKit call, no xpc service lookup, no sysctlbyname (it calls `SYSCTL` by number), no dlopen**; every PC of
  its ~10 ms life lies inside its own binary (file offset `0xd9a40..0x8565ac`); `__mac_syscall` x12 (AMFI/sandbox) is the
  channel left. Conclusion: **a kernel stub of `com_apple_driver_FairPlayIOKit` would never be reached**; the daemon decides
  with inlined syscalls. Lessons recorded: an entitlement names what a program *may* do, never what it does; `mkjobs.py` drops
  `KeepAlive`, so a resident daemon that is not kept alive looks like one that quits. [observed; cause of the early exit unknown]
- **The App Store** shows LOADING for ever. Evidence ladder (`inject_store.c`, `rc.jobs.store`): the bag fetches fine by
  hand (56 KB in 0.5 s, storefront header `143469-2,8`); appstored opens no socket; AMS's "local" account is not a row in accountsd
  (new identifier each boot) so the storefront cannot be written from outside; with `-[ACAccount ams_storefront]` answering
  `143469-2,8`, AMS *does* build a bag request (`-[AMSBagNetworkTask _performFetchWithAttemptedCount:account:storefront:]` →
  `_createRequestWithQueryItems:` → an `AMSMutablePromise`) but the promise is never resolved: no task is ever resumed in the
  whole daemon in six minutes. An AMS thread sits in a Mach call in `_AMSFairPlayGetDeviceType` waiting for a reply from
  fairplayd and 17 promises queue behind it. [observed; the fairplayd link is **inferred**]
- Other denied hardware daemons (bluetooth, locationd, nfcd, biometrickitd, pearld, camera, thermalmonitord, keybagd, sepd…) have
  no hardware or no Secure Enclave.

## 5. IO80211 / Wi-Fi (details in [../subsystems/wifi.md](../subsystems/wifi.md))
- wifid → Apple80211 ioctls (`SIOCGA80211/SIOCSA80211`); `IO80211.framework` fills/reads the same structs
  (`Apple80211GetWithIOCTL`, `_getScanData`, `_addScanResultToList`, `_performAssociation`, `_dispatchAsyncAssocEvent`,
  `_dispatchLinkChangedEvent`). Kernel events class 6 subclass 1, data begins with the 16-byte interface name.
- `_performAssociation` fails with **-3903** before issuing ASSOCIATE when `CARD_CAPABILITIES` lacks TKIP (network offers it).
  Reading aid: the iOS 26.1 (23B85) `IO80211` decompilation in the public repository `EthanArbuckle/iPhone18-3_26.1_23B85_Restore`
  (`IO80211_02.mm _performAssociation`; capabilities struct at `v214+88`, SAE bit check `+97 & 8`). `ipsw` 3.1.672 hangs on the
  local 16.7 dyld cache. [observed -3903; layout from iOS 26 source: inferred for 16.5]
- iOS chooses key management (a transition network: WPA3 where the card says it can) and, per network, a private Wi-Fi
  address in ASSOCIATE at offset `0x1fc`.

## 6. AppleKeyStore / keychain / AMFI (see [kernel-standins.md](kernel-standins.md))

## 7. Boot logo
The Apple logo is **not** drawn by UEFI, the loader or XNU (Silicium's UEFI draws its own `BootLogo.bmp`). It is backboardd's
`BKDisplayRenderOverlayAppleLogo`, drawn by `PUIProgressWindow` from `ProgressUI.framework/apple-logo*.png` (14 CgBI
variants). The device reports DeviceColor unknown and ArtworkDeviceSubType none, so likely `apple-logo@3x~iphone.png` (194x237).
Replacement (2026-10-25): `userland/mkbootlogo.py` (Pillow) writes `userland/bootlogo/` with all 14 variants at original pixel
sizes (aspect kept; `SCALE=2` doubles the canvas); `build.sh` copies `ProgressUI.framework` + logos into the small image's
`/progressui`; `init.c flash_root` bind-mounts it over the volume's framework (no system-image rewrite for flash-root boots).
Installed in slot 0 as the 2x variant; the owner saw it. Whether ProgressUI draws at the image's own size (so 2x really is
bigger) was not confirmed. [observed that it shows; size claim unverified]

## Method
- **Hook by address with guard:** locate a method by class+selector and compare its address with the expected unslid one before
  replacing; log "refused" otherwise.
- **Frame-pointer walks + `ipsw dyld a2s`** to turn unslid addresses into names; the shared-cache slide is printed.
- **Observe, don't guess:** wrap methods to count entries *and* returns (a hang and a never-called method look the same);
  interpose libc/IOKit calls to see what a daemon asks; `systrace=2 systrace_name=NAME` logs every BSD syscall of one process.
- `OSLOG="..."` sends a daemon's `os_log` to the console; `kern.hackios.top` for per-process CPU; `hkprof` (userland sampler,
  `hkprof=1/2`) and `kprof` for profiles; `exctrace=1 applog=1 applog_name=APP` for crash diagnosis of Metal apps (match the
  `usertrap` backtrace against the unstripped `libhackiosair.dylib` with `llvm-symbolizer`).
- Tools: `ipsw` (blacktop), `llvm-symbolizer`, host Python readers in `tools/air`, `tools/metal`.

## iphont inspection interfaces (2026-10-26)

- **Observed in QEMU:** native `kernel_sysctlbyname`, `proc_iterate`/`TASK_ABSOLUTETIME_INFO`, task-locked thread enumeration with saved ARM64 registers, and `IORegistryIterator` returned real XNU state. The event ring captured real kernel execution, and input was rejected without control authorization. These are kernel API observations, not USB or phone delivery tests.
- **Source, compiled only:** the root daemon uses `bootstrap_look_up`/`mach_port_type`, `IORegistryEntryCreateCFProperties`, `task_for_pid`/`vm_read_overwrite`, `getifaddrs`, Mach CPU/VM statistics, direct `posix_spawn`, and Foundation file APIs. Existing private bindings inform `SBSLaunchApplicationWithIdentifier`, `LSApplicationWorkspace`, `IOHIDEventCreateKeyboardEvent`/`IOHIDEventSystemClientDispatchEvent`, and `UIPasteboard`; entitlement/routing/privacy behavior still needs device validation. IOHID dispatch is void and cannot prove delivery.
- **Source, observed capture:** `xnuloader/loader.c:screen_now` reads the display driver's published scanout address and row pitch when valid, falling back to the boot framebuffer. The existing bound permits an address in the display allocation and up to 256 bytes of row padding. `iphont screenshot` converts these real BGRX rows with the Python standard library; physical captures included a boot logo and padded pitch. This does not establish compositor or GPU acceleration health.
- **Source:** `hk_touch_inject` feeds the existing backboardd input queue with boost behavior while excluding remote fingers from the physical recovery chord. Ten finger slots match `inject.c`; actual multitouch/gesture delivery is still unverified.
- **Observed host tooling:** kernel symbols are accepted only for a matching Mach-O UUID. Shared-cache image/offset attribution is available; userspace function-name symbolication remains an `ipsw`/LLVM workflow, not a claimed toolkit capability.

Protocol and exact limitations: [../TOOLKIT.md](../TOOLKIT.md).
