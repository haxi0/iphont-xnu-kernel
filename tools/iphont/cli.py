"""
iphont: the phone, for coding agents. Every command prints one JSON object on stdout:

  {"ok": true,  "command": "...", "result": {...}, "elapsed_ms": N}
  {"ok": false, "command": "...", "error": {"code": "...", "message": "...", "hint": "...", "detail": {...}}, ...}

and exits 0 (ok), 1 (a check or the run failed), 2 (usage), 3 (phone not reachable / not in the needed state),
4 (phone or link busy), 5 (timeout), 6 (refused / unsupported by this loader or kernel), 7 (internal error).
Progress goes to stderr. `iphont --help` lists the commands; docs/TOOLKIT.md describes each one.
"""
import argparse, json, os, re, sys, time, traceback

from . import agent, device, diagnostics, link, logs, stream, symbols, workflow
from .errors import Fail

ROOT = device.ROOT


def _log_text(args):
    """The log a command reads: --file F, else the live one (fetched now), else logs/last.log."""
    if getattr(args, 'file', None):
        f = args.file if os.path.isabs(args.file) else os.path.join(os.getcwd(), args.file)
        if not os.path.isfile(f):
            raise Fail('not_found', f'{f} not found')
        return open(f, errors='replace').read(), f
    if getattr(args, 'live', False):
        os.makedirs(device.path('logs'), exist_ok=True)
        f = device.path(f'logs/iphont-live-{os.getpid()}-{time.time_ns()}.log')
        return device.fetch_log(f), f
    f = device.path('logs/last.log')
    if not os.path.exists(f):
        raise Fail('not_found', 'no logs/last.log yet and no run in progress', hint='iphont run, or --file')
    return open(f, errors='replace').read(), os.path.realpath(f)


def cmd_state(a):
    r = device.info()
    r['resident_image'] = device.resident()
    return r


def cmd_enter(a):
    return device.enter()


def cmd_run(a):
    def progress(text):
        tail = [l for l in text.rstrip().split('\n')[-1:]]
        print(f'iphont: run: {len(text)} bytes of log; last: {tail[0][:120] if tail else ""}', file=sys.stderr)
    r = device.run(secs=a.secs, bootargs=(a.bootargs if a.bootargs is not None else device.profile().get('BOOTARGS', '')) + (' ' + a.extra_bootargs if a.extra_bootargs else ''), done=a.done,
                   ramdisk_image=a.ramdisk, follow=a.follow, keep_live=a.keep_live, checkpoint=a.checkpoint,
                   on_progress=progress if not a.quiet else None)
    s = r['summary']
    if s['panics'] or s['init']['failed'] or any(not v['pass'] for v in s['marks']['verdicts']):
        r['verdict'] = 'failed'
        raise Fail('run_failed', f"the run ended '{r['result']}' with {s['panics']} panic(s), {s['init']['failed']} failed "
                   f"init check(s)", hint='iphont panics / iphont log --min-severity error', detail=r)
    r['verdict'] = 'completed'
    r['boot_validated'] = s['init']['summary_seen'] and not s['init']['failed']
    return r


def cmd_stop(a):
    return device.stop(force=a.force)


def cmd_wait(a):
    if not link.wait_free(a.seconds):
        raise Fail('timeout', f'a run started from {link.run_held()} still holds the phone after {a.seconds} s')
    return {'free': True}


def cmd_log(a):
    text, src = _log_text(a)
    recs = logs.lines(text)
    if a.summary:
        return {'source': src, **logs.summary(text), 'components': logs.components(recs)}
    sel = list(logs.select(recs, component=a.component, min_severity=a.min_severity, grep=a.grep, section=a.section,
                           since=a.since))
    total = len(sel)
    if a.tail is not None:
        if a.tail < 0: raise Fail('usage', 'tail must be nonnegative')
        sel = sel[-a.tail:] if a.tail else []
    if a.head is not None:
        if a.head < 0: raise Fail('usage', 'head must be nonnegative')
        sel = sel[:a.head]
    if a.text:
        for r in sel:
            print(r['text'])
        sys.exit(0)
    return {'source': src, 'matched': total, 'returned': len(sel), 'last_line': len(recs), 'lines': sel}


