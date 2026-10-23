#!/usr/bin/env python3
"""
The owner key: verified boot that the phone's owner holds, not a vendor.

  ownerkey.py init [KEYFILE]     make a new Ed25519 key (default ~/.hackios/owner.key, mode 600) and pin
                                 its public half into the loader (xnuloader/owner_key.h, not in git)
  ownerkey.py pin [KEYFILE]      pin an existing key into the loader again (a new checkout)
  ownerkey.py show [KEYFILE]     the public key and its fingerprint
  ownerkey.py unpin              back to a loader that takes unsigned bundles

A loader built with a pinned key (xnuloader/build.sh) boots only bundles signed by that key
(mkbundle.py --sign): a bundle written into darwin by anything else -- a stray tool, a bad update, someone
with the phone for a minute -- is refused and the previous slot or Android boots instead. The private key
never leaves the computer and never goes into the repository; keep a copy of it somewhere safe: without it
the next bundle cannot be signed, and the loader must be rebuilt unpinned or with a new key.
"""
import os, stat, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
DEFAULT = os.path.join(os.path.expanduser('~'), '.hackios', 'owner.key')
HEADER = os.environ.get('OWNER_KEY_H') or os.path.join(ROOT, 'xnuloader', 'owner_key.h')


def _crypto():
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives import serialization
    except ImportError:
        sys.exit('ownerkey: needs the Python package `cryptography` (pip install cryptography)')
    return Ed25519PrivateKey, serialization


def load(path):
    Priv, ser = _crypto()
    raw = open(path, 'rb').read()
    if len(raw) != 32:
        sys.exit('ownerkey: %s is not a raw 32-byte Ed25519 key' % path)
    return Priv.from_private_bytes(raw)


def public(key):
    _, ser = _crypto()
    return key.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)


def signer(path):
    if not os.path.exists(path):
        sys.exit('ownerkey: no key at %s (ownerkey.py init makes one)' % path)
    key = load(path)
    return key.sign


def fingerprint(pub):
    import hashlib
    return hashlib.sha256(pub).hexdigest()[:16]


def write_header(pub):
    body = ', '.join('0x%02x' % b for b in pub) if pub else ''
    with open(HEADER, 'w') as f:
        f.write('/* made by common/flash/ownerkey.py: the owner key the loader checks boot bundles against */\n')
        f.write('#define OWNER_KEY_PINNED %d\n' % (1 if pub else 0))
        f.write('static const uint8_t owner_key[32] = { %s };\n' % body)
        if pub:
            f.write('static const char owner_key_fp[] = "%s";\n' % fingerprint(pub))
        else:
            f.write('static const char owner_key_fp[] = "";\n')
    print('ownerkey: %s written (%s); rebuild the loader: xnuloader/build.sh'
          % (os.path.relpath(HEADER, ROOT), 'pinned ' + fingerprint(pub) if pub else 'unpinned'))


def main(a):
    cmd = a[0] if a else 'show'
    path = a[1] if len(a) > 1 else DEFAULT
    if cmd == 'init':
        if os.path.exists(path):
            sys.exit('ownerkey: %s exists already; `pin` it, or move it away first' % path)
        Priv, ser = _crypto()
        key = Priv.generate()
        os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, stat.S_IRUSR | stat.S_IWUSR)
        os.write(fd, key.private_bytes(ser.Encoding.Raw, ser.PrivateFormat.Raw, ser.NoEncryption()))
        os.close(fd)
        print('ownerkey: new key in %s -- keep a copy of it somewhere safe' % path)
        write_header(public(key))
    elif cmd == 'pin':
        write_header(public(load(path)))
    elif cmd == 'unpin':
        write_header(b'')
    elif cmd == 'show':
        if not os.path.exists(path):
            print('ownerkey: no key at %s' % path)
            return 1
        pub = public(load(path))
        print('public key  %s\nfingerprint %s' % (pub.hex(), fingerprint(pub)))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
