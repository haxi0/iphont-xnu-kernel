"""Build integration with the repository's existing compiler drivers and reproducible validation artifacts."""
import json
import os
import re
import signal
import subprocess
import sys
import time
from . import agent, device
from .errors import Fail


def build(component, source=None, timeout=1800):
    commands = {
        'kernel': './xnu-build.sh -j8 -k',
        'loader': 'OUT=../out/iphont/loader.efi xnuloader/build.sh',
        'agent': '. ./xnu-env.sh; xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 -fobjc-arc -framework Foundation -framework IOKit -o out/iphont/iphontd userland/src/iphontd.m',
        'images': 'REST=1 FLASHBOOT=1 CRYPTEXES=flash userland/build.sh',
    }
    if component not in commands or timeout <= 0:
        raise Fail('usage', 'invalid component or timeout')
    output = device.path('out/iphont')
    os.makedirs(output, exist_ok=True)
    logfile = os.path.join(output, f'{component}-{time.time_ns()}.log')
    env = dict(os.environ)
    if source:
        env['XNU_SRC'] = os.path.abspath(source)
    started = time.time()
    with open(logfile, 'wb') as log:
        proc = subprocess.Popen(['nix-shell', '--run', commands[component]], cwd=device.ROOT, env=env,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            status = proc.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as e:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL); proc.wait()
            if isinstance(e, KeyboardInterrupt): raise
            raise Fail('timeout', 'build timed out; compiler process group stopped', detail={'log': logfile})
    result = {'component': component, 'exit_code': status, 'log': logfile, 'elapsed_s': round(time.time() - started, 2)}
    if status:
        # XNU's documented export checks may fail after a valid kernel links. Report that distinction explicitly.
        kernel = os.path.join(env.get('XNU_SRC', device.path('xnu-src/xnu-8796.121.2')),
                              'BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837')
        result['fresh_kernel_linked'] = os.path.isfile(kernel) and os.path.getmtime(kernel) >= started
        with open(logfile, errors='replace') as f: text = f.read()
        errors = re.findall(r'make(?:\[\d+\])?: \*\*\* \[(.*?)\] Error \d+', text)
        known = any('config/Makefile:' in e for e in errors)
        only_known = errors and all('config/Makefile:' in e or 'makedefs/MakeInc.' in e or 'Makefile:239: all' in e for e in errors)
        if component == 'kernel' and status == 2 and os.path.isfile(kernel) and known and only_known and not re.search(r'\berror:|undefined symbol', text, re.I):
            result.update(artifact=kernel, status='linked_with_known_export_failures',
                          checks={'compile_and_link': 'passed', 'export_checks': 'known repository failure; not passed'})
            return result
        raise Fail('build_failed', 'build driver returned a failure; inspect its log and fresh_kernel_linked', detail=result)
    return result


def tests(level):
    if level == 'host':
        r = subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tools/iphont/tests', '-v'],
                           cwd=device.ROOT, env=dict(os.environ, PYTHONPATH=device.path('tools')),
                           capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise Fail('check_failed', 'iphont host tests failed', detail={'output': r.stdout + r.stderr})
        return {'scope': 'host only', 'output': r.stdout + r.stderr}
    from . import diagnostics
    results = []
    probes = [('agent', lambda: agent.request('hello')),
              ('processes', lambda: agent.request('processes')),
              ('registry', lambda: agent.request('registry')),
              ('kernel_version', lambda: agent.sysctl('kern.osrelease', 'string')),
              ('screenshot', lambda: device.screenshot(device.path('out/iphont/device-test.png'), 4))]
    for name, probe in probes:
        try:
            result = probe()
            if name == 'processes' and not result['data']['processes']: raise Fail('check_failed', 'no processes returned')
            if name == 'registry' and not result['data']['entries']: raise Fail('check_failed', 'empty IOService plane')
            results.append({'check': name, 'passed': True, 'result': result})
        except Fail as error:
            results.append({'check': name, 'passed': False, 'error': error.as_json()})
    report = {'scope': 'live device, read-only smoke checks', 'checks': results}
    if not all(r['passed'] for r in results):
        raise Fail('check_failed', 'device smoke checks did not all pass', detail=report)
    return report


