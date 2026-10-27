# Audio: speakers, microphone, haptics path

Sound is the audio DSP's, not the application processor's. The SoC's audio block (LPASS: clocks, I2S/MI2S/TDM,
SoundWire, the codec macros) belongs to a Hexagon DSP (Android's "adsp") that stays in reset until the kernel loads
its signed image. **Reading an LPASS clock register from the application processor ends the kernel with an
external abort**; the DSP owns that block and is spoken to only through shared memory.

Status (2026-10-26):

| | redwood | instantnoodle |
|---|---|---|
| speakers | plays (2 x Awinic AW88261 on TDM) | **plays, heard by the owner 2026-10-24** (2 x NXP TFA9874 on tertiary MI2S): lock sound whole, volume right |
| iOS UI sounds, ringtones, alerts, local notifications, volume keys | reach the DSP | reach the DSP (recordings show it) |
| main microphone | works (analog, ADC1 of the WCD938x, SoundWire) | works, verified by the owner's claps (digital DMIC through the TX macro) |
| headphones / USB-C analog audio | not done | **not done** (FSA4480 + WCD938x RX + MBHC) |
| speaker protection | replaced by a software "guard" | same, plus TFADSP protection not done |
| haptics | Awinic AW86927 | Awinic AW8697 (see [input-peripherals.md](input-peripherals.md)) |
| earpiece, Bluetooth audio, call audio | none | none |

## Layers

```
iOS mediaserverd (CoreAudio HAL) --- HKAudio.driver  (userland/src/audio_plugin.c, an AudioServerPlugIn)
        presents: "Speaker" (out + a silent input), "Hawking" (silent input, required companion),
                  "Actuator" (1-ch out: the Taptic Engine), "Codec" (1-ch in: the built-in microphone)
        -> /dev/hk-audio (bsd/dev/hk_audio.c)   /dev/hk-haptic   /dev/hk-mic
kernel:  hk_sound.c   ASM stream + ADM device path + AFE port, through the DSP's APR services
         hk_guard.c   what the samples go through before the amplifiers (software speaker protection)
         hk_speaker.c / instantnoodle_speaker.c   amplifier setup over I2C, start/stop
         hk_mic.c     microphone path
         hk_adsp.c    DSP start, SMEM/SMP2P/GLINK/QRTR/APR  (copy of the Wi-Fi DSP's approach in hk_wifi.c)
```

### The DSP (`hk_adsp.c`, `adsp=1`)

Image: `adsp.mdt` and `adsp.b02`..`adsp.b36` (20 MB) from the modem partition's `image/` (`/vendor/firmware_mnt/image`).
They go into the memory reserved for the DSP and TrustZone is asked to authenticate and start it. Redwood: reserved
64 MB at `0x86700000`, processor 2 in SMEM, SMP2P items 443/429/423, IPCC client 3, TrustZone image 1. OnePlus 8:
memory `0x8fe00000`/`0x2500000` with a relocatable mdt and byte tails, and the driver makes SMEM items 478/479
itself because no Android ran. Steps (`adsp_step=N`): shared memory, firmware check, AOP mailbox, TZ header, load,
SMP2P, RPMh rails (LCX/LMX at level 384 while booting), GLINK, QRTR on `IPCRTR`, APR on `apr_audio_svc`. What is
started is stopped on exit and on a panic's way out (the DSP asked first, then TZ, then the AOP). A DSP left
running by an earlier kernel is found by its item and shut down first. `adsp_again=1` (start although the previous
DSP was shut down without acknowledging a stop) **reset the phone twice out of two** on the Wi-Fi DSP; do not.

`hk_adsp.c` also hosts the **battery manager** path on redwood (the DSP's charger service, `battmgr`), used for the
battery and charger on redwood (`HKBattery`).

### Output (`hk_sound.c`, `sound=N`, needs `adsp=1`)

Android sends speaker sound through three DSP services: **ASM** (a stream the samples are written into), **ADM**
(the matrix that routes a stream to a device path, "COPP") and **AFE** (the port at the SoC's interface). The samples
sit in kernel memory the DSP reads through the SMMU as stream `0x1801` (Android: `qcom,msm-audio-ion`, addresses
from `0x10000000`, the stream's low 4 bits in bits 32..35 of every address handed to the DSP).

| `sound=` | Meaning |
|---|---|
| 1 | the port: pins, bit clock, group, port started; the clock and frame sync looked at; held `sound_secs` then undone |
| 2 | + a stream, device path and a 1 kHz -20 dBFS tone for at most 3 s; the DSP's "written" events say whether it takes 48 kHz. Amplifiers off (silent) |
| 3 | + the amplifiers, faded in to `sound_below` eighths of a dB under Android's level. **Audible** |
| 4 | what iOS plays (`/dev/hk-audio`) until the run ends: path kept running, amplifiers started when a write is not silence, stopped after 3 s of silence |

Redwood's amplifiers are on the primary TDM interface (`PRI_TDM_RX_0`, AFE port `0x9000` in group `0x9100`, GPIO 97/100/99
bit clock/frame sync/data). The OnePlus 8's are on the **tertiary MI2S** (`TERT_MI2S_RX` `0x1004`, clock `0x104`, GPIO
133/135/134/137), two 32-bit slots at 48 kHz. Message layouts: Linux's `sound/soc/qcom/qdsp6/` (`q6afe.c`, `q6adm.c`, `q6asm.c`).

Volume: iOS sets the plug-in's volume control once, to its top (0 dB); the virtual audio device keeps the iPhone X's
speaker in software-volume mode, so the buttons and Control Center scale the samples themselves and at volume 0 what
is written is silence (the amplifiers stay off). The plug-in's own scale is 48 dB in a straight line, 3 dB for each
of iOS's sixteen steps.

### Amplifiers

- **Redwood: Awinic AW88261** (chip ID 0x2113, "smart PA") x2 on I2C of serial engine 3 (GPIO 12/13); address `0x35`
  is channel 0 (left, slot 0), `0x34` channel 1; no reset pin. Set up exactly as Android's driver (version 2.0.0
  `aw882xx_device.c`), with the "Music" profile of `/vendor/etc/aw882xx_acf.bin` (the phone's own, in `fw-audio/`);
  55 of its 57 registers read back from the idle chips as the file has them. The DSP's Awinic algorithm (excursion and
  coil-temperature protection from current/voltage feedback) does not run; Android's battery-dependent table for a
  cold amplifier is applied as a "monitor" reduction.
- **OnePlus 8: NXP TFA9874** x2 (rev `0x0c74`) on I2C of serial engine 15 (GPIO 44/45; the SMB1390 charge pump and
  FSA4480 USB-C switch share the bus and are not addressed). `tfa98xx_right` `0x34` (reset GPIO 91, slot 0, "P" in
  the container, has the receiver profile) and `tfa98xx_left` `0x35` (reset GPIO 99, slot 1, "Q", the bottom speaker),
  both resets active high. Set up as NXP's driver v6.5.3 does for a cold TFA9874 from the container
  `/vendor/firmware/tfa98xx.cnt` (profile "speaker"). The chip has no DSP of its own and no volume/mute the driver
  uses, so **the level is applied to the samples** by `hk_guard_digital()`, and amplifiers are powered down after 3 s of
  silence. Amplifier health: alarm/state 8 -> reset, other non-9 states tolerated 1 s; the status register 0x10 idle
  values 0x0016/0x0017 are OK bits. **The kernel default is `sound_amps=0`** (amps set up but never started); the
  device profile passes `sound_amps=1 sound_below=96` (12 dB under Android's maximum).
  - Stock evidence for the level: no attenuation at Android's maximum (speaker media/ring/notification curves end at
    0 dB, system -6 dB; ACDB `MTP_Speaker_cal.acdb` device 15: TFADSP gain 0x7fffff = 0 dB, MBDRC present but its
    makeup undecoded, COPP PBE off, gain 0x10C35 = unity). So `sound_below` is "dB under stock maximum".
  - A bug found and fixed: the first 150 ms of the lock sound were lost because of a silent-start plus fade. The
    amplifiers start in 26-39 ms, within the DSP's 160 ms queue, and the level is kept (kernel `44ba0e0`).

### The guard (`hk_guard.c`)

Android protects small speakers in the DSP; here nothing knows the speakers' parameters, so a software guard holds
what reaches them to levels safe whatever they are, with no feedback: a 200 Hz second-order Butterworth high-pass;
power averaged over 0.2 s, 3 s and 60 s limited to 3, 6 and 9 dB under a full-scale sine at Android's level; a peak
limiter (no sample above -3 dB at Android's level, recovery constant 43 ms); a "monitor" reduction from the amplifier
watch; and a gain `sound_gain` (default and maximum 96 eighths of a dB = 12 dB), because iOS's own processing for the
iPhone X's speaker keeps a full-scale tone at top volume 12.6-14.3 dB under full scale. Integer arithmetic only (the
kernel's threads do no floating point). For amplifiers without a volume the guard also puts the level on the samples,
falling to a new level within a block and rising 6 dB per 20 ms block from -90 dB. `sound_guard_as=N` is a test knob.

### Microphone (`hk_mic.c`, `mic=N`, `RC_JOBS=rc.jobs.mic`)

Redwood: the analog main microphone at ADC1 of the WCD938x over SoundWire, into decimator 1 of the TX macro,
out through `TX_CDC_DMA_TX_3`, into an ASM stream. Before any of it the LPASS clocks the DSP gives out
(`lpass_core_hw_vote`, `lpass_audio_hw_vote`, `tx_core_clk` 0x30c and `tx_npl_clk` 0x30d at 19.2 MHz) must be
requested; without them the macros' registers must not be touched (a bus hang). `mic=1` looks, `2` brings the codec
up far enough to answer on SoundWire, `3` listens for `mic_secs`, `4` feeds `/dev/hk-mic` for iOS.

OnePlus 8: the same LPASS and codec but a **digital** main mic (OxygenOS mixer path `handset-mic` = `dmic2`, "TX DMIC MUX2"
DMIC1 into decimator 2, `TX_CDC_DMA_TX_3`, one channel), powered by the codec's MIC BIAS1 (1.8 V). The codec is only
brought up for its bias (reset TLMM gpio32 driven as a GPIO because the pin controller refuses its function, 16 mA,
output high); the clock is the VA macro's DMIC0 at MCLK/4 (2.4 MHz), LPASS pins 6 and 7 (function 1, 8 mA), no
SoundWire data port, the decimator at +6 dB (`TX_DEC2 Volume` 90). Decimator 2 is unmuted after 50 ms and its filter
settled after 300 ms (OnePlus's `TX_MACRO_DMIC_*_DELAY_MS`). Measured by the owner's claps: noise floor -75 dBFS,
claps -55..-59 dBFS (run committed in `ab036b1`).

Hard rules found on the phone (details in [../research/hardware-qualcomm.md](../research/hardware-qualcomm.md)):

- The DMIC clock pin needs its **output enabled** (config bit 9, the device tree's `output-high`); without it no
  clock leaves the pin and the data pin stays flat.
- **Never vote the VA core clock `0x30b` or write the VA GFMux `0x33a0000`** (reads 0 = TX MCLK): NoC CrashDump at the
  TX macro `0x3220500`.
- A `kmem_alloc` of 589,824 panicked in `MiniIOMobileFramebuffer::mapDisplay` because the **arm IO submap (8 MB,
  permanent)** was exhausted; the mic driver had mapped 1 MB of TLMM, it now maps one pin page. Keep `ml_io_map` blocks small.
- `rc.jobs.mic` plays a tone; use `rc.jobs.micquiet` at night. `rec_sound_mic=1` records what the mic hears.

### Recording sound for tests

`rec_sound=N` keeps the first N seconds of what iOS played in a memory region that survives the return to the loader;
`xl sound` fetches it and `tools/sound.py` makes a WAV. With `rec_sound_fed=1` it records what the guard let through
(what reaches the amplifiers). The log line when the writer stops says how long it wrote, at what rate, how much was
not silence and how loud. The loader keeps only the first 256 KB and last 1 MB of the kernel log; use `adsp_loud=1` or
`speaker_probe=3` for early detail. Test programs: `sound_test` (`alert TYPE` for ToneLibrary alerts, `notification
BUNDLE`), `RC_JOBS=rc.jobs.speakers|alerts|volume`.

## Not done

- USB-C analog headphones: FSA4480 + WCD938x RX + MBHC.
- TFADSP protection: AFE module `0x1000B911` on `TERT_MI2S_RX`, VI on `TERT_MI2S_TX` (`0x1000B912`, topology
  `0x1000B909`), container messages via `AFE_PARAM_ID_TFADSP_RX_CFG`.
- Mic: only the main microphone; others and noise suppression are not set up. Call audio and the earpiece: none.

## Files

`xnu/files/bsd/dev/hk_adsp.c`, `hk_sound.c`, `hk_audio.c`, `hk_guard.c`, `hk_speaker.c`, `instantnoodle_speaker.c`, `hk_mic.c`,
`hk_virtio_sound.c` (QEMU); `xnu/files/bsd/sys/hk_audio.h`; userland repo: `userland/src/audio_plugin.c`, `sound_test.c`;
tools `tools/sound.py`. Firmware: `fw-audio/` (never in the repository).

## Bluetooth test caveat — 2026-10-26 22:31

CMF Buds Pro 2 classic pairing/authentication/encryption and AAC A2DP negotiation are hardware-verified;
owner hears no sound and reports Voice Memos recording failure. The Bluetooth test boot `221539` omitted
`adsp=1 sound=4 mic=4` and forced `sound_amps=0`, so it did not enable the previously verified built-in paths.
That test does not establish a built-in audio regression or identify which recording input Voice Memos selected.

Found and fixed a separate packaging omission: the HKAudio-only HAL overlay hid the original BTAudioHALPlugin.driver,
and the extraction selector omitted it. `BLUETOOTH=1` now retains that unmodified plug-in alongside HKAudio.
Userland build and host packaging/gate checks pass; audible output and buds microphone still need a phone test.
See [bluetooth.md](bluetooth.md#audio-gap-found-after-pairing--2026-10-26-2230) for the bounded quiet-tone probe.
System/deep-sleep work belongs to the other Claude agent; this fix changes no sleep/power implementation.
