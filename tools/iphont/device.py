"""
What the loader does for the computer: get the phone into it, run a kernel, upload the system image, the
screen, the log, the boot area of an installed phone. The same steps as ./xl, as functions that return data.
"""
import functools, hashlib, os, re, struct, subprocess, sys, tempfile, time, zlib

from . import link, logs
from .errors import Fail

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KERNEL = 'xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837'
LOADER = 'xnuloader/loader.efi'
DEVICE = os.environ.get('IPHONT_DEVICE', 'instantnoodle')


def path(p):
    return os.path.join(ROOT, p)


def profile(name=DEVICE):
    """The device profile (common/devices/NAME.conf) as a dict, read by the shell as common/lib.sh does."""
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', name):
        raise Fail('usage', 'invalid device profile name')
    conf = path(f'common/devices/{name}.conf')
    if not os.path.isfile(conf):
        raise Fail('not_found', f'no device profile {conf}')
    result = subprocess.run(['bash', '-ec', 'set -a; . "$1"; env -0', 'iphont-profile', conf], capture_output=True, cwd=ROOT, timeout=10)
    if result.returncode:
        raise Fail('usage', f'failed to load device profile {conf}', detail={'stderr': result.stderr.decode(errors='replace')[-2000:]})
    return dict(kv.split('=', 1) for kv in result.stdout.decode().split('\0') if '=' in kv)


def kernel_file():
    k = os.environ.get('XL_KERNEL') or path(KERNEL)
    if os.path.isfile(k): return k
    fallback = path('prebuilt/mach.development.bcm2837')
    if os.path.isfile(fallback): return fallback
    raise Fail('no_kernel', 'no built or prebuilt kernel', hint='iphont build kernel')


def loader_file():
    selected = os.environ.get('IPHONT_LOADER') or path('out/iphont/loader.efi')
    if os.environ.get('IPHONT_LOADER') and not os.path.isfile(selected):
        raise Fail('not_found', f'IPHONT_LOADER artifact does not exist: {selected}')
    if os.path.isfile(selected): return selected
    return path(LOADER) if os.path.isfile(path(LOADER)) else path('prebuilt/loader.efi')


def payload(out, items, cmdline=None, usb=False):
    """An "XNUP" container (xnuloader/mkpayload.py's format)."""
    if any(not re.fullmatch(r'[a-zA-Z0-9_.-]{1,15}', name) for name, _ in items):
        raise Fail('usage', 'payload entry names must contain 1..15 ASCII letters, digits, dots, hyphens, or underscores')
    size = 8 + 24 + sum(24 + ((os.path.getsize(file) + 15) & ~15) for _, file in items)
    if cmdline: size += 24 + ((len(cmdline.encode()) + 1 + 15) & ~15)
    if usb: size += 40
    if size > 0x03f00000:
        raise Fail('refused', 'payload exceeds the loader download window; upload the large image with iphont ramdisk instead', detail={'bytes': size})
    body = b''

    def add(name, data):
        nonlocal body
        body += struct.pack('<16sII', name.encode(), len(data), 0) + data + b'\0' * (-len(data) % 16)
    for name, f in items:
        with open(f, 'rb') as fh:
            add(name, fh.read())
    if cmdline:
        add('cmdline', cmdline.encode() + b'\0')
    if usb:
        add('usb', b'1\0')
    body += b'\0' * 24
    with open(out, 'wb') as fh:
        fh.write(struct.pack('<II', 0x50554E58, len(body)) + body)
    return out


def require_loader():
    s = link.state()
    if s != 'loader':
        raise Fail('not_in_loader', f'the phone is not in the loader (state: {s})',
                   hint='iphont enter (from the stock fastboot), or the installed menu\'s Developer mode')


def idle_operation(fn):
    @functools.wraps(fn)
    def idle(*args, **kwargs):
        with link.RunLock(ROOT), link.transaction():
            require_loader()
            if (link.getvar('result') or '').startswith('running'):
                raise Fail('phone_busy', 'a kernel is running outside the host run lock')
            return fn(*args, **kwargs)
    return idle


