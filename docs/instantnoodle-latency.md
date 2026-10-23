# OnePlus 8 touch and presentation

Measured on the phone on 2026-10-23 (RAM-only runs; the logs are named below). The panel runs at **90 Hz with
no tearing**, frames are double-buffered and locked to the panel's TE, and touch is interrupt-driven. All of
this is the default on instantnoodle.

## What the display is

`fb_probe=1` dumps it. The firmware leaves a DSI command-mode panel driven by CTL0: VIG0 and VIG1, which are the
two 540-pixel halves, both fetching the boot framebuffer at `0x9c000000` (stride 4320). They feed LM0/LM1, then
dual DSC, then INTF1. INTF1's tear check is on, and **autorefresh is on** (`0x80000001`), so the MDP re-fetches
that memory on every TE. The MDP fetches through apps-SMMU context bank 4. Reading that context bank or its page
tables sends the phone to Qualcomm CrashDump mode, and this hypervisor's SMMU has no ATOS, so a mapping can only
be found out by trying. The second framebuffer at `0x9ca00000`, inside the firmware's display region, works.

## What changed, and why

- **Scanout (`fb_push`).** At the first frame autorefresh goes off, as in Linux's
  `dpu_hw_intf_disable_autorefresh`. Each frame is copied into the framebuffer the panel is not being sent
  from, VIG0/VIG1 are pointed at it, and CTL0 is flushed and started. The tear check holds the transfer to
  the panel's TE, and PINGPONG_0's done interrupt (MDP INTR bit 8, MDSS SPI 83) frees the other buffer.
  `fb_push=1` keeps one buffer and waits for each transfer before the next copy. `fb_push=0` gives back the
  firmware's autorefresh. On return to the loader, and on a panic, the boot framebuffer and autorefresh come
  back (`redwood_display_quiesce`).
- **90 Hz.** The panel's own switch, as Android's device tree has it for `timing@1` of the ANA6705: 9 DCS long
  writes through DSI0's command DMA, at the same DSI clock. The packets sit past the last pixel of the boot
  framebuffer, in memory the display SMMU already maps. `fb_hz=60` switches the panel back, since it keeps its
  rate across runs. The TE counter is retuned and the measured TE rate is logged.
- **CoreAnimation at 90.** iOS takes the panel's frame period from the framebuffer property
  `IOMFBDisplayRefresh` (`displayMinRefreshInterval` in 2^-32 s, `displayRefreshStepMachTime` in mach time),
  not from `TimingElements`. Without it, an iPhone X is 60 Hz everywhere. The window server's frame clock is
  our VSync notification (`ca_vbl`, automatic when the panel is faster than 60), and it fires at the panel's
  TE as the tear check's read-pointer interrupt reports it, not on a timer that drifts against the panel.
  `ca_fps` (automatic too; `ca_fps=0` turns it off) gives SpringBoard's animations and display links a 30–90
  range with a high-frame-rate reason, as a ProMotion iPhone's get. Without it, the busy-second median was
  74 fps instead of 87.
- **The tear.** The window server alternates two surfaces (3 and 4) and draws the next frame as soon as
  `IOMobileFramebufferSwapWait` returns, which used to be immediately: the GPU wrote the pixels being copied.
  `SwapWait` now waits until the surface it will draw into next has been copied. That is every swap but the
  last when the surfaces alternate, and all of them otherwise. A detector logs GPU work naming a surface
  during its copy; it found 0.
- **The scaling, by the GPU** (`fb_gpu=1`, the default). The two framebuffers are surfaces for the window
  server's GPU (`MiniIOSurfaceCreateScanout`), both inside the firmware's `cont_splash_region@9c000000`
  (0x2300000 bytes), mapped write-combined. Once CoreAnimation's frame is drawn, `userland/src/eagl.c` asks for one
  that is neither waiting to be pushed nor being pushed (framebuffer selector 200), scales the frame into it with
  `glBlitFramebuffer(..., GL_LINEAR)` and says so once that has gone to the GPU (201). The swap that follows
  pushes that framebuffer as it is, after its GPU fence, if the frame's surface has had no GPU work since; the
  processor copies nothing. The GPU's render targets need rows in multiples of 64 bytes, so the pipes read rows
  of 4352 bytes instead of the firmware's 4320 (`SSPP_SRC_YSTRIDE0`). The firmware's pitch, and the last frame
  repacked to it, go back with the boot framebuffer for the loader. A swap with no such frame (another layer,
  a surface new to the display) is copied by the processor as before, into a framebuffer the window server
  does not hold. `fb_gpu=0` keeps everything as before.
