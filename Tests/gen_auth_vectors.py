#!/usr/bin/env python3
# Writes Tests/auth_vectors.h: the known answers Tests/test_crypto.c and
# Tests/test_auth.c check Shared/gip_crypto.c, gip_auth.c and gip.c against.
#
#   gen_auth_vectors.py VECTORS_DIR OUT.h        (OPENSSL=... picks the openssl binary)
#
# Everything here is computed independently of the C code: SHA-256 and HMAC
# by Python's hashlib/hmac, the PRF by a direct transcription of xone's
# auth/crypto.c gip_auth_compute_prf (checked against the published TLS 1.2
# P_SHA256 vector), RSA and P-256 by Python integers, and the RSA and ECDH
# results cross-checked with OpenSSL: the PKCS#1 v1.5 ciphertext is
# decrypted by `openssl pkeyutl -decrypt` with the private key, the shared
# secret is `openssl pkeyutl -derive`'s. The keys and the certificate in
# VECTORS_DIR are test material made with openssl (genpkey, req -x509) and
# committed, so the output is the same on every run; `make -C Tests
# auth-vectors-check` regenerates it and compares.
#
# The full handshakes are a model of xone's host (auth/auth.c, the framing of
# bus/protocol.c) driven by a simulated pad: the frames gip.c must send,
# byte for byte, with its sequence numbers (seq_sec, from 1).
#
# The PRF and the handshake model follow xone, Copyright (C) 2021-2023
# Severin von Wnuck-Lipinski, GPL-2.0-or-later.
#
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import hmac
import os
import shutil
import struct
import subprocess
import sys
import tempfile

VDIR, OUT = sys.argv[1], sys.argv[2]
OPENSSL = os.environ.get('OPENSSL') or shutil.which('openssl') or 'openssl'
TMP = tempfile.mkdtemp(prefix='xpad-auth-')


def ossl(*args, data=None):
    return subprocess.run([OPENSSL] + list(args), input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=True).stdout


def path(name):
    return os.path.join(VDIR, name)


def tmp(name, data):
    p = os.path.join(TMP, name)
    with open(p, 'wb') as f:
        f.write(data)
    return p


# ---- DER, just enough -----------------------------------------------------------------

def der_tlv(b, i):
    tag, i = b[i], i + 1
    n, i = b[i], i + 1
    if n & 0x80:
        k, n = n & 0x7F, 0
        for _ in range(k):
            n, i = n << 8 | b[i], i + 1
    return tag, b[i:i + n], i + n


def rsa_from_spki(spki):
    _, body, _ = der_tlv(spki, 0)               # SubjectPublicKeyInfo
    _, _, j = der_tlv(body, 0)                  # AlgorithmIdentifier
    tag, bits, _ = der_tlv(body, j)             # BIT STRING
    assert tag == 0x03 and bits[0] == 0
    rsapub = bits[1:]
    _, seq, _ = der_tlv(rsapub, 0)
    t1, n, k = der_tlv(seq, 0)
    t2, e, _ = der_tlv(seq, k)
    assert t1 == 2 and t2 == 2
    return int.from_bytes(n, 'big'), int.from_bytes(e, 'big'), rsapub


def ec_keys(name):
    spki = ossl('pkey', '-in', path(name), '-pubout', '-outform', 'DER')
    assert spki[-65] == 0x04
    priv_der = ossl('pkey', '-in', path(name), '-outform', 'DER')
    i = priv_der.find(b'\x02\x01\x01\x04\x20')
    assert i >= 0
    return priv_der[i + 5:i + 37], spki[-64:]


# ---- the primitives, in Python ---------------------------------------------------------------

def prf(key, label, seed, n):                   # xone auth/crypto.c:59-91
    H = lambda m: hmac.new(key, m, hashlib.sha256).digest()
    a, out = H(label + seed), b''
    while len(out) < n:
        out += H(a + label + seed)
        a = H(a)
    return out[:n]


P = 2**256 - 2**224 + 2**192 + 2**96 - 1
N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
G = (0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
     0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)


