/*
 * GIP authentication end to end: Shared/gip.c with gip_auth.c and
 * gip_crypto.c against a simulated pad that answers xone's handshake with
 * the messages of auth_vectors.h (gen_auth_vectors.py: a model of xone's host,
 * its RSA and ECDH results checked with OpenSSL). Every 0x06 frame the host
 * sends must be the model's, byte for byte: the v1 (RSA) and v2 (ECDH)
 * handshakes, the chunked Host Secret and Host Public Key with their
 * acknowledgements, "complete" only after the pad's Finished value checks
 * out. Then what goes wrong: a wrong Finished value, silence, an error, a
 * refusal, a certificate without a key, a point off the curve, oversized or
 * misplaced chunks, the empty last chunk, a RESET, a bring-up or a Hello in
 * the middle, the bring-ups after a fallback, the opt-in and opt-out
 * switches (GIP_OPT_SERIAL too), the keepalive, and a fuzz run.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gip.h"
#include "test.h"
#include "auth_vectors.h"

#include <stdlib.h>

/* ---- the host's OUT endpoint, recorded ----------------------------------------- */

#define REC_MAX 512

typedef struct {
    uint8_t f[REC_MAX][64];
    int len[REC_MAX];
    int n;
} rec;

static int rec_send(void *ctx, const uint8_t *p, int len)
{
    rec *r = (rec *)ctx;

    CHECK(p != NULL);
    CHECK(len > 0 && len <= 64);                /* the driver's kFrameMax */
    if (r->n < REC_MAX && len > 0 && len <= 64) {
        memcpy(r->f[r->n], p, (size_t)len);
        r->len[r->n] = len;
        r->n++;
    }
    return 1;
}

/* ---- framing, as xone does it (bus/protocol.c) ------------------------------------ */

static int put_varint(uint8_t *b, long v)
{
    int n = 0;

    do {
        b[n] = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b[n] |= 0x80;
        n++;
    } while (v);
    return n;
}

static int put_header(uint8_t *b, uint8_t cmd, uint8_t opts, uint8_t seq, long plen, long off)
{
    uint8_t v[8];
    int n = 3;

    b[0] = cmd; b[1] = opts; b[2] = seq;
    n += put_varint(b + n, plen);
    if (((opts & 0x80) && off ? n + put_varint(v, off) : n) % 2) {
        b[n - 1] |= 0x80;
        b[n++] = 0;
    }
    if (opts & 0x80) n += put_varint(b + n, off);
    return n;
}

static int get_varint(const uint8_t *b, int len, int *pos, long *v)
{
    int shift = 0;

    *v = 0;
    while (*pos < len && shift <= 21) {
        uint8_t c = b[(*pos)++];
        *v |= (long)(c & 0x7F) << shift;
        if (!(c & 0x80)) return 1;
        shift += 7;
    }
    return 0;
}

/* ---- the simulated pad ---------------------------------------------------------------- */

typedef struct {
    gip_t g;
    xpad_state st;
    rec r;
    long now;
    int next;                   /* the first host frame not yet looked at */
    uint8_t seq;
    int version;                /* which answers: 1 or 2 */
    /* a host message coming in in chunks */
    uint8_t in[2048];
    long in_total;
    /* behaviour */
    int bad_finish, error_on, refuse, no_key, bad_point, ask_ack, silent_after, reset_once;
    int answered;               /* requests answered */
    int completes;              /* "complete" frames seen */
    /* what the host sent */
    uint8_t wire[64][64];
    int wire_len[64];
    int nwire;
    long host_acks[64];         /* the host's acknowledgements of 0x06 chunks: bytes it says it has */
    int nacks;
} pad;

static void pad_rx(pad *P, const uint8_t *b, int n)
{
    uint8_t *copy = (uint8_t *)malloc((size_t)n);       /* exact size: ASan sees overreads */

    memcpy(copy, b, (size_t)n);
    P->now += 3;
    gip_receive(&P->g, copy, n, P->now, &P->st, rec_send, &P->r);
    free(copy);
}

static uint8_t pad_seq(pad *P)
{
    if (++P->seq == 0) ++P->seq;
    return P->seq;
}

/* One auth message from the pad: a frame, or chunks of 58 (first asks for an
 * ack, the last with data does not: the host acknowledges it anyway, as
 * xone does), then the empty one. */