def cmd_follow(a):
    """Stream the live log as JSON lines (one object per new log line) until the run ends or --seconds pass."""
    for record in stream.follow(a.seconds, a.interval, a.component, a.min_severity, a.grep, a.output):
        print(json.dumps(record), flush=True)
    return {'stream_ended': True}


def cmd_panics(a):
    text, src = _log_text(a)
    return {'source': src, 'panics': [symbols.symbolize_panic(p) for p in logs.panics(text)]}


def cmd_crashes(a):
    text, src = _log_text(a)
    return {'source': src, 'crashes': [symbols.symbolize_crash(c) for c in logs.crashes(text)]}


def cmd_checks(a):
    text, src = _log_text(a)
    return {'source': src, 'init': logs.checks(text), **logs.marks(text)}


def cmd_screenshot(a):
    out = a.out or device.path('logs/screen.png')
    return device.screenshot(out, scale=a.scale)


def cmd_ramdisk(a):
    return device.ramdisk(a.image or device.path('userland/system.apfs'))


def cmd_health(a):
    return device.health()


def cmd_lastlog(a):
    return device.lastlog(a.out or device.path('logs/lastlog.txt'))


def cmd_settime(a):
    return device.settime()


def cmd_memmap(a):
    return device.memmap()


def cmd_capture(a):
    default = {'recording': 'logs/recording.mp4', 'sound': 'logs/sound.wav', 'camera': 'logs/camera'}[a.kind]
    return device.fetch_capture(a.kind, a.out or device.path(default))


def cmd_install(a):
    return device.install(a.bundle, authorize=a.authorize)


def cmd_flashpart(a):
    return device.flashpart(a.part, a.image, a.expect, authorize=a.authorize)


def cmd_flashboot(a):
    return device.flashboot(a.secs)


def cmd_reboot(a):
    return device.reboot(to=a.to, force=a.force)


def cmd_symbolize(a):
    if a.kernel:
        k = symbols.kernel_symbolizer(a.text_base, symbols.kernel_for_uuid(a.uuid) if a.uuid else symbols.KERNEL_UNSTRIPPED)
        if k is None:
            raise Fail('not_found', 'no unstripped kernel for that UUID')
        return {'symbols': [{'address': x, 'symbol': k(x)} for x in a.addresses]}
    u = symbols.user_symbolizer(a.slide or '0x0')
    if u is None:
        raise Fail('not_found', 'no dyld shared cache under ios165/')
    return {'symbols': [{'address': x, 'where': u(x)} for x in a.addresses]}


def cmd_agent(a):
    return agent.request('hello')


def cmd_agent_result(a):
    return agent.request('result', timeout=a.timeout, sequence=a.sequence)


def cmd_sysctl(a):
    return agent.sysctl(a.name, a.format)


def cmd_query(a):
    return agent.request(a.command if a.command in ('processes', 'registry') else f'{a.command} {a.target}')


def cmd_input(a):
    return agent.input_event(a.kind, a.finger, a.x, a.y)


def cmd_tap(a):
    return agent.gesture([(a.x, a.y)], a.duration, a.finger)


def cmd_swipe(a):
    if not 1 <= a.steps <= 120:
        raise Fail('usage', 'steps must be in 1..120')
    return agent.gesture([(round(a.x1 + (a.x2 - a.x1) * i / a.steps),
                           round(a.y1 + (a.y2 - a.y1) * i / a.steps)) for i in range(a.steps + 1)],
                          a.duration, a.finger)


def cmd_button(a):
    button = {'lock': 1, 'volume-up': 2, 'volume-down': 3, 'home': 4}[a.button]
    if not 0 <= a.duration <= 10:
        raise Fail('usage', 'button duration must be in 0..10 seconds')
    return agent.request(f'press {button} {round(a.duration * 1000)}')