def ec_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    if p1[0] == p2[0] and (p1[1] + p2[1]) % P == 0:
        return None
    if p1 == p2:
        lam = 3 * (p1[0] * p1[0] - 1) * pow(2 * p1[1], -1, P) % P
    else:
        lam = (p2[1] - p1[1]) * pow(p2[0] - p1[0], -1, P) % P
    x = (lam * lam - p1[0] - p2[0]) % P
    return x, (lam * (p1[0] - x) - p1[1]) % P


def ec_mul(k, pt):
    r = None
    while k:
        if k & 1:
            r = ec_add(r, pt)
        pt = ec_add(pt, pt)
        k >>= 1
    return r


def on_curve(pt):
    return (pt[1] ** 2 - (pt[0] ** 3 - 3 * pt[0] + B)) % P == 0


assert on_curve(G) and ec_mul(N, G) is None
be = lambda x, n=32: x.to_bytes(n, 'big')
xy = lambda pt: be(pt[0]) + be(pt[1])
pt_of = lambda b: (int.from_bytes(b[:32], 'big'), int.from_bytes(b[32:], 'big'))


def pkcs1(n, e, msg, ps):
    k = (n.bit_length() + 7) // 8
    assert len(ps) == k - 3 - len(msg) and 0 not in ps
    em = b'\x00\x02' + ps + b'\x00' + msg
    return be(pow(int.from_bytes(em, 'big'), e, n), k)


def openssl_decrypt(key, ct):
    return ossl('pkeyutl', '-decrypt', '-inkey', path(key), '-in', tmp('ct.bin', ct), '-pkeyopt',
                'rsa_padding_mode:pkcs1')


# ---- known answers ----------------------------------------------------------------------------------

V = []          # (C declaration, value) in order


def arr(name, data):
    V.append(('bytes', name, bytes(data)))


def num(name, value):
    V.append(('num', name, value))


def blist(name, items):
    V.append(('list', name, [bytes(x) for x in items]))


# SHA-256: FIPS 180-2's examples, then every length 0..200 of a pattern
# (each padding case), then a million 'a'.
assert hashlib.sha256(b'abc').hexdigest() == \
    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
arr('av_sha_abc', hashlib.sha256(b'abc').digest())
arr('av_sha_448', hashlib.sha256(b'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq').digest())
arr('av_sha_896', hashlib.sha256(b'abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno'
                                 b'ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu').digest())
arr('av_sha_million_a', hashlib.sha256(b'a' * 1000000).digest())
pattern = bytes((i * 31 + 7) & 0xFF for i in range(200))
arr('av_sha_pattern', b''.join(hashlib.sha256(pattern[:n]).digest() for n in range(201)))

# HMAC-SHA256: RFC 4231 test cases 1-4, 6, 7 (5 is a truncation).
rfc4231 = [
    (b'\x0b' * 20, b'Hi There'),
    (b'Jefe', b'what do ya want for nothing?'),
    (b'\xaa' * 20, b'\xdd' * 50),
    (bytes(range(1, 26)), b'\xcd' * 50),
    (b'\xaa' * 131, b'Test Using Larger Than Block-Size Key - Hash Key First'),
    (b'\xaa' * 131, b'This is a test using a larger than block-size key and a larger than block-size data. '
                    b'The key needs to be hashed before being used by the HMAC algorithm.'),
]
assert hmac.new(*rfc4231[0], hashlib.sha256).hexdigest() == \
    'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7'
assert hmac.new(*rfc4231[1], hashlib.sha256).hexdigest() == \
    '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843'
blist('av_hmac_key', [k for k, _ in rfc4231])
blist('av_hmac_msg', [m for _, m in rfc4231])
blist('av_hmac_mac', [hmac.new(k, m, hashlib.sha256).digest() for k, m in rfc4231])

# The PRF: the TLS 1.2 P_SHA256 vector (100 bytes), then xone's own labels.
tls = prf(bytes.fromhex('9bbe436ba940f017b17652849a71db35'), b'test label',
          bytes.fromhex('a0ba9f936cda311827a6f796ffd5198c'), 100)