static void pad_send(pad *P, const uint8_t *m, int len)
{
    uint8_t f[80];
    uint8_t seq = pad_seq(P);
    int h, off;

    if (len <= 58) {
        h = put_header(f, 0x06, (uint8_t)(P->ask_ack ? 0x30 : 0x20), seq, len, 0);
        memcpy(f + h, m, (size_t)len);
        pad_rx(P, f, h + len);
        return;
    }
    h = put_header(f, 0x06, 0xF0, seq, 58, len);
    memcpy(f + h, m, 58);
    pad_rx(P, f, h + 58);
    for (off = 58; off < len; off += 58) {
        int n = len - off < 58 ? len - off : 58;
        h = put_header(f, 0x06, 0xA0, seq, n, off);
        memcpy(f + h, m + off, (size_t)n);
        pad_rx(P, f, h + n);
    }
    h = put_header(f, 0x06, 0xA0, seq, 0, len);
    pad_rx(P, f, h);
}

static void pad_auth_ack(pad *P, uint8_t cmd)
{
    const uint8_t a[6] = { 0x00, 0xC1, 0x00, cmd, 0x00, 0x00 };

    pad_send(P, a, 6);
}

/* The answer to a request for `cmd`. */
static void pad_answer(pad *P, uint8_t cmd)
{
    uint8_t m[1100];
    const uint8_t *src = NULL;
    int n = 0;

    P->answered++;
    if (P->silent_after && P->answered > P->silent_after) return;
#define USE(v) (src = (v), n = (int)sizeof(v))
    switch (cmd) {
    case 0x02: if (P->version == 2) USE(av_v2_upgrade); else USE(av_v1_client_hello); break;
    case 0x03: USE(av_v1_client_cert); break;
    case 0x08: USE(av_v1_client_finish); break;
    case 0x22: USE(av_v2_client_hello); break;
    case 0x23: USE(av_v2_client_cert); break;
    case 0x24: USE(av_v2_client_pubkey); break;
    case 0x27: USE(av_v2_client_finish); break;
    default: CHECK(!"a request for a message the pad does not have"); return;
    }
#undef USE
    memcpy(m, src, (size_t)n);
    if ((cmd == 0x08 || cmd == 0x27) && P->bad_finish) m[10 + 5] ^= 0x40;
    if (cmd == P->error_on) m[2] = 0x07;
    if (cmd == 0x03 && P->no_key) {
        int i;
        for (i = 10; i + 4 <= n; i++)
            if (m[i] == 0x30 && m[i + 1] == 0x82 && m[i + 2] == 0x01 && m[i + 3] == 0x0a) m[i + 4] = 0x05;
    }
    if (cmd == 0x24 && P->bad_point) m[10 + 63] ^= 0x01;
    pad_send(P, m, n);
}

/* A whole host message: acknowledge it, answer a request, note "complete". */
static void pad_message(pad *P, const uint8_t *m, int n)
{
    if (n == 2 && m[0] == 0x01 && m[1] == 0x00) {
        P->completes++;
        return;
    }
    CHECK(n >= 14);
    if (n < 14) return;
    CHECK_EQ(m[0], 0x00);
    if (m[1] == 0x41) {                         /* a handshake message: OK */
        if (P->reset_once) {
            static const uint8_t reset[2] = { 0x01, 0x01 };
            P->reset_once = 0;
            pad_send(P, reset, 2);
            return;
        }
        if (P->silent_after && P->answered >= P->silent_after) return;
        pad_auth_ack(P, P->refuse && m[3] == 0x01 ? 0x02 : 0x01);
    } else if (m[1] == 0x42) {                  /* a request */
        pad_answer(P, m[3]);
    } else {
        CHECK(!"host message with unknown options");
    }
}

/* Every host frame since the last call, in order (answers may add more). */
static void pad_run(pad *P)
{
    while (P->next < P->r.n) {
        const uint8_t *f = P->r.f[P->next];
        int len = P->r.len[P->next], pos = 3;
        long plen, second = 0;
        uint8_t opts;

        P->next++;
        if (f[0] == 0x01 && len == 13 && f[5] == 0x06) {        /* the host acknowledges our chunk */
            if (P->nacks < 64) P->host_acks[P->nacks++] = (long)f[7] | (long)f[8] << 8;
            continue;
        }
        if (f[0] != 0x06) continue;
        opts = f[1];
        CHECK(get_varint(f, len, &pos, &plen));
        if (opts & 0x80) CHECK(get_varint(f, len, &pos, &second));
        CHECK_EQ(pos + plen, len);
        if (P->nwire < 64) {
            memcpy(P->wire[P->nwire], f, (size_t)len);
            P->wire_len[P->nwire++] = len;
        }
        if (opts & 0x10) {                      /* our GIP ack: 01 20 SS 09 00 06 20 got.. 00 00 left.. */
            long got = (opts & 0x80) ? ((opts & 0x40) ? plen : second + plen) : plen;
            long total = (opts & 0x80) ? ((opts & 0x40) ? second : P->in_total) : plen;
            long left = total - got;
            const uint8_t a[13] = { 0x01, 0x20, f[2], 0x09, 0x00, 0x06, 0x20, (uint8_t)got, (uint8_t)(got >> 8),
                                    0x00, 0x00, (uint8_t)left, (uint8_t)(left >> 8) };
            if (opts & 0x40) P->in_total = second;
            if (opts & 0x80 && plen) memcpy(P->in + ((opts & 0x40) ? 0 : second), f + pos, (size_t)plen);
            pad_rx(P, a, 13);
            if (!(opts & 0x80)) pad_message(P, f + pos, (int)plen);
            continue;
        }
        if (!(opts & 0x80)) {
            pad_message(P, f + pos, (int)plen);
        } else if (plen) {
            if (opts & 0x40) P->in_total = second;
            memcpy(P->in + ((opts & 0x40) ? 0 : second), f + pos, (size_t)plen);
        } else {
            pad_message(P, P->in, (int)P->in_total);
        }
    }
}

