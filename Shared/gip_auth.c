/*
 * GIP authentication, the host's side: the handshake of Linux's xone
 * (auth/auth.c), message for message. See gip_auth.h; the protocol notes
 * with line references are in docs/DESIGN.md ("GIP authentication").
 *
 * v1, RSA (xone always opens with it):
 *   host  Host Hello 01 (random_host)          pad  ack
 *   host  request 02                           pad  Client Hello 02 (random_client)
 *   host  request 03                           pad  Certificate 03 (an RSA-2048 key in it)
 *   host  Host Secret 05 (RSA(PMS))            pad  ack
 *   host  Host Finish 07                       pad  ack
 *   host  request 08                           pad  Client Finish 08: checked
 *   host  complete
 * v2, ECDH (the pad answers request 02 with a data command other than 02):
 *   the transcript starts over; Host Hello 21, ack, request 22, Client Hello
 *   22, request 23, Certificate 23, request 24, Client Public Key 24, Host
 *   Public Key 25, ack, Host Finish 26, ack, request 27, Client Finish 27.
 *
 * Master secret = PRF(PMS (v1) or SHA-256(ECDH X) (v2), "Master Secret",
 * random_host || random_client, 48). Host Finished = PRF(master, "Host
 * Finished", transcript before the Host Finish, 32); the pad's must equal
 * PRF(master, "Device Finished", transcript before the Client Finish, 32).
 * The transcript hashes every host handshake message from its data header
 * to the end of its payload (no trailer), and every pad message xone
 * accepts from byte 6 to its end; never requests, acks or "complete". The
 * pad's message is hashed before the host's answer to it is built.
 *
 * Differences from xone: the work xone defers to a work queue (RSA, ECDH,
 * "complete") is done at once (about a millisecond); failures are reported
 * so gip.c can fall back (xone just stops); a control message from the pad
 * (context 01: a RESET) starts the handshake over (at most twice); the
 * certificate's RSA key is found by parsing (the first DER RSAPublicKey
 * with a 2048-bit modulus) rather than by its "30 82 01 0a" prefix alone.
 *
 * Derived from xone, Copyright (C) 2023 Severin von Wnuck-Lipinski
 * <severinvonw@outlook.de>, GPL-2.0-or-later. This adaptation is
 * GPL-3.0-or-later: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. It is distributed WITHOUT ANY WARRANTY; see the GNU General Public
 * License (LICENSE at the top of this repository).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gip_auth.h"
#include "gip.h"                /* xpad_log, xpad_random */

#include <string.h>

#define HDR_LEN        6        /* handshake header */
#define FULL_HDR_LEN  10        /* handshake + data header */
#define TRAILER_LEN    8
#define RANDOM_LEN    32
#define PMS_LEN       48
#define FINISHED_LEN  32
#define CERT_MAX    1024        /* GIP_AUTH_CERTIFICATE_MAX_LEN */
#define RSA_BYTES    256        /* the Host Secret carries 256 bytes: RSA-2048 */
#define MAX_RESTARTS   2

/* Handshake commands (auth.c:18-33). */
#define HOST_HELLO       0x01
#define CLIENT_HELLO     0x02
#define CLIENT_CERT      0x03
#define HOST_SECRET      0x05
#define HOST_FINISH      0x07
#define CLIENT_FINISH    0x08
#define HOST_HELLO2      0x21
#define CLIENT_HELLO2    0x22
#define CLIENT_CERT2     0x23
#define CLIENT_PUBKEY2   0x24
#define HOST_PUBKEY2     0x25
#define HOST_FINISH2     0x26
#define CLIENT_FINISH2   0x27

/* What the host asks for: the payload the pad is to send (auth.c:103-160). */
#define CLIENT_HELLO_LEN    80      /* random 32 + 48 */
#define CLIENT_FINISH_LEN   64      /* finished 32 + 32 */
#define CLIENT_HELLO2_LEN  172      /* random 32 + 108 + 32 */
#define CLIENT_CERT2_LEN   768      /* header 4 + 136 + chip 32 + revision 20 + 576 */
#define CLIENT_PUBKEY2_LEN 128      /* X || Y 64 + 64 */

