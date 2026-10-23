#!/usr/bin/env python3
"""
Install, update and switch HackiOS on a phone as a dual boot next to Android -- from a computer, with the
phone in Android (adb with root) or in fastboot.

  flashctl.py status                     what is where: slots, darwin, the boot bundles, the UEFI   (writes nothing)
  flashctl.py backup                     the partitions the install touches or depends on -> backups/<serial>/<time>/
  flashctl.py write-bundle BUNDLE        a boot bundle into the older of darwin's two boot slots, read back
  flashctl.py write-uefi IMAGE           the UEFI with the loader into the spare slot's boot partition, read back
  flashctl.py switch hackios             make the spare slot active and reboot into HackiOS (from Android)
  flashctl.py switch android             make Android's slot active again (from fastboot; HackiOS's menu does it too)
  flashctl.py restore                    put the spare slot's boot partition back from the newest backup
  flashctl.py health                     the boot guard: what was started, how often, what came up   (writes nothing)
  flashctl.py verify [PUBKEY|KEYFILE]    read both boot slots back whole: SHA-256, and the signature   (writes nothing)
  flashctl.py rollback                   boot the previous version from now on (until `unhold` or a new bundle)
  flashctl.py unhold                     the newest version again, and forget a generation given up on
  flashctl.py doctor                     everything the install needs, checked one by one             (writes nothing)
  flashctl.py menu-on IMAGE              the menu on every power-on: Android's HackiOS module (KernelSU / Magisk)
                                         makes the HackiOS slot active again once Android is up (common/flash/rearm)
  flashctl.py menu-off                   remove that module: the phone starts Android directly again

The phone must be the device the profile is for: install.sh passes the profile's CODENAMES in
HACKIOS_CODENAMES, and nothing is written to a phone whose ro.product.device is not one of them.

How the dual boot is laid out (xnuloader/flashboot.c): the phone runs Android from one A/B slot; the other
slot's boot partition holds the UEFI with our loader. Android's partitions are never written. darwin holds the
iOS volume, the cryptexes, the boot area (two bundle slots) and the var store. Switching slots is what
`bootctl set-active-boot-slot` / `fastboot --set-active` do; the loader's menu switches back.

Safety:
  * every write is preceded by a check that it goes where it should (darwin by tools/darwin-write.py's own
    identity checks; the boot partition by name, slot and size) and followed by a read back;
  * write-uefi refuses without a backup of the partition it overwrites, and never touches Android's slot;
  * switch refuses while a Virtual A/B update is not merged, and when the spare slot has no UEFI of ours;
  * Android can always be brought back from fastboot (volume down + power): `fastboot --set-active=<slot>`.
Set FLASHCTL_YES=1 to answer the confirmations (scripts); every action goes into logs/flashctl.log.
"""
import hashlib, json, os, struct, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bundlefmt as fmt

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DW = [sys.executable, os.path.join(ROOT, 'tools', 'darwin-write.py')]
LOG = os.path.join(ROOT, 'logs', 'flashctl.log')
BS = fmt.BLOCK
AREA_FIRST, SLOT_BLOCKS, SLOTS = fmt.AREA_FIRST, fmt.SLOT_BLOCKS, fmt.SLOTS
TMP = '/data/local/tmp/hackios-flash.img'


class Refused(Exception):
    pass


def log(line):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as f:
        f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), line))
    print('flashctl: ' + line)


def sh(script):
    r = subprocess.run(['adb', 'shell', 'su -c sh'], input=script.encode(), capture_output=True, timeout=3600)
    if r.returncode != 0:
        raise Refused('the phone said %d: %s' % (r.returncode, (r.stdout + r.stderr).decode(errors='replace')[-300:]))
    return r.stdout.decode(errors='replace').strip()


def raw(cmd):
    r = subprocess.run(['adb', 'exec-out', 'su -c "%s"' % cmd], capture_output=True, timeout=1800)
    if r.returncode != 0:
        raise Refused('reading from the phone: exit %d' % r.returncode)
    return r.stdout


def confirm(what):
    if os.environ.get('FLASHCTL_YES') == '1':
        return
    if input('%s\ntype YES to go on: ' % what).strip() != 'YES':
        raise Refused('not confirmed')


def in_android():
    r = subprocess.run(['adb', 'get-state'], capture_output=True, text=True)
    return r.stdout.strip() == 'device'


