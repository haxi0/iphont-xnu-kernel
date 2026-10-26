# iphont: device debugging for coding agents

`./iphont` is a Python standard-library CLI for the OnePlus 8 (`instantnoodle`). It uses the system `fastboot` executable and the repository's resident-loader USB connection. It does not use Android/ADB after boot, run a network service, or require a GUI, Rust, or an MCP server.

**Validation status (2026-10-26):** physical-phone state, log snapshots, screenshots, and a short PNG capture stream have been exercised. Seven native kernel probes pass under QEMU, including event capture and denied control. The new kernel, loader, and Objective-C daemon compile; the isolated APFS image passes both readers. The owner explicitly deferred a RAM loader reload, so the new USB mailbox, touch delivery, daemon operations, and deep diagnostics remain **hardware-unverified**. Do not delete their legacy equivalents yet.

## Discover and run

```sh
./iphont --help
./iphont describe                         # JSON command/argument catalog
./iphont state
./iphont screenshot out/screen.png --scale 2
./iphont log --live --summary
./iphont log --file logs/last.log --component adreno,display --min-severity warning
nix-shell --run './iphont test host'       # includes the compiled C protocol fixture
```

Runtime requirements are Python 3 and `fastboot` (`android-tools` on NixOS). Build commands use the existing `shell.nix` and Theos toolchain. The C host fixture needs `cc`; outside `nix-shell` that check reports an explicit skip. No command contacts an internet service for debugging.

Every regular command emits one JSON object:

```json
{"schema":1,"command":"state","ok":true,"result":{},"elapsed_ms":123}
```

Native daemon failures preserve POSIX, Mach, IOKit, Foundation, or SpringBoard error domains.

Failures contain `error.code`, `error.message`, and, where available, `hint`, `errno`, and diagnostic `detail`. Exit codes: 0 success, 1 device/build/check/I/O failure, 2 invalid usage, 3 unreachable device or missing tool, 4 busy, 5 timeout, 6 refused/unsupported, 7 internal error, 130 interrupted. `screen`, `follow`, and `monitor` emit JSONL records followed by a final envelope. `log --text` deliberately emits plain text. `IPHONT_COMPACT=1` removes JSON indentation.

The loader reports its detected `board`; control and partition writes require it to match the selected profile. Entry from stock fastboot checks `FASTBOOT_PRODUCT` before packaging or booting.

Use `IPHONT_SERIAL` when multiple fastboot devices are connected; otherwise operations refuse an ambiguous selection. State reports loader/kernel-running state independently from the host run lock. `iphont wait` waits for that lock; it does not reset the phone.

## Components and ownership

| Component | Files | Responsibility |
|---|---|---|
| Host | `iphont`, `tools/iphont/{cli,link,device,agent,logs,symbols,stream,diagnostics,workflow}.py` | JSON CLI, existing fastboot protocol, files, log parsing, bounded capture, diagnostics, builds/tests |
| Loader extension | `xnuloader/iphont.c`, included from `loader.c` | Mailbox transfers and incremental kernel log reads on the existing USB endpoint |
| Kernel agent | `xnu/files/bsd/dev/iphont.c`, `bsd/sys/iphont.h` | Opt-in diagnostic thread, timestamped events, root-only `/dev/iphont`, control authorization |
| Native inspectors | `osfmk/arm64/iphont_threads.c`, `iokit/Kernel/IPhont.cpp` | XNU task/thread state and actual IOService registry objects |
| Userspace agent | `userland/src/iphontd.m`, `com.iphont.agent.plist`, `userland/build.sh` | Foundation, IOKit, Mach services, filesystem, process and application operations |

The host/kernel/loader files belong to the kernel repository. The daemon and its build/job integration belong to the userland repository (`common/ugit`). New userland files remain hidden by the whitelist until explicitly added by name. Nothing was committed or pushed as part of implementation.

The daemon job follows `/dev/iphont` with launchd `PathState` and a restart throttle; actual launchd behavior still needs device validation.

The loader remains on its existing core and shares memory with XNU. `loader_live` adds the `XNULIVE4` marker, cumulative log counter, and mailbox pointer. Existing loader 0.5 commands and older kernel layouts remain usable. `getvar:iphont` negotiates extension version 1 independently from `getvar:version`. An old loader produces `old_loader`; it never produces synthetic diagnostic results.