assert tls.hex().startswith('e3f229ba727be17b8d122620557cd453c2aab21d07c3d495329b52d4e61edb5a6b301791e90d35c9')
arr('av_prf_tls12', tls)
prf_key, prf_seed = bytes(range(48)), bytes(range(0x40, 0x80))
for n in (1, 16, 31, 32, 33, 48, 64, 65):
    arr('av_prf_ms_%d' % n, prf(prf_key, b'Master Secret', prf_seed, n))
arr('av_prf_session', prf(prf_key, b'EXPORTER DAWN data channel session key for controller', prf_seed, 16))

# RSA: the committed 2048-bit key (e = 65537) and one with e = 3, PKCS#1
# v1.5 with fixed PMS and padding, decrypted by OpenSSL.
rsa_spki = ossl('pkey', '-in', path('rsa2048.pem'), '-pubout', '-outform', 'DER')
rsa_n, rsa_e, rsa_pub = rsa_from_spki(rsa_spki)
assert rsa_n.bit_length() == 2048 and rsa_e == 65537 and rsa_pub[:4] == b'\x30\x82\x01\x0a'
rsa3_spki = ossl('pkey', '-in', path('rsa2048_e3.pem'), '-pubout', '-outform', 'DER')
rsa3_n, rsa3_e, rsa3_pub = rsa_from_spki(rsa3_spki)
assert rsa3_e == 3 and rsa3_n.bit_length() == 2048
pms = bytes(range(0xc0, 0xf0))
ps = bytes((i % 255) + 1 for i in range(256 - 3 - 48))
ct = pkcs1(rsa_n, rsa_e, pms, ps)
assert openssl_decrypt('rsa2048.pem', ct) == pms
ct3 = pkcs1(rsa3_n, rsa3_e, pms, ps)
assert openssl_decrypt('rsa2048_e3.pem', ct3) == pms
arr('av_rsa_n', be(rsa_n, 256))
num('av_rsa_e', rsa_e)
arr('av_rsa_pub_der', rsa_pub)          # bare RSAPublicKey (PKCS#1)
arr('av_rsa_spki_der', rsa_spki)        # SubjectPublicKeyInfo around it
arr('av_rsa3_n', be(rsa3_n, 256))
arr('av_rsa3_pub_der', rsa3_pub)
arr('av_pms', pms)
arr('av_pkcs1_ps', ps)
arr('av_rsa_ct', ct)
arr('av_rsa3_ct', ct3)
arr('av_rsa_raw_in', be(2, 256))
arr('av_rsa_raw_out', be(pow(2, rsa_e, rsa_n), 256))
cert = open(path('cert.der'), 'rb').read()
assert len(cert) <= 1024
cert_key_at = cert.find(b'\x30\x82\x01\x0a')
assert cert_key_at > 0 and cert[cert_key_at:cert_key_at + 270] == rsa_pub
arr('av_cert', cert)
num('av_cert_key_at', cert_key_at)

# P-256: two committed key pairs, the shared secret from OpenSSL.
host_priv, host_pub = ec_keys('p256_host.pem')
client_priv, client_pub = ec_keys('p256_client.pem')
assert xy(ec_mul(int.from_bytes(host_priv, 'big'), G)) == host_pub
assert xy(ec_mul(int.from_bytes(client_priv, 'big'), G)) == client_pub
shared = ossl('pkeyutl', '-derive', '-inkey', path('p256_host.pem'), '-peerkey',
              tmp('peer.der', ossl('pkey', '-in', path('p256_client.pem'), '-pubout')))
assert len(shared) == 32
assert be(ec_mul(int.from_bytes(host_priv, 'big'), pt_of(client_pub))[0]) == shared
assert be(ec_mul(int.from_bytes(client_priv, 'big'), pt_of(host_pub))[0]) == shared
arr('av_ec_host_priv', host_priv)
arr('av_ec_host_pub', host_pub)
arr('av_ec_client_priv', client_priv)
arr('av_ec_client_pub', client_pub)
arr('av_ec_shared', shared)
arr('av_ec_g', xy(G))
arr('av_ec_n_minus_1', be(N - 1))
arr('av_ec_neg_g', xy((G[0], P - G[1])))
arr('av_ec_2g', xy(ec_mul(2, G)))
arr('av_ec_3g', xy(ec_mul(3, G)))
arr('av_ec_p', be(P))
arr('av_ec_n', be(N))


