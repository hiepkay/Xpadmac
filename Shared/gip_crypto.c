/*
 * SHA-256, HMAC-SHA256, xone's PRF, RSA PKCS#1 v1.5 encryption and P-256
 * ECDH for GIP authentication, written for XPadMac in plain C99. See
 * gip_crypto.h.
 *
 * Big numbers are little-endian arrays of 32-bit limbs; products are taken
 * in 64 bits. One Montgomery multiplication (CIOS) serves both RSA (modulus
 * up to 64 limbs) and the P-256 field (8 limbs); R^2 mod m is found by
 * doubling, so no table of precomputed constants needs trusting beyond the
 * curve's own (FIPS 186-4 D.1.2.3). P-256 points are added and doubled in
 * Jacobian coordinates (a = -3), scalar multiplication is plain
 * double-and-add, and the inverse is Fermat's (a^(p-2)). Correctness is
 * what matters here, not speed or constant time: one handshake costs two
 * P-256 scalar multiplications (or one 2048-bit RSA public operation), a
 * millisecond or so.
 *
 * Known-answer checks: Tests/test_crypto.c (FIPS 180-2, RFC 4231, the TLS
 * 1.2 PRF vector, and RSA/ECDH vectors checked against OpenSSL by
 * Tests/gen_auth_vectors.py).
 *
 * The PRF (gip_prf) is derived from xone auth/crypto.c
 * (gip_auth_compute_prf), Copyright (C) 2023 Severin von Wnuck-Lipinski
 * <severinvonw@outlook.de>, GPL-2.0-or-later, used under the "or later"
 * option. The rest of this file was written for XPadMac.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * General Public License (LICENSE at the top of this repository).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gip_crypto.h"

#include <string.h>

void gip_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;

    while (n--) *v++ = 0;
}

/* ---- SHA-256 (FIPS 180-4) ----------------------------------------------------------- */

static const uint32_t sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, k;
    size_t i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
               (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void gip_sha256_init(gip_sha256_t *c)
{
    static const uint32_t h0[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };

    memcpy(c->h, h0, sizeof h0);
    c->bytes = 0;
    c->used = 0;
}

void gip_sha256_update(gip_sha256_t *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;

    c->bytes += n;
    while (n) {
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += take;
        p += take;
        n -= take;
        if (c->used == 64) {
            sha_block(c->h, c->buf);
            c->used = 0;
        }
    }
}

void gip_sha256_final(gip_sha256_t *c, uint8_t out[GIP_SHA256_LEN])
{
    uint64_t bits = c->bytes * 8u;
    size_t i;

    c->buf[c->used++] = 0x80;
    if (c->used > 56) {
        memset(c->buf + c->used, 0, 64 - c->used);
        sha_block(c->h, c->buf);
        c->used = 0;
    }
    memset(c->buf + c->used, 0, 56 - c->used);
    for (i = 0; i < 8; i++) c->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_block(c->h, c->buf);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
    gip_wipe(c, sizeof *c);
}

void gip_sha256(const void *p, size_t n, uint8_t out[GIP_SHA256_LEN])
{
    gip_sha256_t c;

    gip_sha256_init(&c);
    gip_sha256_update(&c, p, n);
    gip_sha256_final(&c, out);
}

/* ---- HMAC-SHA256 (RFC 2104) and the PRF ------------------------------------------------ */

void gip_hmac_init(gip_hmac_t *h, const uint8_t *key, size_t klen)
{
    uint8_t k[64], pad[64];
    size_t i;

    memset(k, 0, sizeof k);
    if (klen > sizeof k) gip_sha256(key, klen, k);
    else if (klen) memcpy(k, key, klen);
    for (i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36u);
    gip_sha256_init(&h->inner);
    gip_sha256_update(&h->inner, pad, sizeof pad);
    for (i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5Cu);
    gip_sha256_init(&h->outer);
    gip_sha256_update(&h->outer, pad, sizeof pad);
    gip_wipe(k, sizeof k);
    gip_wipe(pad, sizeof pad);
}

void gip_hmac_update(gip_hmac_t *h, const void *p, size_t n)
{
    gip_sha256_update(&h->inner, p, n);
}

void gip_hmac_final(gip_hmac_t *h, uint8_t out[GIP_SHA256_LEN])
{
    uint8_t ih[GIP_SHA256_LEN];

    gip_sha256_final(&h->inner, ih);
    gip_sha256_update(&h->outer, ih, sizeof ih);
    gip_sha256_final(&h->outer, out);
    gip_wipe(ih, sizeof ih);
}

void gip_hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t n, uint8_t out[GIP_SHA256_LEN])
{
    gip_hmac_t h;

    gip_hmac_init(&h, key, klen);
    gip_hmac_update(&h, msg, n);
    gip_hmac_final(&h, out);
}

