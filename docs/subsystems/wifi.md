# Wi-Fi

Both phones run Linux's **ath11k** driver, compiled unmodified, on a compatibility layer (`wifi/lx/`), under the
station, `en0` and Apple80211 layers described below. The radios differ: redwood's is a WCN6750 inside the SoC,
the OnePlus 8's is a QCA6390 on PCIe. Bluetooth (the same chip) is not touched.

Status (2026-10-26):

| | redwood (WCN6750) | instantnoodle (QCA6390) |
|---|---|---|
| join from Settings, DHCP, Safari | verified on the phone (55-90 Mbit/s) | verified on the phone (RAM-only runs, 2026-10-25) |
| networks joined so far | not recorded in detail (README: "join from Settings, Safari over Wi-Fi"); not re-checked since the OnePlus work | WPA2 hotspot (5 GHz ch 44), WPA2 "mixed" mesh with TKIP group cipher, WPA3-only hotspot (SAE by hash-to-element, PMF) |
| sleep/wake with the radio on | not applicable (no system sleep) | built (`hk_power_will_sleep`), **not tried on the phone** |
| auto-join across reboots | not checked | impossible in RAM-only runs (`/private/var` is new every boot); needs a run with `var_store=1 ufs_write=1` |
| 802.11r (FT) | not done | not done (iOS is told the card has no FT) |

## Layers

```
Settings / wifid / configd                     (iOS)
   |  SIOCGA80211 / SIOCSA80211 ioctls on any socket (apple80211req)
bsd/dev/hk_apple80211.c    Apple80211 requests answered from the station boundary; events to wifid (class 6 subclass 1)
bsd/dev/hk_sta.c           the station boundary (sys/hk_sta.h): one thread does scans/joins; remembers the last scan
bsd/dev/hk_net.c           en0 on the kernel's interface KPI (no IONetworkingFamily), Ethernet framing
iokit/Kernel/HKWLAN.cpp    registry entries wifid and SystemConfiguration look for (IO80211Controller, IONetworkInterface)
iokit/Kernel/MiniIONetworkStack.cpp   the service configd's interface namer waits for
bsd/dev/hk_lx.c  +  wifi/lx/   the compatibility layer (a FreeBSD LinuxKPI-derived layer plus ours) ath11k builds on
ath11k (Linux 7.0.14, tools/mk-ath11k.sh)    mac80211 station, firmware conversation over QMI
bsd/dev/hk_qmi.c, hk_wlfw.c    QMI (Linux's interface, our code) and a conversation stand-in (see below)
bsd/dev/hk_wifi.c          the radio itself: SMEM/SMP2P/GLINK/QRTR, firmware loading through TrustZone (redwood)
bsd/dev/instantnoodle_wifi.c   PCIe 0 bring-up (instantnoodle)
```

Everything above `hk_wifi.c` is shared by the two phones.

### How iOS reaches a driver without IO80211Family