def cmd_user(a):
    args = vars(a).copy()
    for key in ('command', 'fn', 'operation'): args.pop(key, None)
    return agent.user(a.operation, **{k: v for k, v in args.items() if v is not None})


def cmd_screen(a):
    for record in stream.frames(a.directory, a.seconds, a.fps, a.scale):
        print(json.dumps(record), flush=True)
    return {'directory': os.path.abspath(a.directory), 'manifest': 'recording.json'}


def cmd_diagnose(a):
    result = diagnostics.snapshot()
    if a.output:
        with open(a.output, 'w') as f: json.dump(result, f, indent=2)
    if 'agent_error' in result:
        raise Fail('unsupported', 'deep diagnostics require the device agent', detail=result)
    return result


def cmd_compare(a):
    with open(a.before) as f: before = json.load(f)
    with open(a.after) as f: after = json.load(f)
    return diagnostics.compare(before, after)


def cmd_report(a):
    return diagnostics.report(a.directory)


def cmd_get(a):
    import base64, tempfile
    target = os.path.abspath(a.local)
    if os.path.exists(target): raise Fail('refused', 'download destination already exists')
    offset, size, identity = 0, None, None
    temp = tempfile.NamedTemporaryFile(prefix='.iphont-', dir=os.path.dirname(target), delete=False)
    try:
        with temp:
            while size is None or offset < size:
                result = agent.user('read', path=a.remote, offset=offset, length=24000)['data']
                if not result.get('ok'): raise Fail('device_error', 'remote file read failed', detail=result)
                chunk = base64.b64decode(result['base64'], validate=True)
                current = (result['size'], result.get('inode'), result.get('mtime_ns'))
                if identity is not None and current != identity:
                    raise Fail('device_error', 'remote file changed during download')
                identity = current
                size = result['size']
                if not chunk and offset < size: raise Fail('device_error', 'file changed or read made no progress')
                temp.write(chunk); offset += len(chunk)
        os.link(temp.name, target)
    finally:
        os.unlink(temp.name)
    return {'file': target, 'bytes': offset}


def cmd_put(a):
    import base64
    offset, result = 0, None
    with open(a.local, 'rb') as f:
        while True:
            chunk = f.read(1536)
            if not chunk and result is not None: break
            result = agent.user('write', name=a.name, offset=offset, base64=base64.b64encode(chunk).decode())['data']
            if not result.get('ok'): raise Fail('device_error', 'upload failed; partial file retained', detail=result)
            offset += len(chunk)
            if not chunk: break
    return {'path': result['path'], 'bytes': offset}


def cmd_describe(a):
    commands = []
    for action in parser()._actions:
        if isinstance(action, argparse._SubParsersAction):
            for name, sub in action.choices.items():
                commands.append({'name': name, 'description': sub.description,
                    'arguments': [{'name': arg.dest, 'flags': arg.option_strings, 'required': arg.required,
                                   'choices': list(arg.choices) if arg.choices is not None else None}
                                  for arg in sub._actions if arg.dest != 'help']})
    return {'schema': 1, 'commands': commands,
            'agent_bootargs': 'iphont=1 iphont_control=1 exctrace=1 applog=1',
            'limits': ['no automatic flashing', 'control opt-in per development boot', 'see docs/TOOLKIT.md for validation status']}


def cmd_build(a):
    return workflow.build(a.component, a.source, a.timeout)


def cmd_test(a):
    return workflow.tests(a.level)


