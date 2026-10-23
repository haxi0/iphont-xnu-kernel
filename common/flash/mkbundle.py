#!/usr/bin/env python3
"""
Make a boot bundle for an installed device: the "XNUP" container the loader runs (kernel, boot volume,
command line -- the same one `xl run` sends) behind a header that the loader checks before it boots it:
the container's SHA-256 and, with --sign, an Ed25519 signature by the owner key (common/flash/ownerkey.py).

The bundle goes into one of the two boot slots of darwin (formats: common/flash/bundlefmt.py).

usage: mkbundle.py -o bundle.img --kernel K --ramdisk R [--cmdline "..."] [--generation N] [--label TEXT]
                   [--sign [KEYFILE]]
"""
import argparse, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import bundlefmt as fmt
import ownerkey

PAYLOAD_LIMIT = 0x04000000 - 0x100000

ap = argparse.ArgumentParser()
ap.add_argument('-o', '--output', required=True)
ap.add_argument('--kernel', required=True)
ap.add_argument('--ramdisk', required=True)
ap.add_argument('--cmdline', default='')
ap.add_argument('--resident', default=None, help="the image `xl ramdisk` would upload (md0; the --ramdisk is then md1)")
ap.add_argument('--generation', type=int, default=None, help='default: seconds since 1970 (newer wins)')
ap.add_argument('--label', default=None, help='shown in the boot menu (default: the date)')
ap.add_argument('--sign', nargs='?', const=ownerkey.DEFAULT, default=None, metavar='KEYFILE',
                help='sign with the owner key (default %s)' % ownerkey.DEFAULT)
args = ap.parse_args()

with tempfile.TemporaryDirectory() as td:
    cont = os.path.join(td, 'payload.bin')
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'xnuloader', 'mkpayload.py'), '-o', cont,
                           f'kernel={args.kernel}', f'ramdisk={args.ramdisk}'] +
                          ([f'resident={args.resident}'] if args.resident else []) + ['--cmdline', args.cmdline],
                          stdout=subprocess.DEVNULL)
    data = open(cont, 'rb').read()
if len(data) > PAYLOAD_LIMIT or len(data) > (fmt.SLOT_BLOCKS - 1) * fmt.BLOCK:
    sys.exit(f'mkbundle: the container is {len(data)} bytes, more than a slot or the payload window takes')
gen = args.generation if args.generation is not None else int(time.time())
label = args.label if args.label is not None else time.strftime('%Y-%m-%d %H:%M')
sign = ownerkey.signer(args.sign) if args.sign else None
out = fmt.make_header(gen, data, label, sign) + data
out += b'\0' * (-len(out) % fmt.BLOCK)
open(args.output, 'wb').write(out)
print(f'mkbundle: {args.output}: generation {gen} "{label}", container {len(data)} bytes, '
      f'{len(out) // fmt.BLOCK} blocks, {"signed" if sign else "unsigned"}')
