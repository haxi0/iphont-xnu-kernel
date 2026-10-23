#!/usr/bin/env python3
"""
A phone's darwin: where it is and what it is, in one file that the loader, the kernel and tools/darwin-write.py
all take it from. Every phone gets its darwin cut from the end of its own userdata (tools/userdata-carve.py),
so two phones of the same model have it in different places (a 128 GB and a 256 GB POCO X5 Pro differ by
tens of millions of blocks) and with different GUIDs.

  darwinid.py from-gpt GPT_HEAD SERIAL   read darwin's entry out of a GPT (8 blocks, as the carve makes them)
                                         and write phones/SERIAL.darwin
  darwinid.py header FILE OUT            the C header the loader is built with (xnuloader/build.sh, DARWIN_ID=)
  darwinid.py bootargs FILE              the kernel's darwin_lba= darwin_blocks= (common/flash/install.sh)
  darwinid.py show FILE
  darwinid.py mark FILE OUT              (a file-backed darwin) the marker block that proves the file is still the
                                         one mapped: written into darwin at MARK_BLOCK by the installer
  darwinid.py where FILE BLOCK           (a file-backed darwin) darwin's 4 KB block BLOCK, as a block of its host
  darwinid.py put FILE IMAGE DISK OFFSET BLOCK
                                         (tests) IMAGE into a disk image DISK whose host partition starts at byte
                                         OFFSET, at darwin's block BLOCK, through the map: common/flash/qemu-test.sh
  darwinid.py get FILE DISK OFFSET BLOCK COUNT   ... and COUNT blocks back out, to standard output

Two kinds of darwin:
  partition  a GPT partition of its own, cut from the end of userdata (tools/userdata-carve.py)
  file       a file in userdata that f2fs never moves (tools/android/mkpinned: pinned and allocated in full,
             as Android keeps its swap files and Dynamic System Update images), reached through the map of
             where its blocks lie (FIEMAP) -- no change to the partition table or to the filesystem's size.
             The loader builds a block device of darwin's blocks out of that map (xnuloader/darwinfile.c).

phones/ is not in the repository: it describes one person's phone. Without a file, everything keeps the values
of the phone the port was brought up on.
"""
import json, os, struct, sys, uuid

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
PHONES = os.path.join(ROOT, 'phones')
BS = 4096
APFS = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')
LEAST = 3145728                 # the var store's two slots end here
MARK_BLOCK = 2091521            # a file-backed darwin's marker: right after the boot state (common/flash/bundlefmt.py)
DEFAULT = {'first': 25374720, 'last': 30617594, 'partn': 34,
           'unique': 'fd6854ad-24aa-4fc3-94fe-290116858d68', 'type': str(APFS)}


def from_gpt(head_path, serial):
    head = open(head_path, 'rb').read()
    h = head[BS:2 * BS]
    if h[:8] != b'EFI PART':
        sys.exit('darwinid: no GPT header in %s' % head_path)
    elba, count, esize = struct.unpack_from('<Q', h, 72)[0], struct.unpack_from('<I', h, 80)[0], struct.unpack_from('<I', h, 84)[0]
    found = None
    for i in range(count):
        e = head[elba * BS + i * esize:elba * BS + (i + 1) * esize]
        if len(e) < 128:
            break
        if e[56:128].decode('utf-16le', 'replace').split('\0')[0] == 'darwin':
            found = {'first': struct.unpack_from('<Q', e, 32)[0], 'last': struct.unpack_from('<Q', e, 40)[0],
                     'partn': i + 1, 'unique': str(uuid.UUID(bytes_le=e[16:32])),
                     'type': str(uuid.UUID(bytes_le=e[:16]))}
    if found is None:
        sys.exit('darwinid: no entry named darwin in %s' % head_path)
    if found['type'] != str(APFS) or found['last'] - found['first'] + 1 < LEAST:
        sys.exit('darwinid: darwin is not an APFS container of at least %d blocks' % LEAST)
    os.makedirs(PHONES, exist_ok=True)
    out = os.path.join(PHONES, serial + '.darwin')
    json.dump(found, open(out, 'w'), indent=1)
    print('darwinid: %s: LBA %d..%d (%.2f GiB), sda%d, %s' % (out, found['first'], found['last'],
          (found['last'] - found['first'] + 1) * BS / 2**30, found['partn'], found['unique']))


def load(path):
    return json.load(open(path)) if path and os.path.exists(path) else dict(DEFAULT)


def extents_sum(d):
    h = 0xcbf29ce484222325
    for at, n in d['extents']:
        for b in struct.pack('<QQ', at, n):
            h ^= b
            h = (h * 0x100000001b3) & 0xffffffffffffffff
    return h


