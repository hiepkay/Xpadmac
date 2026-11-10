/* GIP authentication, the host's side, as Linux's xone performs it
 * (auth/auth.c): a TLS-like handshake carried in command 0x06 system
 * frames, v1 (RSA-2048, PKCS#1 v1.5 key transport) or v2 (ECDH on P-256),
 * PRF = TLS 1.2's P_SHA256, one running SHA-256 transcript; once the pad's
 * Finished value checks out the host sends "complete" (06 20 SS 02 01 00).
 *
 * This file only builds and reads the 0x06 payloads (the "auth messages");
 * gip.c frames them, splits the long ones into chunks driven by the pad's
 * acknowledgements, reassembles the pad's chunked ones, and keeps the time.
 *
 *   gip_auth_start   a fresh handshake: the v1 Host Hello to send
 *   gip_auth_rx      one whole message from the pad: the next host message
 *                    to send, "complete", or a failure
 *   gip_auth_end     stop (secrets wiped)
 *
 * Message layout (big-endian lengths):
 *   handshake header  [0] context 00 (01 = control)  [1] options (b0 ack,
 *                     b1 request, 0x40 from host, 0xC0 from client)  [2] error
 *                     [3] command  [4..5] length of what follows (no trailer)
 *   data header       [6] command  [7] version (1, or 2 from 0x21 on)  [8..9] length
 *   payload, then (host messages) 8 zero bytes of trailer
 *
 * Derived from xone, Copyright (C) 2023 Severin von Wnuck-Lipinski,
 * GPL-2.0-or-later (auth/auth.c, auth/crypto.c); this adaptation is
 * GPL-3.0-or-later, as the rest of XPadMac.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XPAD_GIP_AUTH_H
#define XPAD_GIP_AUTH_H

#include "gip_crypto.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { GIP_AUTH_IDLE, GIP_AUTH_RUNNING, GIP_AUTH_DONE, GIP_AUTH_FAILED };

/* gip_auth_rx's result. */
enum {
    GIP_AUTH_NOTHING,           /* nothing to send (not authenticating) */
    GIP_AUTH_SEND,              /* out[0..*olen) is the next host message, sent asking for an ack */
    GIP_AUTH_COMPLETE,          /* the pad is authenticated: send "complete" (01 00, no ack) */
    GIP_AUTH_FAIL               /* given up; a->why says why */
};

#define GIP_AUTH_MSG_MAX 274    /* the longest host message (v1 Host Secret) */
#define GIP_AUTH_IN_MAX 1088    /* the longest pad message taken (a v1 certificate: 6 + 4 + 1024 + trailer) */

typedef struct {
    int state;
    int version;                /* 1 (RSA) or 2 (ECDH) */
    uint8_t last_sent;          /* the host's last handshake message (requests excluded) */
    int restarts;               /* handshakes started over within this one (v2 upgrade excluded) */
    long t_start, t_step;       /* when it started; when the pad was last heard from or sent to */
    const char *why;            /* why it failed */
    const char *name;           /* for the log */
    long t_open;
    gip_sha256_t transcript;
    uint8_t random_host[32], random_client[32];
    gip_rsa_pub_t rsa;
    uint8_t ecdh_priv[GIP_P256_LEN];
    uint8_t peer_pub[2 * GIP_P256_LEN];
    uint8_t master[48];
} gip_auth_t;

void gip_auth_init(gip_auth_t *a, const char *name, long t_open);

/* A new handshake (any old one dropped): the v1 Host Hello into out, its
 * length returned (58), or 0 if max is too small. */
int  gip_auth_start(gip_auth_t *a, long now, uint8_t *out, int max);

/* One whole auth message from the pad (a 0x06 payload, reassembled). */
int  gip_auth_rx(gip_auth_t *a, const uint8_t *msg, int len, long now, uint8_t *out, int max, int *olen);

/* Stops the handshake in `state` (IDLE, DONE or FAILED) and wipes its secrets. */
void gip_auth_end(gip_auth_t *a, int state, const char *why);

const char *gip_auth_state_name(int state);

#ifdef __cplusplus
}
#endif

#endif
