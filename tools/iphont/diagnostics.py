"""Evidence-based layer checks. A registered service alone never proves rendering or hardware health."""
import json
import os
import time
from . import agent, device, logs
from .errors import Fail

PROBES = {
    'kernel': [('kern.osrelease', 'string'), ('kern.version', 'string'), ('hw.ncpu', 'uint32')],
    'cpu': [('kern.hackios.cpuspeed', 'string'), ('kern.hackios.top', 'string')],
    'memory': [('hw.memsize', 'uint64'), ('vm.page_free_count', 'uint32')],
    'thermal': [('kern.hackios.thermal', 'string'), ('kern.hackios.thermal_hottest', 'int32'),
                ('kern.hackios.thermal_pressure', 'int32')],
    'battery': [('kern.hackios.battery_soc', 'int32'), ('kern.hackios.battery_voltage_mv', 'int32'),
                ('kern.hackios.battery_temperature_c', 'int32')],
    'storage': [('kern.hackios.ufs_requests', 'uint64'), ('kern.hackios.ufs_errors', 'uint64')],
    'display': [('kern.hackios.frames', 'uint32'), ('kern.hackios.display_on', 'int32')],
}
REGISTRY = {'framebuffer': ['IOMobileFramebuffer'], 'surfaces': ['IOSurface'],
            'gpu': ['HKGPU', 'IOAccelerator'], 'network': ['HKWLAN', 'IONetwork']}
DEPENDENCIES = {'cpu': ['kernel'], 'memory': ['kernel'], 'thermal': ['kernel'], 'battery': ['kernel'],
                'storage': ['kernel'], 'display': ['kernel', 'framebuffer'], 'framebuffer': ['kernel'],
                'surfaces': ['kernel'], 'gpu': ['kernel', 'surfaces'], 'network': ['kernel'],
                'userspace': ['kernel'], 'compositor': ['userspace', 'display', 'surfaces']}


def snapshot():
    out = {'schema': 1, 'captured_ns': time.time_ns(), 'connection': device.info(), 'layers': {}}
    layers = out['layers']
    try:
        out['agent'] = agent.request('hello')
    except Fail as error:
        out['agent_error'] = error.as_json()
        return out
    for layer, probes in PROBES.items():
        evidence, errors = {}, {}
        for name, fmt in probes:
            try:
                evidence[name] = agent.sysctl(name, fmt)['data']
            except Fail as error:
                errors[name] = error.as_json()
        layers[layer] = {'status': 'responsive' if not errors else 'partial' if evidence else 'unavailable',
                         'evidence': evidence, 'errors': errors}
    try:
        registry = agent.request('registry')['data']['entries']
        out['registry'] = registry
        for layer, names in REGISTRY.items():
            entries = [e for e in registry if any(s.lower() in (e['name'] + e['class']).lower() for s in names)]
            layers[layer] = {'status': 'registered' if entries else 'unavailable', 'evidence': entries,
                             'limitation': 'registration does not prove successful hardware operations'}
    except Fail as error:
        out['registry_error'] = error.as_json()
    try:
        processes = agent.request('processes')['data']['processes']
        out['processes'] = processes
        layers['userspace'] = {'status': 'observed' if any(p['pid'] == 1 for p in processes) else 'unavailable',
                               'evidence': [p for p in processes if p['pid'] == 1]}
        layers['compositor'] = {'status': 'present' if any(p['name'] == 'backboardd' for p in processes) else 'unavailable',
                                'evidence': [p for p in processes if p['name'] in ('backboardd', 'SpringBoard')],
                                'limitation': 'process presence does not prove a frame reached the display'}
    except Fail as error:
        out['process_error'] = error.as_json()
    for name, layer in layers.items():
        deps = DEPENDENCIES.get(name, [])
        layer['dependencies'] = deps
        layer['unavailable_dependencies'] = [d for d in deps if layers.get(d, {}).get('status') == 'unavailable']
        # Preserve the direct error even when a dependency is unavailable.
        if layer['status'] == 'unavailable' and layer['unavailable_dependencies']:
            layer['status'] = 'blocked'
    return out


def compare(before, after):
    changes = []
    for name in sorted(set(before.get('layers', {})) | set(after.get('layers', {}))):
        a, b = before.get('layers', {}).get(name), after.get('layers', {}).get(name)
        if a != b:
            changes.append({'layer': name, 'before': a, 'after': b})
    return {'changes': changes, 'note': 'counters and cumulative CPU time change naturally; inspect evidence before declaring a regression'}


def report(directory):
    os.makedirs(directory, exist_ok=False)
    result = snapshot()
    with open(os.path.join(directory, 'snapshot.json'), 'w') as f:
        json.dump(result, f, indent=2)
    try:
        text = device.fetch_log(os.path.join(directory, 'console.log'))
        with open(os.path.join(directory, 'log-summary.json'), 'w') as f:
            json.dump(logs.summary(text), f, indent=2)
    except Fail as error:
        result['log_error'] = error.as_json()
    try:
        result['screenshot'] = device.screenshot(os.path.join(directory, 'screen.png'), 2)
    except Fail as error:
        result['screen_error'] = error.as_json()
    import subprocess
    result['versions'] = {}
    for name, args in [('kernel_repo', ['git', 'rev-parse', 'HEAD']),
                       ('userland_repo', ['git', '--git-dir=.userland.git', 'rev-parse', 'HEAD'])]:
        r = subprocess.run(args, cwd=device.ROOT, capture_output=True, text=True, timeout=10)
        result['versions'][name] = r.stdout.strip() if r.returncode == 0 else {'error': r.stderr.strip()}
    with open(os.path.join(directory, 'report.json'), 'w') as f:
        json.dump(result, f, indent=2)
    return {'directory': os.path.abspath(directory), 'files': sorted(os.listdir(directory)),
            'note': 'contains device logs and state; review private data before sharing'}