def slots():
    """(android's slot letter, the spare slot letter)"""
    cur = sh('getprop ro.boot.slot_suffix').strip().lstrip('_')
    if cur not in ('a', 'b'):
        raise Refused('not an A/B phone (ro.boot.slot_suffix is %r)' % cur)
    return cur, 'b' if cur == 'a' else 'a'


# The properties a phone's identity is read from, most trustworthy first. A ported ROM may dress the phone up
# as another model (this one's ro.product.device says OP5D2BL1, a OnePlus), but the bootloader's own
# hardware SKU and the board name stay what the phone is.
IDENTITY_PROPS = ('ro.boot.hardware.sku', 'ro.boot.product.hardware.sku', 'ro.product.board',
                  'ro.product.system.device', 'ro.product.device')


def identity():
    """refuse a phone that is not the device the profile is for; returns the codename that matched"""
    want = os.environ.get('HACKIOS_CODENAMES', '').split()
    if not want:
        raise Refused('no device profile given (HACKIOS_CODENAMES): run through common/flash/install.sh or '
                      '`common/hackios`, which take it from common/devices/<device>.conf')
    seen = {p: sh('getprop %s' % p).strip() for p in IDENTITY_PROPS}
    for p in IDENTITY_PROPS:
        if seen[p] in want:
            return seen[p]
    raise Refused('this phone is not %s (%s): nothing is written to it' % (
        ' / '.join(want), ', '.join('%s=%s' % (p, v) for p, v in seen.items() if v)))


def unlocked():
    """the bootloader's real state, from the kernel's boot parameters: a ROM's properties may say otherwise"""
    boot = sh('cat /proc/bootconfig 2>/dev/null; tr " " "\\n" < /proc/cmdline')
    for line in boot.splitlines():
        if 'verifiedbootstate' in line:
            return 'orange' in line
    return sh('getprop ro.boot.flash.locked') == '0' or sh('getprop ro.boot.verifiedbootstate') == 'orange'


def preflight():
    if not in_android():
        raise Refused('the phone is not in Android with adb (adb get-state)')
    if sh('id -u') != '0':
        raise Refused('no root shell (su)')
    identity()
    if not unlocked():
        raise Refused('the bootloader is locked')
    cur, spare = slots()
    pending = sh('ls /metadata/ota/snapshots 2>/dev/null | wc -l')
    return cur, spare, pending.strip() not in ('', '0')


def part_size(name):
    return int(sh('blockdev --getsize64 /dev/block/by-name/%s' % name))


def sha_of_part(name, size):
    return sh('head -c %d /dev/block/by-name/%s | sha256sum' % (size, name)).split()[0]


def darwin_block(blk, count=1):
    return raw('dd if=/dev/block/by-name/darwin bs=%d skip=%d count=%d 2>/dev/null' % (BS, blk, count))


def bundle_headers():
    """the two boot slots' headers, read from darwin (None: empty or torn)"""
    return [fmt.parse_header(darwin_block(AREA_FIRST + s * SLOT_BLOCKS)) for s in range(SLOTS)]


def boot_state():
    return fmt.parse_state(darwin_block(fmt.STATE_BLOCK))


def write_state(st):
    """the boot state into darwin, through darwin-write.py's checks like every other write there"""
    path = os.path.join(ROOT, 'out', 'boot-state.img')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(fmt.make_state(**st))
    r = subprocess.run(DW + ['write', path, str(fmt.STATE_BLOCK)], env=dict(os.environ, DARWIN_WRITE='yes'))
    if r.returncode != 0:
        raise Refused('darwin-write refused or failed writing the boot state')
    if fmt.parse_state(darwin_block(fmt.STATE_BLOCK)) != fmt.parse_state(open(path, 'rb').read()):
        raise Refused('the boot state did not read back the same')