def cmd_text(a):
    # USB HID usages for a US keyboard. Reject unsupported characters before sending any keys.
    mapping = {chr(ord('a') + i): (4 + i, False) for i in range(26)}
    mapping.update({chr(ord('A') + i): (4 + i, True) for i in range(26)})
    mapping.update({c: (30 + i, False) for i, c in enumerate('1234567890')})
    mapping.update({' ': (44, False), '\n': (40, False), '\t': (43, False)})
    for c, usage in zip('-=[]\\;\'`,./', [45,46,47,48,49,51,52,53,54,55,56]): mapping[c] = (usage, False)
    for c, usage in zip('!@#$%^&*()', range(30,40)): mapping[c] = (usage, True)
    if any(c not in mapping for c in a.text): raise Fail('unsupported', 'text supports the US ASCII keyboard map only')
    for c in a.text:
        usage, shift = mapping[c]
        primary, release_errors = None, []
        try:
            if shift: agent.user('key', usage=225, down=True)
            agent.user('key', usage=usage, down=True)
        except BaseException as error:
            primary = error
        finally:
            for key in ([usage, 225] if shift else [usage]):
                try: agent.user('key', usage=key, down=False)
                except Fail as error: release_errors.append(error.as_json())
        if primary:
            if isinstance(primary, Fail) and release_errors:
                primary.detail = {'original': primary.detail, 'release_errors': release_errors}
            raise primary
        if release_errors:
            raise Fail('device_error', 'key release failed', detail={'release_errors': release_errors})
    return {'characters_dispatched': len(a.text), 'note': 'IOHID dispatch provides no delivery acknowledgement'}


def cmd_monitor(a):
    any_values = False
    for record in stream.monitor(a.seconds, a.interval, a.field):
        any_values = any_values or bool(record.get('values'))
        print(json.dumps(record), flush=True)
    if not any_values: raise Fail('no_answer', 'no monitor probe succeeded')
    return {'monitor_ended': True}


def cmd_prepare(a):
    return workflow.prepare()


def cmd_kernel_test(a):
    return workflow.kernel_test(a.kernel, a.seconds)


class JsonParser(argparse.ArgumentParser):
    def error(self, message):
        raise Fail('usage', message)