wifid talks Apple80211 over ioctls whenever the driver offers no user client. `HKWLAN.cpp` deliberately gives the
interface service **no user client**: the framework's attempt to open one fails, it logs "Falling back to IOCTL
path", and from then on every request is an ioctl answered by `hk_apple80211.c`. SystemConfiguration lists interfaces
that are `IONetworkInterface` objects and calls one Wi-Fi when its parent is an `IO80211Controller`; configd makes a
network service (DHCP, routing) only for a listed interface, and wifid waits for that ("failed to become routable
interface") before Settings ticks the network. The interface namer in configd waits for `IONetworkStack`; without
`MiniIONetworkStack` SCNetworkInterfaceCopyAll blocks for five minutes and SpringBoard blocks behind wifid.

A join: `ASSOCIATE` (20) carries the network name, perhaps the access point, and the passphrase as typed; it is
answered at once, the station joins on its own thread, event 9 reports the outcome, and `ASSOCIATE_RESULT` (21)
gives a result number the framework turns into its errors (1 success, 21 invalid PMK = wrong passphrase, 9 timeout,
2 anything else). Request types and layouts are Apple's; where iOS 16.5 differs the layout was read out of
`IO80211.framework`. The log shows every request (never what it carries).

The **capability bits** wifid reads (`CARD_CAPABILITIES`) matter a great deal. iOS's `_performAssociation` fails with
`-3903` before ASSOCIATE if the card lacks TKIP while the network offers it. Bits set on the OnePlus 8: 1 (TKIP),
2, 3, 6, 9, 10, 12 (TKIPMIC), 14, 17, 19, 52 (PMF), 75 (SAE); FT (33) is deliberately unset. The struct layout was
read from iOS 26's `IO80211` (see [../research/ios-userland.md](../research/ios-userland.md)).

## Redwood: WCN6750 on the WPSS

The radio's firmware runs on a Hexagon DSP ("WPSS") that stays in reset until loaded. `hk_wifi.c` is built as steps
(`wifi_step=N` performs steps 1..N, reads before writes): SMEM, SMP2P, the IPCC doorbells, the AOP mailbox
(told the image is being loaded before it honours the DSP's requests for power), RPMh supplies, pins, the signed
firmware (`wpss.mdt`, `.b02`..`.b07`) loaded and authenticated through TrustZone (image 6), then GLINK and QRTR.
`wifi=1` is all of it on a thread beside the system's start. Rules found: `wifi_again=1` (start the DSP although the
previous one was shut down without acknowledging a stop) **has reset the phone twice out of two tries**; do not.

## OnePlus 8: QCA6390 on PCIe 0

`instantnoodle_wifi.c` powers the radio (RPMh rails pm8150 S6/S5, pm8009 S2, pm8150a S8/L5, then `WLAN_EN` gpio20),
brings PCIe 0 up (GDSC `0x6b004`, clocks, PHY sequence of 147 writes from the device tree, `PERST` gpio79, `CLKREQ`
gpio80 function 1, link training to Gen2, iATU, BAR 0 at `0x60300000`, 1 MB), and answers configuration space.
**A register of PCIe 0 must not be read before its domain and clocks are on: that is a NoC error and CrashDump.**
ath11k's own `pci.c`, `pcic.c` and `mhi.c` then run on it. The MHI host is ours (`wifi/lx/src/mhi_host.c`, written
from the protocol and OpenBSD's qwx(4) (ISC) description; no code from Linux's GPL MHI host), tested against a
simulated device by `tools/mhi-test/run.sh`. QMI goes over the device's IPCR channel (MHI channels 20/21) into the
kernel's QRTR (`lx_mhi.c`); from there it is redwood's stack.

Hard-won facts (all verified on the phone, 2026-10-25):

1. **Android's firmware asserts under ath11k** (`image/qca6390/amss20.bin`, WLAN.HST.1.0.1.r1-00805) at the first mode
   request. Use linux-firmware's `ath11k/QCA6390/hw2.0/amss.bin` + `m3.bin` (WLAN.HST.1.0.1-05266 "V2_TO_X86": the build
   made for hosts that run ath11k) with the OnePlus's board data `image/12wlan.b0c`.
2. PCI coherent DMA memory must be aligned to a power of two at least its size (a Linux guarantee; `lx_base.c`
   `linux_dma_alloc_coherent`); without it the firmware crashes.
3. MHI: wait for the device's mission execution-environment event and the M0 state before starting channels (the
   register says "mission" earlier; a command sent then is lost); re-ring doorbells every 100 ms.
4. SMMU stream **0x1c01 with mask 0**, not 0x1c00 with mask 1; `RADIO_TABLES` 48.
5. **Mixed-mode networks**: a WPA2 mesh whose group cipher is TKIP (pairwise TKIP+CCMP) failed with `-3903` before
   ASSOCIATE because the capability bits lacked TKIP *and* the station refused a TKIP group key. Real TKIP group
   support was added (key_set TKIP with the 32-byte key's Michael halves swapped as wpa_supplicant does, group key
   length check, Michael in `lx_wpa.c` plus `wifi/lx/include/crypto/hash.h` replacing LinuxKPI's NULL stub, MIC-failure
   report and 60 s countermeasures). 802.11r (FT, AKM 4) was **not** the cause; a 422 refusal in the log is harmless.
6. The station checks message 3's RSN element and RSNXE against the beacon's.

Firmware layout: `fw-wifi/amss.bin`, `m3.bin`, `12wlan.b0c`, `mac` (the radio's address, one line: from the phone's
`persist` partition `qcom/wlan_mac.bin`; **never print it**) and optionally `network` (name and passphrase on two
lines, joined at the start). `userland/build.sh` puts them in the small image (`fw/`; `SHARE=1` leaves out `mac` and
`network`); the kernel takes the address from `fw/mac` when no `wifi_mac=` is given. Android's own firmware files are
kept in `fw-wifi/android/`. The extraction commands are in [../devices/instantnoodle.md](../devices/instantnoodle.md).

## The station (shared)