static const char ms_label[] = "Master Secret";
static const char hf_label[] = "Host Finished";
static const char df_label[] = "Device Finished";

const char *gip_auth_state_name(int state)
{
    static const char *const names[] = { "idle", "running", "done", "failed" };

    return state >= GIP_AUTH_IDLE && state <= GIP_AUTH_FAILED ? names[state] : "?";
}

static long since(const gip_auth_t *a, long now) { return now - a->t_open; }

void gip_auth_init(gip_auth_t *a, const char *name, long t_open)
{
    memset(a, 0, sizeof *a);
    a->name = name ? name : "?";
    a->t_open = t_open;
    a->state = GIP_AUTH_IDLE;
}

void gip_auth_end(gip_auth_t *a, int state, const char *why)
{
    a->state = state;
    a->why = why;
    gip_wipe(&a->transcript, sizeof a->transcript);
    gip_wipe(a->random_host, sizeof a->random_host);
    gip_wipe(a->random_client, sizeof a->random_client);
    gip_wipe(&a->rsa, sizeof a->rsa);
    gip_wipe(a->ecdh_priv, sizeof a->ecdh_priv);
    gip_wipe(a->peer_pub, sizeof a->peer_pub);
    gip_wipe(a->master, sizeof a->master);
}

static int fail(gip_auth_t *a, long now, const char *why)
{
    xpad_log("gip %s +%ld auth v%d failed: %s", a->name, since(a, now), a->version, why);
    gip_auth_end(a, GIP_AUTH_FAILED, why);
    return GIP_AUTH_FAIL;
}

/* A host handshake message (auth.c gip_auth_send_pkt): headers, body,
 * trailer; hashed from the data header to the body's end. */
static int build(gip_auth_t *a, uint8_t cmd, const uint8_t *body, int blen, uint8_t *out, int max)
{
    int total = FULL_HDR_LEN + blen + TRAILER_LEN;
    int dlen = 4 + blen;

    if (total > max) return 0;
    out[0] = 0x00;                              /* handshake */
    out[1] = 0x41;                              /* acknowledge | from host */
    out[2] = 0x00;
    out[3] = cmd;
    out[4] = (uint8_t)(dlen >> 8);
    out[5] = (uint8_t)dlen;
    out[6] = cmd;
    out[7] = cmd >= HOST_HELLO2 ? 0x02 : 0x01;
    out[8] = (uint8_t)(blen >> 8);
    out[9] = (uint8_t)blen;
    if (blen) memcpy(out + FULL_HDR_LEN, body, (size_t)blen);
    memset(out + FULL_HDR_LEN + blen, 0, TRAILER_LEN);
    a->last_sent = cmd;
    gip_sha256_update(&a->transcript, out + HDR_LEN, (size_t)dlen);
    return total;
}

/* A request (auth.c gip_auth_request_pkt): the handshake header alone,
 * its length the payload wanted plus the data header; not hashed. */
static int request(gip_auth_t *a, uint8_t cmd, int want, uint8_t *out, int max, int *olen, long now)
{
    int dlen = want + 4;

    if (max < HDR_LEN + TRAILER_LEN) return fail(a, now, "no room for a request");
    out[0] = 0x00;
    out[1] = 0x42;                              /* request | from host */
    out[2] = 0x00;
    out[3] = cmd;
    out[4] = (uint8_t)(dlen >> 8);
    out[5] = (uint8_t)dlen;
    memset(out + HDR_LEN, 0, TRAILER_LEN);
    *olen = HDR_LEN + TRAILER_LEN;
    return GIP_AUTH_SEND;
}

static int sent(gip_auth_t *a, long now, int len, int *olen, const char *what)
{
    if (!len) return fail(a, now, "no room for the host message");
    *olen = len;
    xpad_log("gip %s +%ld auth v%d: %s", a->name, since(a, now), a->version, what);
    return GIP_AUTH_SEND;
}

/* The Host Hello (v1: random, 4 + 4 unknown; v2: random, 4 unknown), on a
 * fresh transcript and a fresh random. */
