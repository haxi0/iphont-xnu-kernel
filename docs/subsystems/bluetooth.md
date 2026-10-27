# Bluetooth — OnePlus 8 / QCA6390

Status, 2026-10-26: **native BLE/GATT and classic headset pairing/encryption verified; audio playback and reconnection unfinished.**
Opt-in only: kernel `bt=1`, userland `BLUETOOTH=1`. Installed boot bundle is unchanged.

## Hardware and transport

The live Android device tree identifies the Bluetooth half of QCA6390 on **QUP0 SE6 UART at
0x998000**, not on Wi-Fi's PCIe/MHI transport. GIC interrupt 639 (SPI 607), protocol 2 UART
firmware, working FIFO interface (16 TX / 32 RX words), QUP version 0x20050000 / 16x sampling.
TLMM north: GPIO16 CTS, 17 RTS, 18 TX, 19 RX, function 1; GPIO21 BT_EN; GPIO124 SW_CTRL input.

Shared supplies: `smpa6` 950 mV, `smpf2` 950 mV, `smpa5` 1900 mV, `smpc8` 1350 mV.
BT-only supply: `ldoa16` 3024 mV. Wi-Fi and Bluetooth now retain shared rails while either user needs them;
their enable transitions are spaced by 100 ms. Coexistence and concurrent Wi-Fi sleep remain unverified.
SE6 GCC RCG 0x17730 is not in DFS mode. The driver programs Linux's clock rates for 115200 and 3000000 baud.
The S_AHB branch uses hardware gating: its OFF bit alone is not a failed clock vote.

`instantnoodle_bt.c` supplies power, clocks, RTS/CTS UART, interrupt-driven receive, bounded transmit,
and counters. `hk_bt.c` loads firmware, handles H4 and Qualcomm in-band sleep, and exposes the kernel
control socket `com.apple.uart.bluetooth`. Controller capability masks and HCI replies pass through unchanged.
`HKBluetooth.cpp` publishes the registry service and transport/address properties.

## Firmware and reuse

Firmware remains ignored and local in `fw-bt/`: `htbtfw20.tlv` and `htnv20.bin`, extracted read-only from
this phone's backed-up Bluetooth FAT partition. `userland/build.sh` puts them in the small image's `fw/`.
Do not commit firmware, addresses, pairing keys, dumps, or Apple binaries.

Protocol and register references:

- [Linux hci_qca.c](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_qca.c): IBS and power lifecycle.
- [Linux btqca.c](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btqca.c): version, TLV/NVM, firmware download.
- [Linux qcom_geni_serial.c](https://github.com/torvalds/linux/blob/master/drivers/tty/serial/qcom_geni_serial.c): UART/FIFO.
- Linux `gcc-sm8250.c`, `pinctrl-sm8250.c`, and `pwrseq-qcom-wcn.c`; vendor `msm_geni_serial.c` for RFR_OPEN.
- UniXNU's local sheng support tables list Bluetooth unavailable; no reusable working driver was found in the inspected trees.
  [Upstream sheng project](https://github.com/UniXNU/UniXNU-sheng) remains a reference, not this port's architecture.

On the phone, QCA6390 reports SoC 0x400a0200, ROM 0x200, manufacturer 29. Version and TLV replies use
Command Complete, not the older ROME vendor-event form. Firmware patch downloads in 759 segments; NVM in 24.
NVM tag 17 baud is its second byte. IBS wake/sleep/ack bytes are 0xfd/0xfe/0xfc between H4 packets.
Ignoring these caused stalls; the transport now acknowledges wake and wakes the controller before host traffic.

## Native iOS integration and diagnosed failures

`BLUETOOTH=1` loads stock bluetoothd and BTServer jobs; `BT_HCI_TRANSPORT` selects
`H4:POSIX:com.apple.uart.bluetooth,3000000`, avoiding the unavailable Skywalk UART path.
`btboot.c` implements this board's actual power/firmware lifecycle under the BlueTool Mach service name.
Apple BlueTool's built-in scripts target iPhone controllers; external scripts are gated by release status.
That approach was abandoned. No internal-release spoofing is used.

Observed startup failures:

1. iOS's fixed HCI init list requires optional Write Default Erroneous Data Reporting (0x0c5b).
   QCA6390 advertises neither it nor read command 0x0c5a, and returns Unknown HCI Command to both.
   Earlier experimental mask edits and synthetic success replies were removed.
2. iOS chooses Broadcom vendor capabilities from the emulated iPhone model. ANTENNA_PREF_B receives
   a real Unknown HCI Command reply, which its Broadcom completion path mishandles until a 15 s timeout.
3. Clearing vendor bits once is insufficient: iOS rebuilds them after HCI init. `inject_bt.c` selects
   its generic vendor descriptor and omits only the unsupported optional init step through a private init-table entry.
4. Proprietary Broadcom multi-advertising remains a separate flag. Its 0xfd48 request failed to initialize
   the random address. Clearing that flag at lazy factory creation makes iOS send standard 0x2005 instead.
5. iOS then mixed its init query 0x204a (extended periodic advertiser list size) with legacy discovery commands.
   Controller-only reset/query comparisons prove this causes 0x2007/0x200b to return Command Disallowed (12).
   The board init table now omits that query to keep its legacy command family consistent.
   See [research findings](../research/bluetooth-ios.md) for the Bluetooth specification rule and proof.

The userland integration checks bluetoothd UUID and expected instructions/pointers before changing its
private init table. dyld image zero is not reliably the executable here: find it by UUID.
Both omitted steps use the native continue-one-step function (0x1001c204c), not the finish-with-status
function (0x1001c20b0). Using the latter prematurely ended initialization and restored the Broadcom timeout.
Only iOS 16.5 / 20F66 is supported by these guards. No on-disk Apple binary is modified.

## Evidence and remaining work

Historical phone logs retained locally:

- `152726-run.log.live`: controller self-test records 145 reports in four seconds.
- `063850-run.log.live`: native HCI startup fails at unsupported 0x0c5b.
- `153148-run.log.live.*`: synthetic-support experiment reached STACK_READY but repeatedly timed out on Broadcom VSCs.
  This experiment is no longer the implementation.
- `172302-run.log.live.*`: truthful HCI path reaches STACK_READY with the generic descriptor, then exposes
  the separate multi-advertising/random-address failure. General boot/idle checks passed; Bluetooth did not.

- `212729-run.log.live`: isolated 0x204a conflict; 180 controller scan reports after reset.

- `213153-run.log.live.*`: native legacy HCI scan succeeds with no daemon restart; one sleep/wake passes.
  Unfiltered CLI discovery returns zero callbacks; the session has FG:0. Explicit service scans are required.
- `213520-run.log.live.*`: explicit test-service discovery, connection and GATT read reach the Linux host.
  Write fails with ATT error 13 because the initial host attribute capacity was four bytes; fixture corrected to 32.
- `213836-run.log.live.*`: native discovery PASS (two reports) before and after sleep; initial connection,
  service/characteristic discovery and a 22-byte GATT write/readback PASS. Sleep/GPU/display checks PASS.
  Post-wake reconnect completes, then the peer requests MITM security. iOS logs “Ignoring security request as
  there are no sessions that support it!”, pairing status 4815 and disconnect status 705. Second GATT probe FAIL.
  Overall 10/11 checks; this is not reliable reconnection or pairing proof. No daemon restart in this run.

Host check: `nix-shell --run 'python3 tools/bt/check.py'` checks UUID/table guards and runs the actual driver's
H4 and NVM parsers under address/undefined sanitizers, including fragmented packets, oversized-packet recovery,
and malformed NVM rejection. Review fixed an ignored `nvm_fix()` failure and short-header access. The latest
kernel containing that validation fix compiled/linked at 21:40; it was not booted after the owner-action stop.
Native probe `bt_scan` uses CoreBluetooth with a Bluetooth usage description and no permission overrides.
It scans the controlled Linux peer UUID; `--link` verifies real GATT data, `--all` retains the exploratory
unfiltered scan. See [TESTING.md](../TESTING.md#bluetooth-on-instantnoodle-opt-in-ram-only) for peer setup.

## Supervised headset result — 2026-10-26 22:19

`221539-run.log.live.*`: owner paired CMF Buds Pro 2 in native Bluetooth Settings and reported Connected;
a host screenshot independently confirms the entry. HCI Simple Pairing Complete status 0, Authentication Complete
status 0, Encryption Change status 0 with encryption enabled. Native AVDTP selected AAC; remote SET CONFIG and
OPEN were accepted, media-channel completion OI_OK, state OPEN. No daemon restart observed during this capture.
This is real classic pairing/encryption and A2DP negotiation, not proof of audible output or HFP/SCO.
The final NVM validation kernel was used in this boot and successfully loaded firmware/started the controller.

Next owner step: play a short audio sample through the buds at low volume and report both channels, or authorize
a bounded quiet test tone. No audio test has been started by the agent. Sleep reconnect and persistent bonds
remain separate gates. Manual RAM test window ends around 22:25:39 MSK; no flash writes are authorized.

## Earlier stop and resume (21:42; pairing gate subsequently reopened)

Implementation stopped at 21:42 because supervised native pairing is needed. Do not disable MITM requirements,
fabricate keys/results, elevate the CLI's privileges, or auto-confirm prompts to get a passing test.
Owner action: be present with the phone and a controlled peer in pairing mode; initiate pairing in iOS
Settings → Bluetooth and confirm any matching code/consent on both devices. The next session must verify
that native Settings/BTServer supplies the pairing session, then capture the actual security exchange.
A missing native pairing session remains an integration issue; owner presence alone does not prove it fixed.
For audio, have a headset available and approve an audible test before playback.

Phone returned to its resident loader; no partition was flashed and installed boot remains unchanged.
The temporary host GATT application/advertisement was removed; host adapter restored powered off,
not pairable, with zero advertisements. Existing paired devices were not touched.

Not verified: general foreground BLE app discovery, BLE authenticated encryption,
reliable reconnection after sleep, A2DP/HFP/SCO audio, persistent bonds after reboot, or Wi-Fi coexistence. Controller inquiry completed in an
earlier test with zero discoverable peers; that does not prove native classic discovery or pairing.

## Audio gap found after pairing — 2026-10-26 22:30

Owner hears no ringtones/system audio and Voice Memos recording fails in `221539`. This is a failure report,
not a successful audio test. Source audit identifies two independent conditions:

- **Observed in build artifacts:** `/hackios/hal` contains only HKAudio.driver. The flash-root bind hides all stock
  system HAL plug-ins, including the original BTAudioHALPlugin.driver; select-system.py also excluded that bundle.
  bluetoothd's published XPC audio devices therefore have no native CoreAudio Bluetooth plug-in to receive them.
- **Observed in boot arguments:** test omitted DSP/output/mic enable args and forced speaker amplifiers off.
  Built-in silence and unavailable built-in recording are expected in that recipe; neither identifies the selected mic.

The source selection and `BLUETOOTH=1` packaging now retain the original BTAudioHALPlugin.driver alongside HKAudio,
for flash-root overlays and system-image builds. Missing native bundle fails the build instead of silently dropping it.
Extracted only that bundle from the existing local rootfs.dmg; no Apple binary modification or redistribution.
Host checks verify selection, original bundle byte identity and retained HKAudio. Userland build passed; phone test pending.
Native plug-in has AudioSkywalkPipe/AudioRingBuffer paths; transport availability needs runtime logs, not assumptions.

Owner approved a quiet sound. `sound_test bluetooth-tone SECONDS` waits up to 300 seconds for the exact CMF Buds Pro 2
A2DP output route and then plays two seconds at 1% sample peak. No target route means no tone. Host tests cover route
and session rejection plus fixed duration/level, using framework-call fixtures; they do not prove CoreAudio behavior.
Next RAM test needs re-pairing. Use `adsp=1 sound=4 mic=4 sound_amps=0`, plus Bluetooth flags, to enable built-in
backend/mic diagnostics while keeping physical speakers muted. Do not claim buds microphone selection without route evidence.

Another Claude agent owns system/deep sleep. This work makes no sleep/power changes; coordinate through the shared phone lock.

## Corrected audio image on phone — 2026-10-27 00:28

`002629-run.log.live.*` boots the corrected native HAL bundle with DSP/output/mic enabled and physical speakers
muted. CMF Buds Pro 2 are Connected in Settings. Probe sees BluetoothA2DPOutput, exact buds name, volume 0.30;
AudioQueueStart returns 0 and all 96000 frames of the two-second 1%-peak tone are handed over. Native AVDTP
START is accepted and state changes OPEN → STREAMING at 00:27:20. No restart observed. This proves routing,
queue acceptance and stream setup; audible output still awaits the owner. Microphone route/data remain unverified.
Shared lock was free; current kernel includes the other agent's sleep/display fixes. No sleep code changed.

## Archived implementation handoff (historical 2026-10-26 21:42)


- Current owner task is **HackiOS native Bluetooth on instantnoodle**, not a switch to another port. Preserved all dirty work.
  Native QCA6390 BLE discovery, initial connection, GATT discovery and 22-byte write/readback are now hardware-verified.
  Filtered discovery also works after one successful sleep. **Pairing, encryption, audio and reliable reconnection are unfinished.**
- Root causes fixed: unsupported optional 0x0c5b init; Broadcom vendor capability/early multi-advertising selection;
  init's extended 0x204a query locking out later legacy 0x2007/0x200b. Controller reset/query A/B test proves the latter.
  No synthetic HCI replies/capability masks, internal-release spoofing, or safeguard bypass remains.
- `inject_bt.c` is UUID/expected-pointer/instruction guarded for 20F66. Factory wrapper clears only proprietary multi-adv;
  final init step selects generic vendor descriptor. Omitted init steps call **0x1001c204c (continue)**; 0x1001c20b0 finishes
  the whole sequence and is wrong for a middle step. Find dyld image by UUID, not index zero. Details:
  [bluetooth subsystem](subsystems/bluetooth.md), [research](research/bluetooth-ios.md), [test setup](TESTING.md#bluetooth-on-instantnoodle-opt-in-ram-only).
- **Stop condition reached; implementation stopped.** `213836-run.log.live.*`: initial GATT PASS; post-wake connection
  requests MITM security, then iOS says “Ignoring security request as there are no sessions that support it!” (4815/705).
  Owner must be present with a controlled peer in pairing mode, use Settings → Bluetooth, and confirm matching code/consent
  on both devices. Next session must diagnose/verify native Settings/BTServer pairing-session registration with those logs.
  Do not auto-confirm, fabricate security results, weaken MITM, or grant the CLI private pairing privileges.
- Tests: `212729` controller A/B + 180 reports; `213153` no restart + sleep PASS; `213520` GATT read, fixture write-size failure;
  `213836` **10/11**, discovery twice PASS, first GATT PASS, sleep/GPU/display PASS, second GATT FAIL (security above).
  Unfiltered CLI scan has FG:0/no results; explicit UUID is required by CoreBluetooth background rules. General foreground
  and classic discovery need separate proof. Host H4/NVM sanitizer fixtures and binary guards PASS; `tools/t host` 5/5.
- Latest kernel built/linked 21:40 (normal symbol-export errors remain). Review fixed NVM short-header access, malformed-tag
  rejection and an ignored validation failure; exported `xnu/files/bsd/dev/hk_bt.c` matches. **That final validation change is
  not booted**, because the owner-action stop followed. Earlier native proof uses the preceding kernel.
- New ignored userland files must be preserved explicitly when a commit is eventually requested:
  `userland/src/{btboot.c,inject_bt.c,bt_scan.m,bt_scan.plist}`, `userland/com.hackios.bluetool.plist`.
  Also preserve kernel driver files, `tools/bt/`, and docs; no commit/push requested. No whole-tree export from stale sources.
- All phone runs were silent (`sound_amps=0`) and RAM-only; nothing flashed. Phone is back in resident loader.
  Temporary host GATT/advertisement removed; adapter restored off/not pairable/zero advertisements. Existing bonds untouched.
  Audio requires a headset and permission for an audible test. Installed boot still predates Bluetooth.


## Recording failure isolated — 2026-10-27 00:36

Owner reports Voice Memos crashes while buds are connected, no lock sound, and temporary black screen after lock.
`002629` confirms the app's play-and-record routing uses CMF HFP (`phpb`, `tsco`) at 00:27:33; VirtualStream
reports 'what' obtaining a client format for 16-bit/8-kHz/mono. HCI Setup Synchronous Connection (0x0428)
completes successfully (event 0x2c/status 0). mediaserverd restarts (PID 40 → 239) around 00:27:56. Existing
logs lack its fatal stack, so neither format error nor controller setup proves the crash's root cause.

Silent no-buds isolation `003355-run.log.live.*` uses existing record_test with `exctrace=1 applog=1`.
Built-in input is selected and records 96000 non-silent samples/two seconds; Voice Memos opens without a
captured exception. Thus built-in framework recording works; headset recording still needs a traced reproduction.
No source fix was attempted on incomplete evidence. Next owner action: re-pair buds and tap Record in Voice
Memos in a traced RAM session. Lock silence is expected with speaker amplifiers intentionally disabled;
the transient black screen is reported to the shared handoff for the sleep agent, without assigning a cause.


## Bluetooth audio fixed at the transport — 2026-10-27 01:15

Two RAM runs with the owner and CMF Buds Pro 2 (`005834`, `010727`; `bt=1 bt_trace=2 adsp=1 sound=4 mic=4 sound_amps=0 exctrace=1`,
image `BLUETOOTH=1 OSLOG='com.apple.bluetoothd com.apple.mediaserverd'`, jobs `userland/rc.jobs.btaudio`).

- **A2DP: heard by the owner** in `010727` (lock sound, Voice Memos start chime). `bt_trace=2` shows the media path working:
  RTP/AAC (LATM) ACL packets of ~390 bytes, each completed by the chip (Number of Completed Packets). The earlier
  "0 completions in 002629" was a log-merge artefact (identical lines removed), not a missing path.
- **HFP/SCO: the cause of the Voice Memos failure, mediaserverd restart and bluetoothd "HCI initialization failed" restart.**
  As soon as bluetoothd sent SCO data, QCA6390 held CTS for good (every later UART write timed out; IBS wake failed).
  `005834` (legacy set-up translated): the chip's SCO over HCI arrived (real microphone samples), bluetoothd ran 5 packets
  ahead of the air, logged `Assertion failed: fAudioBytesOutstanding <= MAX_VOICE_SIZE_IN_BYTES`, sent one 8-byte packet,
  then a burst of 5 -- stall. Fix in `hk_bt.c`:
  1. legacy Setup/Accept Synchronous Connection (0x0428/0x0429) go out as Enhanced (0x043d/0x043e) with data path 0 (HCI),
     codec fields as Linux `hci_conn.c`; the Command Status goes back with the legacy opcode. Boot-arg `bt_sco_hci=0` disables.
  2. SCO from iOS is queued and sent in packets of the chip's own SCO size, at most 2 ahead of the packets the chip sends
     (PipeWire's pattern), queue 8 packets, oldest dropped. Boot-arg `bt_sco_pace=0` disables. Counters in `kern.hackios.bt`.
  `010727` with both: three voice links (ringtone previews) up and down cleanly, 24,240 SCO bytes sent, 0 dropped,
  253 chip packets, **no UART timeout, no daemon restart**. Whether step 1 alone matters is unproven (legacy path never traced).
- **Remaining: Voice Memos recording through the buds.** In `010727` its one attempt (01:10:50) never got an SCO link:
  bluetoothd sent `+CIEV` and `+BCS:1` (all 5 ACL packets completed by the chip within 230 ms), the buds answered
  (`AT+CLCC`, `AT+BCS=1`) only 3.2 s later, after bluetoothd's codec-connection limit (`Audio Creation failed 1809`);
  mediaserverd's input then timed out in `Priming` and the app went away. `exctrace=1` recorded **no exception or abort**
  for the app (pid 251): not a crash in the usual sense. Ringtone links using the same procedure succeeded 3/3 earlier.
  Next: repeat the recording 2-3 times with `applog=1 applog_name=VoiceMemos` to see whether the buds' late answer repeats.
- Speakers stay silent in these recipes on purpose (`sound_amps=0`); with buds connected iOS plays ringtones over HFP/SCO.
- `sound_test bluetooth-level PEAK` (max 0.1, owner-approved 2026-10-27) raises the buds-only diagnostic tone.
- First bring-up attempt after power-on timed out at 115200 baud in both runs; the existing power-cycle retry recovered it.

### Voice Memos through the buds — runs `011859`, `012839` (2026-10-27 01:20-01:37)

- `011859` (`applog=1 applog_name=VoiceMemos`): the buds answered at once, the voice link came up and SCO flowed
  (12,160 bytes, 124 chip packets, no stall), and **Voice Memos crashed twice** (pids 258, 260) with
  `libc++abi: terminating due to uncaught exception of type std::__1::system_error: thread_policy_set realtime failed:
  Mach: (os/kern) invalid argument` -- CoreAudio's real-time I/O thread request refused by the kernel. That is "the crash".
- Stock XNU accepts a time-constraint computation of 50 us..50 ms with constraint >= computation (`thread_policy.c`,
  `sched_prim.c`). Added (in `xnu-src` `osfmk/kern/thread_policy.c` only -- **not yet exported to `xnu/hackios.patch`**):
  a computation over 50 ms is cut to 50 ms instead of refused (boot-arg `rt_clamp=0` restores stock), and every cut or
  refusal prints `thread_policy: PROC: real-time period/computation/constraint ...`.
- `012839` with that kernel: **owner confirms recording through the buds works and plays back** (10 s, 01:31:52-02:02,
  pid 243, no crash). But no `thread_policy:` line was printed: no request needed cutting or was refused in that run,
  so the cut is **not proven** to be the fix; what made 011859's request invalid is unknown. If the crash returns,
  the new log line gives the asked values; other refusal paths in `thread_policy_set()` (priority checks) do not log.