/* Hello, metadata, bring-up (whose Host Hello the pad then answers). */
static void pad_start(pad *P, int version, const uint8_t *random, size_t nrandom, int opts, uint16_t vid)
{
    memset(P, 0, sizeof *P);
    P->version = version;
    test_random_seed(1);
    if (random) test_random_feed(random, nrandom);
    gip_init(&P->g, "dock", vid, 0x1004, opts, 0);
    P->g.reads = 1;
    xpad_state_reset(&P->st);
    P->now = 100;
    {
        const uint8_t hello[] = { 0x02, 0x20, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00 };
        const uint8_t meta[] = { 0x04, 0x20, 0x02, 0x02, 0x00, 0x00 };
        pad_rx(P, hello, (int)sizeof hello);
        pad_rx(P, meta, (int)sizeof meta);
    }
}

static int frames_equal(const pad *P, const av_bytes *want, int count)
{
    int i, ok = P->nwire == count;

    CHECK_EQ(P->nwire, count);
    for (i = 0; i < count && i < P->nwire; i++) {
        CHECK_EQ(P->wire_len[i], want[i].n);
        if (P->wire_len[i] == want[i].n) CHECK_MEM(P->wire[i], want[i].p, (size_t)want[i].n);
        ok = ok && P->wire_len[i] == want[i].n && !memcmp(P->wire[i], want[i].p, (size_t)want[i].n);
    }
    return ok;
}

static int all_zero(const uint8_t *p, size_t n)
{
    while (n--)
        if (*p++) return 0;
    return 1;
}

static pad P;                   /* big: not on the stack */

/* ---- the handshakes ----------------------------------------------------------------------- */

static void test_v1(void)
{
    long t;
    int i;

    TEST("auth v1 (RSA): every host frame as xone sends it");
    test_log_clear();
    pad_start(&P, 1, av_v1_random, sizeof av_v1_random, 0, 0x3537);
    /* The bring-up: START, LED, Host Hello in place of "security complete", rumble stop. */
    CHECK_EQ(P.r.n, 5);
    CHECK_EQ(P.r.f[1][0], 0x05);
    CHECK_EQ(P.r.f[2][0], 0x0a);
    CHECK_EQ(P.r.len[3], 62);
    CHECK_MEM(P.r.f[3], av_v1_wire_0, 62);
    CHECK_EQ(P.r.f[4][0], 0x09);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);
    pad_run(&P);
    frames_equal(&P, av_v1_wire, AV_V1_WIRE_COUNT);
    CHECK_EQ(P.completes, 1);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_DONE);
    CHECK_EQ(P.g.auth_done, 1);
    CHECK_EQ(P.g.auth_fallbacks, 0);
    CHECK_EQ(test_random_drawn(), 32 + 48 + 205);
    CHECK_EQ(test_random_left(), 0);
    CHECK(test_log_find(0, "auth v1: certificate (710 bytes), RSA-2048 key at 112, e = 65537") >= 0);
    CHECK(test_log_find(0, "auth v1: client finish verified") >= 0);
    CHECK(all_zero(P.g.auth.master, sizeof P.g.auth.master));       /* secrets wiped */
    CHECK(all_zero(P.g.auth.random_host, sizeof P.g.auth.random_host));

    TEST("auth v1: the host acknowledges the pad's chunks as xone does");
    /* Client hello (90 bytes: 58 + 32), certificate (720: 13 chunks) and
     * client finish (74: 58 + 16): the first chunk (asked) and the last with
     * data (unasked), never the empty one. */
    CHECK_EQ(P.nacks, 6);
    CHECK_EQ(P.host_acks[0], 58);
    CHECK_EQ(P.host_acks[1], 90);
    CHECK_EQ(P.host_acks[2], 58);
    CHECK_EQ(P.host_acks[3], (long)sizeof av_v1_client_cert);
    CHECK_EQ(P.host_acks[4], 58);
    CHECK_EQ(P.host_acks[5], (long)sizeof av_v1_client_finish);

    TEST("auth v1: quiet once done");
    {
        const uint8_t status[] = { 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00 };
        pad_rx(&P, status, (int)sizeof status);
        CHECK_EQ(P.g.state, GIP_ACTIVE);
    }
    for (t = P.now; t < P.now + 30000; t += 20) gip_tick(&P.g, t, rec_send, &P.r);
    i = P.r.n;
    pad_run(&P);
    CHECK_EQ(P.r.n, i);
    CHECK_EQ(P.nwire, AV_V1_WIRE_COUNT);
}

static void test_v2(void)
{
    TEST("auth v2 (ECDH): every host frame as xone sends it");
    test_log_clear();
    pad_start(&P, 2, av_v2_random, sizeof av_v2_random, 0, 0x3537);
    pad_run(&P);
    frames_equal(&P, av_v2_wire, AV_V2_WIRE_COUNT);
    CHECK_EQ(P.completes, 1);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_DONE);
    CHECK_EQ(P.g.auth.version, 2);
    CHECK_EQ(test_random_drawn(), 96);
    CHECK(test_log_find(0, "the pad answers 02 with 22: v2 (ECDH), starting over") >= 0);
    CHECK(test_log_find(0, "auth v2: client finish verified") >= 0);
    CHECK(all_zero(P.g.auth.ecdh_priv, sizeof P.g.auth.ecdh_priv));
}

static void test_ps_redraw(void)
{
    uint8_t rnd[32 + 48 + 205 + 3];

    TEST("auth v1: zero bytes drawn for the padding are drawn again");
    memcpy(rnd, av_v1_random, 80);
    rnd[80] = 0x00;                             /* zeros among the padding's bytes: skipped */
    memcpy(rnd + 81, av_v1_random + 80, 100);
    rnd[181] = 0x00;
    rnd[182] = 0x00;
    memcpy(rnd + 183, av_v1_random + 180, 105);
    pad_start(&P, 1, rnd, sizeof rnd, 0, 0x3537);
    pad_run(&P);
    frames_equal(&P, av_v1_wire, AV_V1_WIRE_COUNT);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_DONE);
    CHECK_EQ(test_random_left(), 0);
}

/* ---- what goes wrong ----------------------------------------------------------------------- */

/* The last host frame: the plain "complete" (06 20 SS 02 01 00). */
static int ends_with_complete(const pad *P)
{
    const uint8_t *f = P->r.f[P->r.n - 1];

    return P->r.n > 0 && P->r.len[P->r.n - 1] == 6 && f[0] == 0x06 && f[1] == 0x20 && f[3] == 0x02 &&
           f[4] == 0x01 && f[5] == 0x00;
}