- **The copy**, where there still is one, is shared out in 48-row pieces taken in turn, not fixed thirds. A
  copying thread on a small core, or one preempted, no longer holds up the frame.
- **Touch.** S6SY761 reports arrive by the TLMM summary interrupt (`touch_irq=0`: 10 ms polling). The worker
  runs at priority 92, because at the default 81 the copying threads delayed it by up to 7 ms at 90 Hz.

`xl screenshot` takes the framebuffer the kernel pushed last, at the pitch it was written at (`loader_screen` in
`osfmk/arm64/loader_return.c`; a loader built before this reads the boot framebuffer at 4320, skewed).

## Numbers (ms, median / p95)

| stage | firmware autorefresh, 60 Hz (`022537`) | 90 Hz, processor scaling (`164134`) | now: GPU scaling (`184104`) |
|---|---|---|---|
| SwapEnd → presenter (queue) | 0.04 / 30.1 | 0.03 / 5.7 | 0.03 / 0.08 |
| SwapEnd → frame ready to push (copy, or the push before) | 12.1 / 46.8 | 5.9 / 17.1 | 5.8 / 10.8 |
| kick → panel RAM written (TE + 8.7 ms transfer) | none (tore) | 11.36 / 11.36 | 11.36 / 11.37 |
| SwapEnd → panel RAM written | not measurable | 17.2 / 28.5 (max 229) | 17.2 / 22.2 (max 118) |
| frames in a busy second (p50 / p90) | ≤ 60 | 87 / 88 | 87 / 88 |
| swaps a refresh late (busy windows) | | 5.6% | 2.2% |
| GPU busy (busy windows) | | 16.9% | 18.7% |
| touch report line → worker | 0.016 / 0.032 (IRQ), poll period 13.6 / 15.3 | 0.017 / 0.031 | 0.015 / 0.028 |
| touch I2C read (se13) | 2.2–2.8 | 3.1 / 3.6 | 3.0 / 3.8 |
| kernel post → HID read → dispatch | 0.02 → 0.08 | 0.03 → 0.16 | 0.02 → 0.09 |

With the GPU scaling, 98–100% of the frames of a busy five seconds are pushed as the GPU drew them (`184104`,
`185234`), and the processor no longer spends some 5 ms of three cores on each. The median from SwapEnd to the
panel did not move: it is one push in flight at a time, TE plus an 8.7 ms transfer, and a frame waits for the push
before it. The tail did. The two runs are of different hands on the screen, so the late-swap figures are a
comparison of like loads, not of one replayed.

## Procedure

```sh
. common/lib.sh; load_device instantnoodle          # never a bare ./xl enter (redwood's UEFI)
nix-shell --run './xl enter && ./xl ramdisk userland/flashboot.apfs'
XL_SECS=180 XL_FOLLOW=4 XL_FOLLOW_KEEP=1 XL_CMDLINE="cpus=8 cpufreq=1 bootcpu=7 ufs=2 root_apfs=ufs_darwin gpu=1 metal=2 metal_profile=1 touch_lat=1 log_launchd=0" ./xl run
# fallbacks: fb_hz=60 (panel back to 60), fb_push=1 (one framebuffer), fb_push=0 (firmware autorefresh),
# ca_vbl=0 / ca_fps=0 (CoreAnimation's own timer and caps), touch_irq=0 (polling)
python3 tools/pipeline-latency.py logs/HHMMSS-run.log logs/HHMMSS-run.log.live.*
```

`gpu=1` works from a cold start (logs `180011`, `180356`). It used to send the phone to CrashDump mode there: this
TrustZone crashes when asked to give up the zap shader (image 13) before it has been handed that image's header in
this boot, and step 19 asked exactly that first. The header now goes first (`step_zap_load`, `redwood_adreno.c`).

Host checks: `CC=clang python3 tools/instantnoodle-input-test.py`, `python3 tools/pipeline-latency.py --self-test`.

References: Linux `drivers/gpu/drm/msm/disp/dpu1` (`catalog/dpu_6_0_sm8250.h`, `dpu_hw_ctl.c`,
`dpu_hw_intf.c`, `dpu_hw_interrupts.c`, `dpu_hwio.h`), `drivers/input/touchscreen/s6sy761.c`,
`drivers/pinctrl/qcom/pinctrl-sm8250.c`, `drivers/gpu/drm/msm/dsi/dsi_host.c` (command DMA); the panel's 60/90 Hz
switch from the phone's own device tree (`../ios-instantnoodle/research/instantnoodle-live.dts`, `timing@0/1`);
the animation-range idea from PoomSmart's CAHighFPS.
