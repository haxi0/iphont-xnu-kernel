# Research: the kernel stand-ins for Apple's closed drivers

Open-source XNU is a kernel without Apple's platform. Everything below replaces a closed kext or a missing piece with
the least that lets the stock iOS 16.5 user space run, with the **protocol worked out from the user-space side of iOS
16.5** (selectors, structure layouts, answer encodings). Evidence tags as in [ios-userland.md](ios-userland.md).
Related: [../architecture.md](../architecture.md), [ios-userland.md](ios-userland.md).

| Stand-in | File | Replaces | Fidelity |
|---|---|---|---|
| Platform expert | `iokit/Kernel/GenericARMPE.cpp` | `AppleARMPlatform` | publishes device-tree nodes as nubs, names the machine, starts cores and the other stand-ins. Without a platform expert IOKit falls into `IOPanicPlatform` ("Unable to find driver for this platform"). No RTC driver: the time of day comes from whoever started the kernel (loader/`xl settime`) |
| `MiniIOSurface` | `iokit/Kernel/MiniIOSurface.cpp/.h` | `IOSurface.kext` | surfaces = wired kernel memory: one page of shared state clients read directly, then pixels; each client maps the whole surface once; shared as Mach send rights or by number. **Missing:** child surfaces, wrapping client memory, purgeability, compression, notifications. Unknown selectors are logged once and refused |
| `MiniIOMobileFramebuffer` | `iokit/Kernel/MiniIOMobileFramebuffer.cpp` | the display driver family under `IOMobileFramebuffer.framework` | swap/vsync/power/backlight; see [../subsystems/display.md](../subsystems/display.md) |
| `MiniAppleARMBacklight` | `iokit/Kernel/MiniAppleARMBacklight.cpp` | `AppleARMBacklight` | brightness property set; see display.md |
| `MiniAppleKeyStore` | `iokit/Kernel/MiniAppleKeyStore.cpp` | `AppleKeyStore` (keybag; real one talks to the Secure Enclave) | models **one fixed situation: a device with no passcode, never locked, unlocked since boot**. No keys exist. securityd gives each keychain item a key wrapped by the key of its protection class (`kAKSWrapKey`/`kAKSUnwrapKey`): answered, so the keychain works, but with nothing secret the "wrapping" is a reversible transformation that **protects nothing** and the keychain lives in `/private/var`. Bag creation/loading and keys kept in the store are refused (logged once). Answer format: DER `SET of SEQUENCE { UTF8String key, INTEGER value }` |
| `MiniIONetworkStack` | `iokit/Kernel/MiniIONetworkStack.cpp` | `IONetworkStack` | configd's interface namer waits for it; without it `SCNetworkInterfaceCopyAll` blocks 5 minutes (`*TIMEOUT*`). Refuses the naming request; interfaces are born named |
| `HKWLAN` | `iokit/Kernel/HKWLAN.cpp` | `IO80211Family` objects | properties only; see wifi.md |
| `HKBattery` | `iokit/Kernel/HKBattery.cpp` | the battery drivers that publish an `IOPMPowerSource` | see input-peripherals.md |
| `HKGPU` | `iokit/Kernel/HKGPU.cpp` | `IOAccelerator`/AGX | msm-DRM-shaped user client for Mesa, see gpu.md |
| AMFI | `libkern/amfi/miniamfi.c`, `miniamfi_entitlements.cpp`; `bsd/kern/kern_minimac.c` | AppleMobileFileIntegrity | registers its own function table; **no trust cache, no signature policy**. A process holds the entitlements its program's signature lists; a program without a signature holds none. Every ad-hoc-signed binary is taken for a platform binary (launchd panics otherwise); unsigned programs (ours) are not. The CoreEntitlements part of the table is empty (only the PPL pmap reaches it) |
| Sandbox | `bsd/kern/kern_minimac.c` | Seatbelt | every query "allowed", every apply "succeeds" without applying; the one real answer is which container a process is in. **Neither stand-in is a security mechanism.** MAC policy answers exactly `__mac_syscall` requests 0x5a (AMFI, dyld's policy check at every process start) and 0x60, others return `ENOPOLICY`; measured for fairplayd: AMFI request 90 = 0, Sandbox requests 2 and 4 = 0 and nothing else |
| minicrypto | `osfmk/minicrypto/` | corecrypto | MD5/SHA-1/SHA-2, HMAC, DRBG, AES (**no XTS, no GCM**); written here, Apple's corecrypto not used. A kernel NEON SHA-256 acceleration is not allowed (FP state in kernel) |
| `ramfs` | `bsd/miscfs/ramfs/ramfs.c` | a writable `/private/var` and `/tmp` | read-write filesystem in kernel memory (nodes are kernel structures; contents in individually allocated pages; page cache only for mmap); one mutex per mount, never held while touching user memory (a fault could come back as a page-in); lost at shutdown |
| `apfs` | `bsd/miscfs/apfs/` | APFS | see [../subsystems/storage.md](../subsystems/storage.md) |
| `pthread kext` | `bsd/pthread/pthread_kext_static.c` | the pthread kext | linked into the kernel |
| `kpc` | `osfmk/arm64/kpc.c` | Apple PMU counters | PMU with no counters; kpc/kperf stay linked but inert |
| `iig` stubs | `xnu-deps/iig-impl/OSObject.iig.cpp` | Apple's closed DriverKit generator `iig` | generated from the linker's list of undefined symbols (`xnu-deps/gen-iig-stubs.py`); the 217 `exported name not in import list` complaints at link are about these |

## Protocols recovered (what each user client is asked)
- **IOMobileFramebuffer:** selector 12 is `RequestPowerChange` (backboardd: 0 off, 1 on); selectors 200/201 are ours (a
  framebuffer to scale into / "scaled"); the swap request layout and the `IOMFBDisplayRefresh` properties are in the source.
- **IOSurface:** user-client selectors, traps and the layout of structures the kernel hands to `IOSurface.framework`;
  `MiniIOSurfaceCreateScanout` is ours.
- **AppleKeyStore:** selectors and the DER answers above, from `MobileKeyBag.framework` and `libaks` as seen from user
  space. `keychain_test.c` (userland) checks what the keychain answered when the stand-in did not wrap keys.
- **Backlight:** see display.md.
- **FairPlay:** nothing — the daemon never opens an IOKit client (see ios-userland.md §4).

## What the platform patch fixes (not Apple-hardware related)
`xnu/hackios.patch` also fixes what stops open XNU from building and running outside Apple: Linux makefiles, the
`BCM2837` configuration as base, bugs in abandoned code (mockfs, the non-PPL pmap, percpu on a single processor), and so
on. `export.sh` regenerates the patch **from the whole tree**, not from your changes: a tree that fell behind rolls back
others' work (symptom: kernel builds and **crashes at boot right after `Stackshot Succeeded`**). See [../BUILD.md](../BUILD.md).

## Mistakes worth not repeating
- The kernel build "normally ends with an error": `check_all_exports` (IOKit symbols in `config/`; three symbolsets; 217
  `exported name not in import list` about DriverKit RPC names). The kernel is linked by then; judge by the file's date.
- The path to the repository must not contain spaces (XNU's makefiles do not survive them).
