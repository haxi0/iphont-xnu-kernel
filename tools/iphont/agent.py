"""Versioned requests over the loader's existing fastboot interface, without network services."""
import json
import math
import os
import secrets
import struct
import tempfile
import time

from . import link
from .errors import Fail

HEADER = struct.Struct('<IIiIQ')
DARWIN = {1: 'EPERM', 2: 'ENOENT', 3: 'ESRCH', 5: 'EIO', 6: 'ENXIO', 22: 'EINVAL',
          28: 'ENOSPC', 32: 'EPIPE', 35: 'EAGAIN', 45: 'ENOTSUP', 60: 'ETIMEDOUT', 70: 'ESTALE'}


def request(command, timeout=20, sequence=None):
    raw = command.encode('utf-8')
    if not raw or len(raw) >= 4096 or b'\0' in raw or not math.isfinite(timeout) or timeout <= 0:
        raise Fail('usage', 'request must contain 1..4095 UTF-8 bytes without NUL; timeout must be positive')
    if sequence is not None and not 1 <= sequence <= 0xffffffff:
        raise Fail('usage', 'sequence must be in 1..4294967295')
    seq = sequence if sequence is not None else secrets.randbelow(0xffffffff) + 1
    started_ns = time.time_ns()
    with tempfile.TemporaryDirectory(prefix='iphont-rpc-') as tmp, link.transaction():
        if link.getvar('iphont') != '1':
            raise Fail('old_loader', 'resident loader has no iphont mailbox',
                       hint='build xnuloader and RAM-boot its new image; do not flash boot partitions')
        if sequence is None and command.startswith(('input ', 'gesture ', 'press ', 'trace ', 'user ')):
            from .device import DEVICE
            if link.getvar('board') != DEVICE:
                raise Fail('refused', 'device control requires positive loader board identification')
        if sequence is None:
            for at in range(0, len(raw), 16):
                link.oem('iphont-put', f'{at:x}', raw[at:at + 16].hex())
            link.oem('iphont-go', f'{seq:x}', f'{len(raw):x}')
        deadline = time.monotonic() + timeout
        while True:
            try:
                link.oem('iphont-get', f'{seq:x}', timeout=max(.1, min(5, deadline - time.monotonic())))
                data = link.fetch(os.path.join(tmp, 'reply'))
                break
            except Fail as error:
                if 'iphont pending' not in error.message:
                    raise
            if time.monotonic() >= deadline:
                raise Fail('agent_timeout', 'request did not finish; outcome is unknown, do not replay mutations',
                           detail={'sequence': seq, 'request': command[:80]},
                           hint=f'iphont agent-result {seq}; a disconnected daemon or blocked kernel may need investigation')
            time.sleep(.05)
    received_ns = time.time_ns()
    if len(data) < HEADER.size:
        raise Fail('no_answer', 'short agent response header')
    version, got_seq, error, size, device_ns = HEADER.unpack_from(data)
    if version != 1 or got_seq != seq or size > 65536 or len(data) != HEADER.size + size:
        raise Fail('no_answer', 'agent response version, sequence, or size mismatch')
    if error:
        code = 'unsupported' if error in (6, 45) else 'refused' if error == 1 else 'device_error'
        raise Fail(code, f'XNU request failed: {DARWIN.get(error, "errno " + str(error))}',
                   detail={'errno': error, 'sequence': seq, 'device_ns': device_ns, 'request': command[:80]})
    try:
        result = json.loads(data[HEADER.size:])
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise Fail('no_answer', f'invalid device JSON: {e}')
    if not isinstance(result, dict):
        raise Fail('no_answer', 'device response must be a JSON object')
    return {'data': result, 'sequence': seq, 'device_ns': device_ns,
            'host_send_ns': started_ns, 'host_receive_ns': received_ns,
            'clock_note': 'device monotonic time; host wall-clock bounds include transfer and execution'}


def user(operation, **args):
    result = request('user ' + json.dumps({'op': operation, **args}, separators=(',', ':'), ensure_ascii=True, allow_nan=False))
    if result['data'].get('ok') is False:
        raise Fail('device_error', f'userspace {operation} failed', detail=result)
    return result


def sysctl(name, fmt='auto'):
    if not name or not all(c.isalnum() or c in '._' for c in name):
        raise Fail('usage', 'invalid sysctl name')
    r = request('sysctl ' + name)
    raw = bytes.fromhex(r['data']['hex'])
    if fmt == 'auto':
        # Types of the repository's explicit string handlers, not a content-based guess.
        string_names = ('kern.version', 'kern.osrelease', 'kern.osversion', 'hw.machine', 'hw.model',
                        'kern.hackios.cpuspeed', 'kern.hackios.top', 'kern.hackios.thermal')
        fmt = 'string' if name in string_names else 'hex'
    if fmt == 'string':
        value = raw.rstrip(b'\0').decode('utf-8', 'replace')
    elif fmt in ('int32', 'uint32', 'int64', 'uint64'):
        size = 4 if fmt.endswith('32') else 8
        if len(raw) != size:
            raise Fail('device_error', f'{name}: expected {size} bytes, received {len(raw)}')
        value = int.from_bytes(raw, 'little', signed=fmt.startswith('int'))
    else:
        value = raw.hex()
    r['data'].update(name=name, value=value, format=fmt)
    return r


def input_event(kind, finger=0, x=0, y=0):
    types = {'down': 1, 'move': 2, 'up': 3, 'button-down': 4, 'button-up': 5}
    if kind not in types or not 0 <= x <= 65535 or not 0 <= y <= 65535:
        raise Fail('usage', 'invalid event or coordinates (normalized range 0..65535)')
    if not (0 <= finger < 10 if types[kind] <= 3 else 1 <= finger <= 4):
        raise Fail('usage', 'finger must be 0..9; button must be 1..4')
    return request(f'input {types[kind]} {finger} {x} {y}')


def gesture(points, duration=.3, finger=0):
    if not points or not 0 <= duration <= 30:
        raise Fail('usage', 'gesture needs points and duration in 0..30 seconds')
    for x, y in points:
        if not 0 <= x <= 65535 or not 0 <= y <= 65535:
            raise Fail('usage', 'coordinates must be in 0..65535')
    if not 0 <= finger < 10 or duration > 10:
        raise Fail('usage', 'finger must be 0..9 and duration at most 10 seconds')
    # Schedule gesture timing on device; USB round trips must not stretch a swipe into many seconds.
    steps = max(1, len(points) - 1)
    if steps > 120: raise Fail('usage', 'gesture has more than 120 steps')
    x1, y1 = points[0]; x2, y2 = points[-1]
    return request(f'gesture {x1} {y1} {x2} {y2} {round(duration * 1000)} {steps} {finger}')