static void test_what_fails(void)
{
    long t;
    int n;

    TEST("auth a wrong Finished value: no real complete, the plain one instead");
    test_log_clear();
    pad_start(&P, 1, av_v1_random, sizeof av_v1_random, 0, 0x3537);
    P.bad_finish = 1;
    pad_run(&P);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_FAILED);
    CHECK_EQ(P.g.auth_done, 0);
    CHECK_EQ(P.g.auth_fallbacks, 1);
    CHECK(ends_with_complete(&P));
    CHECK(test_log_find(0, "the pad's Finished value does not match") >= 0);
    CHECK(test_log_find(0, "auth given up (the pad's Finished value does not match): security complete") >= 0);

    TEST("auth v2: a wrong Finished value");
    pad_start(&P, 2, av_v2_random, sizeof av_v2_random, 0, 0x3537);
    P.bad_finish = 1;
    pad_run(&P);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_FAILED);
    CHECK(ends_with_complete(&P));

    TEST("auth silence: the plain complete 2 s after the last word");
    test_log_clear();
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    {
        const uint8_t status[] = { 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00 };
        pad_rx(&P, status, (int)sizeof status);     /* up: no bring-up again to muddle it */
    }
    P.silent_after = 1;                         /* hello acked, client hello sent, then nothing */
    pad_run(&P);
    n = P.r.n;
    CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);
    for (t = P.now; t < P.now + 1999; t += 10) gip_tick(&P.g, t, rec_send, &P.r);
    CHECK_EQ(P.r.n, n);
    gip_tick(&P.g, P.now + 2000, rec_send, &P.r);
    CHECK_EQ(P.r.n, n + 1);
    CHECK(ends_with_complete(&P));
    CHECK_EQ(P.g.auth.state, GIP_AUTH_FAILED);
    CHECK(test_log_find(0, "auth v1: no answer in 2000 ms") >= 0);

    TEST("auth a pad that never answers the Host Hello");
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    CHECK_EQ(P.r.n, 5);
    {
        const uint8_t status[] = { 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00 };
        pad_rx(&P, status, (int)sizeof status);     /* the pad comes up all the same */
    }
    CHECK_EQ(P.g.state, GIP_ACTIVE);
    for (t = 120; t < 2110; t += 20) gip_tick(&P.g, t, rec_send, &P.r);
    CHECK_EQ(P.r.n, 5);
    gip_tick(&P.g, 2110, rec_send, &P.r);
    CHECK_EQ(P.r.n, 6);
    CHECK(ends_with_complete(&P));
    CHECK_EQ(P.r.f[5][2], 0x02);                /* seq_sec: the Host Hello had 01 */

    TEST("auth the pad reports an error");
    test_log_clear();
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    P.error_on = 0x03;
    pad_run(&P);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_FAILED);
    CHECK(ends_with_complete(&P));
    CHECK(test_log_find(0, "the pad reports error 07 (command 03)") >= 0);

    TEST("auth the pad refuses the handshake");
    test_log_clear();
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    P.refuse = 1;
    pad_run(&P);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_FAILED);
    CHECK(ends_with_complete(&P));
    CHECK(test_log_find(0, "handshake refused by the pad: 02") >= 0);

    TEST("auth a certificate without a key");
    test_log_clear();
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    P.no_key = 1;
    pad_run(&P);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_FAILED);
    CHECK(test_log_find(0, "no RSA-2048 key in the certificate") >= 0);

    TEST("auth v2: a public key off the curve");
    test_log_clear();
    pad_start(&P, 2, NULL, 0, 0, 0x3537);
    P.bad_point = 1;
    pad_run(&P);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_FAILED);
    CHECK(ends_with_complete(&P));
    CHECK(test_log_find(0, "the pad's public key is not on P-256") >= 0);

    TEST("auth the pad asks for acks on its frames");
    pad_start(&P, 1, av_v1_random, sizeof av_v1_random, 0, 0x3537);
    P.ask_ack = 1;
    pad_run(&P);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_DONE);
    frames_equal(&P, av_v1_wire, AV_V1_WIRE_COUNT);
}

static void test_chunk_limits(void)
{
    uint8_t f[80];
    int h;

    TEST("auth a pad message over the limit is dropped");
    test_log_clear();
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    h = put_header(f, 0x06, 0xF0, 0x40, 58, GIP_AUTH_IN_MAX + 1);
    memset(f + h, 0x30, 58);
    pad_rx(&P, f, h + 58);
    CHECK(test_log_find(0, "a 1089-byte message from the pad, over 1088: dropped") >= 0);
    h = put_header(f, 0x06, 0xA0, 0x40, 58, 58);
    pad_rx(&P, f, h + 58);
    h = put_header(f, 0x06, 0xA0, 0x40, 0, GIP_AUTH_IN_MAX + 1);
    pad_rx(&P, f, h);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);     /* nothing handed over */

    TEST("auth a chunk past the message's end is dropped");
    h = put_header(f, 0x06, 0xF0, 0x41, 58, 100);
    memset(f + h, 0x11, 58);
    pad_rx(&P, f, h + 58);
    h = put_header(f, 0x06, 0xA0, 0x41, 58, 58);     /* 58 + 58 > 100 */
    pad_rx(&P, f, h + 58);
    CHECK(test_log_find(0, "chunk at 58 (58 bytes) past the message's 100: dropped") >= 0);
    h = put_header(f, 0x06, 0xA0, 0x41, 0, 100);
    pad_rx(&P, f, h);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);
    h = put_header(f, 0x06, 0xA0, 0x42, 0, 100);     /* an empty chunk with nothing begun */
    pad_rx(&P, f, h);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);

    TEST("auth the empty last chunk is never acknowledged, even asked (xone)");
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    {
        int n0 = P.r.n, i, acks = 0;
        h = put_header(f, 0x06, 0xF0, 0x43, 58, 100);     /* asked: acked */
        memset(f + h, 0x22, 58);
        pad_rx(&P, f, h + 58);
        h = put_header(f, 0x06, 0xA0, 0x43, 42, 58);      /* the last with data, unasked: acked */
        memset(f + h, 0x22, 42);
        pad_rx(&P, f, h + 42);
        h = put_header(f, 0x06, 0xB0, 0x43, 0, 100);      /* empty, asked: not acked */
        pad_rx(&P, f, h);
        for (i = n0; i < P.r.n; i++) acks += P.r.f[i][0] == 0x01 && P.r.len[i] == 13 && P.r.f[i][5] == 0x06;
        CHECK_EQ(acks, 2);
        CHECK_EQ(P.r.f[n0][0], 0x01);
        CHECK_EQ(P.r.f[n0][7], 58);
        CHECK_EQ(P.r.f[n0 + 1][0], 0x01);
        CHECK_EQ(P.r.f[n0 + 1][7], 100);
    }
}

