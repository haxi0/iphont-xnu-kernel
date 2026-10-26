# GPU: Adreno, OpenGL ES and the Metal driver

The phone's Adreno draws CoreAnimation's frames through OpenGL ES (Mesa/freedreno) and, as research in progress,
Metal. This page separates what is **finished and verified** (OpenGL ES for the window server) from what is
**work in progress** (Metal). The Metal-to-NIR shader compiler research lives in the userland repository's
`docs/air.md` and `docs/air-report.md`; this page only summarizes what it means for the user.

| Device | GPU | Kernel file | State |
|---|---|---|---|
| redwood | Adreno 642L (A660 family), firmware `a660_*` | `xnu/files/bsd/dev/hk_adreno.c` | OpenGL ES works with `gpu=1`; no per-program page tables, no fence interrupt, no DCVS |
| instantnoodle | Adreno 650 (`kona` variant, chip id `0x06050002`), firmware `a650_*` | same file, `kona` branches | OpenGL ES works; DCVS, hang recovery, per-program page tables, fence IRQ, clock gating, sleep collapse; Metal in progress |

Vendor firmware (`a6xx_gmu`, `sqe`, zap shader) is never in the repository; it is taken from the phone's own
vendor partition (`fw-gpu/`, see [../BUILD.md](../BUILD.md)).

## Kernel side

### Bring-up (`hk_adreno.c`)

`gpu=1` starts the GPU while the system starts and keeps it for the window server (the `HKGPU` service). The
file's steps are how the hardware was found out and they stay for diagnosis; `adreno=N` runs steps 1 to N and
stops. Reads come before writes, and every step says what it is about to touch.

| Step | What |
|---|---|
| 1-3 | reads: GPU clocks in GCC, RPMh supply names, fuses (which GPU/speed bin), GPU clock controller |
| 4-5 | writes: the CX power switch (the part that is not the 3D core: GMU, bus interface, GPU SMMU) and its clocks, checked with the SoC frequency counter |
| 6-9 | reads: GMU registers, GMU's RPMh channel, the GPU's SMMU and its first context |
| 10 | writes two words into GMU memory and reads them back |
| 11-12 | asks TrustZone whether it takes calls from this kernel; has it check the signature of the **zap shader** (header goes first, see below) |
| 13-16 | page tables and an SMMU context for the GMU; GMU firmware loaded and started; connection to RPMh; HFI queues |
| 17 | talks to the GMU: asks for the 3D core's power |
| 18-21 | the 3D core's translation context with ring and microcode (`a6xx_sqe`), zap shader loaded, command processor started, core commanded out of secure mode (skipping the A660-only `CP_CHICKEN_DBG`/`QOS` writes on `kona`) |
| 22 | a picture filled with three colours, shown on the panel |

Rules learned the hard way (full account in [../research/hardware-qualcomm.md](../research/hardware-qualcomm.md)):

- An access the hypervisor does not like ends the run, at once or a quarter of a minute later, often silently.
- **Cold `gpu=1` used to send the phone to CrashDump mode.** TrustZone crashes when asked to give up the zap
  shader (image 13, `PAS_SHUTDOWN`) before it has been handed that image's header in the same boot, and step 19
  asked exactly that first. The header now goes first (`step_zap_load`; commit `3e84e9c`). A cold cycle is cheap:
  `xl reboot`, stock fastboot, `xl enter`.
- A failed GPU run can leave CX stuck on: recover with `fastboot reboot bootloader` (no flashing), then re-enter.
- Steps are re-entrant now only because `block_new()` frees old memory, `gmu_block_count` is reset and the 27 ms clock
  measurements are skipped on resume (found while building sleep collapse).

### The service `HKGPU` (`xnu/files/iokit/Kernel/HKGPU.cpp`)

A program that opens `HKGPU` speaks the interface of Linux's **msm DRM driver**, the one Mesa's freedreno knows:
every request is a user-client call with the request number as selector (user side: `tools/mesa/hackios-drm`,
standing in for libdrm inside Mesa). Implemented: parameters, buffers (create, get iova, map, free), command
submission, wait for fence, plus one request that is not Linux's: a buffer that is an IOSurface's memory
(`surfaceBuffer()`, `kHKSurface` 0x7e). Buffers are wired, zero-filled, write-combined kernel memory entered in the
3D core's SMMU context (`gpu_cached=1` makes them cached). Not implemented: fences and buffers as file
descriptors; interrupts on redwood (waiting there is asking again five times a millisecond).

### OnePlus 8 features

- **Per-program page tables (`gpu_pt`, default on `kona`).** `CP_SMMU_TABLE_UPDATE` switches context bank 0's TTBR0
  per HKGPU client. The first 1 GB is shared (kernel ring/fences, privileged); program virtual addresses start at
  `0x40000000` out of one global list. The a650 `CP_PROTECT` list keeps a program's commands away from registers
  the kernel owns; the SCM aperture (MP 0x1b) is accepted by this TrustZone. Verified on the phone
  (`userland/src/gpu_iso_test.c`, `RC_JOBS=rc.jobs.gpuiso`, passed three times including a protect violation
  followed by recovery; the system shows 0 SMMU faults; 90 Hz cost unchanged at 490 us per submission). A request
  the SMMU refuses is counted against the program (`MSM_PARAM_FAULTS`). Not done: `CPU_PREP` waits for all GPU work
  rather than per buffer; no per-client memory cap; redwood has `gpu_pt` off (no a660 protect list).