static int hello(gip_auth_t *a, int version, uint8_t *out, int max)
{
    uint8_t body[RANDOM_LEN + 8];
    int blen = version == 1 ? RANDOM_LEN + 8 : RANDOM_LEN + 4;

    a->version = version;
    gip_sha256_init(&a->transcript);
    xpad_random(a->random_host, sizeof a->random_host);
    memset(body, 0, sizeof body);
    memcpy(body, a->random_host, RANDOM_LEN);
    return build(a, version == 1 ? HOST_HELLO : HOST_HELLO2, body, blen, out, max);
}

int gip_auth_start(gip_auth_t *a, long now, uint8_t *out, int max)
{
    int n;

    gip_auth_end(a, GIP_AUTH_IDLE, NULL);
    a->state = GIP_AUTH_RUNNING;
    a->restarts = 0;
    a->last_sent = 0;
    a->t_start = a->t_step = now;
    n = hello(a, 1, out, max);
    if (!n) gip_auth_end(a, GIP_AUTH_FAILED, "no room for the host hello");
    return n;
}

/* The transcript so far, without ending it. */
static void snapshot(const gip_auth_t *a, uint8_t out[GIP_SHA256_LEN])
{
    gip_sha256_t copy = a->transcript;

    gip_sha256_final(&copy, out);
}

static void seed(const gip_auth_t *a, uint8_t s[2 * RANDOM_LEN])
{
    memcpy(s, a->random_host, RANDOM_LEN);
    memcpy(s + RANDOM_LEN, a->random_client, RANDOM_LEN);
}

static int host_finish(gip_auth_t *a, uint8_t cmd, long now, uint8_t *out, int max, int *olen)
{
    uint8_t t[GIP_SHA256_LEN], f[FINISHED_LEN];
    int n;

    snapshot(a, t);
    gip_prf(a->master, sizeof a->master, hf_label, t, sizeof t, f, sizeof f);
    n = build(a, cmd, f, (int)sizeof f, out, max);
    gip_wipe(f, sizeof f);
    return sent(a, now, n, olen, "host finish");
}

/* The pad acknowledged the last host message (auth.c:335-357). */
static int acknowledged(gip_auth_t *a, long now, uint8_t *out, int max, int *olen)
{
    switch (a->last_sent) {
    case HOST_HELLO2:  return request(a, CLIENT_HELLO2, CLIENT_HELLO2_LEN, out, max, olen, now);
    case HOST_PUBKEY2: return host_finish(a, HOST_FINISH2, now, out, max, olen);
    case HOST_FINISH2: return request(a, CLIENT_FINISH2, CLIENT_FINISH_LEN, out, max, olen, now);
    case HOST_HELLO:   return request(a, CLIENT_HELLO, CLIENT_HELLO_LEN, out, max, olen, now);
    case HOST_SECRET:  return host_finish(a, HOST_FINISH, now, out, max, olen);
    case HOST_FINISH:  return request(a, CLIENT_FINISH, CLIENT_FINISH_LEN, out, max, olen, now);
    default:           return fail(a, now, "an acknowledgement out of turn");
    }
}

static void hash_msg(gip_auth_t *a, const uint8_t *msg, int len)
{
    gip_sha256_update(&a->transcript, msg + HDR_LEN, (size_t)(len - HDR_LEN));
}

/* v1: the certificate's RSA key encrypts a random 48-byte premaster secret
 * (no version bytes) with PKCS#1 v1.5 padding (auth.c:446-488). */
static int host_secret(gip_auth_t *a, long now, uint8_t *out, int max, int *olen)
{
    uint8_t pms[PMS_LEN], ps[RSA_BYTES - 3 - PMS_LEN], enc[RSA_BYTES], s[2 * RANDOM_LEN];
    size_t i;
    int n;

    xpad_random(pms, sizeof pms);
    for (i = 0; i < sizeof ps; i++) {           /* the padding: non-zero random bytes */
        int tries = 0;
        do xpad_random(&ps[i], 1);
        while (!ps[i] && ++tries < 64);
        if (!ps[i]) ps[i] = 0x01;
    }
    if (!gip_rsa_encrypt_pkcs1(&a->rsa, pms, sizeof pms, ps, enc)) {
        gip_wipe(pms, sizeof pms);
        return fail(a, now, "RSA encryption failed");
    }
    seed(a, s);
    gip_prf(pms, sizeof pms, ms_label, s, sizeof s, a->master, sizeof a->master);
    gip_wipe(pms, sizeof pms);
    gip_wipe(ps, sizeof ps);
    n = build(a, HOST_SECRET, enc, (int)sizeof enc, out, max);
    return sent(a, now, n, olen, "host secret (RSA-2048)");
}