/* xone auth/crypto.c gip_auth_compute_prf:
 *   A1 = HMAC(K, label || seed)
 *   while bytes are wanted: out += HMAC(K, A_i || label || seed); A_i+1 = HMAC(K, A_i)
 * which is TLS 1.2's P_SHA256(secret, label || seed). The keyed state is
 * set up once and copied for every HMAC. */
void gip_prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *seed, size_t slen,
             uint8_t *out, size_t olen)
{
    gip_hmac_t keyed, h;
    uint8_t a[GIP_SHA256_LEN], block[GIP_SHA256_LEN];
    size_t llen = strlen(label);

    gip_hmac_init(&keyed, key, klen);
    h = keyed;
    gip_hmac_update(&h, label, llen);
    gip_hmac_update(&h, seed, slen);
    gip_hmac_final(&h, a);
    while (olen) {
        size_t take = olen < sizeof block ? olen : sizeof block;

        h = keyed;
        gip_hmac_update(&h, a, sizeof a);
        gip_hmac_update(&h, label, llen);
        gip_hmac_update(&h, seed, slen);
        gip_hmac_final(&h, block);
        memcpy(out, block, take);
        out += take;
        olen -= take;
        h = keyed;
        gip_hmac_update(&h, a, sizeof a);
        gip_hmac_final(&h, a);
    }
    gip_wipe(&keyed, sizeof keyed);
    gip_wipe(a, sizeof a);
    gip_wipe(block, sizeof block);
}

/* ---- big numbers ----------------------------------------------------------------------- */

#define BN_MAX (GIP_RSA_MAX_BYTES / 4)  /* limbs */

static void bn_from_be(uint32_t *r, size_t nl, const uint8_t *b, size_t blen)
{
    size_t i;

    memset(r, 0, nl * sizeof r[0]);
    for (i = 0; i < blen && i / 4 < nl; i++) r[i / 4] |= (uint32_t)b[blen - 1 - i] << (8 * (i % 4));
}

static void bn_to_be(uint8_t *b, size_t blen, const uint32_t *r, size_t nl)
{
    size_t i;

    for (i = 0; i < blen; i++) b[blen - 1 - i] = i / 4 < nl ? (uint8_t)(r[i / 4] >> (8 * (i % 4))) : 0;
}

static int bn_cmp(const uint32_t *a, const uint32_t *b, size_t nl)
{
    while (nl--) {
        if (a[nl] != b[nl]) return a[nl] > b[nl] ? 1 : -1;
    }
    return 0;
}

static int bn_is_zero(const uint32_t *a, size_t nl)
{
    uint32_t acc = 0;

    while (nl--) acc |= a[nl];
    return acc == 0;
}

