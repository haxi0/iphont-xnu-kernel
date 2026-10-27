# QCA6390 and iOS Bluetooth findings — 2026-10-26

See [the subsystem page](../subsystems/bluetooth.md) for implementation and test scope.

- **[observed]** Firmware from this phone responds through SE6 UART with Command Complete for Qualcomm TLV/version
  operations. Older ROME vendor-event assumptions are wrong for this controller. NVM tag 17 stores baud in byte 1.
- **[source]** Linux `hci_qca.c`/`btqca.c` supply the firmware and IBS protocol; `qcom_geni_serial.c` supplies UART sequencing.
  [Upstream Linux driver](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_qca.c).
- **[observed]** Stock iOS 16.5 HCI initialization requires optional 0x0c5b despite its absence from the supported-command mask.
  QCA6390 returns Unknown HCI Command. The board init table now omits the optional step; replies/masks are unmodified.
- **[observed]** The emulated iPhone model selects Broadcom vendor commands. ANTENNA_PREF_B's unknown-command reply
  becomes a 15-second completion timeout. Vendor capability flags are reconstructed after HCI initialization.
  Selecting the generic descriptor at that point prevents those commands. Selecting it before stack customization
  instead fails callback registration with “Unknown chipset 0”; the factory must initially retain its expected type.
- **[observed]** Proprietary multi-advertising is cached earlier. The factory wrapper must clear only that flag before
  customization caches it. This changes random-address setup from Broadcom 0xfd48 to standard HCI 0x2005.
- **[observed]** `212729-run.log.live` isolates an independent command-family conflict. After Reset, 0x2005, 0x2007,
  and 0x200b all return success. After Reset then LE Read Periodic Advertiser List Size (0x204a), 0x2005 succeeds
  but 0x2007 and 0x200b return Command Disallowed (12). Reset restores legacy scanning (180 reports/four seconds).
- **[source]** Bluetooth Core, HCI section 3.1.1, forbids mixing legacy and extended advertising/scanning commands
  since reset. Its extended list includes the apparently harmless periodic-list-size query.
  [Bluetooth Core specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-60/out/en/host-controller-interface/host-controller-interface-functional-specification.html).
- **[observed]** iOS's legacy discovery path must omit 0x204a during initialization to keep that
  command family consistent. Extended/periodic advertising remains unfinished; controller capability bits stay truthful.
- **[observed]** dyld image zero may be an injected library here. Find bluetoothd by UUID before calculating its slide.
  Guard every changed pointer and function word. Hooks support 20F66 only and leave mismatched images untouched.
- **[research]** UniXNU-sheng's published support tables did not supply working Bluetooth. Sandcastle's Bluetooth work
  targets Linux on Apple hardware and Broadcom firmware, not an iOS QCA6390 host integration.
  [UniXNU](https://github.com/UniXNU/UniXNU-sheng), [Sandcastle](https://github.com/corellium/projectsandcastle),
  [iOS V60](https://github.com/ch1tmdgus/ios-v60) (same SoC/controller; firmware/UART research, no verified native pairing).

No internal-release spoofing is used. Do not reinstate the removed experiment that fabricated command support/success.
No pairing, encryption, audio, persistent bonds or coexistence claim follows from controller discovery alone.

- **[observed]** `213153-run.log.live.*`: no restart, controller reports received before/after a successful sleep.
  Unfiltered CLI scans return zero callbacks with FG:0. `213520` explicit test-service scan connects to the Linux
  host and reads its GATT value. Its write reaches BlueZ but returns ATT Invalid Attribute Value Length (13),
  traced to the test server's four-byte capacity, not UART/ACL loss.
- **[source / inferred]** Apple requires explicit service UUIDs for background scans. The CLI's FG:0 explains
  its unfiltered result; verify general discovery separately in a foreground app, without privilege changes.
  [CoreBluetooth scan API](https://developer.apple.com/documentation/corebluetooth/cbcentralmanager/scanforperipherals(withservices:options:)).

- **[observed]** `213836-run.log.live.*`: filtered native discovery returns two reports before and after sleep;
  initial GATT transfers 22 bytes and reads back identical data. The next connection completes, receives a peer
  MITM security request, then iOS rejects it because no registered session supports pairing (4815; HCI disconnect 705).
  This failure does not establish a UART sleep/wake fault. It also does not establish functional pairing.
  Work stopped for supervised native pairing; no privileges, security requirements or confirmation flow were bypassed.
- **[observed, host]** NVM validation returned failure but its caller ignored it; short files could be read before
  length validation. Fixed size checks, incomplete-tag rejection and failure propagation; actual parser sanitizer
  fixtures pass. Kernel compile/link passed; its usual symbol-export stage still fails. Latest kernel not hardware-tested.

- **[observed, phone + owner]** `221539` native Bluetooth Settings pairs/connects CMF Buds Pro 2. HCI Simple
  Pairing Complete and Authentication Complete return status 0; Encryption Change returns 0/enabled 1.
  AAC is selected; AVDTP SET CONFIG and OPEN accepted, media channel OI_OK/OPEN. No heard-playback proof yet.
  Therefore the earlier CLI no-supported-pairing-session failure does not establish broken native Settings pairing.

- **[observed, source/artifacts]** HAL overlay contained only HKAudio, while rootfs.dmg contains BTAudioHALPlugin.driver
  (com.apple.audio.BTAudioHALPlugin). The extraction selector excluded it. Restored original bundle selection/packaging;
  no native playback proof yet. Native audio-device publication in bluetoothd does not prove CoreAudio can open it.
- **[observed, owner]** Buds remain silent and Voice Memos recording fails in the pairing boot. That boot also disabled
  built-in DSP/output/mic paths; selected recording input is unknown. Next diagnostics must distinguish route from transport.

- **[observed]** `002629`: Voice Memos selects CMF HFP recording at 00:27:33, client-format error 'what'
  for 16-bit/8-kHz/mono; synchronous HCI connection succeeds. Audio server subsequently restarts. No fatal stack
  was captured; do not equate the format error with a proven crash cause.
- **[observed, phone]** `003355`: without buds, existing AVAudioRecorder probe records 96000 non-silent samples
  in two seconds through MicrophoneBuiltIn, then Voice Memos launches without a captured exception. No audio-source
  code changed. Next reproduction requires headset pairing plus native Record under exception tracing.

- **[observed 2026-10-27]** iOS 16.5 bluetoothd routes A2DP through a Skywalk user-pipe nexus to the HAL plug-in
  (`[BTAudioSkywalk] Creating skywalk nexus`); this kernel's Skywalk (MASTER.arm64 `config_nexus_user_pipe`) carries it:
  AAC/RTP ACL leaves bluetoothd and completes (`005834`, `010727`). Heard by the owner.
- **[observed]** iOS sends voice as H4 SCO over HCI (AudioSendThread, 120-byte CVSD/16-bit packets), sets SCO flow control
  off (0x0c2f 0) and paces by itself, running up to 5 packets ahead of the controller's 4 SCO buffers; QCA6390 then holds
  CTS permanently. Pacing in the transport (chip-sized packets, <=2 ahead of the chip's own SCO) removes the stall.
- **[source]** Linux selects QCA's voice path only through Enhanced Setup Synchronous Connection's data path id
  (0 HCI, 1 non-HCI: `hci_qca.c` `qca_get_data_path_id()`); no public QCA VSC for a default SCO route was found.
- **[observed]** With buds connected, ringtone previews use a "virtual call" over HFP/SCO, not A2DP. bluetoothd waits for the
  headset's AT+CLCC/AT+BCS after `+CIEV`/`+BCS` before Setup Synchronous Connection, ~3 s limit (status 1809).