- **Hang recovery (`gpu_recover=1`, default on `kona`).** A GPU that stops answering is reset (3D core half) and
  restarted so only the submission that hung is lost. Verified: 3 hangs of 3 recovered (`rc.jobs.metalrecover`).
  A CrashDump is not recoverable.
- **DCVS (`gpu_dcvs=1`).** Every 50 ms the core's busy time decides: above 50% straight to 525 MHz; one level down
  (400, 305) after four periods under 25%; only the three levels the GMU was given at start, at the supply levels
  the phone's device tree names. A change is one HFI message (120-143 us). From `gpu_hot` (85.0 C default; the
  phone's own trip is 95 C) it is held at 305 MHz, by the GPU's own sensors (`board_thermal_gpuss_0/1`; SM8250:
  tsens0 15 and tsens1 8). The GPU spent 75-83% of the time at 305 MHz in the gesture scripts, sensors read 4-5 C
  less, frame rates and late frames unchanged. `gpu_dcvs=0` holds 525 MHz.
- **Touch boost (`gpu_boost=MS`, default 300, 0 off).** A touch puts the GPU at its top level for 300 ms. One run each:
  late frames 5.8% -> 5.0%, p95 17.9 -> 16.7 ms (the same build twice differs by about 0.5 points). Late frames
  are mostly waiting for SpringBoard's commit, not the GPU.
- **Hardware clock gating (`gpu_hwcg=0` turns it off).** Linux's `a650_hwcg[]` (49 registers; `tools/mk-hwcg.py`
  makes `bsd/dev/hk_a650_hwcg.h` from it and Mesa's register names), applied in hw_init as `a6xx_set_hwcg`. Verified
  the GPU draws and frames are the same. **The power saved has not been measured** (no way to read the drain with
  the cable in).
- **Fence interrupt (`gpu_irq=1`, GIC 332).** `RBBM_INT_0` bit 20 from `CACHE_FLUSH_TS`; waits sleep instead of poll.
- **Block collapse in system sleep (`gpu_sleep=1`, default).** `hk_gpu_suspend()`/`hk_gpu_resume()` in
  `hk_adreno.c`, `hk_gpu_sleep_begin()/end()` in `HKGPU.cpp`, called from `hk_power_will_sleep()/did_wake()`.
  Before: CX and GX stayed on in every sleep. Now `tools/t power` shows CX **off** and GX **off** in every sleep
  (10 sleeps in one run) and the wake takes **133 ms** (it was 615). The block is given back as at the end of a
  run (`gpu_block_stop()`: GMU slumber, zap shader and CX half given up; the vendor's `a6xx_gmu_suspend`) and
  steps 1..21 run again on wake (`a6xx_gmu_resume`). The programs' buffers, tables and fences are memory of ours and
  stay; the fence word is put back to the last submission's number. `HKGPU.cpp` holds `gGPULock` from the one
  call to the other so nothing submits and no register of a collapsed block is touched (that would hang the phone).
  A submission not done at the sleep leaves the GPU on for that sleep. `gpu_cycle=N` tries N stop/start cycles at boot.
  **Not done: IFPC** (the GMU collapsing GX by itself between frames while awake), which needs the GMU's power config
  and an OOB handshake around every host register access; the mainline driver does not do it for this GPU either.
- Facts: `glFlush` is 4.6 ms median (submission plus waiting); GMEM versus sysmem rendering changes nothing
  (`FD_MESA_DEBUG=sysmem`: 5.6% against 6.0% late); shader compile and texture upload are in boot frame 1 only.
  The Mesa shader cache is disabled at build (`-Dshader-cache=disabled`).

## User side: OpenGL ES for CoreAnimation (`userland/src/eagl.c`, userland repo)

`libhackios-gl.dylib` puts an OpenGL ES implementation (Mesa 25.0.7 with freedreno on Darwin, `tools/mk-mesa.sh`,
patches in `tools/mesa/`) behind iOS's `EAGLContext` inside backboardd, so the window server's renderer is the
OpenGL ES one. It is loaded only when the kernel started the GPU (`gpu=1`, service `HKGPU` present); otherwise
CoreAnimation uses its own software renderer. Build with `GL=1`. Boot-args: `gl=0` never, `gl=1` also without a GPU
(Mesa software, for working on `eagl.c` in QEMU).

CoreAnimation asks six things of an `EAGLContext` (`-initWithAPI:properties:`, `-API`, `-setParameter:to:`,
`-getMacroContextPrivate` which is a table of functions called at offsets of iOS's own OpenGLES library listed in
`src/eagl_slots.txt` and generated by `mkeagl.py`, `-texImageIOSurface:...`, `-attachImage:toCoreSurface:...`). IOSurfaces
become GPU textures without copying where the driver can share the memory (`hk_gl_surface_texture()`).

Shaders: CoreAnimation draws a layer with one of ninety "functions" (a fragment shader each). Its OpenGL ES renderer
lacks some; `eagl.c` supplies stand-ins (e.g. vibrancy, group-over compositing, a four-sample blur `sb3`) and some
draws are made with no program at all. Findings and the pitfalls are in
[../research/ios-userland.md](../research/ios-userland.md). Diagnostic boot-args (`gltrace*`, `glcensus`, `glshaders`,
`glshow*`, `glskip`, `glplain`, `glwait`, `glcopy`, `glfetch`, `glyes`) are listed in [../BOOT_ARGS.md](../BOOT_ARGS.md).

Verified on the phone: lock screen, home screen, Control Center, Settings, App Library with translucent pods
(grey-pod bug fixed 2026-10-25: the vibrant shader used `mix(behind, s, a)` instead of source-over), notification
banners, scrolling. See [../KNOWN_ISSUES.md](../KNOWN_ISSUES.md) for visual defects still open (the poster
switcher, `sb3` blur refusal, the Control Center glyph opacity workaround).

## Metal (work in progress, instantnoodle only)

**Do not read "Metal runs" as "Metal apps work".** What exists is an experimental driver, off by default.

- `metal=1`: Metal serves test programs. `metal=2 metal_profile=1`: Metal is handed to the whole system (backboardd's
  compositor too); this reaches the home screen. Metal for the whole system is **not** the default; chronod's widgets
  still have two pipelines refused.
- The driver is `userland/src/hkmetal.m` (about 7,500 lines): a `_MTLDevice` subclass declared in source, registered
  via Metal's own `MTLAddDevice` with `MTLDeviceSPI` declared at run time (`class_addProtocol`); it opens `HKGPU`
  the way Mesa's shim does. The IOGPU road was measured shut (`IOServiceOpen` with Apple's type 1 returns
  `kIOReturnUnsupported`). Shader compilation is `libhackiosair.dylib` (AIR bitcode to NIR to ir3), loaded with
  `dlopen` so a device exists even without a GPU.
