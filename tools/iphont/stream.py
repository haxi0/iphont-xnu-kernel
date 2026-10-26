"""Bounded capture streams. Slow consumers cause backpressure, never an unbounded queue."""
import codecs
import json
import os
import struct
import tempfile
import time
from . import device, link, logs
from .errors import Fail


def frames(directory, seconds=10, fps=2, scale=2):
    if not 0 < seconds <= 86400 or not 0 < fps <= 30 or scale < 1:
        raise Fail('usage', 'seconds must be in (0,86400], fps in (0,30], scale positive')
    os.makedirs(directory, exist_ok=False)
    manifest = {'schema': 1, 'started_ns': time.time_ns(), 'requested_fps': fps, 'complete': False,
                'note': 'each PNG comes from the display driver framebuffer; capture may tear during scanout'}
    count, disconnected = 0, False
    index_path = os.path.join(directory, 'frames.jsonl')
    index = open(index_path, 'w')
    start = time.monotonic()
    try:
        while time.monotonic() - start < seconds:
            before = time.monotonic()
            try:
                frame = device.screenshot(os.path.join(directory, f'{count:06d}.png'), scale)
            except Fail as error:
                if error.code not in ('timeout', 'absent', 'no_answer', 'not_in_loader'):
                    raise
                disconnected = True
                yield {'event': 'connection_lost', 'error': error.as_json()}
                time.sleep(min(1, max(0, seconds - (time.monotonic() - start))))
                continue
            if disconnected:
                yield {'event': 'connection_restored'}
                disconnected = False
            frame['host_ns'] = time.time_ns(); frame['elapsed_s'] = before - start
            index.write(json.dumps(frame) + '\n'); index.flush(); count += 1
            yield {'event': 'frame', **frame}
            time.sleep(max(0, min(seconds - (time.monotonic() - start), 1 / fps - (time.monotonic() - before))))
        if not count: raise Fail('no_answer', 'no frames were captured')
        manifest['complete'] = not disconnected
    finally:
        index.close()
        manifest['elapsed_s'] = time.monotonic() - start
        manifest['actual_fps'] = count / max(.001, manifest['elapsed_s'])
        manifest['frame_count'] = count
        # Assemble the compatible JSON manifest without retaining a long recording in host memory.
        target = os.path.join(directory, 'recording.json')
        with open(target + '.tmp', 'w') as out, open(index_path) as records:
            out.write(json.dumps(manifest)[:-1] + ', "frames":[')
            for n, line in enumerate(records):
                out.write((',' if n else '') + line.strip())
            out.write(']}')
        os.replace(target + '.tmp', target)


def tail(cursor):
    with tempfile.TemporaryDirectory(prefix='iphont-tail-') as tmp, link.transaction():
        if link.getvar('iphont') != '1':
            raise Fail('old_loader', 'incremental streaming requires the new iphont loader; use log --live for a snapshot')
        try:
            link.oem('iphont-log', f'{cursor:x}')
        except Fail as error:
            if 'incremental logs need a running XNULIVE4 kernel' not in error.message: raise
            result = link.getvar('result')
            if result and not result.startswith('running'):
                raise Fail('not_running', 'kernel returned', detail={'result': result})
            raise Fail('old_kernel', 'incremental logs require an XNULIVE4 kernel')
        data = link.fetch(os.path.join(tmp, 'log'))
    if len(data) < 32:
        raise Fail('no_answer', 'short incremental log header')
    start, end, total, lost = struct.unpack_from('<QQQQ', data)
    if end < start or end > total or len(data) != 32 + end - start:
        raise Fail('no_answer', 'invalid incremental log offsets')
    return start, end, total, lost, data[32:]


def follow(seconds=900, interval=2, component=None, min_severity=None, grep=None, output=None):
    if seconds <= 0 or interval <= 0:
        raise Fail('usage', 'stream duration and interval must be positive')
    deadline = time.monotonic() + seconds
    cursor, pending, disconnected = 0, '', False
    decoder = codecs.getincrementaldecoder('utf-8')('replace')
    dest = open(output, 'ab') if output else None
    try:
        while time.monotonic() < deadline:
            try:
                start, end, total, lost, data = tail(cursor)
            except Fail as error:
                if error.code == 'not_running':
                    file = device.path(f'logs/iphont-final-{time.time_ns()}.log')
                    text = device.fetch_log(file)
                    yield {'event': 'run_ended', 'result': error.detail['result'], 'final_log': file,
                           'summary': logs.summary(text), 'cursor': cursor}
                    return
                if error.code not in ('timeout', 'absent', 'no_answer', 'not_in_loader'): raise
                if not disconnected: yield {'event': 'connection_lost', 'error': error.as_json()}
                disconnected = True
                time.sleep(min(interval, max(0, deadline - time.monotonic())))
                continue
            if disconnected:
                yield {'event': 'connection_restored'}
                disconnected = False
            if start < cursor:
                yield {'event': 'boot_changed', 'old_cursor': cursor, 'new_cursor': start}
                pending = ''; decoder.reset()
            if lost:
                yield {'event': 'log_gap', 'lost_bytes': lost, 'from': cursor, 'to': start}
                pending = ''; decoder.reset()
            if dest:
                dest.write(data); dest.flush()
            pending += decoder.decode(data)
            parts = pending.split('\n'); pending = parts.pop()
            for r in logs.select(logs.lines('\n'.join(parts)), component=component, min_severity=min_severity, grep=grep):
                if r['text']:
                    yield {'event': 'log', 'host_ns': time.time_ns(), 'byte_range': [start, end], **r}
            while len(pending) > 65536:
                yield {'event': 'log_fragment', 'partial': True, 'text': pending[:65536], 'byte_range': [start, end]}
                pending = pending[65536:]
            cursor = end
            if end < total:
                continue
            state = link.getvar('result')
            if state and not state.startswith('running'):
                file = device.path(f'logs/iphont-final-{time.time_ns()}.log')
                text = device.fetch_log(file)
                yield {'event': 'run_ended', 'result': state, 'cursor': cursor, 'final_log': file, 'summary': logs.summary(text)}
                return
            time.sleep(interval)
    finally:
        if dest:
            dest.close()


def monitor(seconds=30, interval=2, fields=None):
    from . import agent
    if not 0 < seconds <= 86400 or not 0 < interval <= 3600:
        raise Fail('usage', 'invalid monitor duration or interval')
    fields = fields or ['kern.hackios.frames:uint32', 'kern.hackios.thermal_hottest:int32', 'hw.memsize:uint64']
    specs = []
    for field in fields:
        name, sep, fmt = field.partition(':')
        if not sep or fmt not in ('auto', 'hex', 'string', 'int32', 'uint32', 'int64', 'uint64'):
            raise Fail('usage', 'monitor fields use NAME:FORMAT')
        specs.append((name, fmt))
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        start = time.monotonic()
        values, errors = {}, {}
        for name, fmt in specs:
            try: values[name] = agent.sysctl(name, fmt)
            except Fail as error: errors[name] = error.as_json()
        yield {'event': 'sample', 'host_ns': time.time_ns(), 'values': values, 'errors': errors,
               'sample_ms': round((time.monotonic() - start) * 1000)}
        time.sleep(max(0, min(end - time.monotonic(), interval - (time.monotonic() - start))))