def enter():
    """Into the resident loader: from the stock bootloader's fastboot by `fastboot boot` (nothing is flashed)."""
    s = link.state()
    if s == 'loader':
        return {'state': 'loader', 'entered': False}
    if s == 'absent':
        deadline = time.monotonic() + 180
        while s == 'absent' and time.monotonic() < deadline:
            time.sleep(3)
            s = link.state()
        if s == 'absent':
            raise Fail('absent', 'no fastboot device on USB for 180 s',
                       hint='after a reset with the cable in, the stock bootloader does not show up on USB: unplug, '
                            'start fastboot (Power + Volume down) or the installed menu\'s Developer mode, replug')
        if s == 'loader':
            return {'state': 'loader', 'entered': False}
    prof = profile()
    actual = link.getvar('product')
    if not prof.get('FASTBOOT_PRODUCT') or actual != prof['FASTBOOT_PRODUCT']:
        raise Fail('refused', 'stock bootloader product does not match the device profile', detail={'actual': actual, 'expected': prof.get('FASTBOOT_PRODUCT')})
    from .workflow import prepare
    image = prepare()['image']
    for attempt in (1, 2):              # right after fastboot comes up the first boot is sometimes not taken
        try:
            out = link.fastboot('boot', image, timeout=40, check=False)
        except Fail:
            out = ''
        if 'OKAY' not in out:
            time.sleep(3)
            continue
        time.sleep(6)
        for _ in range(15):
            if link.state() == 'loader':
                return {'state': 'loader', 'entered': True, 'attempts': attempt}
            time.sleep(2)
        if link.state() != 'fastboot':
            break
    raise Fail('no_answer', f'the loader did not come up on USB (state: {link.state()})')


def info():
    s = link.state()
    r = {'state': s, 'run_held_by': link.run_held()}
    if s == 'loader':
        major, minor = link.loader_version()
        r['loader_version'] = f'{major}.{minor}'
        r['board'] = link.getvar('board')
        r['result'] = link.getvar('result')
        geom = link.getvar('screen')
        if geom:
            w, h, stride = map(int, geom.split())
            r['screen'] = {'width': w, 'height': h, 'stride': stride}
        r['running'] = (r['result'] or '').startswith('running')
    return r


def fetch_log(dest):
    """The log as it stands: the loader's, and during a run the kernel's so far. Returns the text."""
    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    with link.transaction():
        link.oem('log', timeout=20)
        data = link.fetch(dest + '.raw', timeout=30)
    text = data.replace(b'\0', b'').decode('utf-8', 'replace')
    with open(dest, 'w') as f:
        f.write(text)
    return text


def screenshot(out, scale=1):
    """The frame the panel shows (during a run: the one the kernel's display driver last sent), as a PNG."""
    if not isinstance(scale, int) or scale < 1:
        raise Fail('usage', 'scale must be a positive integer')
    require_loader()
    out = os.path.abspath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    t0 = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='iphont-screen-') as tmp, link.transaction():
        geom = link.getvar('screen')
        if not geom:
            raise Fail('unsupported', 'the loader has no display')
        w, h, stride = map(int, geom.split())
        if not (0 < w <= 16384 and 0 < h <= 16384 and w * 4 <= stride <= w * 4 + 4096 and scale <= min(w, h)):
            raise Fail('usage', 'invalid framebuffer geometry or scale')
        link.oem('screenshot')
        raw = link.fetch(os.path.join(tmp, 'frame.raw'), timeout=60)
    t1 = time.monotonic()
    if len(raw) < stride * h:
        raise Fail('no_answer', f'short screen: {len(raw)} of {stride * h} bytes')
    write_png(out, raw, w, h, stride, scale)
    return {'file': out, 'width': w // scale, 'height': h // scale, 'source': {'width': w, 'height': h, 'stride': stride},
            'fetch_ms': round((t1 - t0) * 1000), 'bytes': os.path.getsize(out),
            'sha1_raw': hashlib.sha1(raw[:stride * h]).hexdigest()}


