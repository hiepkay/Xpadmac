/*
 * Shared/gip_crypto.c against known answers: SHA-256 (FIPS 180-2's examples
 * and every length 0..200), HMAC-SHA256 (RFC 4231), the PRF (the TLS 1.2
 * P_SHA256 vector and xone's labels), RSA PKCS#1 v1.5 encryption (the
 * ciphertexts OpenSSL decrypted back to the plaintext), the DER key parser
 * (a bare RSAPublicKey, a SubjectPublicKeyInfo, a self-signed certificate
 * with an empty subject) and P-256 (small multiples of G, n - 1, and the
 * ECDH secret OpenSSL derived). The vectors: auth_vectors.h, written by
 * gen_auth_vectors.py.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gip_crypto.h"
#include "test.h"
#include "auth_vectors.h"

#include <stdlib.h>
#include <time.h>

static void test_sha256(void)
{
    static const uint8_t empty[32] = { 0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
                                       0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
                                       0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 };
    static const uint8_t abc[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                     0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                     0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    uint8_t d[32], pattern[200], *a;
    gip_sha256_t c, copy;
    size_t i, n;

    TEST("sha256 FIPS 180-2");
    gip_sha256("", 0, d);
    CHECK_MEM(d, empty, 32);
    gip_sha256("abc", 3, d);
    CHECK_MEM(d, abc, 32);
    CHECK_MEM(d, av_sha_abc, 32);
    gip_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    CHECK_MEM(d, av_sha_448, 32);
    gip_sha256("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs"
               "mnopqrstnopqrstu", 112, d);
    CHECK_MEM(d, av_sha_896, 32);
    a = (uint8_t *)malloc(1000000);
    CHECK(a != NULL);
    if (a) {
        memset(a, 'a', 1000000);
        gip_sha256(a, 1000000, d);
        CHECK_MEM(d, av_sha_million_a, 32);
        gip_sha256_init(&c);                        /* in uneven pieces */
        for (i = 0, n = 1; i < 1000000; i += n, n = n * 3 % 1000 + 1)
            gip_sha256_update(&c, a + i, i + n > 1000000 ? 1000000 - i : n);
        gip_sha256_final(&c, d);
        CHECK_MEM(d, av_sha_million_a, 32);
        free(a);
    }

    TEST("sha256 every length 0..200");
    for (i = 0; i < sizeof pattern; i++) pattern[i] = (uint8_t)(i * 31 + 7);
    for (n = 0; n <= 200; n++) {
        gip_sha256(pattern, n, d);
        CHECK_MEM(d, av_sha_pattern + 32 * n, 32);
        gip_sha256_init(&c);                        /* a byte at a time */
        for (i = 0; i < n; i++) gip_sha256_update(&c, pattern + i, 1);
        gip_sha256_final(&c, d);
        CHECK_MEM(d, av_sha_pattern + 32 * n, 32);
    }

    TEST("sha256 a copied context is a snapshot");
    gip_sha256_init(&c);
    gip_sha256_update(&c, pattern, 100);
    copy = c;
    gip_sha256_final(&copy, d);
    CHECK_MEM(d, av_sha_pattern + 32 * 100, 32);
    gip_sha256_update(&c, pattern + 100, 100);      /* the original goes on */
    gip_sha256_final(&c, d);
    CHECK_MEM(d, av_sha_pattern + 32 * 200, 32);
}

static void test_hmac(void)
{
    static const uint8_t tc1[32] = { 0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf,
                                     0xce, 0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83,
                                     0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7 };
    uint8_t mac[32];
    gip_hmac_t h;
    int i;

    TEST("hmac-sha256 RFC 4231");
    {
        static const uint8_t key[20] = { 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                         0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b };
        gip_hmac_sha256(key, sizeof key, "Hi There", 8, mac);
        CHECK_MEM(mac, tc1, 32);
    }
    for (i = 0; i < AV_HMAC_KEY_COUNT; i++) {
        gip_hmac_sha256(av_hmac_key[i].p, (size_t)av_hmac_key[i].n, av_hmac_msg[i].p, (size_t)av_hmac_msg[i].n,
                        mac);
        CHECK_MEM(mac, av_hmac_mac[i].p, 32);
        gip_hmac_init(&h, av_hmac_key[i].p, (size_t)av_hmac_key[i].n);     /* in two pieces */
        gip_hmac_update(&h, av_hmac_msg[i].p, 3);
        gip_hmac_update(&h, av_hmac_msg[i].p + 3, (size_t)av_hmac_msg[i].n - 3);
        gip_hmac_final(&h, mac);
        CHECK_MEM(mac, av_hmac_mac[i].p, 32);
    }
}