def cmd_status():
    cur, spare, pending = preflight()
    print('android slot      : _%s' % cur)
    print('hackios slot      : _%s (boot_%s, %d MB)' % (spare, spare, part_size('boot_' + spare) >> 20))
    print('A/B update pending: %s' % ('YES -- finish it in Android before switching' if pending else 'no'))
    dw = subprocess.run(DW + ['check'], capture_output=True, text=True)
    print('darwin            : %s' % ('identity right' if dw.returncode == 0 else 'NOT as expected: ' + dw.stdout.strip()[-200:]))
    blk = raw('dd if=/dev/block/by-name/darwin bs=%d count=1 2>/dev/null' % BS)
    print('iOS volume        : %s' % ('present (APFS container)' if blk[32:36] == b'NXSB' else 'missing -- write it first (README)'))
    for s, h in enumerate(bundle_headers()):
        print('boot slot %d       : %s' % (s, 'generation %d "%s", %d bytes, %s' % (
            h['generation'], h['label'], h['size'], 'signed' if h['signed'] else 'unsigned') if h else 'empty'))
    head = raw('dd if=/dev/block/by-name/boot_%s bs=4096 count=1 2>/dev/null' % spare)
    print('boot_%s           : %s' % (spare, 'an Android boot image' if head[:8] == b'ANDROID!' else 'unknown contents'))


def cmd_backup():
    cur, spare, _ = preflight()
    serial = subprocess.run(['adb', 'get-serialno'], capture_output=True, text=True).stdout.strip()
    d = os.path.join(ROOT, 'backups', serial, time.strftime('%Y%m%d-%H%M%S'))
    os.makedirs(d)
    names = [p + '_' + s for p in ('boot', 'vbmeta', 'dtbo', 'vendor_boot', 'init_boot') for s in ('a', 'b')]
    manifest = {}
    for n in names:
        if sh('[ -e /dev/block/by-name/%s ] && echo y || echo n' % n) != 'y':
            continue
        data = raw('dd if=/dev/block/by-name/%s bs=1048576 2>/dev/null' % n)
        open(os.path.join(d, n + '.img'), 'wb').write(data)
        manifest[n] = hashlib.sha256(data).hexdigest()
        log('backup %s: %d bytes, %s' % (n, len(data), manifest[n][:16]))
    # the GPT of the unit that holds boot_a/boot_b (the slot switch rewrites its attributes)
    dev = sh('basename $(readlink -f /dev/block/by-name/boot_a) | sed "s/[0-9]*$//"')
    for name, script in (('gpt-primary', 'dd if=/dev/block/%s bs=4096 count=6 2>/dev/null' % dev),
                         ('gpt-backup', 'n=$(( $(blockdev --getsz /dev/block/%s) / 8 )); dd if=/dev/block/%s bs=4096 skip=$((n - 5)) count=5 2>/dev/null' % (dev, dev))):
        data = raw(script)
        open(os.path.join(d, '%s-%s.img' % (name, dev)), 'wb').write(data)
        manifest['%s-%s' % (name, dev)] = hashlib.sha256(data).hexdigest()
    manifest['android_slot'] = cur
    json.dump(manifest, open(os.path.join(d, 'manifest.json'), 'w'), indent=1)
    log('backup in %s' % d)


def newest_backup():
    serial = subprocess.run(['adb', 'get-serialno'], capture_output=True, text=True).stdout.strip()
    base = os.path.join(ROOT, 'backups', serial)
    if not os.path.isdir(base) or not os.listdir(base):
        return None
    return os.path.join(base, sorted(os.listdir(base))[-1])


