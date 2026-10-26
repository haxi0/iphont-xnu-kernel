# Research and reverse-engineering knowledge base

Permanent record of what has been **found out** about iOS internals, Qualcomm hardware, the stock boot chain and our own tooling.
Every page tags statements **[observed]** (seen on a phone/QEMU), **[source]** (read from Linux/vendor source) or **[inferred]**;
guesses are called hypotheses. No Apple disassembly, proprietary binaries, firmware, keys or assets are stored: names, selectors, offsets and
behaviour only. Add to these pages when you learn something (see [../../AGENTS.md](../../AGENTS.md)).

| Page | Subject |
|---|---|
| [ios-userland.md](ios-userland.md) | backboardd/CoreAnimation, SpringBoard, mediaserverd, CoreBrightness, IO80211, denied daemons (CommCenter, fairplayd, App Store, vmd...), boot logo, methodology |
| [kernel-standins.md](kernel-standins.md) | the kernel replacements for IOSurface, framebuffer, KeyStore, AMFI/Sandbox, corecrypto, ramfs; protocols recovered |
| [hardware-qualcomm.md](hardware-qualcomm.md) | what crashes the phone, SMEM/SMP2P/GLINK/QRTR/AOP/TZ, SMMU, PMIC arbiter, pins, display/GPU register facts, bring-up method |
| [boot-chain-and-recovery.md](boot-chain-and-recovery.md) | XBL/ABL/UEFI/loader, XNUP container, memory, stock ABL quirks, OnePlus 8 recovery, UEFI port notes |
| [failed-approaches.md](failed-approaches.md) | dead ends and costly mistakes, one line each |
| [checks.md](checks.md) | "how our own checks lie": eleven measurement pitfalls |
| [../air.md](../air.md), [../air-report.md](../air-report.md) | **Metal/AIR → NIR shader frontend research** (userland repository): 5,000-line lab notebook and the generated corpus report. Research, not shipped functionality |

Where findings also live in code: long design comments at the top of the kernel files (`hk_adreno.c`, `hk_wifi.c`, `hk_adsp.c`, `hk_mic.c`, `hk_camera.c`,
`hk_power.c`, `hk_ufshc.c`, `apfs_write.c`, `apfs_store.c`) and userland files (`inject*.c`, `eagl.c`, `hkmetal.m`, `hkdrawable.m`, `inject_fairplay.c`,
`inject_store.c`, `mkjobs.py`). The `docs/metal.md` that `air.md` cites in seven places **does not exist in either repository** (its measurements are
only quoted in `air.md` and in source comments such as `hkdrawable.m`'s 15.46 ms figure and the `0x188...` addresses of QuartzCore).

Not preserved because it is not available in the repository: the Claude conversation transcripts and private memory of earlier sessions
beyond what was written into these pages; the sibling-project notes (`ios-instantnoodle/NOTES.md`, 1,343 lines, `ios-topaz/`, `unixnu/docs`) describe other ports
and were not mined in this pass (candidate future work).