/* v2: a fresh P-256 key pair, the shared X hashed, the master secret from it
 * (auth.c:257-294, crypto.c:200-244). */
static int host_pubkey(gip_auth_t *a, long now, uint8_t *out, int max, int *olen)
{
    uint8_t pub[2 * GIP_P256_LEN], x[GIP_P256_LEN], h[GIP_SHA256_LEN], s[2 * RANDOM_LEN];
    int tries = 0, n;

    do xpad_random(a->ecdh_priv, sizeof a->ecdh_priv);
    while (!gip_p256_priv_ok(a->ecdh_priv) && ++tries < 8);
    if (!gip_p256_public(a->ecdh_priv, pub)) return fail(a, now, "no P-256 key pair");
    if (!gip_p256_ecdh(a->ecdh_priv, a->peer_pub, x)) return fail(a, now, "the pad's public key is not on P-256");
    gip_sha256(x, sizeof x, h);
    seed(a, s);
    gip_prf(h, sizeof h, ms_label, s, sizeof s, a->master, sizeof a->master);
    gip_wipe(x, sizeof x);
    gip_wipe(h, sizeof h);
    gip_wipe(a->ecdh_priv, sizeof a->ecdh_priv);
    n = build(a, HOST_PUBKEY2, pub, (int)sizeof pub, out, max);
    return sent(a, now, n, olen, "host public key (P-256)");
}

/* Client Finish (08 or 27): the pad's Finished value against ours. */
static int client_finish(gip_auth_t *a, const uint8_t *p)
{
    uint8_t t[GIP_SHA256_LEN], f[FINISHED_LEN];
    int ok;

    snapshot(a, t);
    gip_prf(a->master, sizeof a->master, df_label, t, sizeof t, f, sizeof f);
    ok = memcmp(p, f, sizeof f) == 0;
    gip_wipe(f, sizeof f);
    return ok;
}

/* A data message (auth.c:541-589): checked, hashed, answered. */
static int data(gip_auth_t *a, const uint8_t *msg, int len, long now, uint8_t *out, int max, int *olen)
{
    const uint8_t *p = msg + FULL_HDR_LEN;
    int plen = len - FULL_HDR_LEN;
    size_t at = 0;

    switch (msg[6]) {
    case CLIENT_HELLO:
        if (plen < CLIENT_HELLO_LEN) return fail(a, now, "client hello too short");
        memcpy(a->random_client, p, RANDOM_LEN);
        hash_msg(a, msg, len);
        xpad_log("gip %s +%ld auth v1: client hello (%d bytes)", a->name, since(a, now), plen);
        return request(a, CLIENT_CERT, CERT_MAX, out, max, olen, now);
    case CLIENT_CERT:
        if (plen > CERT_MAX) return fail(a, now, "certificate over 1024 bytes");
        if (!gip_rsa_find_pub(p, (size_t)plen, RSA_BYTES, &a->rsa, &at))
            return fail(a, now, "no RSA-2048 key in the certificate");
        hash_msg(a, msg, len);
        xpad_log("gip %s +%ld auth v1: certificate (%d bytes), RSA-2048 key at %lu, e = %lu", a->name,
                 since(a, now), plen, (unsigned long)at, (unsigned long)a->rsa.e);
        return host_secret(a, now, out, max, olen);
    case CLIENT_HELLO2:
        if (plen < CLIENT_HELLO2_LEN) return fail(a, now, "client hello v2 too short");
        memcpy(a->random_client, p, RANDOM_LEN);
        hash_msg(a, msg, len);
        xpad_log("gip %s +%ld auth v2: client hello (%d bytes)", a->name, since(a, now), plen);
        return request(a, CLIENT_CERT2, CLIENT_CERT2_LEN, out, max, olen, now);
    case CLIENT_CERT2:
        if (plen < CLIENT_CERT2_LEN) return fail(a, now, "certificate v2 too short");
        hash_msg(a, msg, len);
        xpad_log("gip %s +%ld auth v2: certificate (%d bytes, header %02x %02x %02x %02x)", a->name,
                 since(a, now), plen, p[0], p[1], p[2], p[3]);
        return request(a, CLIENT_PUBKEY2, CLIENT_PUBKEY2_LEN, out, max, olen, now);
    case CLIENT_PUBKEY2:
        if (plen < CLIENT_PUBKEY2_LEN) return fail(a, now, "client public key too short");
        memcpy(a->peer_pub, p, sizeof a->peer_pub);
        hash_msg(a, msg, len);
        xpad_log("gip %s +%ld auth v2: client public key (%d bytes)", a->name, since(a, now), plen);
        return host_pubkey(a, now, out, max, olen);
    case CLIENT_FINISH:
    case CLIENT_FINISH2:
        if (plen < CLIENT_FINISH_LEN) return fail(a, now, "client finish too short");
        if (!client_finish(a, p)) return fail(a, now, "the pad's Finished value does not match");
        hash_msg(a, msg, len);
        xpad_log("gip %s +%ld auth v%d: client finish verified, authenticated in %ld ms", a->name,
                 since(a, now), a->version, now - a->t_start);
        gip_auth_end(a, GIP_AUTH_DONE, NULL);
        return GIP_AUTH_COMPLETE;
    default:
        return fail(a, now, "a message the host did not ask for");
    }
}

