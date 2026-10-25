"""
The on-flash formats of the installed mode, shared by mkbundle.py, flashctl.py and ownerkey.py. The loader's
side is xnuloader/flashboot.c; the two must agree byte for byte (common/flash/qemu-test.sh checks they do).

darwin's boot area, in 4 KiB blocks:

  2042368 + N * 24576   boot slot N (N = 0, 1): a header block, the "XNUP" container behind it
  2091520               the boot state: what the loader started last, how often, and what came up
  2091521               (a file-backed darwin) its marker: common/flash/darwinid.py
  2091522               the date: an offset from the PMIC's clock (HKCLOCK1, xl settime; xnuloader/flashboot.c)

Bundle header (HKBOOT02), little endian:

  0   magic "HKBOOT02"
  8   u64 generation           the newest good slot boots
  16  u64 size                 bytes of container behind the header block
  24  u64 flags                bit 0: signed
  32  char label[32]           shown in the boot menu ("v0.2.0 2026-10-22"), NUL padded
  64  u8 sha256[32]            of the container
  96  u8 signature[64]         Ed25519 over bytes 0..95, by the owner key (zero when unsigned)
  160 u64 header_sum           FNV-1a 64 over bytes 0..159: a torn header is not mistaken for one

Boot state (HKSTATE1):

  0   magic "HKSTATE1"
  8   u64 trying               the generation the loader started last
  16  u32 tries                how many times in a row it started that one without it coming up
  20  u32 flags                bit 0: hold -- the user asked for the previous version (flashctl rollback)
                               bit 1: the last choice in the menu was Android (the countdown's default)
                               bit 2: switched off with the cable in (the next start, by the cable: off-mode charging)
  24  u64 good                 the last generation that came up (userland: var_store bootok)
  32  u64 boots                every start of HackiOS the loader made
  40  u64 bad                  a generation given up on (MAX_TRIES starts, never up): the countdown skips it
  48  u64 sum                  FNV-1a 64 over bytes 0..47
"""
import hashlib, struct

BLOCK = 4096
AREA_FIRST, SLOT_BLOCKS, SLOTS = 2042368, 24576, 2
STATE_BLOCK = AREA_FIRST + SLOTS * SLOT_BLOCKS
MAX_TRIES = 3
HEADER_MAGIC, STATE_MAGIC = b'HKBOOT02', b'HKSTATE1'
FLAG_SIGNED = 1
STATE_HOLD = 1
STATE_LAST_ANDROID = 2
STATE_OFF_CHARGING = 4


def fnv(data, h=0xcbf29ce484222325):
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xffffffffffffffff
    return h


def make_header(generation, container, label='', sign=None):
    """the 4 KiB header block for `container`; sign(message) -> 64-byte Ed25519 signature, or None"""
    lab = label.encode()[:31]
    head = HEADER_MAGIC + struct.pack('<QQQ', generation, len(container), FLAG_SIGNED if sign else 0)
    head += lab + b'\0' * (32 - len(lab)) + hashlib.sha256(container).digest()
    head += sign(head) if sign else b'\0' * 64
    head += struct.pack('<Q', fnv(head))
    return head + b'\0' * (BLOCK - len(head))


def parse_header(blk):
    """a dict, or None when the block is no sound header"""
    if len(blk) < 168 or blk[:8] != HEADER_MAGIC or struct.unpack_from('<Q', blk, 160)[0] != fnv(blk[:160]):
        return None
    gen, size, flags = struct.unpack_from('<QQQ', blk, 8)
    return {'generation': gen, 'size': size, 'signed': bool(flags & FLAG_SIGNED),
            'label': blk[32:64].split(b'\0')[0].decode(errors='replace'),
            'sha256': blk[64:96].hex(), 'message': bytes(blk[:96]), 'signature': bytes(blk[96:160])}


def make_state(trying=0, tries=0, flags=0, good=0, boots=0, bad=0, **_):
    s = STATE_MAGIC + struct.pack('<QIIQQQ', trying, tries, flags, good, boots, bad)
    s += struct.pack('<Q', fnv(s))
    return s + b'\0' * (BLOCK - len(s))


def parse_state(blk):
    if len(blk) < 56 or blk[:8] != STATE_MAGIC or struct.unpack_from('<Q', blk, 48)[0] != fnv(blk[:48]):
        return None
    trying, tries, flags, good, boots, bad = struct.unpack_from('<QIIQQQ', blk, 8)
    return {'trying': trying, 'tries': tries, 'flags': flags, 'good': good, 'boots': boots, 'bad': bad}


def verify(pubkey, header):
    """True when the header carries a good signature by `pubkey` (32 raw bytes)"""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import InvalidSignature
    try:
        Ed25519PublicKey.from_public_bytes(pubkey).verify(header['signature'], header['message'])
        return True
    except InvalidSignature:
        return False