static void test_prf(void)
{
    static const uint8_t secret[16] = { 0x9b, 0xbe, 0x43, 0x6b, 0xa9, 0x40, 0xf0, 0x17,
                                        0xb1, 0x76, 0x52, 0x84, 0x9a, 0x71, 0xdb, 0x35 };
    static const uint8_t tls_seed[16] = { 0xa0, 0xba, 0x9f, 0x93, 0x6c, 0xda, 0x31, 0x18,
                                          0x27, 0xa6, 0xf7, 0x96, 0xff, 0xd5, 0x19, 0x8c };
    /* The published TLS 1.2 PRF (SHA-256) vector's first 16 bytes. */
    static const uint8_t tls_head[16] = { 0xe3, 0xf2, 0x29, 0xba, 0x72, 0x7b, 0xe1, 0x7b,
                                          0x8d, 0x12, 0x26, 0x20, 0x55, 0x7c, 0xd4, 0x53 };
    uint8_t out[100], key[48], seed[64];
    size_t i;

    TEST("prf = TLS 1.2 P_SHA256");
    memset(out, 0xEE, sizeof out);
    gip_prf(secret, sizeof secret, "test label", tls_seed, sizeof tls_seed, out, 100);
    CHECK_MEM(out, tls_head, 16);
    CHECK_MEM(out, av_prf_tls12, 100);

    TEST("prf lengths, xone's labels");
    for (i = 0; i < sizeof key; i++) key[i] = (uint8_t)i;
    for (i = 0; i < sizeof seed; i++) seed[i] = (uint8_t)(0x40 + i);
#define PRF_LEN(n)                                                                     \
    do {                                                                               \
        memset(out, 0xEE, sizeof out);                                                 \
        gip_prf(key, sizeof key, "Master Secret", seed, sizeof seed, out, n);          \
        CHECK_MEM(out, av_prf_ms_##n, n);                                              \
        CHECK_EQ(out[n], 0xEE);                     /* not a byte more */              \
    } while (0)
    PRF_LEN(1);
    PRF_LEN(16);
    PRF_LEN(31);
    PRF_LEN(32);
    PRF_LEN(33);
    PRF_LEN(48);
    PRF_LEN(64);
    PRF_LEN(65);
    gip_prf(key, sizeof key, "EXPORTER DAWN data channel session key for controller", seed, sizeof seed, out, 16);
    CHECK_MEM(out, av_prf_session, 16);
}

static void test_rsa(void)
{
    gip_rsa_pub_t k;
    uint8_t out[256], ps[205], der[300];
    size_t used = 0, at = 0;

    TEST("rsa parse a bare RSAPublicKey");
    CHECK_EQ(gip_rsa_parse_pub(av_rsa_pub_der, sizeof av_rsa_pub_der, &k, &used), 1);
    CHECK_EQ(used, sizeof av_rsa_pub_der);
    CHECK_EQ(k.nlen, 256);
    CHECK_EQ(k.e, AV_RSA_E);
    CHECK_MEM(k.n, av_rsa_n, 256);
    CHECK_EQ(gip_rsa_parse_pub(av_rsa3_pub_der, sizeof av_rsa3_pub_der, &k, NULL), 1);
    CHECK_EQ(k.e, 3);
    CHECK_MEM(k.n, av_rsa3_n, 256);

    TEST("rsa find the key in a SubjectPublicKeyInfo and a certificate");
    CHECK_EQ(gip_rsa_parse_pub(av_rsa_spki_der, sizeof av_rsa_spki_der, &k, NULL), 0);   /* not itself one */
    CHECK_EQ(gip_rsa_find_pub(av_rsa_spki_der, sizeof av_rsa_spki_der, 256, &k, &at), 1);
    CHECK_MEM(k.n, av_rsa_n, 256);
    CHECK_EQ(gip_rsa_find_pub(av_cert, sizeof av_cert, 256, &k, &at), 1);
    CHECK_EQ(at, AV_CERT_KEY_AT);
    CHECK_EQ(k.e, AV_RSA_E);
    CHECK_MEM(k.n, av_rsa_n, 256);
    CHECK_EQ(gip_rsa_find_pub(av_cert, sizeof av_cert, 128, &k, &at), 0);   /* no 1024-bit key in it */
    CHECK_EQ(gip_rsa_find_pub(av_cert, AV_CERT_KEY_AT + 200, 256, &k, &at), 0);   /* cut inside the key */

    TEST("rsa malformed keys");
    memcpy(der, av_rsa_pub_der, sizeof av_rsa_pub_der);
    der[0] = 0x31;
    CHECK_EQ(gip_rsa_parse_pub(der, sizeof av_rsa_pub_der, &k, NULL), 0);  /* not a SEQUENCE */
    memcpy(der, av_rsa_pub_der, sizeof av_rsa_pub_der);
    der[4] = 0x04;
    CHECK_EQ(gip_rsa_parse_pub(der, sizeof av_rsa_pub_der, &k, NULL), 0);  /* not an INTEGER */
    memcpy(der, av_rsa_pub_der, sizeof av_rsa_pub_der);
    der[264] &= 0xFE;
    CHECK_EQ(gip_rsa_parse_pub(der, sizeof av_rsa_pub_der, &k, NULL), 0);  /* even modulus */
    memcpy(der, av_rsa_pub_der, sizeof av_rsa_pub_der);
    der[269] = 0x00;
    CHECK_EQ(gip_rsa_parse_pub(der, sizeof av_rsa_pub_der, &k, NULL), 0);  /* even exponent */
    CHECK_EQ(gip_rsa_parse_pub(av_rsa_pub_der, sizeof av_rsa_pub_der - 1, &k, NULL), 0);  /* cut */
    {
        size_t cut;
        for (cut = 0; cut < sizeof av_rsa_pub_der; cut++)
            CHECK_EQ(gip_rsa_parse_pub(av_rsa_pub_der, cut, &k, NULL), 0);
    }
    memcpy(der, av_rsa_pub_der, sizeof av_rsa_pub_der);       /* a 5-byte exponent */
    der[3] = 0x0C;
    der[265] = 0x02; der[266] = 0x05; der[267] = 0x01; der[268] = 0x00; der[269] = 0x00; der[270] = 0x00;
    der[271] = 0x01;
    CHECK_EQ(gip_rsa_parse_pub(der, 272, &k, NULL), 0);
    der[266] = 0x05; der[267] = 0x00; der[268] = 0x00; der[269] = 0x01; der[270] = 0x00; der[271] = 0x01;
    CHECK_EQ(gip_rsa_parse_pub(der, 272, &k, NULL), 1);       /* 5 bytes, but a leading zero: 65537 */
    CHECK_EQ(k.e, 65537);

    TEST("rsa PKCS#1 v1.5 encryption = what OpenSSL decrypts");
    CHECK_EQ(gip_rsa_parse_pub(av_rsa_pub_der, sizeof av_rsa_pub_der, &k, NULL), 1);
    CHECK_EQ(gip_rsa_encrypt_pkcs1(&k, av_pms, sizeof av_pms, av_pkcs1_ps, out), 1);
    CHECK_MEM(out, av_rsa_ct, 256);
    CHECK_EQ(gip_rsa_public(&k, av_rsa_raw_in, out), 1);
    CHECK_MEM(out, av_rsa_raw_out, 256);
    CHECK_EQ(gip_rsa_parse_pub(av_rsa3_pub_der, sizeof av_rsa3_pub_der, &k, NULL), 1);
    CHECK_EQ(gip_rsa_encrypt_pkcs1(&k, av_pms, sizeof av_pms, av_pkcs1_ps, out), 1);
    CHECK_MEM(out, av_rsa3_ct, 256);

    TEST("rsa refusals");
    CHECK_EQ(gip_rsa_parse_pub(av_rsa_pub_der, sizeof av_rsa_pub_der, &k, NULL), 1);
    memcpy(ps, av_pkcs1_ps, sizeof ps);
    ps[100] = 0;
    CHECK_EQ(gip_rsa_encrypt_pkcs1(&k, av_pms, sizeof av_pms, ps, out), 0);     /* a zero in PS */
    CHECK_EQ(gip_rsa_encrypt_pkcs1(&k, av_rsa_ct, 246, ps, out), 0);            /* message too long */
    CHECK_EQ(gip_rsa_public(&k, av_rsa_n, out), 0);                             /* input not below n */
    CHECK_EQ(gip_rsa_public(NULL, av_rsa_raw_in, out), 0);
}

static void test_p256(void)
{
    uint8_t pub[64], x[32], k[32], bad[64];
    int i;

    TEST("p256 small multiples of G, n - 1");
    memset(k, 0, sizeof k);
    k[31] = 1;
    CHECK_EQ(gip_p256_public(k, pub), 1);
    CHECK_MEM(pub, av_ec_g, 64);
    k[31] = 2;
    CHECK_EQ(gip_p256_public(k, pub), 1);
    CHECK_MEM(pub, av_ec_2g, 64);
    k[31] = 3;
    CHECK_EQ(gip_p256_public(k, pub), 1);
    CHECK_MEM(pub, av_ec_3g, 64);
    CHECK_EQ(gip_p256_public(av_ec_n_minus_1, pub), 1);
    CHECK_MEM(pub, av_ec_neg_g, 64);
    CHECK_EQ(gip_p256_pub_ok(av_ec_g), 1);
    CHECK_EQ(gip_p256_pub_ok(av_ec_neg_g), 1);

    TEST("p256 key pairs and the shared secret = OpenSSL's");
    CHECK_EQ(gip_p256_public(av_ec_host_priv, pub), 1);
    CHECK_MEM(pub, av_ec_host_pub, 64);
    CHECK_EQ(gip_p256_public(av_ec_client_priv, pub), 1);
    CHECK_MEM(pub, av_ec_client_pub, 64);
    CHECK_EQ(gip_p256_ecdh(av_ec_host_priv, av_ec_client_pub, x), 1);
    CHECK_MEM(x, av_ec_shared, 32);
    CHECK_EQ(gip_p256_ecdh(av_ec_client_priv, av_ec_host_pub, x), 1);
    CHECK_MEM(x, av_ec_shared, 32);

    TEST("p256 private keys out of range");
    memset(k, 0, sizeof k);
    CHECK_EQ(gip_p256_priv_ok(k), 0);
    CHECK_EQ(gip_p256_public(k, pub), 0);
    CHECK_EQ(gip_p256_priv_ok(av_ec_n), 0);
    CHECK_EQ(gip_p256_priv_ok(av_ec_n_minus_1), 1);
    memset(k, 0xFF, sizeof k);
    CHECK_EQ(gip_p256_priv_ok(k), 0);
    CHECK_EQ(gip_p256_ecdh(k, av_ec_client_pub, x), 0);

    TEST("p256 points that are not on the curve");
    memcpy(bad, av_ec_client_pub, 64);
    bad[63] ^= 0x01;
    CHECK_EQ(gip_p256_pub_ok(bad), 0);
    CHECK_EQ(gip_p256_ecdh(av_ec_host_priv, bad, x), 0);
    memset(bad, 0, sizeof bad);
    CHECK_EQ(gip_p256_pub_ok(bad), 0);                      /* (0, 0): infinity's usual stand-in */
    memcpy(bad, av_ec_p, 32);
    memcpy(bad + 32, av_ec_g + 32, 32);
    CHECK_EQ(gip_p256_pub_ok(bad), 0);                      /* x = p */
    memcpy(bad, av_ec_g, 32);
    memcpy(bad + 32, av_ec_p, 32);
    CHECK_EQ(gip_p256_pub_ok(bad), 0);                      /* y = p */
    memset(bad, 0xFF, sizeof bad);
    CHECK_EQ(gip_p256_pub_ok(bad), 0);
    for (i = 0; i < 64; i += 7) {                           /* any flipped bit */
        memcpy(bad, av_ec_host_pub, 64);
        bad[i] ^= (uint8_t)(1u << (i % 8));
        CHECK_EQ(gip_p256_pub_ok(bad), 0);
    }

    TEST("p256 cost");
    {
        clock_t t0 = clock();
        for (i = 0; i < 10; i++) {
            gip_p256_public(av_ec_host_priv, pub);
            gip_p256_ecdh(av_ec_host_priv, av_ec_client_pub, x);
        }
        if (getenv("XPAD_TEST_VERBOSE"))
            fprintf(stderr, "  p256: key pair + ECDH %.2f ms (this build)\n",
                    (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 10);
        CHECK_MEM(x, av_ec_shared, 32);
    }
}

int main(void)
{
    test_sha256();
    test_hmac();
    test_prf();
    test_rsa();
    test_p256();
    return test_done("crypto");
}