static void test_restarts(void)
{
    long t;
    int i, hellos;

    TEST("auth a RESET from the pad starts over");
    {
        uint8_t rnd[32 + sizeof av_v1_random];
        memset(rnd, 0x77, 32);                  /* the first Host Hello's, then the vectors' */
        memcpy(rnd + 32, av_v1_random, sizeof av_v1_random);
        test_log_clear();
        pad_start(&P, 1, rnd, sizeof rnd, 0, 0x3537);
    }
    P.reset_once = 1;
    pad_run(&P);
    CHECK(test_log_find(0, "auth: reset by the pad: starting over") >= 0);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_DONE);    /* the second time through, on a fresh transcript */
    CHECK_EQ(P.completes, 1);
    CHECK_EQ(P.nwire, 1 + AV_V1_WIRE_COUNT);
    for (i = 1; i < P.nwire && i <= AV_V1_WIRE_COUNT; i++) {      /* the vectors' frames, one seq on */
        uint8_t want[64];
        int n = av_v1_wire[i - 1].n;
        memcpy(want, av_v1_wire[i - 1].p, (size_t)n);
        want[2]++;
        CHECK_EQ(P.wire_len[i], n);
        if (P.wire_len[i] == n) CHECK_MEM(P.wire[i], want, (size_t)n);
    }

    TEST("auth a Hello again in the middle: the handshake is left alone (xone)");
    test_log_clear();
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    CHECK_EQ(P.r.n, 5);
    P.now = 700;
    {
        const uint8_t hello[] = { 0x02, 0x20, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00 };
        pad_rx(&P, hello, (int)sizeof hello);
    }
    CHECK_EQ(P.r.n, 5);                         /* no bring-up, no new Host Hello */
    CHECK_EQ(P.g.bursts, 1);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);
    CHECK(test_log_find(0, "hello again while the auth handshake moves on: left alone") >= 0);

    TEST("auth a Hello again after the fallback: a bring-up with the plain complete");
    for (t = 120; t <= 2120; t += 20) gip_tick(&P.g, t, rec_send, &P.r);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_FAILED);
    CHECK_EQ(P.r.n, 6);
    CHECK(ends_with_complete(&P));
    P.now = 2700;
    {
        const uint8_t hello[] = { 0x02, 0x20, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00 };
        pad_rx(&P, hello, (int)sizeof hello);
    }
    CHECK_EQ(P.r.n, 10);
    CHECK_EQ(P.g.bursts, 2);
    CHECK_EQ(P.r.len[8], 6);
    CHECK_MEM(P.r.f[8], "\x06\x20\x03\x02\x01\x00", 6);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_FAILED);

    TEST("auth no answer, no status: one complete, a whole T_UP, then plain bring-ups");
    test_log_clear();
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    P.next = P.r.n;                             /* the pad never answers the Host Hello */
    CHECK_EQ(P.r.n, 5);
    for (t = 120; t < 2120; t += 20) gip_tick(&P.g, t, rec_send, &P.r);
    CHECK_EQ(P.r.n, 5);
    gip_tick(&P.g, 2120, rec_send, &P.r);
    CHECK_EQ(P.r.n, 6);                         /* the complete alone: no bring-up in the same tick */
    CHECK(ends_with_complete(&P));
    CHECK_EQ(P.g.bursts, 1);
    for (t = 2140; t < 4120; t += 20) gip_tick(&P.g, t, rec_send, &P.r);
    CHECK_EQ(P.r.n, 6);                         /* T_UP from the complete */
    gip_tick(&P.g, 4120, rec_send, &P.r);
    CHECK_EQ(P.r.n, 10);
    CHECK_EQ(P.g.bursts, 2);
    CHECK_EQ(P.r.f[6][0], 0x05);
    CHECK_EQ(P.r.len[8], 6);                    /* the plain complete, not a new Host Hello */
    CHECK_MEM(P.r.f[8], "\x06\x20\x03\x02\x01\x00", 6);
    for (t = 4140; t <= 9000; t += 20) gip_tick(&P.g, t, rec_send, &P.r);
    CHECK_EQ(P.g.bursts, 3);
    CHECK_EQ(P.g.state, GIP_SILENT);
    for (hellos = 0, i = 0; i < P.r.n; i++) hellos += P.r.f[i][0] == 0x06 && P.r.len[i] == 62;
    CHECK_EQ(hellos, 1);
    CHECK_EQ(P.g.auth_fallbacks, 1);
    CHECK(test_log_find(0, "bring-up 2 (no status or input in 2 s)") >= 0);
    {                                           /* a Hello in SILENT: identified again, the handshake again */
        const uint8_t hello[] = { 0x02, 0x20, 0x07, 0x04, 0x00, 0x00, 0x00, 0x00 };
        const uint8_t meta[] = { 0x04, 0x20, 0x08, 0x02, 0x00, 0x00 };
        int n0 = P.r.n;
        P.now = 9100;
        pad_rx(&P, hello, (int)sizeof hello);
        pad_rx(&P, meta, (int)sizeof meta);
        CHECK_EQ(P.r.n, n0 + 5);
        CHECK_EQ(P.r.f[n0 + 3][0], 0x06);
        CHECK_EQ(P.r.len[n0 + 3], 62);
        CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);
    }

    TEST("auth no bring-up again while the handshake moves on");
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    P.next = P.r.n;                             /* the pad stays quiet... */
    for (t = 120; t <= 4000; t += 20) {
        if (t % 1000 == 0) {                    /* ...but for an ack every second */
            const uint8_t a[13] = { 0x01, 0x20, 0x01, 0x09, 0x00, 0x06, 0x20, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00 };
            P.now = t;
            pad_rx(&P, a, 13);
        }
        gip_tick(&P.g, t, rec_send, &P.r);
    }
    CHECK_EQ(P.g.bursts, 1);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);
    for (hellos = 0, i = 0; i < P.r.n; i++) hellos += P.r.f[i][0] == 0x06;
    CHECK_EQ(hellos, 1);

    TEST("auth a re-announce while active stops the handshake");
    test_log_clear();
    pad_start(&P, 1, NULL, 0, 0, 0x3537);
    {
        const uint8_t status[] = { 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00 };
        const uint8_t hello[] = { 0x02, 0x20, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00 };
        pad_rx(&P, status, (int)sizeof status);
        CHECK_EQ(P.g.state, GIP_ACTIVE);
        pad_rx(&P, hello, (int)sizeof hello);
    }
    CHECK_EQ(P.g.auth.state, GIP_AUTH_IDLE);
    P.next = P.r.n;
    pad_auth_ack(&P, 0x01);                     /* a word for the old handshake */
    CHECK(test_log_find(0, "auth message (6 bytes) while idle: ignored") >= 0);
    gip_tick(&P.g, P.now + 100, rec_send, &P.r);    /* the new bring-up, a new handshake */
    CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);
}