- Measured working on the phone: blits, clears, 4x MSAA resolve, command-buffer completion, fences, events and
  shared events (within one process), heaps (`metal_sync_test` 10/10); **263 of Apple's 271 system fragment shaders
  draw and the 258 compared agree with softpipe**; 47 compute kernels; command buffers cost one GPU submission and
  one fence (`metal_sync=1` restores a fence per draw: 107 us against 7,086 us for eight full-screen draws);
  Apple Maps runs: its pipelines build and its command buffers complete without error (`RC_JOBS=rc.jobs.maps`,
  offline, **no picture checked**).
- Not implemented (the driver says so once per feature): mesh/object shaders, tessellation, indirect command buffers,
  tile shading and imageblocks, visible/intersection function tables, indirect draws and dispatches, more than one
  viewport, depth clip clamp, vertex amplification, GPU counter sampling, texture access counters, texture views,
  wide lines, per-binding LOD clamps, wrapping caller memory, heaps do not suballocate, shared events across
  processes.
- Third-party Metal apps: none tried. Weather, Photos and Measure never opened a Metal device in the tests.
- Corpus numbers (456 modules in the system `.metallib` files, 2026-10-23): all read by our bitcode reader; 321
  translate and compile through ir3 (every entry point: 271 fragment, 47 compute, 3 vertex); the other 135 are
  `air.visible` library functions (all translate when wrapped); 255 identical to LLVM's execution and drawn; 30
  run on the phone with a finished result. Source of truth and caveats: `docs/air.md`, `docs/air-report.md`
  (generated by `tools/air/corpus.py`). Reaching ir3 means it compiled, not that it is correct.
- A known Metal-related abort: SpringBoard's Siri orb (`SUICOrbView -_updateMetalTextureRasterSize:`) asks Metal for
  something it will not give; `inject_sb.c` prints what Metal answered (`siri_orb=0` skips the prewarm, a
  diagnostic only). Whether it still aborts under `metal=2` is not recorded.
- Why IOSurface drawables matter: `-[CAMetalLayer nextDrawable]` sends a device one message,
  `-newTextureWithDescriptor:iosurface:plane:` (`hkdrawable.m`; research in air.md).

## Verification record

Verified on the phone: cold `gpu=1` (logs `180011`, `180356`), GPU scaling into scanout, hang recovery 3/3, fence IRQ,
page-table isolation x3, DCVS, block collapse in sleep (8+ sleeps incl. 5 rapid and a 15 s powerd sleep), Metal
tests listed above. Not measured: power saved by hwcg or collapse; real-world Metal app rendering.

## Files

`xnu/files/bsd/dev/hk_adreno.c`, `hk_a650_hwcg.h`, `xnu/files/iokit/Kernel/HKGPU.cpp`, `MiniIOSurface.cpp`; userland
repo: `userland/src/eagl.c`, `hkmetal.m`, `hkdrawable.m`, `gpu_iso_test.c`, `metal_*test.m`, `tools/mesa/`, `tools/air/`,
`tools/mk-mesa.sh`, `tools/mk-hwcg.py` (kernel repo).