def prepare():
    """Package a OnePlus-specific RAM loader image without changing the shared Mu-Silicium build output."""
    import shutil
    profile = device.profile()
    target = profile.get('UEFI_TARGET')
    if target != 'instantnoodle':
        raise Fail('refused', 'iphont RAM loader packaging currently supports instantnoodle only')
    mu = device.path('Mu-Silicium')
    fd = profile.get('UEFI_FD') or os.path.join(mu, f'Build/{target}Pkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd')
    dtb = profile.get('DTB') or os.path.join(mu, f'Resources/DTBs/{target}.dtb')
    loader = device.loader_file()
    for file in (fd, dtb, loader):
        if not os.path.isfile(file): raise Fail('not_found', f'RAM boot input missing: {file}')
    work = device.path('out/iphont/bootimage')
    os.makedirs(work, exist_ok=True)
    shutil.copyfile(device.path('mkxnuimg.py'), os.path.join(work, 'mkxnuimg.py'))
    own_mu = os.path.join(work, 'Mu-Silicium')
    shutil.copytree(os.path.join(mu, 'BootShim'), os.path.join(own_mu, 'BootShim'), dirs_exist_ok=True)
    shutil.copytree(os.path.join(mu, 'Resources/Scripts'), os.path.join(own_mu, 'Resources/Scripts'), dirs_exist_ok=True)
    shutil.copyfile(os.path.join(mu, 'Resources/ramdisk'), os.path.join(own_mu, 'Resources/ramdisk'))
    payload = device.path('out/iphont/usb-payload.bin')
    image = device.path('out/iphont/loader-ram.img')
    device.payload(payload, [('loader.efi', loader)], usb=True)
    env = dict(os.environ, **{k: profile.get(k, '') for k in ('FD_BASE', 'FD_SIZE', 'PAYLOAD_BASE', 'UEFI_TARGET')},
               UEFI_FD=os.path.abspath(fd), DTB=os.path.abspath(dtb))
    logfile = device.path('out/iphont/prepare.log')
    with open(logfile, 'wb') as log:
        r = subprocess.run(['nix-shell', '--run', 'python3 out/iphont/bootimage/mkxnuimg.py -p out/iphont/usb-payload.bin -o out/iphont/loader-ram.img'],
                           cwd=device.ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    if r.returncode:
        raise Fail('build_failed', 'RAM loader packaging failed', detail={'log': logfile})
    return {'image': image, 'loader': loader, 'profile': target, 'log': logfile,
            'flashed': False, 'booted': False, 'next': 'only fastboot boot this image when the owner permits a RAM loader reload'}


def kernel_test_results(text):
    """QEMU replays its console. Recover complete JSON copies, never count the replay as extra probes."""
    records, incomplete = {}, 0
    for line in text.splitlines():
        if 'iphont-test: {' not in line:
            continue
        try:
            record = json.loads(line.split('iphont-test: ', 1)[1])
        except json.JSONDecodeError:
            incomplete += 1
            continue
        records[record['probe']] = record
    expected = {'hello', 'sysctl kern.osrelease', 'processes', 'registry', 'threads 0', 'events 0', 'control denied'}
    errors = []
    if set(records) != expected:
        errors.append('missing or unexpected probes: ' + ', '.join(sorted(expected ^ set(records))))
    for name, record in records.items():
        if record['errno'] != (1 if name == 'control denied' else 0):
            errors.append(f'{name}: errno {record["errno"]}')
    if 'hello' in records and (records['hello'].get('result') or {}).get('control') is not False:
        errors.append('read-only authorization did not remain disabled')
    if 'sysctl kern.osrelease' in records:
        raw = bytes.fromhex((records['sysctl kern.osrelease'].get('result') or {}).get('hex', ''))
        if not raw.rstrip(b'\0').startswith(b'22.'):
            errors.append('unexpected Darwin release')
    for name, key in [('processes', 'processes'), ('registry', 'entries'), ('threads 0', 'threads'), ('events 0', 'events')]:
        if name in records and not (records[name].get('result') or {}).get(key):
            errors.append(f'{name}: empty native result')
    if re.search(r'(?:^|\]: )panic\(cpu', text, re.M):
        errors.append('kernel panicked')
    if 'iphont-test: done' not in text or '--- loader_return_after expired ---' not in text:
        errors.append('test completion or orderly timed return missing')
    return {'scope': 'actual XNU under QEMU; no USB or iOS daemon validation', 'probes': records,
            'incomplete_console_copies': incomplete, 'errors': errors, 'passed': not errors}


def kernel_test(kernel, seconds=25):
    import shutil
    import tempfile
    if not os.path.isfile(kernel) or not 10 <= seconds <= 120:
        raise Fail('usage', 'kernel-test needs an existing kernel and seconds in 10..120')
    with open('/proc/meminfo') as f:
        available = next(int(l.split()[1]) for l in f if l.startswith('MemAvailable:'))
    if available < 3 * 1024 * 1024:
        raise Fail('refused', 'less than 3 GiB available; do not compete with another QEMU instance')
    output = device.path('out/iphont')
    os.makedirs(output, exist_ok=True)
    work = tempfile.mkdtemp(prefix='kernel-test-', dir=output)
    os.makedirs(os.path.join(work, 'qemu')); os.makedirs(os.path.join(work, 'xnuloader'))
    run_script = os.path.join(work, 'qemu', 'run.sh')
    shutil.copyfile(device.path('qemu/run.sh'), run_script); os.chmod(run_script, 0o755)
    shutil.copyfile(device.loader_file(), os.path.join(work, 'xnuloader', 'loader.efi'))
    init = device.path('out/iphont/idle-init')
    compile_log = os.path.join(work, 'init-build.log')
    with open(compile_log, 'wb') as f:
        r = subprocess.run(['nix-shell', '--run', '. ./xnu-env.sh; xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o out/iphont/idle-init tools/iphont/tests/idle_init.c'],
                           cwd=device.ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=60)
    if r.returncode:
        raise Fail('build_failed', 'QEMU process-1 fixture did not compile', detail={'log': compile_log})
    device.payload(os.path.join(work, 'qemu', 'payload.bin'), [('kernel', os.path.abspath(kernel)), ('ramdisk', init)],
                   cmdline=f'loader_return_after={seconds} cpus=2 iphont=1 iphont_selftest=1 -v')
    logfile = os.path.join(work, 'console.log')
    import shlex
    with open(logfile, 'wb') as f:
        r = subprocess.run(['nix-shell', '--run', 'QEMU_SMP=2 QEMU_MEM=6G ' + shlex.join([run_script, str(seconds + 35)])],
                           cwd=device.ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=seconds + 45)
    with open(logfile, errors='replace') as f: result = kernel_test_results(f.read())
    if r.returncode not in (0, 124):
        result['errors'].append(f'unexpected QEMU wrapper exit {r.returncode}')
        result['passed'] = False
    result.update(log=logfile, process_exit=r.returncode, kernel=os.path.abspath(kernel))
    with open(os.path.join(work, 'result.json'), 'w') as f: json.dump(result, f, indent=2)
    if not result['passed']:
        raise Fail('check_failed', 'XNU QEMU self-test failed', detail=result)
    return result