static void test_switches(void)
{
    long t;
    int i, sec;

    TEST("auth GIP_OPT_FAKE_AUTH: the plain complete for a VID 0x3537 pad");
    pad_start(&P, 1, NULL, 0, GIP_OPT_FAKE_AUTH, 0x3537);
    CHECK_EQ(P.r.n, 5);
    CHECK_EQ(P.r.len[3], 6);
    CHECK_MEM(P.r.f[3], "\x06\x20\x01\x02\x01\x00", 6);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_IDLE);
    CHECK_EQ(test_random_drawn(), 0);

    TEST("auth GIP_OPT_AUTH: any vendor");
    pad_start(&P, 1, av_v1_random, sizeof av_v1_random, GIP_OPT_AUTH, 0x045E);
    CHECK_EQ(P.r.len[3], 62);
    pad_run(&P);
    frames_equal(&P, av_v1_wire, AV_V1_WIRE_COUNT);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_DONE);

    TEST("auth GIP_OPT_SERIAL: xone's serial-number request after the Host Hello");
    pad_start(&P, 1, av_v1_random, sizeof av_v1_random, GIP_OPT_SERIAL, 0x3537);
    CHECK_EQ(P.r.n, 6);
    CHECK_EQ(P.r.len[3], 62);
    CHECK_EQ(P.r.len[4], 5);
    CHECK_MEM(P.r.f[4], "\x1e\x20\x04\x01\x04", 5);
    CHECK_EQ(P.r.f[5][0], 0x09);
    pad_run(&P);
    frames_equal(&P, av_v1_wire, AV_V1_WIRE_COUNT);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_DONE);

    TEST("auth other vendors: the plain complete");
    pad_start(&P, 1, NULL, 0, 0, 0x045E);
    CHECK_EQ(P.r.len[3], 6);
    CHECK_EQ(P.g.auth.state, GIP_AUTH_IDLE);

    TEST("auth keepalive: no security frame in the middle of a handshake");
    pad_start(&P, 1, NULL, 0, GIP_OPT_KEEPALIVE, 0x3537);
    {
        const uint8_t status[] = { 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00 };
        pad_rx(&P, status, (int)sizeof status);
    }
    for (t = 120; t <= 20000; t += 20) {
        if (t % 1000 == 0) {                    /* the handshake kept alive by acks */
            const uint8_t a[13] = { 0x01, 0x20, 0x01, 0x09, 0x00, 0x06, 0x20, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00 };
            P.now = t;
            pad_rx(&P, a, 13);
        }
        gip_tick(&P.g, t, rec_send, &P.r);
    }
    CHECK_EQ(P.g.auth.state, GIP_AUTH_RUNNING);
    for (sec = 0, i = 5; i < P.r.n; i++) sec += P.r.f[i][0] == 0x06;
    CHECK_EQ(sec, 0);
    CHECK_EQ(P.r.f[P.r.n - 1][0], 0x0a);        /* the keepalive: START and LED only */
    CHECK_EQ(P.r.f[P.r.n - 2][0], 0x05);
}