Implemented in `wifi/lx/src` (`lx_80211.c`, `lx_wpa.c`, `lx_sae.c`) and tested on the computer by
`tools/wpa-test/run.sh` against an independent Python access point and IEEE 802.11-2020 Annex J.10's vectors:

- open networks; WPA2 with a passphrase (PSK and PSK-SHA256);
- CCMP between station and AP, CCMP or TKIP to everybody (Michael, MIC-failure report and the 60-second countermeasures);
- WPA3 SAE on group 19 by hunting-and-pecking or by hash-to-element (with the RSNXE and the anti-clogging token
  container when the AP asks for a token);
- protected management frames (BIP-CMAC-128, SA Query) wherever both sides can;
- block ack (`wifi_ba`), wide channels (`wifi_wide`), interrupts (`wifi_irq`): all on with `wifi=1`.

**Not done:** 802.11r (FT) roaming; TKIP between station and AP (the first WPA); WPA3's other groups and password
identifiers; roaming. Built and host-tested but **not seen on the phone**: iOS's per-network private Wi-Fi address
(`ASSOCIATE` offset `0x1fc` -> `hk_sta_join.address` -> `station_address()` -> `hk_net_address` sets en0's link-layer
address), AKM 6, the RSNE/RSNXE downgrade check, WPA3 transition mode (the hotspot never actually switched).

For sleep the radio is taken down (network left, firmware stopped, link off) and brought back after the wake; a
firmware crash is recovered by ath11k's reset and the station's restart and iOS rejoins. Neither has been run through
a real system sleep on the phone.

## `hk_wlfw.c`: a stand-in to be removed

`hk_wlfw.c` is "A STAND-IN, TO BE THROWN AWAY when ath11k's own qmi.c runs here": a hand-made exchange with the Wi-Fi
firmware service used to learn from the phone before the real driver was wired. With `wifi_lx=1`/`2` step 15 is
ath11k's own. What the phone said in the stand-in's time (2026-10-13, firmware WLAN.MSL.1.0.1-01277): every request
is answered with success; the regulatory database goes down in 4 pieces and `bd_m20gl.elf` in 10; "init done" comes
283 ms after the first request; the firmware gives no board id (ath11k then takes 0xff), chip id 1 of family 0xb, the
radio's registers at `0x61e00000`, 2 MB; it does not say "ready" until a mode is requested; asked for cold-boot
calibration it says "calibration done" 6.7 s later; the mode "off" after that is refused (error 48); with
`wifi_mission=1` logging, configuration (without the shadow-register list) and mode "mission" are all taken (342 ms)
and the radio's registers answer. The message tables are ath11k's own, extracted when the kernel is built
(`tools/mk-wlfw.sh`).

## Debugging

- `wifi_loud=1` prints the whole bring-up. `OSLOG="com.apple.backboardd com.apple.wifid"` (userland image build) shows
  wifid's syslog ("authSel", "Failed to associate: -3903"). `hk_apple80211` prints a request trail at the run's end
  (`xl stop`).
- Live log `logs/*-run.log.live` is a rolling snapshot: early lines scroll off; grep while the run is going.
- Host tests: `nix-shell -p gcc --run 'bash tools/mhi-test/run.sh'`; `tools/wpa-test/run.sh`; `tools/qmi-test/run.sh`.
- Building the driver: `nix-shell --run 'tools/mk-ath11k.sh && SKIP="loader images boot" common/build.sh instantnoodle'`
  (no clang outside `nix-shell` on the NixOS host). `tools/mk-ath11k.sh` fetches Linux 7.0.14 and FreeBSD 15.1 sources
  at pinned hashes; none are in the repository.
- The project's rule from the owner (2026-10-25): do not run POCO (redwood) checks.

## Files

`xnu/files/bsd/dev/hk_wifi.c`, `instantnoodle_wifi.c`, `hk_lx.c`, `hk_qmi.c`, `hk_wlfw.c`, `hk_sta.c`, `hk_apple80211.c`,
`hk_net.c`, `hk_virtio_net.c`; `xnu/files/iokit/Kernel/HKWLAN.cpp`, `MiniIONetworkStack.cpp`; `wifi/lx/src/*`,
`wifi/lx/patches/*`; `tools/mk-ath11k.sh`, `tools/mk-wlfw.sh`, `tools/mhi-test`, `tools/wpa-test`, `tools/qmi-test`.
Licenses: [../../prebuilt/NOTICES.md](../../prebuilt/NOTICES.md).