# ---- the handshakes ---------------------------------------------------------------------------------------

TR = bytes(8)


def host_msg(cmd, body):                        # auth.c:162-184: (message, the bytes hashed)
    dlen = 4 + len(body)
    m = bytes([0, 0x41, 0, cmd]) + struct.pack('>H', dlen) + bytes([cmd, 2 if cmd >= 0x21 else 1]) + \
        struct.pack('>H', len(body)) + body + TR
    return m, m[6:6 + dlen]


def request(cmd, want):                         # auth.c:186-198
    return bytes([0, 0x42, 0, cmd]) + struct.pack('>H', want + 4) + TR


def client_msg(cmd, body, ver=1, hcmd=None):    # as xone parses it (auth.c:565-589)
    return bytes([0, 0xC0, 0, cmd if hcmd is None else hcmd]) + struct.pack('>H', 4 + len(body)) + \
        bytes([cmd, ver]) + struct.pack('>H', len(body)) + body


def varint(v):
    out = bytearray()
    while True:
        c, v = v & 0x7F, v >> 7
        out.append(c | (0x80 if v else 0))
        if not v:
            return bytes(out)


def header(cmd, opts, seq, plen, off=None):     # protocol.c:286-304
    h = bytearray([cmd, opts, seq]) + varint(plen)
    chunk = opts & 0x80
    alen = 3 + len(varint(plen)) + (len(varint(off)) if chunk and off else 0)
    if alen % 2:
        h[-1] |= 0x80
        h.append(0)
    if chunk and off:
        h += varint(off)
    return bytes(h)


def frames(seq, payload, opts=0x30):            # protocol.c:408-436, 1080-1150
    if len(payload) <= 58:
        return [header(6, opts, seq, len(payload)) + payload]
    out = [header(6, opts | 0xC0, seq, 58, len(payload)) + payload[:58]]
    off = 58
    while off < len(payload):
        n = min(58, len(payload) - off)
        o = 0xA0 | (0x10 if len(payload) - off <= 58 else 0)
        out.append(header(6, o, seq, n, off) + payload[off:off + n])
        off += n
    out.append(header(6, 0xA0, seq, 0, len(payload)))
    return out


assert frames(4, bytes(274))[0][:6] == bytes.fromhex('06f0043a9202')
assert [f[:6].hex() for f in frames(4, bytes(274))[1:]] == \
    ['06a004ba003a', '06a004ba0074', '06a0043aae01', '06b0042ae801', '06a004009202']
assert [f[:6].hex() for f in frames(7, bytes(82))] == ['06f007ba0052', '06b00798003a', '06a007800052']

# v1: the pad answers with the committed certificate.
rh1 = bytes(range(0x10, 0x30))
rc1 = bytes(range(0x80, 0xa0))
t = hashlib.sha256()
wire = []
m, h = host_msg(0x01, rh1 + bytes(8)); t.update(h); wire += frames(1, m)
wire += frames(2, request(0x02, 80))
ch = client_msg(0x02, rc1 + bytes(range(48))); t.update(ch[6:])
wire += frames(3, request(0x03, 1024))
cm = client_msg(0x03, cert); t.update(cm[6:])
ms1 = prf(pms, b'Master Secret', rh1 + rc1, 48)
m, h = host_msg(0x05, ct); t.update(h); wire += frames(4, m)
hf1 = prf(ms1, b'Host Finished', t.copy().digest(), 32)
m, h = host_msg(0x07, hf1); t.update(h); wire += frames(5, m)
wire += frames(6, request(0x08, 64))
df1 = prf(ms1, b'Device Finished', t.copy().digest(), 32)
fm = client_msg(0x08, df1 + bytes(32))
wire.append(bytes([0x06, 0x20, 7, 0x02, 0x01, 0x00]))
arr('av_v1_random', rh1 + pms + ps)             # what xpad_random gives, in order
arr('av_v1_client_hello', ch)
arr('av_v1_client_cert', cm)
arr('av_v1_client_finish', fm)
arr('av_v1_master', ms1)
arr('av_v1_host_finished', hf1)
arr('av_v1_device_finished', df1)
blist('av_v1_wire', wire)