def cmd_write_bundle(path):
    preflight()
    data = open(path, 'rb').read()
    h = fmt.parse_header(data[:BS])
    if h is None or len(data) % BS or len(data) // BS > SLOT_BLOCKS or len(data) < BS + h['size'] or \
            hashlib.sha256(data[BS:BS + h['size']]).hexdigest() != h['sha256']:
        raise Refused('%s is not a sound bundle from mkbundle.py' % path)
    hs = bundle_headers()
    gen = h['generation']
    # the slot that does not hold the newest good bundle
    good = [(h['generation'], s) for s, h in enumerate(hs) if h]
    slot = 0 if not good else (1 - max(good)[1])
    if good and gen <= max(good)[0]:
        raise Refused('generation %d is not newer than the installed %d' % (gen, max(good)[0]))
    first = AREA_FIRST + slot * SLOT_BLOCKS
    confirm('write a %d MB bundle (generation %d) into darwin boot slot %d (blocks %d..%d)'
            % (len(data) >> 20, gen, slot, first, first + len(data) // BS - 1))
    env = dict(os.environ, DARWIN_WRITE='yes')
    body = path + '.body'
    open(body, 'wb').write(data[BS:])
    head = path + '.head'
    open(head, 'wb').write(data[:BS])
    for step in (DW + ['zero', str(first), '1'], DW + ['write', body, str(first + 1)], DW + ['write', head, str(first)]):
        r = subprocess.run(step, env=env)
        if r.returncode != 0:
            raise Refused('darwin-write refused or failed: %s (the other boot slot is untouched)' % ' '.join(step[2:]))
    os.remove(body); os.remove(head)
    log('bundle generation %d in boot slot %d' % (gen, slot))


def cmd_write_uefi(path):
    cur, spare, _ = preflight()
    img = open(path, 'rb').read()
    if img[:8] != b'ANDROID!':
        raise Refused('%s is not a boot image (mkxnuimg.py makes one)' % path)
    target = 'boot_' + spare
    if spare == cur:
        raise Refused('refusing to write Android\'s own slot')
    size = part_size(target)
    if len(img) > size:
        raise Refused('the image (%d) is bigger than %s (%d)' % (len(img), target, size))
    b = newest_backup()
    if not b or not os.path.exists(os.path.join(b, target + '.img')):
        raise Refused('no backup of %s: run `flashctl.py backup` first' % target)
    want = hashlib.sha256(img).hexdigest()
    confirm('write %s (%d KB, sha256 %s...) to %s -- Android on _%s is not touched' % (path, len(img) >> 10, want[:12], target, cur))
    subprocess.run(['adb', 'push', path, TMP], check=True, capture_output=True)
    sh('[ "$(sha256sum %s | cut -d" " -f1)" = "%s" ] && [ -b /dev/block/by-name/%s ] && '
       'dd if=%s of=/dev/block/by-name/%s bs=1048576 conv=fsync 2>/dev/null; rm -f %s' % (TMP, want, target, TMP, target, TMP))
    got = sha_of_part(target, len(img))
    if got != want:
        raise Refused('read back of %s differs (%s): restore it with `flashctl.py restore`' % (target, got[:12]))
    log('UEFI written to %s, read back right' % target)


def cmd_switch(to):
    if to == 'android':
        r = subprocess.run(['fastboot', 'getvar', 'current-slot'], capture_output=True, text=True, timeout=20)
        if 'current-slot' not in (r.stdout + r.stderr):
            raise Refused('put the phone in fastboot (volume down + power), or choose Android in the HackiOS menu')
        b = newest_backup()
        slot = json.load(open(os.path.join(b, 'manifest.json')))['android_slot'] if b else None
        if slot not in ('a', 'b'):
            raise Refused('no backup says which slot is Android\'s: run `fastboot --set-active=<slot>` yourself')
        subprocess.run(['fastboot', '--set-active=' + slot], check=True)
        subprocess.run(['fastboot', 'reboot'], check=True)
        log('switched to Android (_%s) from fastboot' % slot)
        return
    cur, spare, pending = preflight()
    if pending:
        raise Refused('a Virtual A/B update is still being merged: let Android finish it, reboot once, then switch')
    head = raw('dd if=/dev/block/by-name/boot_%s bs=4096 count=1 2>/dev/null' % spare)
    if head[:8] != b'ANDROID!':
        raise Refused('boot_%s holds no boot image: `flashctl.py write-uefi` first' % spare)
    if not any(bundle_headers()):
        raise Refused('no boot bundle in darwin: `flashctl.py write-bundle` first')
    confirm('make slot _%s active and reboot into HackiOS (the HackiOS menu or fastboot brings Android back)' % spare)
    sh('bootctl set-active-boot-slot %d' % (0 if spare == 'a' else 1))
    log('slot _%s active, rebooting' % spare)
    sh('reboot')


def cmd_restore():
    cur, spare, _ = preflight()
    b = newest_backup()
    src = b and os.path.join(b, 'boot_%s.img' % spare)
    if not src or not os.path.exists(src):
        raise Refused('no backup of boot_%s' % spare)
    cmd_write_uefi_raw(src, 'boot_' + spare)


def cmd_write_uefi_raw(path, target):
    img = open(path, 'rb').read(); want = hashlib.sha256(img).hexdigest()
    confirm('put %s back into %s' % (path, target))
    subprocess.run(['adb', 'push', path, TMP], check=True, capture_output=True)
    sh('[ "$(sha256sum %s | cut -d" " -f1)" = "%s" ] && dd if=%s of=/dev/block/by-name/%s bs=1048576 conv=fsync 2>/dev/null; rm -f %s'
       % (TMP, want, TMP, target, TMP))
    if sha_of_part(target, len(img)) != want:
        raise Refused('read back of %s differs' % target)
    log('%s restored from %s' % (target, path))


def cmd_health():
    preflight()
    hs, st = bundle_headers(), boot_state()
    gens = {h['generation']: h['label'] for h in hs if h}
    name = lambda g: '%d "%s"' % (g, gens[g]) if g in gens else ('%d (no longer installed)' % g if g else '-')
    if st is None:
        print('boot guard        : no boot state yet (HackiOS has not been started from darwin)')
        return
    print('starts of HackiOS : %d' % st['boots'])
    print('started last      : %s, %d time(s) in a row' % (name(st['trying']), st['tries']))
    print('came up last      : %s' % name(st['good']))
    print('given up on       : %s' % name(st['bad']))
    print('held to previous  : %s' % ('yes (flashctl.py unhold)' if st['flags'] & fmt.STATE_HOLD else 'no'))
    if st['trying'] and st['trying'] != st['good']:
        left = fmt.MAX_TRIES - st['tries']
        print('verdict           : %s' % ('NOT COMING UP: the countdown skips it' if left <= 0 else
                                          'not reported up yet: %d more start(s) before it is skipped' % left))
    else:
        print('verdict           : healthy')


def cmd_verify(keyarg=None):
    preflight()
    pub = None
    if keyarg:
        if os.path.exists(keyarg):
            import ownerkey
            pub = ownerkey.public(ownerkey.load(keyarg))
        else:
            pub = bytes.fromhex(keyarg)
    bad = 0
    for s, h in enumerate(bundle_headers()):
        if h is None:
            print('boot slot %d: empty' % s)
            continue
        blocks = (h['size'] + BS - 1) // BS
        body = darwin_block(AREA_FIRST + s * SLOT_BLOCKS + 1, blocks)[:h['size']]
        ok = hashlib.sha256(body).hexdigest() == h['sha256']
        sig = 'unsigned' if not h['signed'] else ('signature not checked (no key given)' if pub is None else
                                                    ('signature GOOD' if fmt.verify(pub, h) else 'signature BAD'))
        print('boot slot %d: generation %d "%s": SHA-256 %s, %s' % (s, h['generation'], h['label'],
                                                                   'good' if ok else 'BAD', sig))
        bad += (not ok) or sig == 'signature BAD'
    if bad:
        raise Refused('%d slot(s) would be refused by the loader' % bad)


def cmd_hold(on):
    preflight()
    hs = [h for h in bundle_headers() if h]
    if on and len(hs) < 2:
        raise Refused('there is no previous version to go back to')
    st = boot_state() or {'trying': 0, 'tries': 0, 'flags': 0, 'good': 0, 'boots': 0, 'bad': 0}
    st['flags'] = (st['flags'] | fmt.STATE_HOLD) if on else (st['flags'] & ~fmt.STATE_HOLD)
    if not on:
        st['bad'] = 0
        st['tries'] = 0
    write_state(st)
    log('boot guard: %s' % ('held to the previous version' if on else 'the newest version again'))


def cmd_doctor():
    """every precondition, each with a verdict; nothing is written"""
    checks = []

    def check(name, fn):
        try:
            detail = fn()
            checks.append((True, name, detail or 'ok'))
        except Exception as e:  # noqa: a check that cannot run is a failed check
            checks.append((False, name, str(e)))

    def tool(t):
        r = subprocess.run(['which', t], capture_output=True, text=True)
        if r.returncode:
            raise Refused('%s is not on PATH' % t)
        return r.stdout.strip()

    check('adb', lambda: tool('adb'))
    check('fastboot', lambda: tool('fastboot'))
    check('python cryptography (signed bundles)', lambda: __import__('cryptography').__version__)
    check('phone in Android', lambda: 'yes' if in_android() else (_ for _ in ()).throw(Refused('adb get-state is not "device"')))
    check('root shell', lambda: 'uid 0' if sh('id -u') == '0' else (_ for _ in ()).throw(Refused('su gives no root')))
    check('device matches the profile', identity)
    check('bootloader unlocked', lambda: 'yes (verifiedbootstate orange)' if unlocked()
          else (_ for _ in ()).throw(Refused('locked: `fastboot flashing unlock` first (wipes the phone)')))
    check('A/B slots', lambda: 'android on _%s, spare _%s' % slots())
    check('no A/B update pending', lambda: 'none' if sh('ls /metadata/ota/snapshots 2>/dev/null | wc -l').strip() in ('', '0')
          else (_ for _ in ()).throw(Refused('an update is merging: reboot Android once and wait')))
    check('darwin partition', lambda: '%d GiB' % (part_size('darwin') >> 30))
    check('iOS volume in darwin', lambda: 'present' if darwin_block(0)[32:36] == b'NXSB'
          else (_ for _ in ()).throw(Refused('missing: write the iOS volume first (README)')))
    check('boot bundles', lambda: ', '.join('gen %d' % h['generation'] for h in bundle_headers() if h) or 'none yet')
    check('a backup of this phone', lambda: newest_backup() or 'none yet (install.sh makes one before it writes)')
    width = max(len(n) for _, n, _ in checks)
    for ok, name, detail in checks:
        print('  %s  %-*s  %s' % ('ok ' if ok else 'NO ', width, name, detail))
    failed = [n for ok, n, _ in checks if not ok]
    if failed:
        raise Refused('%d check(s) failed: %s' % (len(failed), ', '.join(failed)))
    print('doctor: ready for common/flash/install.sh')


def root_manager():
    """'ksud' or 'magisk': whichever manager of root modules the phone has"""
    for tool in ('ksud', 'magisk'):
        if sh('command -v %s >/dev/null 2>&1 && echo y || echo n' % tool) == 'y':
            return tool
    raise Refused('neither KernelSU (ksud) nor Magisk is on the phone: no way to install the boot menu module')


def cmd_menu_on(image):
    """put the rearm module in, configured with the image that is in the spare slot's boot partition"""
    import io, zipfile
    cur, spare, _ = preflight()
    img = open(image, 'rb').read()
    want = hashlib.sha256(img).hexdigest()
    if sha_of_part('boot_' + spare, len(img)) != want:
        raise Refused('boot_%s does not hold %s: `flashctl.py write-uefi %s` first' % (spare, image, image))
    tool = root_manager()
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rearm')
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in ('module.prop', 'service.sh'):
            z.write(os.path.join(here, name), name)
        z.writestr('hackios.conf', 'SPARE=%s\nUEFI_SHA256=%s\nUEFI_BYTES=%d\n' % (spare, want, len(img)))
    path = os.path.join(ROOT, 'out', 'hackios-rearm.zip')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(buf.getvalue())
    confirm('install the HackiOS boot menu module (%s): after Android starts, slot _%s is made active again' % (tool, spare))
    subprocess.run(['adb', 'push', path, '/data/local/tmp/hackios-rearm.zip'], check=True, capture_output=True)
    out = sh('%s /data/local/tmp/hackios-rearm.zip; rm -f /data/local/tmp/hackios-rearm.zip' %
             ('ksud module install' if tool == 'ksud' else 'magisk --install-module'))
    log('boot menu module installed with %s (%s); active from the next start of Android' % (tool, out.splitlines()[-1] if out else 'ok'))


def cmd_menu_off():
    preflight()
    tool = root_manager()
    sh('ksud module uninstall hackios-rearm' if tool == 'ksud' else 'touch /data/adb/modules/hackios-rearm/remove')
    log('boot menu module removed (from the next reboot Android starts directly)')


def main(a):
    try:
        if a[:1] == ['status']: cmd_status()
        elif a[:1] == ['backup']: cmd_backup()
        elif a[:1] == ['write-bundle'] and len(a) == 2: cmd_write_bundle(a[1])
        elif a[:1] == ['write-uefi'] and len(a) == 2: cmd_write_uefi(a[1])
        elif a[:1] == ['switch'] and len(a) == 2 and a[1] in ('hackios', 'android'): cmd_switch(a[1])
        elif a[:1] == ['restore']: cmd_restore()
        elif a[:1] == ['health']: cmd_health()
        elif a[:1] == ['verify'] and len(a) <= 2: cmd_verify(a[1] if len(a) == 2 else None)
        elif a[:1] == ['rollback']: cmd_hold(True)
        elif a[:1] == ['unhold']: cmd_hold(False)
        elif a[:1] == ['doctor']: cmd_doctor()
        elif a[:1] == ['menu-on'] and len(a) == 2: cmd_menu_on(a[1])
        elif a[:1] == ['menu-off']: cmd_menu_off()
        else:
            print(__doc__); return 2
    except Refused as e:
        log('refused: %s' % e)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