def write_png(out, raw, w, h, stride, scale=1):
    """BGRX rows (the panel's format) to an RGB PNG, every scale-th pixel."""
    rows = bytearray()
    for y in range(0, h - h % scale, scale):
        line = raw[y * stride:y * stride + w * 4]
        if scale > 1:
            line = b''.join(line[x * 4:x * 4 + 4] for x in range(0, w - w % scale, scale))
        rgb = bytearray(len(line) // 4 * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
        rows += b'\0' + rgb

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    with open(out, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w // scale, h // scale, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))


@link.exclusive
@idle_operation
def ramdisk(image):
    """The big system image into the phone's resident area (root /dev/md0 for the runs that follow)."""
    require_loader()
    if image == 'none':
        link.oem('ramdisk', 'none')
        try:
            os.unlink(link.RAMDISK)
        except FileNotFoundError:
            pass
        return {'dropped': True}
    image = os.path.abspath(image)
    if not os.path.isfile(image):
        raise Fail('not_found', f'{image} not found')
    size = os.path.getsize(image)
    t0 = time.monotonic()
    link.oem('ramdisk')
    link.fastboot('stage', image, timeout=1800)
    dt = max(time.monotonic() - t0, 0.001)
    with open(link.RAMDISK, 'w') as f:
        f.write(f'{ROOT}\t{image}\t{size}\n')
    return {'image': image, 'bytes': size, 'seconds': round(dt, 1), 'mb_per_s': round(size / 1048576 / dt, 1)}


def resident():
    try:
        who, what, size = open(link.RAMDISK).read().strip().split('\t')
        return {'uploaded_from': who, 'image': what, 'bytes': int(size), 'ours': who == ROOT}
    except (OSError, ValueError):
        return None


def run(secs=None, bootargs=None, done=None, ramdisk_image=None, follow=None, keep_live=False, checkpoint=None,
        on_progress=None):
    """
    Upload the kernel and the small image, run it, follow it, fetch its log; the run's lock is held throughout.
    secs: the longest it may run (loader_return_after); done: a regular expression that ends it as soon as the log has
    it; follow: seconds between live log fetches (default 2 with done). Returns the result with the log's summary.
    """
    enter()
    prof = profile()
    secs = int(prof.get('DEFAULT_SECS') or 8) if secs is None else secs
    if secs <= 0 or secs > 86400:
        raise Fail('usage', 'secs must be in 1..86400')
    if done:
        try:
            re.compile(done)
        except re.error as e:
            raise Fail('usage', f'invalid --done expression: {e}')
    args = prof.get('BOOTARGS', '') + ' ' + prof.get('BOARD_ARGS', '') if bootargs is None else bootargs
    if re.search(r'(^| )(wifi|wifi_step)=', args) and os.path.isfile(path('fw-wifi/mac')):
        args += ' wifi_mac=' + open(path('fw-wifi/mac')).read().strip()        # this phone's own address: not in the repo
    rd = ramdisk_image or os.environ.get('XL_RAMDISK') or path('userland/root.apfs')
    items = [('kernel', kernel_file())]
    if rd not in ('none', 'resident'):
        if not os.path.isfile(rd):
            raise Fail('not_found', f'ramdisk {rd} not found')
        items.append(('ramdisk', rd))
    cmdline = (f'loader_return_after={secs} epoch={int(time.time())} sb_delay={os.environ.get("XL_SB_DELAY", "8")} '
               + args.strip())
    from . import symbols
    uuid = symbols.archive_kernel(kernel_file() + '.unstripped')                     # its symbols, for a panic read later
    with link.RunLock(ROOT):
        if (link.getvar('result') or '').startswith('running'):
            raise Fail('phone_busy', 'a kernel is already running without a held host run lock; do not overwrite it')
        stamp = time.strftime('%Y%m%d-%H%M%S') + f'-{os.getpid()}'
        log_path = path(f'logs/{stamp}-run.log')
        os.makedirs(path('logs'), exist_ok=True)
        payload(path('run-payload.bin'), items, cmdline=cmdline)
        with link.transaction():
            live = link.loader_version() >= (0, 5)
            if not live:
                raise Fail('old_loader', 'iphont run requires loader 0.5 or newer')
            link.fastboot('stage', path('run-payload.bin'), timeout=30)
            link.oem('run', *([str(checkpoint)] if checkpoint is not None else []), timeout=10)
        began = time.monotonic()
        follow = follow or (2 if done else None)
        result, started, silent, stopped, followed, n = None, False, 0, None, began, 0
        deadline = began + secs + 100
        while live:
            try:
                r = link.getvar('result', timeout=6)
            except Fail:
                r = None
            if not r:
                silent += 1
                if silent >= 5:
                    break
            elif r.startswith('running'):
                silent, started = 0, True
            else:
                result = r
                break
            if not started and time.monotonic() - began >= 20:
                break
            if time.monotonic() >= deadline:
                break
            if follow and time.monotonic() - followed >= follow:
                followed, n = time.monotonic(), n + 1
                try:
                    text = fetch_log(log_path + '.live')
                except Fail:
                    text = ''
                if keep_live and text:
                    import shutil
                    shutil.copyfile(log_path + '.live', f'{log_path}.live.{n}')
                if on_progress:
                    on_progress(text)
                if done and not stopped and re.search(done, text):
                    link.oem('stop')
                    stopped = round(time.monotonic() - began, 1)
            time.sleep(1)
        if not live:
            result = link.getvar('result', timeout=secs + 100)
        out = {'log': log_path, 'kernel_uuid': uuid, 'bootargs': cmdline, 'seconds': round(time.monotonic() - began, 1),
               'stopped_by_done_after': stopped, 'resident': resident()}
        if not result:
            out['result'] = None
            out['state'] = link.state()
            try:
                text = fetch_log(path(f'logs/{stamp}-noanswer.loader.log'))
                out['loader_log'] = path(f'logs/{stamp}-noanswer.loader.log')
                out['loader_log_tail'] = text.strip().split('\n')[-6:]
            except Fail:
                pass
            raise Fail('no_answer', 'the kernel ran and went silent' if started else 'the run never started',
                       hint='a hung kernel is reset by the loader after 50 s of silence; look at the loader log, then '
                            'iphont state', detail=out)
        text = fetch_log(log_path)
        try:
            if os.path.islink(path('logs/last.log')) or os.path.exists(path('logs/last.log')):
                os.unlink(path('logs/last.log'))
            os.symlink(os.path.basename(log_path), path('logs/last.log'))
        except OSError:
            pass
        out['result'] = result
        out['summary'] = logs.summary(text)
        if 'hung' in result or 'no-such-core' in result:
            raise Fail('run_failed', 'kernel did not return; saved diagnostics without resetting the phone', detail=out)
        return out


def stop(force=False):
    held = link.run_held()
    if held and not force:
        raise Fail('phone_busy', f'the run was started from {held}', hint='coordinate with the session holding the run, then pass --force only for an explicitly requested stop')
    link.oem('stop')
    return {'asked': True}


@link.exclusive
def health():
    require_loader()
    link.oem('flashstate')
    text = fetch_log(path('logs/loader.log'))
    block = []
    for line in text.split('\n'):
        if re.match(r'flash: (loader|no darwin)', line):
            block = []
        if line.startswith('flash:'):
            block.append(line)
    slots = [dict(zip(('slot', 'text'), (int(m.group(1)), m.group(0))))
             for m in (re.match(r'flash: slot (\d+).*', l) for l in block) if m]
    return {'lines': block, 'slots': slots}


@link.exclusive
def lastlog(out):
    require_loader()
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    link.oem('lastlog')
    data = link.fetch(out + '.raw', timeout=30).replace(b'\0', b'').decode('utf-8', 'replace')
    with open(out, 'w') as f:
        f.write(data)
    return {'file': out, 'summary': logs.summary(data)}


@idle_operation
def settime():
    require_loader()
    link.oem('settime', str(int(time.time())))
    return {'epoch': int(time.time())}


@link.exclusive
def memmap():
    require_loader()
    link.oem('memmap')
    text = fetch_log(path('logs/loader.log'))
    i = text.rfind('memory map')
    ranges = [{'start': m.group(1), 'end': m.group(2), 'type': int(m.group(3)), 'mb': int(m.group(4))}
              for m in re.finditer(r'^\s+([0-9a-f]+) - ([0-9a-f]+)\s+type (\d+)\s+(\d+) MB', text[i:], re.M)]
    return {'ranges': ranges}


def fetch_capture(kind, out):
    """What the last run recorded: screen (rec_secs=N), sound (rec_sound=N), camera frames (cam=3+)."""
    require_loader()
    cmd = {'recording': 'recording', 'camera': 'recording', 'sound': 'sound'}[kind]
    raw = out + '.raw'
    with link.transaction():
        link.oem(cmd)
        link.fetch(raw, timeout=120)
    tool = {'recording': ['tools/recording.py', raw, out], 'sound': ['tools/sound.py', raw, out],
            'camera': ['tools/camraw.py', raw, out]}[kind]
    r = subprocess.run([sys.executable, *tool], capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        raise Fail('run_failed', f'{tool[0]} failed', detail={'output': (r.stdout + r.stderr)[-2000:], 'raw': raw})
    os.unlink(raw)
    return {'file': out, 'tool_output': r.stdout.strip().split('\n')[-3:]}


# ---- writes to the phone's flash: each only with authorize=True, which an agent passes only on the user's word ----

@link.exclusive
def install(bundle, authorize=False):
    if not authorize:
        raise Fail('refused', 'installing writes the phone\'s boot area', hint='needs the user\'s explicit OK, then --authorize')
    require_loader()
    if link.getvar('board') != DEVICE:
        raise Fail('refused', 'install requires a loader that positively identifies the selected board')
    if not os.path.isfile(bundle):
        raise Fail('not_found', f'{bundle} not found')
    link.fastboot('stage', bundle, timeout=120)
    try:
        link.oem('install', timeout=300)
        ok, why = True, None
    except Fail as e:
        ok, why = False, e.message
    h = health()
    if not ok:
        raise Fail('refused', f'not installed: {why}', detail={'health': h})
    return {'installed': bundle, 'health': h}


@link.exclusive
def flashpart(part, image, expect, authorize=False):
    if part not in ('boot_a', 'boot_b'):
        raise Fail('usage', 'only boot_a or boot_b')
    if not authorize:
        raise Fail('refused', f'writing {part} changes what the phone boots', hint='needs the user\'s explicit OK, then --authorize')
    with open(image, 'rb') as f:
        if f.read(8) != b'ANDROID!':
            raise Fail('refused', f'{image} is not an Android boot image')
    if os.path.isfile(expect):
        expect = hashlib.sha256(open(expect, 'rb').read()).hexdigest()
    if not re.fullmatch(r'[0-9a-f]{64}', expect):
        raise Fail('usage', 'EXPECT is a file or 64 hex digits: the partition\'s SHA-256 now (it is checked first)')
    require_loader()
    if link.getvar('board') != DEVICE:
        raise Fail('refused', 'partition writes require a loader that positively identifies the selected board')
    link.fastboot('stage', image, timeout=120)
    try:
        link.oem('flashpart', part, expect[:16], timeout=120)
    except Fail as e:
        raise Fail('refused', f'not written: {e.message}')
    text = fetch_log(path('logs/loader.log'))
    return {'written': part, 'image': image, 'lines': [l for l in text.split('\n') if l.startswith(f'flash: {part}')][-2:]}


def flashboot(secs=900):
    """The installed boot from the loader (its menu, the newest good bundle), no reset on the way."""
    require_loader()
    with link.RunLock(ROOT):
        link.oem('flashboot')
        deadline = time.monotonic() + secs + 100
        while True:
            time.sleep(5)
            r = None
            try:
                r = link.getvar('result', timeout=15)
            except Fail:
                pass
            if r and not r.startswith('running'):
                break
            if time.monotonic() >= deadline:
                raise Fail('timeout', 'no result in time (an untimed bundle runs until switched off)')
        log_path = path(f'logs/{time.strftime("%H%M%S")}-flashboot.log')
        text = fetch_log(log_path)
        return {'result': r, 'log': log_path, 'summary': logs.summary(text)}


def reboot(to=None, force=False):
    held = link.run_held()
    if held and not force:
        raise Fail('phone_busy', f'a run started from {held} is in progress', hint='--force only on the user\'s word')
    if not force and link.state() == 'loader' and (link.getvar('result') or '').startswith('running'):
        raise Fail('phone_busy', 'a kernel is running; stop it cleanly before rebooting')
    link.fastboot(*(['reboot', to] if to else ['reboot']))
    return {'rebooting': to or 'cold reset'}