def parser():
    p = JsonParser(prog='iphont', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command')

    def add(name, fn, help):
        s = sub.add_parser(name, help=help, description=help)
        s.set_defaults(fn=fn)
        return s

    add('state', cmd_state, 'where the phone is: absent / fastboot / loader; the run in progress, the resident image')
    add('enter', cmd_enter, 'get the phone into the resident loader from the stock fastboot (fastboot boot; flashes nothing)')
    s = add('run', cmd_run, 'boot the built kernel + userland from RAM, follow it, fetch and summarize its log')
    s.add_argument('--secs', type=int, help='longest the run may take (default: the profile\'s DEFAULT_SECS)')
    s.add_argument('--bootargs', help='the whole boot-args (default: the profile\'s BOOTARGS)')
    s.add_argument('--extra-bootargs', default='', help=argparse.SUPPRESS)
    s.add_argument('--done', help='regular expression: end the run as soon as the log matches (e.g. "t-mark: done")')
    s.add_argument('--ramdisk', help='the small image sent with the kernel (default userland/root.apfs; none)')
    s.add_argument('--follow', type=int, help='seconds between live log fetches (default 2 with --done)')
    s.add_argument('--keep-live', action='store_true', help='keep every live snapshot (logs/*-run.log.live.N)')
    s.add_argument('--checkpoint', type=int, help='stop at kernel checkpoint N')
    s.add_argument('--quiet', action='store_true')
    s = add('stop', cmd_stop, 'end the run in progress (the kernel returns to the loader with its log)')
    s.add_argument('--force', action='store_true', help='end a run started from another working copy (user\'s word only)')
    s = add('wait', cmd_wait, 'wait until no run holds the phone (do this before every run)')
    s.add_argument('seconds', nargs='?', type=int, default=900)

    def log_args(s, filters=True):
        s.add_argument('--file', help='a saved log instead of the live one / logs/last.log')
        s.add_argument('--live', action='store_true', help='fetch the log of the run in progress')
        if filters:
            s.add_argument('--component', help='comma list: adreno,ufs,display,... (see --summary)')
            s.add_argument('--min-severity', choices=logs.LEVELS)
            s.add_argument('--grep', help='regular expression on the line')
            s.add_argument('--section', help='comma list: loader,kernel,init,rc,jobs,msgbuf,panicbuf')
            s.add_argument('--since', type=int, help='only lines after this line number')
    s = add('log', cmd_log, 'the log as records (line, section, component, severity), filtered')
    log_args(s)
    s.add_argument('--tail', type=int)
    s.add_argument('--head', type=int)
    s.add_argument('--summary', action='store_true', help='counts by severity and component, panics, crashes, init checks')
    s.add_argument('--text', action='store_true', help='plain lines instead of JSON')
    s = add('follow', cmd_follow, 'stream the live log as JSON lines until the run ends')
    s.add_argument('--component')
    s.add_argument('--min-severity', choices=logs.LEVELS)
    s.add_argument('--grep')
    s.add_argument('--interval', type=float, default=2)
    s.add_argument('--seconds', type=int, default=900)
    s.add_argument('--output', help='persist raw incremental console bytes')
    log_args(add('panics', cmd_panics, 'kernel panics in the log, backtraces symbolized with the matching kernel'), False)
    log_args(add('crashes', cmd_crashes, 'user-space crashes in the log (exctrace=1), located in the shared cache'), False)
    log_args(add('checks', cmd_checks, 'init\'s checks and the tests\' t-mark/t-*: PASS|FAIL lines'), False)
    s = add('screenshot', cmd_screenshot, 'the frame the panel shows, as a PNG (works during a run)')
    s.add_argument('out', nargs='?')
    s.add_argument('--scale', type=int, default=1, help='keep every Nth pixel (2: 540x1200)')
    s = add('ramdisk', cmd_ramdisk, 'upload the system image into the resident area (default userland/system.apfs; none)')
    s.add_argument('image', nargs='?')
    add('health', cmd_health, 'the installed phone\'s boot slots and boot guard, as the loader reads them (writes nothing)')
    s = add('lastlog', cmd_lastlog, 'the log the previous start of the loader left (after a reset, a flash boot)')
    s.add_argument('out', nargs='?')
    add('settime', cmd_settime, 'the computer\'s date into the installed phone')
    add('memmap', cmd_memmap, 'the firmware\'s memory map')
    s = add('capture', cmd_capture, 'what the last run recorded: recording (rec_secs=N), sound (rec_sound=N), camera (cam=3)')
    s.add_argument('kind', choices=['recording', 'sound', 'camera'])
    s.add_argument('out', nargs='?')
    s = add('install', cmd_install, 'WRITES FLASH: a boot bundle into the installed phone\'s other boot slot (read back)')
    s.add_argument('bundle')
    s.add_argument('--authorize', action='store_true', help='only after the user said yes to this write')
    s = add('flashpart', cmd_flashpart, 'WRITES FLASH: an image into boot_a/boot_b, only if its SHA-256 now is EXPECT')
    s.add_argument('part')
    s.add_argument('image')
    s.add_argument('expect')
    s.add_argument('--authorize', action='store_true', help='only after the user said yes to this write')
    s = add('flashboot', cmd_flashboot, 'the installed boot from the loader (menu, newest good bundle), no reset')
    s.add_argument('--secs', type=int, default=900)
    s = add('reboot', cmd_reboot, 'reset the phone (bootloader: stock fastboot; recovery: TWRP)')
    s.add_argument('to', nargs='?', choices=['bootloader', 'recovery'])
    s.add_argument('--force', action='store_true')
    s = add('symbolize', cmd_symbolize, 'addresses to names: --kernel (unstripped kernel) or a user process (--slide)')
    s.add_argument('addresses', nargs='+')
    s.add_argument('--kernel', action='store_true')
    s.add_argument('--text-base')
    s.add_argument('--uuid')
    s.add_argument('--slide')
    add('describe', cmd_describe, 'machine-readable command catalog, arguments, and limits')
    add('agent', cmd_agent, 'negotiate the live kernel agent and report authorization')
    s = add('agent-result', cmd_agent_result, 'retrieve an uncertain request by sequence without replaying it')
    s.add_argument('sequence', type=int); s.add_argument('--timeout', type=float, default=20)
    s = add('sysctl', cmd_sysctl, 'read an actual XNU sysctl; unknown types remain hex')
    s.add_argument('name'); s.add_argument('--format', choices=['auto', 'hex', 'string', 'int32', 'uint32', 'int64', 'uint64'], default='auto')
    for name in ('processes', 'registry'):
        add(name, cmd_query, 'live ' + name + ' from XNU (agent required)')
    for name in ('threads', 'stacks'):
        s = add(name, cmd_query, 'inspect ' + name + ' (stacks are written to kernel log)')
        s.add_argument('target', type=int if name == 'threads' else str)
    s = add('input', cmd_input, 'queue one touch/button event in the existing backboardd input path')
    s.add_argument('kind', choices=['down', 'move', 'up', 'button-down', 'button-up'])
    s.add_argument('--finger', type=int, default=0); s.add_argument('--x', type=int, default=0); s.add_argument('--y', type=int, default=0)
    s = add('tap', cmd_tap, 'tap or long press at normalized coordinates 0..65535')
    s.add_argument('x', type=int); s.add_argument('y', type=int); s.add_argument('--duration', type=float, default=.1); s.add_argument('--finger', type=int, default=0)
    s = add('swipe', cmd_swipe, 'swipe through actual input dispatch; coordinates 0..65535')
    for name in ('x1', 'y1', 'x2', 'y2'): s.add_argument(name, type=int)
    s.add_argument('--duration', type=float, default=.3); s.add_argument('--steps', type=int, default=8); s.add_argument('--finger', type=int, default=0)
    s = add('button', cmd_button, 'press a supported phone button via HID dispatch')
    s.add_argument('button', choices=['lock', 'home', 'volume-up', 'volume-down']); s.add_argument('--duration', type=float, default=.1)
    s = add('screen', cmd_screen, 'stream real screenshots as JSONL plus PNG files and a recording manifest')
    s.add_argument('directory'); s.add_argument('--seconds', type=float, default=10); s.add_argument('--fps', type=float, default=2); s.add_argument('--scale', type=int, default=2)
    s = add('diagnose', cmd_diagnose, 'dependency-aware XNU/iOS diagnostics with direct evidence and errors')
    s.add_argument('--output')
    s = add('compare', cmd_compare, 'compare diagnostic snapshots, preserving full evidence')
    s.add_argument('before'); s.add_argument('after')
    s = add('report', cmd_report, 'save a bug report containing state, diagnostics, logs, versions, and screenshot')
    s.add_argument('directory')
    for name in ('ls', 'stat'):
        s = add(name, cmd_user, 'inspect device filesystem through the native userspace agent')
        s.set_defaults(operation=name); s.add_argument('path')
    s = add('exec', cmd_user, 'execute an argv directly on device; bounded time and output, no implicit shell')
    s.set_defaults(operation='exec'); s.add_argument('--timeout', type=float, default=10); s.add_argument('argv', nargs=argparse.REMAINDER)
    s = add('launch', cmd_user, 'ask SpringBoard to launch an application by bundle identifier')
    s.set_defaults(operation='launch'); s.add_argument('bundle')
    s = add('signal', cmd_user, 'send an explicit process signal (pid > 1)')
    s.set_defaults(operation='signal'); s.add_argument('pid', type=int); s.add_argument('signal', type=int, choices=[9, 15, 17, 19])
    s = add('memory', cmd_user, 'read up to 4096 bytes from an authorized user task')
    s.set_defaults(operation='memory'); s.add_argument('pid', type=int); s.add_argument('address'); s.add_argument('length', type=int)
    s = add('key', cmd_user, 'dispatch a USB HID keyboard usage (delivery must be checked separately)')
    s.set_defaults(operation='key'); s.add_argument('usage', type=int); s.add_argument('--down', action='store_true')
    s = add('get', cmd_get, 'download a regular device file in bounded chunks')
    s.add_argument('remote'); s.add_argument('local')
    s = add('put', cmd_put, 'upload a new file under /private/var/root/iphont (never overwrite an existing file)')
    s.add_argument('local'); s.add_argument('name')
    s = add('build', cmd_build, 'build with existing repository compiler drivers; logs and exit status stay explicit')
    s.add_argument('component', choices=['kernel', 'loader', 'agent', 'images']); s.add_argument('--source'); s.add_argument('--timeout', type=float, default=1800)
    s = add('test', cmd_test, 'run host tests or read-only device smoke checks; no automatic boot')
    s.add_argument('level', choices=['host', 'device'], default='host', nargs='?')
    s = add('events', cmd_query, 'read timestamped kernel events after an event sequence')
    s.add_argument('target', nargs='?', type=int, default=0)
    s = add('trace', cmd_query, 'set minimum kernel event severity: 0 debug through 4 critical (control required)')
    s.add_argument('target', type=int, choices=range(5))
    s = add('apps', cmd_user, 'enumerate installed applications through LaunchServices')
    s.set_defaults(operation='apps')
    s = add('mach', cmd_user, 'look up a Mach service and inspect the returned port rights')
    s.set_defaults(operation='mach'); s.add_argument('name')
    s = add('framework', cmd_user, 'load a system framework and optionally resolve a symbol in the userspace agent')
    s.set_defaults(operation='framework'); s.add_argument('path'); s.add_argument('--symbol')
    s = add('clipboard', cmd_user, 'read or set the system text pasteboard where iOS permits access')
    s.set_defaults(operation='clipboard'); s.add_argument('--text')
    s = add('text', cmd_text, 'dispatch supported ASCII text through keyboard HID events; requires delivery validation')
    s.add_argument('text')
    for name in ('system', 'network'):
        s = add(name, cmd_user, 'read live ' + name + ' state through native iOS APIs')
        s.set_defaults(operation=name)
    s = add('properties', cmd_user, 'inspect actual IOKit properties by IOService registry ID')
    s.set_defaults(operation='properties'); s.add_argument('id', type=int)
    s = add('monitor', cmd_monitor, 'stream selected typed sysctls as JSONL, including per-field errors and timing')
    s.add_argument('--seconds', type=float, default=30); s.add_argument('--interval', type=float, default=2)
    s.add_argument('--field', action='append', help='NAME:FORMAT; repeat to select probes and control overhead')
    add('prepare', cmd_prepare, 'package the new loader for a RAM-only instantnoodle boot; never touch the phone')
    s = add('kernel-test', cmd_kernel_test, 'boot an isolated QEMU instance and validate native kernel diagnostics; never use the phone')
    s.add_argument('kernel'); s.add_argument('--seconds', type=int, default=25)
    return p


def main(argv=None):
    p = parser()
    t0 = time.monotonic()
    out = {'schema': 1, 'command': None}
    try:
        a = p.parse_args(argv)
        out['command'] = a.command
        if not getattr(a, 'fn', None):
            raise Fail('usage', 'a command is required; run iphont --help')
        out['result'] = a.fn(a)
        if isinstance(out['result'], dict) and out['result'].get('data', {}).get('ok') is False:
            raise Fail('device_error', 'device operation failed', detail=out.pop('result'))
        out['ok'] = True
        status = 0
    except Fail as e:
        out['ok'] = False
        out['error'] = e.as_json()
        status = e.exit_status()
    except OSError as e:
        out['ok'] = False
        out['error'] = {'code': 'io_error', 'message': str(e), 'errno': e.errno}
        status = 1
    except (ValueError, re.error) as e:
        out['ok'] = False
        out['error'] = {'code': 'usage', 'message': str(e)}
        status = 2
    except KeyboardInterrupt:
        out['ok'] = False
        out['error'] = {'code': 'interrupted', 'message': 'interrupted'}
        status = 130
    except Exception as e:                      # a bug here: say where, as data
        out['ok'] = False
        out['error'] = {'code': 'internal', 'message': f'{type(e).__name__}: {e}',
                        'detail': {'traceback': traceback.format_exc().split('\n')[-12:]}}
        status = 7
    out['elapsed_ms'] = round((time.monotonic() - t0) * 1000)
    print(json.dumps(out, indent=None if os.environ.get('IPHONT_COMPACT') else 1))
    return status