# v2: the pad answers request 02 with data command 22 (the upgrade), then
# the v2 messages; its key pair is the committed client one.
rh0 = bytes(range(0x50, 0x70))                  # v1 Host Hello's random, dropped by the upgrade
rh2 = bytes(range(0x20, 0x40))
rc2 = bytes(range(0x90, 0xb0))
t = hashlib.sha256()
wire = []
m, _ = host_msg(0x01, rh0 + bytes(8)); wire += frames(1, m)
wire += frames(2, request(0x02, 80))
up = client_msg(0x22, rc2 + bytes(140), 2, hcmd=0x02)     # not hashed
m, h = host_msg(0x21, rh2 + bytes(4)); t.update(h); wire += frames(3, m)
wire += frames(4, request(0x22, 172))
ch2 = client_msg(0x22, rc2 + bytes(range(140)), 2); t.update(ch2[6:])
wire += frames(5, request(0x23, 768))
cert2 = b'XCRT' + bytes((i * 7) & 0xFF for i in range(764))
cm2 = client_msg(0x23, cert2, 2); t.update(cm2[6:])
wire += frames(6, request(0x24, 128))
km2 = client_msg(0x24, client_pub + bytes(64), 2); t.update(km2[6:])
ms2 = prf(hashlib.sha256(shared).digest(), b'Master Secret', rh2 + rc2, 48)
m, h = host_msg(0x25, host_pub); t.update(h); wire += frames(7, m)
hf2 = prf(ms2, b'Host Finished', t.copy().digest(), 32)
m, h = host_msg(0x26, hf2); t.update(h); wire += frames(8, m)
wire += frames(9, request(0x27, 64))
df2 = prf(ms2, b'Device Finished', t.copy().digest(), 32)
fm2 = client_msg(0x27, df2 + bytes(32), 2)
wire.append(bytes([0x06, 0x20, 10, 0x02, 0x01, 0x00]))
arr('av_v2_random', rh0 + rh2 + host_priv)
arr('av_v2_upgrade', up)
arr('av_v2_client_hello', ch2)
arr('av_v2_client_cert', cm2)
arr('av_v2_client_pubkey', km2)
arr('av_v2_client_finish', fm2)
arr('av_v2_master', ms2)
arr('av_v2_host_finished', hf2)
arr('av_v2_device_finished', df2)
blist('av_v2_wire', wire)

shutil.rmtree(TMP, ignore_errors=True)

# ---- the header -----------------------------------------------------------------------------------------------


def c_bytes(b, indent='    '):
    lines = []
    for i in range(0, len(b), 16):
        lines.append(indent + ', '.join('0x%02x' % x for x in b[i:i + 16]) + ',')
    return '\n'.join(lines) if lines else indent + '0'


out = ['/* Generated by Tests/gen_auth_vectors.py from Tests/vectors/ (test keys only).',
       ' * Do not edit: make -C Tests auth-vectors regenerates it.',
       ' *',
       ' * SPDX-License-Identifier: GPL-3.0-or-later',
       ' */',
       '#ifndef XPAD_AUTH_VECTORS_H',
       '#define XPAD_AUTH_VECTORS_H',
       '',
       '#include <stdint.h>',
       '',
       'typedef struct { const uint8_t *p; int n; } av_bytes;',
       '']
for kind, name, value in V:
    if kind == 'bytes':
        out.append('static const uint8_t %s[%d] = {\n%s\n};' % (name, max(len(value), 1), c_bytes(value)))
    elif kind == 'num':
        out.append('#define %s %d' % (name.upper(), value))
    else:
        for i, b in enumerate(value):
            out.append('static const uint8_t %s_%d[%d] = {\n%s\n};' % (name, i, max(len(b), 1), c_bytes(b)))
        out.append('static const av_bytes %s[%d] = {\n%s\n};' % (
            name, len(value), '\n'.join('    { %s_%d, %d },' % (name, i, len(b)) for i, b in enumerate(value))))
        out.append('#define %s_COUNT %d' % (name.upper(), len(value)))
out += ['', '#endif', '']
with open(OUT, 'w') as f:
    f.write('\n'.join(out))
