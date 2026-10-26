# Devices

| Device | Doc | Role | State |
|---|---|---|---|
| OnePlus 8 (instantnoodle, SM8250) | [instantnoodle.md](instantnoodle.md) | **active development**; iOS root on its own flash, no Android | SpringBoard, Wi-Fi, audio, sleep, installed boot work (see doc) |
| POCO X5 Pro 5G (redwood, SM7325) | [redwood.md](redwood.md) | where the port was born; owner asked not to run redwood checks | works per the project; not re-verified recently |
| QEMU `virt` | [qemu.md](qemu.md) | no phone needed; CI-like scenarios | works |

Profiles: `common/devices/{instantnoodle,redwood,qemu-virt}.conf` (+ `_template.conf`, documented). `common/hackios devices` lists them.
A kernel binary serves every phone with the same SoC blocks: addresses default to redwood's and `board=` / `board_<block>=` select others
([../architecture.md](../architecture.md#device-specific-code)). Porting a new phone: [../PORTING.md](../PORTING.md).
Cross-device feature matrix:

| Feature | redwood | instantnoodle | QEMU |
|---|---|---|---|
| CPU cores / governor | 8 / yes | 8 / yes | up to 8 / no |
| display | 120 Hz video panel | 90 Hz command panel, MDP push | ramfb |
| GPU | Adreno 642L, GL only | Adreno 650, GL + experimental Metal | none |
| touch | Goodix/FocalTech, polled | S6SY761, interrupt | virtio |
| Wi-Fi | WCN6750 | QCA6390 PCIe | virtio-net |
| speakers / mic | AW88261 / analog | TFA9874 / digital | virtio-sound |
| haptics / torch | yes / yes | yes / yes | no |
| cameras | yes | no | no |
| system sleep | no | yes | test only |
| installed boot | dual boot (Android kept) | standalone | scenario tests |
| battery | DSP battery manager | bq27541 gauge | none |
