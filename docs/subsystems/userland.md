# iOS userland integration

The iOS 16.5 (20F66, iPhone10,3) user space runs **unmodified on disk**: not one byte of Apple's programs,
frameworks or the dyld shared cache is changed. What iOS misses is supplied by (a) kernel stand-ins for closed
drivers and (b) libraries of ours that the kernel inserts into chosen processes with `DYLD_INSERT_LIBRARIES`
(`kern.exec_inject`; dyld honours it because the AMFI stand-in says so). The userland sources belong to the
**userland repository** (`hackios-xnu-userland`, `common/ugit`); see [../architecture.md](../architecture.md) for the
two-repo layout. No Apple file is in either repository: `ios165/fetch.sh` downloads the IPSW from Apple's CDN.

iOS believes the phone is an **iPhone X** (`iPhone10,3`): the identity (`xnuloader/identity.h`) is made from an iPhone's
device tree by `xnuloader/mkidentity.py`. The screen is declared sRGB, not P3 (see [display.md](display.md)).

## Boot of the user space

1. The kernel runs `/sbin/launchd` of the big image = `stage0` (`userland/src/stage0.c`), which mounts the small image on
   `/hackios` and hands over. In a flash-root boot the iOS volume is the root and `init.c` (`flash_root`) bind-mounts the
   small image's pieces over it (e.g. `/progressui`).
2. `init` (process 1, `init.c`): mounts `/private/var` (ramfs, or the HFS+ data volume), runs `etc/rc` (normally `exec
   /sbin/launchd.ios`), checks what the kernel can do, one line per check; as a command runner it also runs a job list.
3. launchd.ios starts the stock jobs chosen by `userland/mkjobs.py` from iOS's own LaunchDaemons. `--rest` adds the
   rest of iOS (on demand: whatever starts by itself, `RunAtLoad`/`KeepAlive`, is removed so launchd runs a job only when
   a Mach service of it is asked for) and the "launch angels". The default job list is `IOS_JOBS` in `build.sh` (notifyd,
   cfprefsd, logd, containermanagerd, runningboardd, backboardd, lsd, powerd, ..., SpringBoard, wifid).
