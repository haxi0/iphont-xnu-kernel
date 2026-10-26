"""
Addresses to names, from files that are on this computer:

  kernel   the unstripped kernel of this build (BUILD/obj/.../mach.development.bcm2837.unstripped): its symbol table,
           shifted by the panic's "Kernel text base" against the file's own __TEXT
  user     iOS's dyld shared cache (ios165/<build>/dyld_shared_cache_arm64): which library an address is in and
           where in it, after the crash's "shared cache slide" is taken off
"""
import bisect, glob, os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KERNEL_UNSTRIPPED = os.path.join(ROOT, 'xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/'
                                       'mach.development.bcm2837.unstripped')

LC_SEGMENT_64, LC_SYMTAB, LC_UUID = 0x19, 0x2, 0x1b
ARCHIVE = os.path.join(ROOT, 'out', 'iphont', 'kernels')    # unstripped kernels by UUID, kept by `iphont run`


def macho_uuid(path):
    with open(path, 'rb') as f:
        head = f.read(65536)
    ncmds, at = struct.unpack_from('<I', head, 16)[0], 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', head, at)
        if cmd == LC_UUID:
            u = head[at + 8:at + 24].hex().upper()
            return f'{u[:8]}-{u[8:12]}-{u[12:16]}-{u[16:20]}-{u[20:]}'
        at += size
    return None


def archive_kernel(path=KERNEL_UNSTRIPPED):
    """Keep this build's unstripped kernel under its UUID (a reflink/hard copy), for panics read later."""
    if not os.path.isfile(path):
        return None
    uuid = macho_uuid(path)
    dest = os.path.join(ARCHIVE, f'{uuid}.unstripped')
    if uuid and not os.path.exists(dest):
        os.makedirs(ARCHIVE, exist_ok=True)
        import shutil
        shutil.copyfile(path, dest + '.tmp')
        os.replace(dest + '.tmp', dest)
        kept = sorted(glob.glob(os.path.join(ARCHIVE, '*.unstripped')), key=os.path.getmtime)
        for old in kept[:-20]:                  # the last 20 builds: ~400 MB
            os.unlink(old)
    return uuid


def kernel_for_uuid(uuid):
    """The unstripped kernel whose LC_UUID is uuid: the current build or an archived one."""
    if uuid and os.path.isfile(KERNEL_UNSTRIPPED) and macho_uuid(KERNEL_UNSTRIPPED) == uuid.upper():
        return KERNEL_UNSTRIPPED
    cand = os.path.join(ARCHIVE, f'{uuid.upper()}.unstripped') if uuid else None
    return cand if cand and os.path.isfile(cand) else None


def macho_symbols(path):
    """(sorted addresses, names, __TEXT vmaddr) of a thin arm64 Mach-O's defined symbols."""
    with open(path, 'rb') as f:
        data = f.read()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
    if magic != 0xfeedfacf:
        raise ValueError(f'{path}: not a 64-bit Mach-O')
    at, text_vm, symtab = 32, None, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == LC_SEGMENT_64:
            name = data[at + 8:at + 24].rstrip(b'\0')
            if name == b'__TEXT':
                text_vm = struct.unpack_from('<Q', data, at + 24)[0]
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from('<IIII', data, at + 8)
        at += size
    syms = []
    if symtab:
        symoff, nsyms, stroff, _ = symtab
        for i in range(nsyms):
            strx, ntype, sect, _, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
            if ntype & 0xe0 or (ntype & 0x0e) != 0x0e or sect == 0 or value == 0:
                continue                        # stabs, and anything not defined in a section
            end = data.index(b'\0', stroff + strx)
            syms.append((value, data[stroff + strx:end].decode('ascii', 'replace')))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms], text_vm


_kernel = None


def kernel_symbolizer(text_base=None, path=KERNEL_UNSTRIPPED):
    """A function address -> 'symbol+0xoff' (None when unknown), for a kernel loaded at text_base."""
    global _kernel
    if not path or not os.path.isfile(path):
        return None
    if _kernel is None or _kernel[0] != (path, os.path.getmtime(path)):
        _kernel = ((path, os.path.getmtime(path)), macho_symbols(path))
    addrs, names, text_vm = _kernel[1]
    slide = (int(text_base, 16) - text_vm) if text_base and text_vm else 0

    def lookup(address):
        a = (int(address, 16) if isinstance(address, str) else address) - slide
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0 or a - addrs[i] > 0x100000:
            return None
        return f'{names[i]}+{a - addrs[i]:#x}'
    return lookup


def cache_dir():
    found = sorted(glob.glob(os.path.join(ROOT, 'ios165', '*', 'dyld_shared_cache_arm64')))
    return found[0] if found else None


_images = None


def cache_images(path=None):
    """Sorted (unslid load address, text size, path) of every image in the shared cache."""
    global _images
    path = path or cache_dir()
    if path is None:
        return None
    if _images is None:
        with open(path, 'rb') as f:
            head = f.read(0x200)
            if not head.startswith(b'dyld_v1'):
                return None
            off, count = struct.unpack_from('<QQ', head, 0x88)       # imagesTextOffset, imagesTextCount
            f.seek(off)
            raw = f.read(32 * count)
            images = []
            for i in range(count):
                load, size, path_off = struct.unpack_from('<QII', raw, 32 * i + 16)
                f.seek(path_off)
                name = f.read(256).split(b'\0', 1)[0].decode('utf-8', 'replace')
                images.append((load, size, name))
        images.sort()
        _images = images
    return _images


def user_symbolizer(slide):
    """An address in a process -> {'image', 'offset'} for shared-cache code (None outside the cache)."""
    images = cache_images()
    if not images:
        return None
    starts = [i[0] for i in images]
    s = int(slide, 16) if isinstance(slide, str) else slide

    def lookup(address):
        a = (int(address, 16) if isinstance(address, str) else address) - s
        i = bisect.bisect_right(starts, a) - 1
        if i < 0 or a >= images[i][0] + images[i][1]:
            return None
        return {'image': images[i][2], 'offset': hex(a - images[i][0])}
    return lookup


def symbolize_panic(p):
    path = kernel_for_uuid(p.get('kernel_uuid'))
    k = kernel_symbolizer(p.get('text_base'), path) if path else None
    if k is None:
        p['symbolized'] = False
        p['symbols_note'] = (f"no unstripped kernel with UUID {p.get('kernel_uuid')} here (current build: "
                             f"{macho_uuid(KERNEL_UNSTRIPPED) if os.path.isfile(KERNEL_UNSTRIPPED) else 'none'}; "
                             f"archived ones: {ARCHIVE})")
        return p
    p['symbols_from'] = path
    p['symbolized'] = True
    p['backtrace_symbols'] = [{'address': a, 'symbol': k(a)} for a in p.get('backtrace', [])]
    for r in ('pc', 'lr'):
        if r in p.get('registers', {}):
            p[r + '_symbol'] = k(p['registers'][r])
    p['caller_symbol'] = k(p['caller'])
    return p


def symbolize_crash(c):
    u = user_symbolizer(c.get('slide', '0x0'))
    c['symbolized'] = False
    c['image_attribution'] = False
    if u is None:
        return c
    c['frames_symbols'] = [{'address': a, 'where': u(a)} for a in c.get('frames', [])]
    for r in ('pc', 'lr'):
        if r in c:
            c[r + '_where'] = u(c[r])
    c['image_attribution'] = any(f['where'] for f in c['frames_symbols']) or any(c.get(r + '_where') for r in ('pc', 'lr'))
    c['symbols_note'] = 'shared-cache image and offset only; function symbols are not available here'
    return c
