#!/usr/bin/env python3
# The access point's side of WPA3 (SAE, group 19, then the four-way handshake of AKM 8), written on Python's own
# integers, hashlib and hmac and the "cryptography" package's AES key wrap and CMAC: an implementation that shares
# nothing with wifi/lx/src/lx_sae.c and lx_wpa.c. For a number of made-up networks it writes down, as C, what an
# access point sends and what a correct station sends back, byte for byte; test_sae.c has ours do the same and
# compares. (IEEE 802.11-2020, 12.4 and 12.7; the curve's numbers are FIPS 186-4's P-256.)
import hashlib
import hmac
import random
import struct
import sys

from cryptography.hazmat.primitives import cmac
from cryptography.hazmat.primitives.ciphers import algorithms
from cryptography.hazmat.primitives.keywrap import aes_key_wrap
from cryptography.hazmat.primitives.asymmetric import ec

rnd = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)

P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
A = P - 3
B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
G = (0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
     0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)


def add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    if p1[0] == p2[0] and (p1[1] + p2[1]) % P == 0:
        return None
    if p1 == p2:
        lam = (3 * p1[0] * p1[0] + A) * pow(2 * p1[1], P - 2, P) % P
    else:
        lam = (p2[1] - p1[1]) * pow(p2[0] - p1[0], P - 2, P) % P
    x = (lam * lam - p1[0] - p2[0]) % P
    return x, (lam * (p1[0] - x) - p1[1]) % P


def times(k, pt):
    r = None
    while k:
        if k & 1:
            r = add(r, pt)
        pt = add(pt, pt)
        k >>= 1
    return r


def neg(pt):
    return pt[0], (P - pt[1]) % P


# the point arithmetic above, checked against the "cryptography" package's own P-256 before it is used
for d in (1, 2, 3, 0xdeadbeef, N - 1, rnd.randrange(1, N)):
    public = ec.derive_private_key(d, ec.SECP256R1()).public_key().public_numbers()
    assert times(d, G) == (public.x, public.y)
assert times(N, G) is None


def kdf(key, label, context, bits):
    out = b''
    i = 1
    while len(out) * 8 < bits:
        out += hmac.new(key, struct.pack('<H', i) + label + context + struct.pack('<H', bits), hashlib.sha256).digest()
        i += 1
    return out[:bits // 8]


def b32(x):
    return x.to_bytes(32, 'big')


def pwe_of(password, a1, a2):
    addresses = max(a1, a2) + min(a1, a2)
    found = None
    for counter in range(1, 41):
        seed = hmac.new(addresses, password + bytes([counter]), hashlib.sha256).digest()
        x = int.from_bytes(kdf(seed, b'SAE Hunting and Pecking', b32(P), 256), 'big')
        if x >= P:
            continue
        rhs = (x * x * x + A * x + B) % P
        if pow(rhs, (P - 1) // 2, P) == 1 and found is None:
            found = (x, seed)
    x, seed = found
    y = pow((x * x * x + A * x + B) % P, (P + 1) // 4, P)
    if (seed[-1] & 1) != (y & 1):
        y = P - y
    return x, y


def random_scalar(source):
    while True:
        v = int.from_bytes(source(32), 'big')
        if 2 <= v < N:
            return v


def commit(pwe, source):
    while True:
        r, mask = random_scalar(source), random_scalar(source)
        scalar = (r + mask) % N
        if scalar >= 2:
            return r, scalar, neg(times(mask, pwe))


def confirm(kck, counter, s1, e1, s2, e2):
    return hmac.new(kck, struct.pack('<H', counter) + b32(s1) + b32(e1[0]) + b32(e1[1]) + b32(s2) + b32(e2[0]) +
                    b32(e2[1]), hashlib.sha256).digest()


def cmac16(key, data):
    c = cmac.CMAC(algorithms.AES(key))
    c.update(data)
    return c.finalize()


def key_message(info, replay, nonce, rsc, data, kck):
    body = struct.pack('>BHH8s32s16s8s8s16sH', 2, info, 16 if info & 0x80 else 0, replay, nonce, bytes(16), rsc, bytes(8),
                       bytes(16), len(data)) + data
    frame = bytearray(struct.pack('>BBH', 2, 3, len(body)) + body)
    if kck is not None:
        frame[81:97] = cmac16(kck, bytes(frame))
    return bytes(frame)


def c_bytes(name, data):
    return 'static const unsigned char %s[] = { %s };\n' % (name, ', '.join('0x%02x' % b for b in data) or '0')


FIELDS = ('password', 'ap', 'station', 'station_random', 'pwe', 'station_commit', 'ap_commit', 'kck', 'pmk', 'pmkid',
          'station_confirm', 'ap_confirm', 'ap_confirm_wrong', 'snonce', 'rsn', 'tk', 'group', 'igtk', 'm1', 'm2', 'm3', 'm4')


def case(n):
    password = bytes(rnd.choice(b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !#') for _ in
                     range(rnd.randint(8, 63)))
    ap, station = bytes([2] + [rnd.randrange(256) for _ in range(5)]), bytes([2] + [rnd.randrange(256) for _ in range(5)])
    pwe = pwe_of(password, ap, station)
    assert (pwe[1] * pwe[1] - (pwe[0] ** 3 + A * pwe[0] + B)) % P == 0

    # the station's random bytes are what the test hands ours, in the order it asks for them
    station_random = bytearray()

    def station_source(k):
        b = rnd.randbytes(k)
        station_random.extend(b)
        return b

    s_rand, s_scalar, s_element = commit(pwe, station_source)
    a_rand, a_scalar, a_element = commit(pwe, rnd.randbytes)
    k = times(s_rand, add(times(a_scalar, pwe), a_element))
    assert k == times(a_rand, add(times(s_scalar, pwe), s_element))
    keyseed = hmac.new(bytes(32), b32(k[0]), hashlib.sha256).digest()
    context = b32((s_scalar + a_scalar) % N)
    keys = kdf(keyseed, b'SAE KCK and PMK', context, 512)
    kck, pmk, pmkid = keys[:32], keys[32:], context[:16]
    s_confirm = struct.pack('<H', 1) + confirm(kck, 1, s_scalar, s_element, a_scalar, a_element)
    a_confirm = struct.pack('<H', 1) + confirm(kck, 1, a_scalar, a_element, s_scalar, s_element)
    wrong_kck = bytes(b ^ 1 for b in kck)
    a_confirm_wrong = struct.pack('<H', 1) + confirm(wrong_kck, 1, a_scalar, a_element, s_scalar, s_element)

    # the four-way handshake of AKM 8: KDF-SHA-256 for the PTK, AES-CMAC for the MIC, descriptor version 0
    anonce, snonce = rnd.randbytes(32), rnd.randbytes(32)
    rsn = bytes.fromhex('301a0100000fac040100000fac040100000fac08c0000000000fac06')    # SAE, MFP required, BIP-CMAC-128
    ptk = kdf(pmk, b'Pairwise key expansion', min(ap, station) + max(ap, station) + min(anonce, snonce) + max(anonce, snonce),
              384)
    ptk_kck, kek, tk = ptk[:16], ptk[16:32], ptk[32:]
    group, igtk = rnd.randbytes(16), rnd.randbytes(16)
    ipn = rnd.randbytes(6)
    plain = rsn + bytes([0xdd, 6 + 16, 0x00, 0x0f, 0xac, 1, 1, 0]) + group + \
        bytes([0xdd, 4 + 2 + 6 + 16, 0x00, 0x0f, 0xac, 9, 4, 0]) + ipn + igtk
    if len(plain) % 8:
        plain += b'\xdd' + bytes(7 - len(plain) % 8)
    replay = rnd.randrange(1 << 40)
    m1 = key_message(0x0088, struct.pack('>Q', replay), anonce, bytes(8), b'', None)
    m2 = key_message(0x0108, struct.pack('>Q', replay), snonce, bytes(8), rsn, ptk_kck)
    m3 = key_message(0x13c8, struct.pack('>Q', replay + 1), anonce, bytes(8), aes_key_wrap(kek, plain), ptk_kck)
    m4 = key_message(0x0308, struct.pack('>Q', replay + 1), bytes(32), bytes(8), b'', ptk_kck)

    values = {
        'password': password, 'ap': ap, 'station': station, 'station_random': bytes(station_random),
        'pwe': b32(pwe[0]) + b32(pwe[1]),
        'station_commit': struct.pack('<H', 19) + b32(s_scalar) + b32(s_element[0]) + b32(s_element[1]),
        'ap_commit': struct.pack('<H', 19) + b32(a_scalar) + b32(a_element[0]) + b32(a_element[1]),
        'kck': kck, 'pmk': pmk, 'pmkid': pmkid, 'station_confirm': s_confirm, 'ap_confirm': a_confirm,
        'ap_confirm_wrong': a_confirm_wrong, 'snonce': snonce, 'rsn': rsn, 'tk': tk, 'group': group,
        'igtk': bytes([4, 0]) + ipn + igtk, 'm1': m1, 'm2': m2, 'm3': m3, 'm4': m4,
    }
    text = ''.join(c_bytes('s%d_%s' % (n, k), values[k]) for k in FIELDS)
    entry = '{ %s }' % ', '.join('s%d_%s, sizeof(s%d_%s)' % (n, k, n, k) for k in FIELDS)
    return text, entry


def psk256_case(n):
    # PSK-SHA256 (AKM 6): the PMK of the passphrase and the name (PBKDF2-SHA1), then as AKM 8 but descriptor version 3
    password = bytes(rnd.choice(b'abcdefghijklmnopqrstuvwxyz0123456789') for _ in range(rnd.randint(8, 63)))
    ssid = bytes(rnd.choice(b'abcdefghijklmnopqrstuvwxyz') for _ in range(rnd.randint(1, 32)))
    ap, station = bytes([2] + [rnd.randrange(256) for _ in range(5)]), bytes([2] + [rnd.randrange(256) for _ in range(5)])
    pmk = hashlib.pbkdf2_hmac('sha1', password, ssid, 4096, 32)
    anonce, snonce = rnd.randbytes(32), rnd.randbytes(32)
    rsn = bytes.fromhex('30140100000fac040100000fac040100000fac06c000')    # PSK-SHA256, MFP required
    ptk = kdf(pmk, b'Pairwise key expansion', min(ap, station) + max(ap, station) + min(anonce, snonce) + max(anonce, snonce),
              384)
    ptk_kck, kek, tk = ptk[:16], ptk[16:32], ptk[32:]
    group, igtk, ipn = rnd.randbytes(16), rnd.randbytes(16), rnd.randbytes(6)
    plain = rsn + bytes([0xdd, 6 + 16, 0x00, 0x0f, 0xac, 1, 1, 0]) + group + \
        bytes([0xdd, 4 + 2 + 6 + 16, 0x00, 0x0f, 0xac, 9, 4, 0]) + ipn + igtk
    if len(plain) % 8:
        plain += b'\xdd' + bytes(7 - len(plain) % 8)
    replay = rnd.randrange(1 << 40)
    m1 = key_message(0x008b, struct.pack('>Q', replay), anonce, bytes(8), b'', None)
    m2 = key_message(0x010b, struct.pack('>Q', replay), snonce, bytes(8), rsn, ptk_kck)
    m3 = key_message(0x13cb, struct.pack('>Q', replay + 1), anonce, bytes(8), aes_key_wrap(kek, plain), ptk_kck)
    m4 = key_message(0x030b, struct.pack('>Q', replay + 1), bytes(32), bytes(8), b'', ptk_kck)
    values = {'password': password, 'ssid': ssid, 'ap': ap, 'station': station, 'snonce': snonce, 'rsn': rsn, 'tk': tk,
              'group': group, 'm1': m1, 'm2': m2, 'm3': m3, 'm4': m4}
    names = ('password', 'ssid', 'ap', 'station', 'snonce', 'rsn', 'tk', 'group', 'm1', 'm2', 'm3', 'm4')
    text = ''.join(c_bytes('p%d_%s' % (n, k), values[k]) for k in names)
    entry = '{ %s }' % ', '.join('p%d_%s, sizeof(p%d_%s)' % (n, k, n, k) for k in names)
    return text, entry


cases = [case(n) for n in range(8)]
print(''.join(t for t, _ in cases))
print('static const struct sae_case sae_cases[] = {\n\t%s\n};' % ',\n\t'.join(e for _, e in cases))
psk256 = [psk256_case(n) for n in range(4)]
print(''.join(t for t, _ in psk256))
print('static const struct psk256_case psk256_cases[] = {\n\t%s\n};' % ',\n\t'.join(e for _, e in psk256))
# a point that is not on the curve, for the refusal of an element
x = G[0]
print(c_bytes('off_curve', struct.pack('<H', 19) + b32(5) + b32(x) + b32((G[1] + 1) % P)))
# BIP-CMAC-128 (802.11w): the MIC of a broadcast deauthentication, over its AAD and its body with the MMIE's MIC zeroed
igtk, ipn = bytes(range(16)), bytes([1, 0, 0, 0, 0, 0])
header = bytes([0xc0, 0x00, 0, 0]) + b'\xff' * 6 + bytes([2, 1, 2, 3, 4, 5]) + bytes([2, 1, 2, 3, 4, 5]) + bytes(2)
body = bytes([3, 0]) + bytes([76, 16, 4, 0]) + ipn
aad = bytes([0xc0, 0x00]) + header[4:22]
mic = cmac16(igtk, aad + body + bytes(8))[:8]
print(c_bytes('bip_igtk', igtk) + c_bytes('bip_frame', header + body + mic))