## Build and prepare without touching the phone

```sh
./iphont build loader
./iphont build agent
./iphont build kernel --source out/iphont/xnu-clean
./iphont prepare                           # out/iphont/loader-ram.img; never boots or flashes
```

`build kernel` without `--source` uses the shared kernel tree. Coordinate before doing that. A copied XNU tree must have its **private `BUILD/` removed** before its first build: generated `SOURCE_DIR` paths otherwise point into the original tree and defeat isolation. Do not remove another session's build directory. Existing post-link export failures are reported as `linked_with_known_export_failures`, with the actual make exit code and `export_checks` explicitly not passed. Other compiler/link/build errors fail the command.

`kernel-test KERNEL` runs the seven native read-only probes in an isolated QEMU instance. It uses a sleeping process-1 fixture and does not touch the phone.

`build images` invokes the existing image builder in the shared userland checkout; it is not an isolated build. It rebuilds artifacts, never boots or flashes. Existing compiler/image drivers remain real dependencies and are not marked replaced.

Current ignored artifacts under `out/iphont/` include the new `loader.efi`, `iphontd`, a private `root.apfs`, the OnePlus-specific `loader-ram.img`, build logs, and test results. `prepare` rebuilds BootShim only inside its own packaging directory, using the profile's OnePlus FD/base/DTB. It refuses a non-instantnoodle profile rather than using redwood defaults.

The small `root.apfs` contains the diagnostic root plus daemon; it does **not** replace the boot-root container (`flashboot.apfs`) used as resident md0 in the installed-volume workflow. Preserve that workflow. For the later approved test, use the ordinary flashboot image as resident md0 and send the new diagnostic root alongside the kernel as md1.

## Later supervised RAM validation

The owner chose **“Continue host/build checks; leave phone running”** on 2026-10-26. Do not execute this sequence until that restriction is lifted:

1. Coordinate with the session using the phone; wait for its run to finish.
2. Enter the stock bootloader using the established recovery procedure. OnePlus ABL may require unplugging/replugging USB after a reset. Never change slots, flash, or lock the bootloader.
3. `fastboot boot out/iphont/loader-ram.img` loads the new loader into RAM only.
4. Upload the usual resident flashboot image with `iphont ramdisk userland/flashboot.apfs` if needed. Confirm `iphont state` reports the intended resident image.
5. Start a bounded run with the rebuilt kernel, `--ramdisk out/iphont/root.apfs`, the device's required normal boot arguments plus `ufs=2 root_apfs=ufs_darwin iphont=1 iphont_control=1 exctrace=1 applog=1 sound_amps=0`. Keep `ufs_write`, `var_store`, and formatting flags disabled for this read-only test. Use `XL_KERNEL` to select the privately built kernel.
6. In another terminal, run `iphont agent`, `iphont test device`, then one feature at a time. Validate touch/keyboard/app delivery through screenshots and logs; queue acknowledgement alone is not proof.

`enter` returns immediately when an existing loader is already running. It never reloads that loader automatically. `IPHONT_LOADER` selects a loader artifact; otherwise the new `out/iphont/loader.efi` takes precedence when present. `stop` and `reboot` refuse any held run unless explicitly forced after coordination, including a run from the same working copy.

## Device authorization and protocol

Normal boots do not enable the agent. `iphont=1` explicitly enables kernel inspection and creates `/dev/iphont` with mode 0600. `iphont_control=1` additionally authorizes input and every userspace RPC. Treat every host physically attached during such a boot as trusted; there is no pairing database or network listener. Do not install a control-enabled image on an unattended device.

Requests are UTF-8 commands, at most 4095 bytes. `oem iphont-put OFFSET HEX` transfers bounded chunks; `iphont-go SEQUENCE LENGTH` publishes one request after checking its byte count; `iphont-get SEQUENCE` stages a completed reply. Each fastboot command fits the existing command-size limit. One request may be outstanding. Release/acquire stores publish sequence numbers across cores. The reply is little-endian `<IIiIQ>`: version, sequence, Darwin errno, payload length, device monotonic nanoseconds, followed by a JSON object. Payloads are at most 64 KiB; overflow is an error, not a truncated success.