/* ---- fuzz ------------------------------------------------------------------------------------------ */

static uint32_t rs = 0xC0FFEE11u;

static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static void test_fuzz(void)
{
    int it, i;

    TEST("auth fuzz: mangled pad messages and frames, 3000 runs");
    for (it = 0; it < 3000; it++) {
        uint8_t m[1100];
        int n;
        const av_bytes *src;
        static const av_bytes msgs[] = {
            { av_v1_client_hello, (int)sizeof av_v1_client_hello },
            { av_v1_client_cert, (int)sizeof av_v1_client_cert },
            { av_v1_client_finish, (int)sizeof av_v1_client_finish },
            { av_v2_upgrade, (int)sizeof av_v2_upgrade },
            { av_v2_client_hello, (int)sizeof av_v2_client_hello },
            { av_v2_client_cert, (int)sizeof av_v2_client_cert },
            { av_v2_client_pubkey, (int)sizeof av_v2_client_pubkey },
            { av_v2_client_finish, (int)sizeof av_v2_client_finish },
        };

        if (it % 50 == 0) pad_start(&P, 1, NULL, 0, (int)(rnd() % 32), 0x3537);
        src = &msgs[rnd() % (sizeof msgs / sizeof msgs[0])];
        n = (int)(rnd() % 3 == 0 ? rnd() % 1100 : (uint32_t)src->n);
        if (n > src->n) {
            for (i = 0; i < n; i++) m[i] = (uint8_t)rnd();
        } else {
            memcpy(m, src->p, (size_t)n);
        }
        for (i = (int)(rnd() % 4); i > 0 && n > 0; i--) m[rnd() % (uint32_t)n] = (uint8_t)rnd();
        if (n >= 4 && rnd() % 4 == 0) m[1] = (uint8_t)(rnd() & 0x03);       /* acks, requests */
        if (rnd() % 5 == 0) {                   /* a raw frame, maybe a chunk */
            uint8_t f[80];
            int h, k = (int)(rnd() % 58);
            uint8_t opts = (uint8_t)(0x20 | (rnd() & 0xD0));
            h = put_header(f, 0x06, opts, (uint8_t)rnd(), k, (long)(rnd() % 1500));
            for (i = 0; i < k; i++) f[h + i] = (uint8_t)rnd();
            pad_rx(&P, f, h + k);
        } else if (n > 0) {
            pad_send(&P, m, n);
        }
        if (rnd() % 3 == 0) gip_tick(&P.g, P.now += (long)(rnd() % 1500), rec_send, &P.r);
        if (rnd() % 2) pad_run(&P);
        if (P.r.n > REC_MAX - 64) P.r.n = P.next = 0;
        CHECK(P.g.auth.state >= GIP_AUTH_IDLE && P.g.auth.state <= GIP_AUTH_FAILED);
        CHECK(P.g.auth_out_len >= 0 && P.g.auth_out_len <= GIP_AUTH_MSG_MAX);
        CHECK(P.g.auth_in_total >= 0 && P.g.auth_in_total <= GIP_AUTH_IN_MAX);
        if (test_failures) break;
    }
}

int main(void)
{
    test_v1();
    test_v2();
    test_ps_redraw();
    test_what_fails();
    test_chunk_limits();
    test_restarts();
    test_switches();
    test_fuzz();
    return test_done("auth");
}