/* r = a + b (r may be a or b); returns the carry. */
static uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b, size_t nl)
{
    uint64_t c = 0;
    size_t i;

    for (i = 0; i < nl; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

/* r = a - b (r may be a or b); returns the borrow. */
static uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, size_t nl)
{
    uint32_t borrow = 0;
    size_t i;

    for (i = 0; i < nl; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (uint32_t)(d >> 63);
    }
    return borrow;
}

typedef struct {
    uint32_t m[BN_MAX];         /* the odd modulus */
    uint32_t r2[BN_MAX];        /* R^2 mod m, R = 2^(32 nl) */
    uint32_t one[BN_MAX];       /* 1 (not in Montgomery form) */
    uint32_t n0;                /* -m^-1 mod 2^32 */
    size_t nl;
} mont_t;

/* m odd, m > 1, nl <= BN_MAX limbs. */
static void mont_init(mont_t *M, const uint32_t *m, size_t nl)
{
    uint32_t x = m[0];
    size_t i, k;

    M->nl = nl;
    memcpy(M->m, m, nl * sizeof m[0]);
    for (k = 0; k < 5; k++) x *= 2u - m[0] * x;     /* Newton: inverse mod 2^32 */
    M->n0 = 0u - x;
    memset(M->one, 0, sizeof M->one);
    M->one[0] = 1;
    /* R^2 mod m: 1 doubled 2 * 32 * nl times, reduced each time. */
    memcpy(M->r2, M->one, nl * sizeof m[0]);
    for (k = 0; k < 64 * nl; k++) {
        uint32_t carry = 0;
        for (i = 0; i < nl; i++) {
            uint32_t top = M->r2[i] >> 31;
            M->r2[i] = M->r2[i] << 1 | carry;
            carry = top;
        }
        if (carry || bn_cmp(M->r2, M->m, nl) >= 0) bn_sub(M->r2, M->r2, M->m, nl);
    }
}

/* r = a * b / R mod m (CIOS), a and b below m; r may be a or b. */
static void mont_mul(const mont_t *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t t[BN_MAX + 2];
    size_t i, j, nl = M->nl;

    /* mont_init's callers pass 8 (P-256) or 64 (RSA-2048) limbs; say so for
     * the static analyzer, which otherwise follows an nl == 0 path. */
    if (nl == 0 || nl > BN_MAX) __builtin_unreachable();
    memset(t, 0, (nl + 2) * sizeof t[0]);
    for (i = 0; i < nl; i++) {
        uint64_t s, c = 0;
        uint32_t q;

        for (j = 0; j < nl; j++) {
            s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[nl] + c;
        t[nl] = (uint32_t)s;
        t[nl + 1] = (uint32_t)(s >> 32);

        q = t[0] * M->n0;
        s = (uint64_t)t[0] + (uint64_t)q * M->m[0];
        c = s >> 32;
        for (j = 1; j < nl; j++) {
            s = (uint64_t)t[j] + (uint64_t)q * M->m[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[nl] + c;
        t[nl - 1] = (uint32_t)s;
        t[nl] = t[nl + 1] + (uint32_t)(s >> 32);
    }
    if (t[nl] || bn_cmp(t, M->m, nl) >= 0) bn_sub(t, t, M->m, nl);
    memcpy(r, t, nl * sizeof t[0]);
}

/* ---- RSA ---------------------------------------------------------------------------------- */

int gip_rsa_public(const gip_rsa_pub_t *k, const uint8_t *in, uint8_t *out)
{
    mont_t M;
    uint32_t n[BN_MAX], m[BN_MAX], x[BN_MAX];
    size_t nl;
    int bit;

    if (!k || !in || !out || k->nlen < 1 || k->nlen > GIP_RSA_MAX_BYTES || !(k->n[k->nlen - 1] & 1u) ||
        k->e == 0)
        return 0;
    nl = (k->nlen + 3) / 4;
    bn_from_be(n, nl, k->n, k->nlen);
    bn_from_be(m, nl, in, k->nlen);
    if (nl == 1 && n[0] == 1) return 0;         /* n = 1 */
    if (bn_cmp(m, n, nl) >= 0) return 0;
    mont_init(&M, n, nl);
    mont_mul(&M, m, m, M.r2);                   /* m R */
    mont_mul(&M, x, M.one, M.r2);               /* R: 1 in Montgomery form */
    for (bit = 31; bit >= 0; bit--) {
        mont_mul(&M, x, x, x);
        if ((k->e >> (unsigned)bit) & 1u) mont_mul(&M, x, x, m);
    }
    mont_mul(&M, x, x, M.one);                  /* out of Montgomery form */
    bn_to_be(out, k->nlen, x, nl);
    gip_wipe(m, sizeof m);
    gip_wipe(x, sizeof x);
    return 1;
}

int gip_rsa_encrypt_pkcs1(const gip_rsa_pub_t *k, const uint8_t *msg, size_t mlen, const uint8_t *ps,
                          uint8_t *out)
{
    uint8_t em[GIP_RSA_MAX_BYTES];
    size_t pslen, i;
    int ok;

    if (!k || !msg || !ps || !out || k->nlen > GIP_RSA_MAX_BYTES || k->nlen < 11 || mlen > k->nlen - 11)
        return 0;
    pslen = k->nlen - 3 - mlen;
    for (i = 0; i < pslen; i++)
        if (!ps[i]) return 0;
    em[0] = 0x00;
    em[1] = 0x02;
    memcpy(em + 2, ps, pslen);
    em[2 + pslen] = 0x00;
    memcpy(em + 3 + pslen, msg, mlen);
    ok = gip_rsa_public(k, em, out);
    gip_wipe(em, sizeof em);
    return ok;
}

/* A DER length at p[*pos] (short form, or long form of 1 or 2 bytes). */
static int der_len(const uint8_t *p, size_t end, size_t *pos, size_t *out)
{
    size_t v = 0, nb;

    if (*pos >= end) return 0;
    if (p[*pos] < 0x80) {
        *out = p[(*pos)++];
        return 1;
    }
    nb = p[(*pos)++] & 0x7Fu;
    if (nb == 0 || nb > 2) return 0;
    while (nb--) {
        if (*pos >= end) return 0;
        v = v << 8 | p[(*pos)++];
    }
    *out = v;
    return 1;
}

/* A non-negative DER INTEGER at p[*pos]: its magnitude (leading zeros dropped). */
static int der_uint(const uint8_t *p, size_t end, size_t *pos, const uint8_t **mag, size_t *mlen)
{
    size_t len;

    if (*pos >= end || p[(*pos)++] != 0x02) return 0;
    if (!der_len(p, end, pos, &len) || len == 0 || len > end - *pos) return 0;
    if (p[*pos] & 0x80) return 0;               /* negative */
    *mag = p + *pos;
    *mlen = len;
    *pos += len;
    while (*mlen && **mag == 0) {
        (*mag)++;
        (*mlen)--;
    }
    return 1;
}

int gip_rsa_parse_pub(const uint8_t *der, size_t len, gip_rsa_pub_t *k, size_t *used)
{
    const uint8_t *n, *e;
    size_t pos = 1, seq, end, nlen, elen, i;
    uint32_t ev = 0;

    if (!der || !k || len < 2 || der[0] != 0x30) return 0;
    if (!der_len(der, len, &pos, &seq) || seq > len - pos) return 0;
    end = pos + seq;
    if (!der_uint(der, end, &pos, &n, &nlen) || !der_uint(der, end, &pos, &e, &elen) || pos != end) return 0;
    if (nlen < 64 || nlen > GIP_RSA_MAX_BYTES || !(n[nlen - 1] & 1u)) return 0;
    if (elen == 0 || elen > 4) return 0;
    for (i = 0; i < elen; i++) ev = ev << 8 | e[i];
    if (ev < 3 || !(ev & 1u)) return 0;
    memcpy(k->n, n, nlen);
    k->nlen = nlen;
    k->e = ev;
    if (used) *used = end;
    return 1;
}

int gip_rsa_find_pub(const uint8_t *cert, size_t len, size_t want_bytes, gip_rsa_pub_t *k, size_t *at)
{
    size_t i;

    if (!cert || !k) return 0;
    for (i = 0; i + 2 <= len; i++) {
        if (cert[i] != 0x30) continue;
        if (gip_rsa_parse_pub(cert + i, len - i, k, NULL) && (!want_bytes || k->nlen == want_bytes)) {
            if (at) *at = i;
            return 1;
        }
    }
    return 0;
}

/* ---- P-256 (FIPS 186-4 D.1.2.3) ----------------------------------------------------------------- */

static const uint8_t p256_p[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t p256_p_minus_2[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfd,
};
static const uint8_t p256_n[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};
static const uint8_t p256_b[32] = {
    0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
    0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b,
};
static const uint8_t p256_g[64] = {
    0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
    0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
    0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
    0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
};

#define FE 8                    /* limbs in a field element */

typedef struct {
    mont_t f;                   /* arithmetic mod p */
    uint32_t one[FE];           /* 1, in Montgomery form */
    uint32_t b[FE];             /* the curve's b, in Montgomery form */
} curve_t;

typedef struct {
    uint32_t x[FE], y[FE], z[FE];   /* Jacobian, Montgomery form; z == 0: infinity */
} jpoint;

static void fe_add(const curve_t *C, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    if (bn_add(r, a, b, FE) || bn_cmp(r, C->f.m, FE) >= 0) bn_sub(r, r, C->f.m, FE);
}

static void fe_sub(const curve_t *C, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    if (bn_sub(r, a, b, FE)) bn_add(r, r, C->f.m, FE);
}

static void fe_mul(const curve_t *C, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    mont_mul(&C->f, r, a, b);
}

/* Big-endian bytes (below p) into Montgomery form. */
static void fe_from_be(const curve_t *C, uint32_t *r, const uint8_t *b)
{
    uint32_t t[FE];

    bn_from_be(t, FE, b, 32);
    fe_mul(C, r, t, C->f.r2);
}

static void fe_to_be(const curve_t *C, uint8_t *b, const uint32_t *a)
{
    uint32_t t[FE];

    fe_mul(C, t, a, C->f.one);
    bn_to_be(b, 32, t, FE);
}

/* r = a^(p-2) = 1/a (a != 0). */
static void fe_inv(const curve_t *C, uint32_t *r, const uint32_t *a)
{
    uint32_t x[FE];
    int i;

    memcpy(x, C->one, sizeof x);
    for (i = 0; i < 256; i++) {
        fe_mul(C, x, x, x);
        if ((p256_p_minus_2[i / 8] >> (7 - i % 8)) & 1u) fe_mul(C, x, x, a);
    }
    memcpy(r, x, sizeof x);
}

static void curve_init(curve_t *C)
{
    uint32_t p[FE];

    bn_from_be(p, FE, p256_p, 32);
    mont_init(&C->f, p, FE);
    fe_mul(C, C->one, C->f.one, C->f.r2);
    fe_from_be(C, C->b, p256_b);
}

/* y^2 == x^3 - 3x + b, Montgomery form. */
static int on_curve(const curve_t *C, const uint32_t *x, const uint32_t *y)
{
    uint32_t l[FE], r[FE], t[FE];

    fe_mul(C, l, y, y);
    fe_mul(C, r, x, x);
    fe_mul(C, r, r, x);
    fe_add(C, t, x, x);
    fe_add(C, t, t, x);
    fe_sub(C, r, r, t);
    fe_add(C, r, r, C->b);
    return bn_cmp(l, r, FE) == 0;
}

/* An X || Y point: on the curve, coordinates below p. */
static int point_from_bytes(const curve_t *C, jpoint *P, const uint8_t *xy)
{
    uint32_t t[FE];

    bn_from_be(t, FE, xy, 32);
    if (bn_cmp(t, C->f.m, FE) >= 0) return 0;
    bn_from_be(t, FE, xy + 32, 32);
    if (bn_cmp(t, C->f.m, FE) >= 0) return 0;
    fe_from_be(C, P->x, xy);
    fe_from_be(C, P->y, xy + 32);
    memcpy(P->z, C->one, sizeof P->z);
    return on_curve(C, P->x, P->y);
}

/* dbl-2001-b (a = -3): delta = Z^2, gamma = Y^2, beta = X gamma,
 * alpha = 3 (X - delta)(X + delta), X3 = alpha^2 - 8 beta,
 * Z3 = (Y + Z)^2 - gamma - delta, Y3 = alpha (4 beta - X3) - 8 gamma^2. */
static void point_dbl(const curve_t *C, jpoint *R, const jpoint *P)
{
    uint32_t delta[FE], gamma[FE], beta[FE], alpha[FE], t[FE], u[FE], x3[FE], y3[FE], z3[FE];

    if (bn_is_zero(P->z, FE)) {
        *R = *P;
        return;
    }
    fe_mul(C, delta, P->z, P->z);
    fe_mul(C, gamma, P->y, P->y);
    fe_mul(C, beta, P->x, gamma);
    fe_sub(C, t, P->x, delta);
    fe_add(C, u, P->x, delta);
    fe_mul(C, alpha, t, u);
    fe_add(C, t, alpha, alpha);
    fe_add(C, alpha, t, alpha);
    fe_mul(C, x3, alpha, alpha);
    fe_add(C, beta, beta, beta);                /* 2 beta */
    fe_add(C, beta, beta, beta);                /* 4 beta */
    fe_add(C, t, beta, beta);                   /* 8 beta */
    fe_sub(C, x3, x3, t);
    fe_add(C, z3, P->y, P->z);
    fe_mul(C, z3, z3, z3);
    fe_sub(C, z3, z3, gamma);
    fe_sub(C, z3, z3, delta);
    fe_mul(C, t, gamma, gamma);
    fe_add(C, t, t, t);
    fe_add(C, t, t, t);
    fe_add(C, t, t, t);                         /* 8 gamma^2 */
    fe_sub(C, y3, beta, x3);
    fe_mul(C, y3, alpha, y3);
    fe_sub(C, y3, y3, t);
    memcpy(R->x, x3, sizeof x3);
    memcpy(R->y, y3, sizeof y3);
    memcpy(R->z, z3, sizeof z3);
}

/* add-1998-cmo-2: U1 = X1 Z2^2, U2 = X2 Z1^2, S1 = Y1 Z2^3, S2 = Y2 Z1^3,
 * H = U2 - U1, r = S2 - S1, X3 = r^2 - H^3 - 2 U1 H^2,
 * Y3 = r (U1 H^2 - X3) - S1 H^3, Z3 = Z1 Z2 H. */
static void point_add(const curve_t *C, jpoint *R, const jpoint *P, const jpoint *Q)
{
    uint32_t z1z1[FE], z2z2[FE], u1[FE], u2[FE], s1[FE], s2[FE], h[FE], r[FE], hh[FE], hhh[FE], v[FE];
    uint32_t x3[FE], y3[FE], z3[FE];

    if (bn_is_zero(P->z, FE)) {
        *R = *Q;
        return;
    }
    if (bn_is_zero(Q->z, FE)) {
        *R = *P;
        return;
    }
    fe_mul(C, z1z1, P->z, P->z);
    fe_mul(C, z2z2, Q->z, Q->z);
    fe_mul(C, u1, P->x, z2z2);
    fe_mul(C, u2, Q->x, z1z1);
    fe_mul(C, s1, P->y, Q->z);
    fe_mul(C, s1, s1, z2z2);
    fe_mul(C, s2, Q->y, P->z);
    fe_mul(C, s2, s2, z1z1);
    fe_sub(C, h, u2, u1);
    fe_sub(C, r, s2, s1);
    if (bn_is_zero(h, FE)) {
        if (bn_is_zero(r, FE)) {
            point_dbl(C, R, P);
        } else {
            memset(R, 0, sizeof *R);            /* P = -Q */
        }
        return;
    }
    fe_mul(C, hh, h, h);
    fe_mul(C, hhh, h, hh);
    fe_mul(C, v, u1, hh);
    fe_mul(C, x3, r, r);
    fe_sub(C, x3, x3, hhh);
    fe_sub(C, x3, x3, v);
    fe_sub(C, x3, x3, v);
    fe_sub(C, y3, v, x3);
    fe_mul(C, y3, r, y3);
    fe_mul(C, s1, s1, hhh);
    fe_sub(C, y3, y3, s1);
    fe_mul(C, z3, P->z, Q->z);
    fe_mul(C, z3, z3, h);
    memcpy(R->x, x3, sizeof x3);
    memcpy(R->y, y3, sizeof y3);
    memcpy(R->z, z3, sizeof z3);
}

/* R = k P, k big-endian (double-and-add, most significant bit first). */
static void point_mul(const curve_t *C, jpoint *R, const uint8_t *k, const jpoint *P)
{
    jpoint acc;
    int i;

    memset(&acc, 0, sizeof acc);
    for (i = 0; i < 256; i++) {
        point_dbl(C, &acc, &acc);
        if ((k[i / 8] >> (7 - i % 8)) & 1u) point_add(C, &acc, &acc, P);
    }
    *R = acc;
    gip_wipe(&acc, sizeof acc);
}

static int point_to_bytes(const curve_t *C, uint8_t *xy, const jpoint *P)
{
    uint32_t zi[FE], zi2[FE], t[FE];

    if (bn_is_zero(P->z, FE)) return 0;
    fe_inv(C, zi, P->z);
    fe_mul(C, zi2, zi, zi);
    fe_mul(C, t, P->x, zi2);
    fe_to_be(C, xy, t);
    fe_mul(C, zi2, zi2, zi);
    fe_mul(C, t, P->y, zi2);
    fe_to_be(C, xy + 32, t);
    return 1;
}

int gip_p256_priv_ok(const uint8_t priv[GIP_P256_LEN])
{
    uint8_t acc = 0;
    size_t i;

    if (!priv) return 0;
    for (i = 0; i < 32; i++) acc |= priv[i];
    return acc != 0 && memcmp(priv, p256_n, 32) < 0;
}

int gip_p256_pub_ok(const uint8_t pub[2 * GIP_P256_LEN])
{
    curve_t C;
    jpoint P;

    if (!pub) return 0;
    curve_init(&C);
    return point_from_bytes(&C, &P, pub);
}

int gip_p256_public(const uint8_t priv[GIP_P256_LEN], uint8_t pub[2 * GIP_P256_LEN])
{
    curve_t C;
    jpoint G, R;
    int ok;

    if (!gip_p256_priv_ok(priv) || !pub) return 0;
    curve_init(&C);
    if (!point_from_bytes(&C, &G, p256_g)) return 0;
    point_mul(&C, &R, priv, &G);
    ok = point_to_bytes(&C, pub, &R);
    gip_wipe(&R, sizeof R);
    return ok;
}

int gip_p256_ecdh(const uint8_t priv[GIP_P256_LEN], const uint8_t peer[2 * GIP_P256_LEN],
                  uint8_t x[GIP_P256_LEN])
{
    curve_t C;
    jpoint Q, R;
    uint8_t xy[64];
    int ok;

    if (!gip_p256_priv_ok(priv) || !peer || !x) return 0;
    curve_init(&C);
    if (!point_from_bytes(&C, &Q, peer)) return 0;
    point_mul(&C, &R, priv, &Q);
    ok = point_to_bytes(&C, xy, &R);
    if (ok) memcpy(x, xy, 32);
    gip_wipe(xy, sizeof xy);
    gip_wipe(&R, sizeof R);
    return ok;
}