int gip_auth_rx(gip_auth_t *a, const uint8_t *msg, int len, long now, uint8_t *out, int max, int *olen)
{
    int dummy;

    if (!olen) olen = &dummy;
    *olen = 0;
    if (a->state != GIP_AUTH_RUNNING || !msg || !out) return GIP_AUTH_NOTHING;
    a->t_step = now;
    /* Control (context 01): COMPLETE 00 or RESET 01. xone would read it as
     * a handshake header; a RESET starts the handshake over here. */
    if (len >= 2 && msg[0] == 0x01) {
        if (msg[1] != 0x01) {
            xpad_log("gip %s +%ld auth: control %02x from the pad, ignored", a->name, since(a, now), msg[1]);
            return GIP_AUTH_NOTHING;
        }
        if (a->restarts >= MAX_RESTARTS) return fail(a, now, "reset by the pad, again");
        a->restarts++;
        xpad_log("gip %s +%ld auth: reset by the pad: starting over", a->name, since(a, now));
        return sent(a, now, hello(a, 1, out, max), olen, "host hello (again)");
    }
    if (len < HDR_LEN) return fail(a, now, "a message under 6 bytes");
    if (msg[2]) {
        xpad_log("gip %s +%ld auth: the pad reports error %02x (command %02x)", a->name, since(a, now), msg[2],
                 msg[3]);
        return fail(a, now, "the pad reports an error");
    }
    if (msg[1] & 0x01) {                        /* an acknowledgement: 01 is "go on" */
        if (msg[3] != 0x01) {
            xpad_log("gip %s +%ld auth: handshake refused by the pad: %02x", a->name, since(a, now), msg[3]);
            return fail(a, now, "handshake refused by the pad");
        }
        return acknowledged(a, now, out, max, olen);
    }
    if (len < FULL_HDR_LEN) return fail(a, now, "a message under 10 bytes");
    if (msg[3] != msg[6]) {                     /* the pad speaks v2 (auth.c:573-579) */
        if (a->version == 2) {
            if (a->restarts >= MAX_RESTARTS) return fail(a, now, "commands differ in a v2 handshake");
            a->restarts++;
        }
        xpad_log("gip %s +%ld auth: the pad answers %02x with %02x: v2 (ECDH), starting over", a->name,
                 since(a, now), msg[3], msg[6]);
        return sent(a, now, hello(a, 2, out, max), olen, "host hello v2");
    }
    return data(a, msg, len, now, out, max, olen);
}