4. Standard error of every job goes to the console (no crash reporter exists: the kernel says "Failed to send exception
   EXC_CORPSE_NOTIFY" and nothing else; `systrace`, `usertrap:`, `userabort:` lines and our own fault reporters are the only record).

**The DENY list** (`mkjobs.py`): jobs never loaded because they drive missing hardware or loop. It contains `watchdog`,
`CommCenter`, `wifi`/`WiFi` (the radio daemon family; `com.apple.wifid` is added by `WIFID`), `bluetooth`, `BTServer`,
`locationd`, `softwareupdate`, `thermalmonitord`, `keybagd`, `nfcd`, `biometrickitd`, `pearld`, `camera`, `audio`,
`ReportCrash`, `usb`, `accessoryd`, `mobileassetd`, `bird`, `homed`, `routined`, `photoanalysisd`, `avconferenced`,
`com.apple.voicemail.vmd` and others. Rules recorded in that file: a daemon can misbehave *because of another entry* (cloudd
spun retrying `nsurlsessiond` because that was denied); retest before trusting an old note; compare with a control from
the same afternoon. `ALLOW="word ..."` lets one through for a trial. Deliberately **not** denied: `mediaserverd` (it is where
VideoToolbox decodes HEIC, the iOS 16 wallpaper layers), `nsurlsessiond`, `apsd`, `mobileactivationd`, `aggregated`.

## Libraries of ours (userland repo, `userland/src/`)

| Library | Process | What |
|---|---|---|
| `libhackios.dylib` (`inject.c`) | backboardd, containermanagerd | virtual HID touch/buttons/slider service from `/dev/hk-touch`; renderer selection; OpenGL ES loader (`eagl.c`); ca_vbl/ca_fps frame clock; personas disabled; a fault reporter |
| `libhackios-sb.dylib` (`inject_sb.c`) | SpringBoard | activation/setup bypass, snapshot format, vibrancy and long-press fixes, alert slider to Do Not Disturb, status-bar clock offset, battery hooks, ca_ambient, layer dump, unlock |
| `libhackios-media.dylib` (`inject_media.c`) | mediaserverd | skips portrait prewarm (crash at address 0), flashlight device, camera device (`camera_device.c`) |
| `libhackios-app.dylib` (`inject_app.c`) | Sileo, apps | gives an app a stderr and a signal-time stack under the shared-cache slide |
| `libhackios-tweaks.dylib` (`inject_tweaks.c`) | Preferences, SpringBoard | tweak loader for `/var/jb/usr/lib/TweakInject` (ElleKit is the engine) |
| `libhackios-store.dylib`, `libhackios-fairplay.dylib` | appstored, fairplayd | experiments, built only with `STOREFRONT=1` |
| `HKAudio.driver` (`audio_plugin.c`) | mediaserverd HAL | the output/mic/actuator devices, see [audio.md](audio.md) |
| `libhackios-gl.dylib` (`eagl.c`) + Mesa | backboardd | OpenGL ES, see [gpu.md](gpu.md) |
| `thermald` (`thermald.c`) | launchd job | posts `com.apple.system.thermalpressurelevel` from `kern.hackios.thermal_pressure` |
| `var_store` (`var_store.c`) | launchd job | persistence, see [storage.md](storage.md) |
| `rwtouch`, `power_test`, test programs | test jobs | scripted touches, power tests, hardware tests |

**Every address into the shared cache is for iOS 16.5 (20F66) iPhone10,3**, and every replacement is checked against what
is expected there before anything is written (`replace_method_of_kind()`: dyld interposing does not reach calls made
inside the shared cache, so methods are replaced by address). A different iOS build needs those tables re-derived.

### Tweaks and package manager

`tools/mk-jb.sh`/`jb-preinstall.py` build a Procursus bootstrap + Sileo for a `/var/jb` prefix (`JB=1`). Sileo, dpkg/apt,
tweaks and SSH are listed as working in the README. `nsurlsessiond` must run or Sileo's downloads sit on "Queued"
for ever. `/var/mobile/tmp` must exist (apps without a data container keep temporary files there).

### Job lists

`userland/rc.jobs*` (about 119 files) are scenarios run by `init`, not a shell: lines split on blanks, no quoting, no
variables, only `>> FILE` and a trailing `&`. iOS 16.5's root has **no shell** (`/bin` is `df` and `ps`); a line beginning
`/bin/sh` fails with status 127 on a flash-root boot. A program that needs a value needs an option for it, not an
environment variable. Notable: `rc.jobs.tour` (unlock and open every app of page one), `rc.jobs.installed` (the installed
boot, no diagnostics), `rc.jobs.power`, `rc.jobs.speakers/alerts/volume/mic/micquiet`, `rc.jobs.gpuiso`, `rc.jobs.metal*`,
`rc.jobs.look`, `rc.jobs.wakeperf`, `rc.jobs.fps`, `rc.jobs.maps`. `mkjobs.py` and `jobinfo.py` generate and inspect them.

## Findings about iOS behaviour here

Condensed from the source comments; the full account with offsets is in [../research/ios-userland.md](../research/ios-userland.md).

## Files and boot-args

Boot-args read by userland (via `sysctl kern.bootargs`): see [../BOOT_ARGS.md](../BOOT_ARGS.md) ("Userland" table).
Build options (`RC`, `JOBS`, `REST`, `WIFID`, `OSLOG`, `INJECT`, `STOREFRONT`, `EXTRA`, `EXTRA_SHARE`, `RC_JOBS`, `GL`, `ADSP`,
`FLASHBOOT`, `CRYPTEXES`, `PREBUILT`, `CHECK`, `SYSTEM`) are documented at the top of `userland/build.sh`; the recipe is in
[../BUILD.md](../BUILD.md).
