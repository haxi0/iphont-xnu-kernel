"""
The USB link to the phone: the XNU loader's fastboot protocol, through the system's `fastboot` program,
under the same locks as ./xl so that both (and every working copy, and every agent session) share one phone.

  $XDG_RUNTIME_DIR/hackios-phone.lock   held for the length of one fastboot call
  $XDG_RUNTIME_DIR/hackios-phone.run    held for the length of a run (by `xl run`, `iphont run`)
  $XDG_RUNTIME_DIR/hackios-phone.who    the working copy that holds the run
"""
import contextlib, fcntl, os, re, subprocess, threading, time

from .errors import Fail

PHONE = os.path.join(os.environ.get('XDG_RUNTIME_DIR') or '/tmp', 'hackios-phone')
LOCK, RUN, WHO, RAMDISK = PHONE + '.lock', PHONE + '.run', PHONE + '.who', PHONE + '.ramdisk'
_local = threading.local()
_threads = threading.RLock()


@contextlib.contextmanager
def transaction():
    """Keep command/stage/fetch sequences indivisible, including nested helper calls."""
    with _threads:
        depth = getattr(_local, 'depth', 0)
        with contextlib.nullcontext() if depth else _Flock(LOCK, 90):
            _local.depth = depth + 1
            try:
                yield
            finally:
                _local.depth = depth


class _Flock:
    def __init__(self, path, wait):
        self.path, self.wait, self.fd = path, wait, None

    def __enter__(self):
        self.fd = os.open(self.path, os.O_CREAT | os.O_RDWR, 0o644)
        deadline = time.monotonic() + self.wait
        while True:
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    os.close(self.fd)
                    raise Fail('link_busy', f'{self.path} held for more than {self.wait} s',
                               hint='another fastboot user (xl, iphont, another session) is mid-transfer')
                time.sleep(0.05)

    def __exit__(self, *exc):
        os.close(self.fd)


def fastboot(*args, timeout=10, check=True):
    """One fastboot call under the link lock. Returns its combined output (str)."""
    with transaction():
        try:
            serial = os.environ.get('IPHONT_SERIAL')
            if not serial:
                found = devices()
                if len(found) > 1:
                    raise Fail('refused', 'multiple fastboot devices; set IPHONT_SERIAL explicitly', detail={'devices': found})
            p = subprocess.run(['fastboot', *(['-s', serial] if serial else []), *args],
                               capture_output=True, text=True, timeout=timeout)
        except FileNotFoundError:
            raise Fail('no_fastboot', 'the fastboot program is not installed', hint='nix-shell -p android-tools')
        except subprocess.TimeoutExpired:
            raise Fail('timeout', f'fastboot {" ".join(args)}: no answer in {timeout} s',
                       hint='the loader is busy, the cable is out, or the phone left the loader (iphont state)')
    out = (p.stdout or '') + (p.stderr or '')
    if check and ('FAILED' in out or p.returncode != 0):
        m = re.search(r'FAILED \(remote: \'?([^\')\n]*)', out)
        raise Fail('refused', f'fastboot {" ".join(args)}: ' + (m.group(1) if m else out.strip()[-300:]),
                   detail={'output': out[-2000:]})
    return out


def getvar(name, timeout=6):
    out = fastboot('getvar', name, timeout=timeout, check=False)
    m = re.search(rf'^{re.escape(name)}: ?(.*)$', out, re.M)
    return m.group(1).strip() if m else None


def oem(*words, timeout=10):
    return fastboot('oem', *words, timeout=timeout)


def fetch(path, timeout=20):
    """The loader's staged buffer (what the last oem command prepared) into a file; returns its bytes."""
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    fastboot('get_staged', path, timeout=timeout)
    with open(path, 'rb') as f:
        return f.read()


def devices():
    try:
        result = subprocess.run(['fastboot', 'devices'], capture_output=True, text=True, timeout=5)
        if result.returncode:
            raise Fail('no_answer', 'fastboot device enumeration failed', detail={'output': result.stderr[-2000:]})
        out = result.stdout
    except FileNotFoundError:
        raise Fail('no_fastboot', 'fastboot is not installed', hint='nix-shell -p android-tools')
    except subprocess.TimeoutExpired:
        raise Fail('timeout', 'fastboot devices timed out')
    found = [l.split()[0] for l in out.splitlines() if 'fastboot' in l]
    serial = os.environ.get('IPHONT_SERIAL')
    return [s for s in found if not serial or s == serial]


def state():
    """absent | fastboot (the stock bootloader's) | loader (ours, idle or watching a run)"""
    if not devices():
        return 'absent'
    product = getvar('product')
    return 'loader' if product == 'xnuloader' else 'fastboot'


def loader_version():
    v = getvar('version') or ''
    m = re.match(r'(\d+)\.(\d+)', v)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def run_held():
    """Who holds the run lock, or None."""
    fd = os.open(RUN, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return None
    except BlockingIOError:
        try:
            return open(WHO).read().strip() or '?'
        except OSError:
            return '?'
    finally:
        os.close(fd)


class RunLock:
    """Held for a whole run, as `xl run` holds it."""

    def __init__(self, root):
        self.root, self.fd = root, None

    def __enter__(self):
        self.fd = os.open(RUN, os.O_CREAT | os.O_RDWR, 0o644)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(self.fd)
            raise Fail('phone_busy', f'a run is in progress, started from {run_held()}',
                       hint='iphont wait, then retry; never end another session\'s run without being asked')
        with open(WHO, 'w') as f:
            f.write(self.root)
        return self

    def __exit__(self, *exc):
        os.close(self.fd)


def wait_free(seconds):
    deadline = time.monotonic() + seconds
    while run_held() is not None:
        if time.monotonic() >= deadline:
            return False
        time.sleep(1)
    return True


def exclusive(fn):
    """Decorate operations that stage data in the loader's single response buffer."""
    import functools
    @functools.wraps(fn)
    def locked(*args, **kwargs):
        with transaction():
            return fn(*args, **kwargs)
    return locked