def mark(path, out):
    d = load(path)
    if d.get('kind') != 'file':
        sys.exit('darwinid: %s is not a file-backed darwin' % path)
    blk = b'HKDARWIN' + struct.pack('<QQQQ', int(d['mark'], 16), d['blocks'], len(d['extents']), extents_sum(d))
    open(out, 'wb').write(blk + b'\0' * (BS - len(blk)))


def where(path, block):
    d = load(path)
    at = 0
    for first, n in d['extents']:
        if block < at + n:
            return first + (block - at)
        at += n
    sys.exit('darwinid: block %d is past darwin\'s %d' % (block, at))


def pieces(d, block, count):
    """(host block, blocks) for darwin's blocks block..block+count-1, along the map"""
    at = 0
    for first, n in d['extents']:
        lo, hi = max(block, at), min(block + count, at + n)
        if lo < hi:
            yield first + (lo - at), hi - lo
        at += n
    if block + count > at:
        sys.exit('darwinid: blocks past darwin\'s %d' % at)


def put(path, image, disk, offset, block):
    d = load(path)
    data = open(image, 'rb').read()
    data += b'\0' * (-len(data) % BS)
    with open(disk, 'r+b') as f:
        done = 0
        for host, n in pieces(d, block, len(data) // BS):
            f.seek(offset + host * BS)
            f.write(data[done:done + n * BS])
            done += n * BS


def get(path, disk, offset, block, count):
    d = load(path)
    with open(disk, 'rb') as f:
        for host, n in pieces(d, block, count):
            f.seek(offset + host * BS)
            sys.stdout.buffer.write(f.read(n * BS))


def header(path, out):
    d = load(path)
    if d.get('kind') == 'file':
        with open(out, 'w') as o:
            o.write('/* made by common/flash/darwinid.py from %s: this phone\'s darwin is a file of %s */\n'
                    % (os.path.relpath(path, ROOT), d['host']))
            o.write('#define DARWIN_FILE 1\n#define DARWIN_FILE_HOST "%s"\n' % d['host'])
            o.write('#define DARWIN_FILE_BLOCKS %dULL\n#define DARWIN_FILE_MARK 0x%sULL\n' % (d['blocks'], d['mark']))
            o.write('#define DARWIN_FILE_SUM 0x%016xULL\n#define DARWIN_FILE_EXTENTS %d\n' % (extents_sum(d), len(d['extents'])))
            o.write('static const struct { uint64_t at, count; } darwin_file_extents[DARWIN_FILE_EXTENTS] = {\n')
            for at, n in d['extents']:
                o.write('\t{ %d, %d },\n' % (at, n))
            o.write('};\n')
            # the partition's values stay defined (unused): the code that checks a partition still compiles
            o.write('#define BLOCK_DARWIN_FIRST 0ULL\n#define BLOCK_DARWIN_LAST 0ULL\n')
            o.write('#define BLOCK_DARWIN_UNIQ { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } }\n')
        return
    u = uuid.UUID(d['unique'])
    f = u.fields
    node = u.bytes[8:]
    with open(out, 'w') as o:
        o.write('/* made by common/flash/darwinid.py from %s: where this phone\'s darwin is */\n'
                % (os.path.relpath(path, ROOT) if path and os.path.exists(path) else 'the defaults'))
        o.write('#define BLOCK_DARWIN_FIRST %dULL\n#define BLOCK_DARWIN_LAST %dULL\n' % (d['first'], d['last']))
        o.write('#define BLOCK_DARWIN_UNIQ { 0x%08x, 0x%04x, 0x%04x, { %s } }\n'
                % (f[0], f[1], f[2], ', '.join('0x%02x' % b for b in node)))
        o.write('#define DARWIN_FILE 0\n')


def main(a):
    if a[:1] == ['from-gpt'] and len(a) == 3:
        from_gpt(a[1], a[2])
    elif a[:1] == ['header'] and len(a) == 3:
        header(a[1], a[2])
    elif a[:1] == ['bootargs'] and len(a) == 2:
        d = load(a[1])
        if d.get('kind') == 'file':
            print('darwin_file=1 darwin_blocks=%d' % d['blocks'])
        else:
            print('darwin_lba=%d darwin_blocks=%d' % (d['first'], d['last'] - d['first'] + 1))
    elif a[:1] == ['mark'] and len(a) == 3:
        mark(a[1], a[2])
    elif a[:1] == ['put'] and len(a) == 6:
        put(a[1], a[2], a[3], int(a[4]), int(a[5]))
    elif a[:1] == ['get'] and len(a) == 6:
        get(a[1], a[2], int(a[3]), int(a[4]), int(a[5]))
    elif a[:1] == ['where'] and len(a) == 3:
        print(where(a[1], int(a[2])))
    elif a[:1] == ['show'] and len(a) == 2:
        print(json.dumps(load(a[1]), indent=1))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