The host holds `$XDG_RUNTIME_DIR/hackios-phone.lock` across complete command/stage/fetch transactions. It reuses `xl`'s run/who/resident-image files. `xl` log and raw-capture transactions were also fixed to keep their staged buffers intact. Shell `xl` callers remain supported while parity testing is incomplete.

A timed-out mutation has an **unknown outcome**. Its error returns the sequence; use `iphont agent-result SEQUENCE` to retrieve it. Never replay it automatically. Only the latest reply is retained: retrieve it before publishing another request. Host send/receive wall-clock bounds and device monotonic time accompany responses; they are correlation bounds, not a claimed precision clock synchronization.

## Commands and actual guarantees

The authoritative option list is `iphont describe` or `COMMAND --help`.

| Commands | What they inspect or change | Validation |
|---|---|---|
| `state`, `wait` | USB state, boot result, shared run lock, resident-image note | Physical state exercised; wait logic host-tested |
| `screenshot OUT --scale N` | BGRX pixels from `screen_now`, including the display driver's current scanout pointer/stride | Physical capture exercised, including padded stride |
| `screen DIR --seconds N --fps N --scale N` | Real PNG frames, JSONL metadata, `recording.json` with actual capture rate | Five physical captures; about 1.6 fps in that run; boot logo observed |
| `log`, `panics`, `crashes`, `checks`, `symbolize` | Saved/live console, kernel panic registers/backtraces, exception traces and init/test verdicts | Saved logs and live phone snapshots exercised |
| `agent`, `agent-result` | Version/authorization/userspace-bridge availability; exact pending outcome | Host C protocol fixture; USB hardware pending |
| `processes`, `threads PID`, `stacks NAME` | Live proc list/CPU totals; thread names, states, last saved PC/LR/SP/FP; existing frame-pointer stack sampler | Built; physical agent validation pending |
| `registry`, `properties ID` | Actual IOService objects, provider IDs, inactive/busy state, IOKit properties | Built; physical agent validation pending |
| `sysctl NAME --format TYPE`, `monitor` | Read-only kernel values; explicitly typed, selected live samples with per-field errors | Host parser/protocol tests; physical agent pending |
| `events [AFTER]`, `trace LEVEL` | Sequence/timestamped event ring; minimum severity 0 debug through 4 critical | Built; event producers listed below |
| `follow --output FILE` | Incremental console bytes and filtered JSONL; byte gaps reported | Host ring-protocol fixture; physical extension pending |
| `input`, `tap`, `swipe`, `button` | Normal input queue → existing backboardd HID code; ten supported finger IDs | Built; delivery pending; gestures timed on-device |
| `key`, `text` | Real IOHID keyboard dispatch; text uses a limited US ASCII map | Built; API has no delivery acknowledgement; keyboard routing unverified |
| `apps`, `launch BUNDLE`, `signal PID SIGNAL` | LaunchServices enumeration, SpringBoard launch request, explicit process signal | Built; framework/entitlement behavior unverified |
| `ls`, `stat`, `get`, `put` | Native file inspection/regular-file reads; bounded transfers | Built; physical agent pending; uploads cannot overwrite and stay under `/private/var/root/iphont` |
| `exec --timeout N /ABS/PATH ARGS...` | Direct `posix_spawn` argv, no implicit shell; output/time cap | Built; exit/signal/timeout/truncation remain explicit |
| `memory PID ADDRESS LENGTH` | `task_for_pid` + bounded `vm_read_overwrite` of an authorized userspace task | Built; task rights may be refused; no kernel arbitrary-memory endpoint |
| `mach NAME`, `framework PATH --symbol SYMBOL` | Bootstrap lookup/port rights; real `dlopen`/`dlsym` | Built; lookup/symbol presence is not a functional RPC test |
| `system`, `network`, `clipboard` | Mach CPU/VM counters, interface addresses/flags, system text pasteboard | Built; iOS privacy/entitlement restrictions may refuse access |
| `diagnose --output FILE`, `compare BEFORE AFTER`, `report DIR` | Dependency-aware snapshots, evidence differences, logs/build identities/screenshots | Host behavior tested; complete device evidence pending |
| `build`, `prepare`, `kernel-test`, `test host`, `test device` | Existing build drivers, RAM packaging, automated checks | Compile/package/host checks run; device smoke intentionally deferred |
| `run`, `ramdisk`, `enter`, `health`, `lastlog`, `memmap`, `capture`, `settime`, `flashboot`, `reboot`, `install`, `flashpart` | Consolidated loader operations and existing capture decoders | New workflow parity not fully verified; retain `xl` and recovery scripts |

