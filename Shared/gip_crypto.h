/* The cryptography GIP authentication needs, in pure C99 (no heap, no OS
 * headers, nothing but <stdint.h> <stddef.h> <string.h>), so that it builds
 * for DriverKit as for the host:
 *
 *  - SHA-256 (FIPS 180-4), with a context that may be copied: a copy
 *    finalised gives the running transcript's hash without ending it;
 *  - HMAC-SHA256 (RFC 2104);
 *  - the PRF of xone's auth/crypto.c (gip_auth_compute_prf), which is TLS
 *    1.2's P_SHA256 (RFC 5246 section 5) with seed = label || seed;
 *  - RSA public-key encryption with PKCS#1 v1.5 padding (block type 2) for
 *    a key of up to 2048 bits, and a parser for the DER RSAPublicKey the
 *    pad's certificate carries;
 *  - ECDH over NIST P-256 (key pair from a caller-chosen private key, shared
 *    secret = the X coordinate, the peer's point checked to be on the curve).
 *
 * Nothing here draws random numbers: the caller passes them in (gip_auth.c
 * takes them from xpad_random()). None of it is constant-time; it does not
 * need to be (a USB pad's session, no secret worth a timing attack).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XPAD_GIP_CRYPTO_H
#define XPAD_GIP_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SHA-256 --------------------------------------------------------------- */

#define GIP_SHA256_LEN 32

typedef struct {
    uint32_t h[8];
    uint64_t bytes;             /* message length so far */
    uint8_t buf[64];
    size_t used;                /* bytes in buf */
} gip_sha256_t;

void gip_sha256_init(gip_sha256_t *c);
void gip_sha256_update(gip_sha256_t *c, const void *p, size_t n);
void gip_sha256_final(gip_sha256_t *c, uint8_t out[GIP_SHA256_LEN]);
void gip_sha256(const void *p, size_t n, uint8_t out[GIP_SHA256_LEN]);

/* ---- HMAC-SHA256 and the PRF ------------------------------------------------------ */

typedef struct {
    gip_sha256_t inner, outer;
} gip_hmac_t;

void gip_hmac_init(gip_hmac_t *h, const uint8_t *key, size_t klen);
void gip_hmac_update(gip_hmac_t *h, const void *p, size_t n);
void gip_hmac_final(gip_hmac_t *h, uint8_t out[GIP_SHA256_LEN]);
void gip_hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t n, uint8_t out[GIP_SHA256_LEN]);

/* out[0..olen) = P_SHA256(key, label || seed); label without its NUL. */
void gip_prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *seed, size_t slen,
             uint8_t *out, size_t olen);

/* ---- RSA ------------------------------------------------------------------------- */

#define GIP_RSA_MAX_BYTES 256   /* 2048 bits */

typedef struct {
    uint8_t n[GIP_RSA_MAX_BYTES];   /* modulus, big-endian, nlen bytes, n[0] != 0 */
    size_t nlen;
    uint32_t e;                     /* public exponent */
} gip_rsa_pub_t;

/* A DER RSAPublicKey (SEQUENCE { INTEGER n, INTEGER e }, PKCS#1) at
 * der[0..len): 1 and *k (and *used, its length) if it is one, with an odd
 * modulus of 512..2048 bits and an odd exponent of 3..2^32-1; else 0. */
int gip_rsa_parse_pub(const uint8_t *der, size_t len, gip_rsa_pub_t *k, size_t *used);

/* The first RSAPublicKey inside cert[0..len) (an X.509 certificate's
 * SubjectPublicKeyInfo holds one in its BIT STRING; a bare one is found
 * too) whose modulus is exactly want_bytes long (0: any length): 1, *k and
 * *at (its offset), or 0. */
int gip_rsa_find_pub(const uint8_t *cert, size_t len, size_t want_bytes, gip_rsa_pub_t *k, size_t *at);

/* out[0..nlen) = in^e mod n, in[0..nlen) big-endian and below n. 1, or 0. */
int gip_rsa_public(const gip_rsa_pub_t *k, const uint8_t *in, uint8_t *out);

/* PKCS#1 v1.5 encryption (RFC 8017 7.2.1): EM = 00 02 PS 00 msg, PS the
 * nlen - 3 - mlen bytes at ps (all non-zero: the caller draws them),
 * out[0..nlen) = EM^e mod n. 1, or 0 (msg too long, a zero in PS). */
int gip_rsa_encrypt_pkcs1(const gip_rsa_pub_t *k, const uint8_t *msg, size_t mlen, const uint8_t *ps,
                          uint8_t *out);

/* ---- P-256 ------------------------------------------------------------------------ */

#define GIP_P256_LEN 32         /* a scalar or a coordinate, big-endian */

/* A point is X || Y, 32 bytes each, big-endian, no 0x04 prefix (as xone,
 * the kernel's ecdh KPP and the pad encode it). */

/* 1 if 0 < priv < n (the group order). */
int gip_p256_priv_ok(const uint8_t priv[GIP_P256_LEN]);

/* 1 if pub is a point on the curve (coordinates below p, not infinity). */
int gip_p256_pub_ok(const uint8_t pub[2 * GIP_P256_LEN]);

/* pub = priv * G. 1, or 0 when priv is out of range. */
int gip_p256_public(const uint8_t priv[GIP_P256_LEN], uint8_t pub[2 * GIP_P256_LEN]);

/* x = the X coordinate of priv * peer. 1, or 0 when priv is out of range or
 * peer is not on the curve. */
int gip_p256_ecdh(const uint8_t priv[GIP_P256_LEN], const uint8_t peer[2 * GIP_P256_LEN],
                  uint8_t x[GIP_P256_LEN]);

/* Overwrites n bytes in a way the compiler keeps (secrets, once done). */
void gip_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif
