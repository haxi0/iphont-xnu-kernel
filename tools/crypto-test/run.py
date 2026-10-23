#!/usr/bin/env python3
"""Check the loader's SHA-256 and Ed25519 (xnuloader/crypto.c) against hashlib and `cryptography`."""
import hashlib, os, random, subprocess, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

here = os.path.dirname(os.path.abspath(__file__))
exe = os.path.join(os.environ.get("TMPDIR", "/tmp"), "hackios-crypto-test")
subprocess.check_call(["cc", "-O2", "-Wall", "-Wextra", "-o", exe, os.path.join(here, "crypto-test.c")])
p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

def ask(line):
    p.stdin.write(line + "\n"); p.stdin.flush()
    return p.stdout.readline().strip()

rnd = random.Random(1)
bad = 0
for n in list(range(0, 260)) + [1000, 4096, 65537]:
    m = bytes(rnd.getrandbits(8) for _ in range(n))
    if ask("sha256 " + (m.hex() or "")) != hashlib.sha256(m).hexdigest():
        print("sha256 differs at length", n); bad += 1
for i in range(40):
    key = Ed25519PrivateKey.generate()
    pk = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    m = bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(0, 300)))
    s = key.sign(m)
    mh = m.hex() or "-"
    if ask(f"verify {s.hex()} {pk.hex()} {mh}") != "1":
        print("a good signature was refused", i); bad += 1
    flip = bytearray(s); flip[rnd.randrange(64)] ^= 1 << rnd.randrange(8)
    if ask(f"verify {bytes(flip).hex()} {pk.hex()} {mh}") != "0":
        print("a corrupted signature was accepted", i); bad += 1
    if m:
        m2 = bytearray(m); m2[0] ^= 1
        if ask(f"verify {s.hex()} {pk.hex()} {bytes(m2).hex()}") != "0":
            print("a signature of another message was accepted", i); bad += 1
p.stdin.close(); p.wait()
print("crypto-test:", "FAILED" if bad else "all good (sha256: 263 lengths, ed25519: 40 keys x 3 cases)")
sys.exit(1 if bad else 0)