`exec` permits only 1–15 seconds, captures at most 24 KiB of combined stdout/stderr, reports truncation, and kills its process group on timeout. It can run commands with side effects; use only explicitly authorized diagnostic commands. Pid 0/1 signals and kernel task memory reads are refused. Uploads use new names and checked offsets; downloads check size/inode/mtime across chunks and fail rather than publish a mixed file. Device JSON errors propagate as failed host results.

Screenshots are framebuffer observations, not compositor API screenshots. They may tear while scanning out. PNG capture is a recording artifact, not an encoded MP4; existing `capture recording` decodes the kernel recording with the existing converter/ffmpeg dependency. Orientation/sensor control, a delivered keyboard/text guarantee, compressed high-fps mirroring, KDP/LLDB integration, GPU queue introspection, and service-specific automated tests remain unfinished.

## Logging and diagnosis

Existing early loader/XNU console, crash traces, driver and compositor logs remain their real execution paths. `exctrace=1` enables fatal exception context; `applog=1` captures userspace stderr; the image builder's `OSLOG` selects daemons forwarded to the console. `touch_lat=1` emits the existing physical and injected input-to-display trace (`log --component tl`). These may be expensive; enable only the subsystems being measured.

Raw log snapshots are retained beside decoded logs as `.raw`; no device partition is written to capture them. The human-readable view strips NULs and decodes invalid UTF-8 with replacement. Severity is marked `severity_source: os_log` when present, otherwise `inferred`; word matching does not prove a failure. The cumulative counter reads the retained early head and moving ring without pretending all history survived. `log_gap` reports discarded bytes. Streams use synchronous output/backpressure, not an unbounded queue. Read streams retry temporary disconnects within their requested duration and emit connection/gap events; mutation requests are never replayed. A final log snapshot is attached when the kernel returns. Long capture manifests are assembled from a disk index instead of accumulating frames in memory.

Kernel events currently instrument actual process exit (`component=1, code=100`, pid and wait status), selected Mach exceptions (`component=1, code=exception`, thread ID and exception code), IOService registration **attempts** (`component=3, code=1`, entry ID/options), agent startup (`component=0, code=1`), and injected input (`component=2`). Events are paged in at most 128 records; use `next` while `more` is true. Event overwrite and contention drops are reported. Early boot enables the event ring before the userspace bridge starts. This is not yet tracing every driver/GPU/framework path.

`iphont_selftest=1` runs read-only kernel probes and a denied-control check at startup, printing `iphont-test` JSON. It is intended for kernel/QEMU integration checks and adds output only when explicitly enabled. `iphont=1` is still required.

Kernel symbolication requires a matching Mach-O UUID; unrelated current symbols are refused. Runs retain the last 20 available matching unstripped kernels under `out/iphont/kernels`. Userspace attribution currently resolves shared-cache image/offset only and explicitly reports function symbolication unavailable. Use the existing `ipsw dyld a2s`/LLVM tools for function names until integrated.

Diagnostics return direct evidence and errors for independent layers. `registered`, `present`, `observed`, and `responsive` have distinct meanings. A registered framebuffer is not proof of GPU acceleration; a backboardd process is not proof of successful presentation. A failed dependency is recorded alongside the direct failure. Snapshot comparisons do not label naturally changing counters as performance regressions.

## Extending and validating

Add a narrow read operation in the existing kernel dispatcher or a userspace operation through `/dev/iphont`; do not add a second USB stack, listener, or plugin framework. Validate sizes and authorization on-device, preserve error domains, bound responses, and make write side effects explicit. `iphont_event(component, level, code, a, b)` is interrupt-safe and never waits for another CPU; a contending event is counted as dropped.

Run `iphont test host`, compile the touched native components, and run the existing `tools/t host` checks. After owner-approved RAM loading, run `iphont test device`; inspect each returned error instead of retrying automatically. Test visual/input changes against the observed phone. Native compilation and C protocol fixtures never substitute for that step.

The [migration inventory](TOOLKIT-MIGRATION.md), [TODO](TODO.md), and [handoff](HANDOFF.md) record retained infrastructure and remaining acceptance work.
