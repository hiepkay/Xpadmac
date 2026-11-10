/*
 * The GIP host (Shared/gip.c): the pad's frames from a real capture of a
 * VID 0x3537 dock (docs/research/gip_bringup_capture.log; its device ID
 * replaced by 00 11 22 33 44 55) replayed frame by frame,
 * the identification step and the bring-up, input decoding, the Guide
 * button, acknowledgements, chunks, modules, the vendor quirks and timers,
 * rumble frames, and a fuzz run.
 *
 *   test_gip RESEARCH_DIR      (docs/research, for the capture)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gip.h"
#include "test.h"

#include <stdlib.h>

/* ---- a pad's OUT endpoint, recorded ---------------------------------------- */

#define REC_MAX 64

typedef struct {
    uint8_t f[REC_MAX][64];
    int len[REC_MAX];
    int n;                  /* frames sent (may exceed REC_MAX; only the first are kept) */
    int fail;               /* make send() report failure */
} rec;

static int rec_send(void *ctx, const uint8_t *p, int len)
{
    rec *r = (rec *)ctx;

    CHECK(p != NULL);
    CHECK(len > 0 && len <= 64);
    if (r->n < REC_MAX && len > 0 && len <= 64) {
        memcpy(r->f[r->n], p, (size_t)len);
        r->len[r->n] = len;
    }
    r->n++;
    return !r->fail;
}

static void rec_clear(rec *r)
{
    memset(r, 0, sizeof *r);
}

#define EXPECT_FRAME(r, i, ...)                                                \
    do {                                                                       \
        static const uint8_t want_[] = { __VA_ARGS__ };                        \
        CHECK((r)->n > (i));                                                   \
        if ((r)->n > (i)) {                                                    \
            CHECK_EQ((r)->len[i], (int)sizeof want_);                          \
            if ((r)->len[i] == (int)sizeof want_)                              \
                CHECK_MEM((r)->f[i], want_, sizeof want_);                     \
        }                                                                      \
    } while (0)

/* The handshake's v1 Host Hello (gip_auth.c), where "security complete"
 * was: 06 30 SS 3a | 00 41 00 01 00 2c 01 01 00 28 | random[32] | 0 x 16. */
#define EXPECT_HOST_HELLO(r, i, seq)                                           \
    do {                                                                       \
        static const uint8_t head_[14] = { 0x06, 0x30, (seq), 0x3a, 0x00, 0x41, 0x00, 0x01, \
                                           0x00, 0x2c, 0x01, 0x01, 0x00, 0x28 }; \
        static const uint8_t zero_[16] = { 0 };                                \
        CHECK((r)->n > (i));                                                   \
        if ((r)->n > (i)) {                                                    \
            CHECK_EQ((r)->len[i], 62);                                         \
            CHECK_MEM((r)->f[i], head_, 14);                                   \
            CHECK_MEM((r)->f[i] + 46, zero_, 16);                              \
        }                                                                      \
    } while (0)

#define RX(g, now, st, r, ...)                                                 \
    rx_bytes((g), (now), (st), (r), (const uint8_t[]){ __VA_ARGS__ },          \
             (int)sizeof((const uint8_t[]){ __VA_ARGS__ }))

/* Through a buffer of exactly the transfer's size, so a read past the end
 * is caught by the address sanitizer. */
static int rx_bytes(gip_t *g, long now, xpad_state *st, rec *r, const uint8_t *b, int n)
{
    uint8_t *copy = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    int ret;

    memcpy(copy, b, (size_t)(n > 0 ? n : 0));
    ret = gip_receive(g, copy, n, now, st, rec_send, r);
    free(copy);
    return ret;
}

/* An input frame 20 00 SS n with payload p[0..n). */
static int rx_input(gip_t *g, long now, xpad_state *st, rec *r, uint8_t seq, const uint8_t *p, int n)
{
    uint8_t f[4 + 64];

    f[0] = 0x20; f[1] = 0x00; f[2] = seq; f[3] = (uint8_t)n;
    memcpy(f + 4, p, (size_t)n);
    return rx_bytes(g, now, st, r, f, 4 + n);
}

/* A pad already brought up and active: Hello, identification, bring-up,
 * status. Uses sequence numbers seq_sys 1..3 (identify, START, LED), seq_sec
 * 1 (security, or a VID 0x3537 pad's Host Hello) and seq_vend 1. */
static void bring_up(gip_t *g, xpad_state *st, rec *r, uint16_t vid, uint16_t pid, int opts)
{
    gip_init(g, "pad", vid, pid, opts, 0);
    g->reads = 1;
    xpad_state_reset(st);
    rec_clear(r);
    RX(g, 100, st, r, 0x02, 0x20, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00);     /* a (short) Hello: identify */
    RX(g, 110, st, r, 0x04, 0x20, 0x01, 0x02, 0x00, 0x00);                 /* its (short) metadata: bring-up */
    RX(g, 120, st, r, 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00);     /* status */
    CHECK_EQ(g->state, GIP_ACTIVE);
    CHECK_EQ(r->n, 5);
    rec_clear(r);
}

/* ---- the capture, replayed ------------------------------------------------- */

static int parse_hex(const char *s, uint8_t *out, int max)
{
    int n = 0;
    unsigned v;
    int used;

    while (n < max && sscanf(s, " %2x%n", &v, &used) == 1) {
        out[n++] = (uint8_t)v;
        s += used;
    }
    return n;
}

/* The probe that made the capture (docs/research/probe4_userspace_gip.c) ran
 * AnyPad's gip.c, which answered the Hello with its bring-up at once; the
 * dock left 1.35 s after going ACTIVE. The pad's frames are replayed here on
 * the probe's timeline (gip_tick every 10 ms, until the dock left) against
 * the gip.c of today, which asks the pad to identify itself first. They
 * still decode exactly as AnyPad logged them; the frames sent are the
 * identification request, then, 100 ms later (no reply in the capture), the
 * captured bring-up, its 05/0A sequence numbers one further on, and the
 * authentication handshake's Host Hello where its "security complete" was
 * (the dock left before the 2 s that would have given the handshake up). */
static void test_capture_replay(const char *research)
{
    static const char *const pad_lines[] = { "hello 1:", "status 87", "input 1:" };
    char path[1024], line[512];
    char expect[64][256];           /* the probe's log lines about the pad's frames, in order */
    uint8_t want_tx[16][64];
    int want_tx_len[16];
    int nexpect = 0, ntx = 0, nrx = 0, i;
    long t_open = -1, t_gone = -1, last = 0;
    unsigned long from;
    gip_t g;
    xpad_state st;
    rec r;
    FILE *f;

    TEST("gip capture replay");
    snprintf(path, sizeof path, "%s/gip_bringup_capture.log", research);
    f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f) return;

    rec_clear(&r);
    test_log_clear();
    while (fgets(line, sizeof line, f)) {
        long t;
        int n, pos = 0;
        const char *msg = strchr(line, ']');

        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, " [%ld] claim%n", &t, &pos) == 1 && pos) {
            /* The probe called gip_init right after claiming the interface. */
            t_open = t;
            gip_init(&g, "dock", 0x3537, 0x1004, 0, t_open);
            xpad_state_reset(&st);
            g.reads = 1;
            last = t_open;
            continue;
        }
        pos = 0;
        if (sscanf(line, " [%ld] rx (%d):%n", &t, &n, &pos) == 2 && pos) {
            uint8_t buf[64];
            int got = parse_hex(line + pos, buf, (int)sizeof buf);

            CHECK(t_open >= 0);
            if (t_open < 0) break;
            CHECK_EQ(got, n);
            for (; last + 10 < t; last += 10) gip_tick(&g, last, rec_send, &r);    /* the probe's loop */
            rx_bytes(&g, t, &st, &r, buf, got);
            nrx++;
            continue;
        }
        pos = 0;
        if (sscanf(line, " [%ld] rx status %d%n", &t, &n, &pos) == 2 && pos) {
            t_gone = t;             /* the transfer error the dock left with */
            continue;
        }
        if (msg && strncmp(msg, "] gip ", 6) == 0) {
            const char *tx = strstr(msg, " tx ");
            if (tx && ntx < 16) {          /* "tx start: 05 20 01 01 00 -> ok" */
                const char *colon = strchr(tx, ':');
                if (colon) {
                    want_tx_len[ntx] = parse_hex(colon + 1, want_tx[ntx], 64);
                    ntx++;
                }
            }
            for (i = 0; i < (int)(sizeof pad_lines / sizeof pad_lines[0]); i++) {
                if (strstr(msg, pad_lines[i]) && nexpect < 64)
                    snprintf(expect[nexpect++], sizeof expect[0], "%s", msg + 2);
            }
        }
    }
    fclose(f);
    CHECK(t_gone > t_open && t_open >= 0);
    if (t_gone <= t_open || t_open < 0) return;
    for (; last + 10 < t_gone; last += 10) gip_tick(&g, last, rec_send, &r);

    /* The capture: Hello, status, input; AnyPad's bring-up, START, LED,
     * security and rumble stop, as the first frames it sent. */
    CHECK_EQ(nrx, 3);
    CHECK_EQ(ntx, 4);
    CHECK_EQ(nexpect, 3);
    /* Now: the request, then that bring-up (started at +870, 100 ms after the
     * Hello), and nothing more before the dock left (a retry would be 2 s on). */
    CHECK_EQ(r.n, 1 + ntx);
    EXPECT_FRAME(&r, 0, 0x04, 0x20, 0x01, 0x00);
    for (i = 0; i < ntx && 1 + i < r.n; i++) {
        uint8_t want[64];
        memcpy(want, want_tx[i], (size_t)want_tx_len[i]);
        if (want[0] == 0x05 || want[0] == 0x0A) want[2]++;  /* seq_sys: the request took 01 */
        if (want[0] == 0x06) continue;                      /* now the Host Hello: below */
        CHECK_EQ(r.len[1 + i], want_tx_len[i]);
        if (r.len[1 + i] == want_tx_len[i]) CHECK_MEM(r.f[1 + i], want, (size_t)want_tx_len[i]);
    }
    EXPECT_FRAME(&r, 1, 0x05, 0x20, 0x02, 0x01, 0x00);
    EXPECT_FRAME(&r, 2, 0x0a, 0x20, 0x03, 0x03, 0x00, 0x01, 0x14);
    CHECK_EQ(want_tx[2][0], 0x06);                      /* AnyPad's third: 06 20 01 02 01 00 */
    EXPECT_HOST_HELLO(&r, 3, 0x01);
    EXPECT_FRAME(&r, 4, 0x09, 0x00, 0x01, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xeb);
    CHECK_EQ(g.auth.state, GIP_AUTH_RUNNING);

    /* The pad's frames read as AnyPad read them, in order, around the new steps. */
    from = 0;
    for (i = 0; i < nexpect; i++) {
        long at = test_log_find(from, expect[i]);
        if (at < 0) {
            test_fail(__FILE__, __LINE__, expect[i]);
            break;
        }
        from = (unsigned long)at + 1;
    }
    CHECK(test_log_find(0, "gip dock +770 tx identify: 04 20 01 00 -> ok") >= 0);
    CHECK(test_log_find(0, "gip dock +770 wait-hello -> wait-up (hello)") >= 0);
    CHECK(test_log_find(0, "gip dock +870 bring-up 1 (no identification reply in 100 ms)") >= 0);
    CHECK(test_log_find(0, "gip dock +870 tx start: 05 20 02 01 00 -> ok") >= 0);

    /* Status and input came while it was identifying: taken in, but the pad
     * goes ACTIVE only after the bring-up, and that came after them. */
    CHECK_EQ(g.state, GIP_WAIT_UP);
    CHECK(!gip_up(&g));
    CHECK_EQ(g.bursts, 1);
    CHECK_EQ(g.hellos, 1);
    CHECK_EQ(g.statuses, 1);
    CHECK_EQ(g.inputs, 1);
    CHECK_EQ(g.frames, 3);
    CHECK_EQ(g.status, 0x87);
    CHECK_EQ(g.has_share, 1);       /* its 32-byte input is the Series layout */
    /* The pad at rest: exactly the idle state, sticks centred to the bit. */
    CHECK_EQ(st.buttons, 0);
    CHECK_EQ(st.lx, 0x8000); CHECK_EQ(st.ly, 0x8000); CHECK_EQ(st.rx, 0x8000); CHECK_EQ(st.ry, 0x8000);
    CHECK_EQ(st.lt, 0); CHECK_EQ(st.rt, 0);

    gip_lost(&g, t_gone);
    CHECK(test_log_find(0, "gip dock +2140 lost: wait-up, 1 hello, 1 status, 1 input, 1 bring-up(s), last frame +801") >= 0);
}

/* ---- bring-up ---------------------------------------------------------------- */

static void test_hello_starts_bring_up(void)
{
    /* The captured Hello (docs/DESIGN.md), device ID replaced. */
    static const uint8_t hello[32] = { 0x02, 0x20, 0x02, 0x1c, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00,
                                       0x00, 0x37, 0x35, 0x04, 0x10, 0x01, 0x00, 0x02, 0x00, 0x49, 0x02,
                                       0x02, 0x00, 0x02, 0x0e, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00 };
    gip_t g;
    xpad_state st;
    rec r;
    long t;

    TEST("gip waits for the hello (VID 0x3537 quirk)");
    gip_init(&g, "dock", 0x3537, 0x1004, 0, 0);
    xpad_state_reset(&st);
    rec_clear(&r);
    test_log_clear();
    CHECK_EQ(g.state, GIP_WAIT_HELLO);
    gip_tick(&g, 20000, rec_send, &r);          /* no reads queued yet: never a write */
    CHECK_EQ(r.n, 0);
    g.reads = 1;
    for (t = 0; t < 10000; t += 20) gip_tick(&g, t, rec_send, &r);
    CHECK_EQ(r.n, 0);                           /* a VID 0x3537 pad is never brought up blind */
    CHECK_EQ(g.hint, 0);
    gip_tick(&g, 10000, rec_send, &r);
    CHECK_EQ(g.hint, 1);
    CHECK(test_log_find(0, "no hello yet: press the controller's Xbox button") >= 0);
    CHECK_EQ(r.n, 0);
    CHECK_EQ(g.state, GIP_WAIT_HELLO);

    TEST("gip hello -> identify");
    CHECK_EQ(rx_bytes(&g, 10010, &st, &r, hello, (int)sizeof hello), 0);
    CHECK_EQ(r.n, 1);
    EXPECT_FRAME(&r, 0, 0x04, 0x20, 0x01, 0x00);
    CHECK_EQ(g.state, GIP_WAIT_UP);
    CHECK(!gip_up(&g));
    CHECK_EQ(g.hellos, 1);
    CHECK_EQ(g.bursts, 0);
    CHECK(test_log_find(0, "hello 1: 3537:1004 firmware 1.2.585.2 hardware 2.14") >= 0);
    CHECK(test_log_find(0, "tx identify: 04 20 01 00 -> ok") >= 0);

    TEST("gip no identification reply -> bring-up after 100 ms");
    for (t = 10010; t < 10110; t += 11) gip_tick(&g, t, rec_send, &r);
    CHECK_EQ(r.n, 1);
    gip_tick(&g, 10110, rec_send, &r);
    CHECK_EQ(r.n, 5);
    EXPECT_FRAME(&r, 1, 0x05, 0x20, 0x02, 0x01, 0x00);
    EXPECT_FRAME(&r, 2, 0x0a, 0x20, 0x03, 0x03, 0x00, 0x01, 0x14);
    EXPECT_HOST_HELLO(&r, 3, 0x01);             /* VID 0x3537: authenticated (gip_auth.c) */
    EXPECT_FRAME(&r, 4, 0x09, 0x00, 0x01, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xeb);
    CHECK_EQ(g.state, GIP_WAIT_UP);
    CHECK(!gip_up(&g));
    CHECK_EQ(g.bursts, 1);
    CHECK(test_log_find(0, "bring-up 1 (no identification reply in 100 ms)") >= 0);

    TEST("gip status -> active");
    rec_clear(&r);
    CHECK_EQ(RX(&g, 10130, &st, &r, 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00), 0);
    CHECK_EQ(g.state, GIP_ACTIVE);
    CHECK(gip_up(&g));
    CHECK_EQ(g.status, 0x87);
    CHECK_EQ(r.n, 0);                           /* no acknowledgement asked */
    /* Once up, the timers stay quiet without GIP_OPT_KEEPALIVE, but for the
     * handshake given up: no answer to the Host Hello in 2 s, so the plain
     * "security complete", once. */
    test_log_clear();
    for (t = 10130; t < 12110; t += 20) gip_tick(&g, t, rec_send, &r);
    CHECK_EQ(r.n, 0);
    for (; t < 60000; t += 20) gip_tick(&g, t, rec_send, &r);
    CHECK_EQ(r.n, 1);
    EXPECT_FRAME(&r, 0, 0x06, 0x20, 0x02, 0x02, 0x01, 0x00);
    CHECK(test_log_find(0, "gip dock +12110 auth given up (no answer): security complete as without it") >= 0);
    CHECK_EQ(g.auth.state, GIP_AUTH_FAILED);
    CHECK_EQ(g.state, GIP_ACTIVE);

    TEST("gip hello again while waiting");
    gip_init(&g, "dock", 0x3537, 0x1004, GIP_OPT_FAKE_AUTH, 0);   /* the handshake holds it back: test_auth */
    g.reads = 1;
    rec_clear(&r);
    rx_bytes(&g, 1000, &st, &r, hello, (int)sizeof hello);
    CHECK_EQ(r.n, 1);                                        /* identify */
    gip_tick(&g, 1100, rec_send, &r);
    CHECK_EQ(r.n, 5);                                        /* the first bring-up */
    rx_bytes(&g, 1599, &st, &r, hello, (int)sizeof hello);   /* within 500 ms of it: not again */
    CHECK_EQ(r.n, 5);
    rx_bytes(&g, 1600, &st, &r, hello, (int)sizeof hello);
    CHECK_EQ(r.n, 9);                                        /* a second, without asking again */
    EXPECT_FRAME(&r, 5, 0x05, 0x20, 0x04, 0x01, 0x00);       /* the next sequence numbers */
    EXPECT_FRAME(&r, 6, 0x0a, 0x20, 0x05, 0x03, 0x00, 0x01, 0x14);
    EXPECT_FRAME(&r, 7, 0x06, 0x20, 0x02, 0x02, 0x01, 0x00);
    EXPECT_FRAME(&r, 8, 0x09, 0x00, 0x02, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xeb);
    rx_bytes(&g, 2100, &st, &r, hello, (int)sizeof hello);
    CHECK_EQ(r.n, 13);                                       /* the third */
    rx_bytes(&g, 2600, &st, &r, hello, (int)sizeof hello);
    CHECK_EQ(r.n, 13);                                       /* no fourth */
    CHECK_EQ(g.bursts, 3);
}

/* The identification request and what ends it. */
static void test_identify(void)
{
    static const uint8_t hello[] = { 0x02, 0x20, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00 };
    gip_t g;
    xpad_state st;
    rec r;

    TEST("gip chunked identification reply -> bring-up at its end");
    gip_init(&g, "dock", 0x3537, 0x1004, 0, 0);
    g.reads = 1;
    xpad_state_reset(&st);
    rec_clear(&r);
    test_log_clear();
    rx_bytes(&g, 1000, &st, &r, hello, (int)sizeof hello);
    CHECK_EQ(r.n, 1);
    EXPECT_FRAME(&r, 0, 0x04, 0x20, 0x01, 0x00);
    /* First chunk: total 186, 3 bytes, ack asked. */
    RX(&g, 1090, &st, &r, 0x04, 0xF0, 0x05, 0x03, 0xBA, 0x01, 0xaa, 0xbb, 0xcc);
    CHECK_EQ(r.n, 2);
    EXPECT_FRAME(&r, 1, 0x01, 0x20, 0x05, 0x09, 0x00, 0x04, 0x20, 0x03, 0x00, 0x00, 0x00, 0xB7, 0x00);
    gip_tick(&g, 1150, rec_send, &r);           /* 150 ms after the request, 60 after the reply's start */
    CHECK_EQ(r.n, 2);
    RX(&g, 1180, &st, &r, 0x04, 0xA0, 0x05, 0x03, 0x03, 0xdd, 0xee, 0xff);  /* a middle chunk */
    gip_tick(&g, 1279, rec_send, &r);
    CHECK_EQ(r.n, 2);
    CHECK_EQ(g.state, GIP_WAIT_UP);
    CHECK_EQ(g.bursts, 0);
    /* The empty last chunk: acknowledged, then the bring-up at once. */
    RX(&g, 1280, &st, &r, 0x04, 0xA0, 0x05, 0x00, 0xBA, 0x01);
    CHECK_EQ(r.n, 7);
    EXPECT_FRAME(&r, 2, 0x01, 0x20, 0x05, 0x09, 0x00, 0x04, 0x20, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00);
    EXPECT_FRAME(&r, 3, 0x05, 0x20, 0x02, 0x01, 0x00);
    EXPECT_FRAME(&r, 6, 0x09, 0x00, 0x01, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xeb);
    CHECK_EQ(g.bursts, 1);
    CHECK(test_log_find(0, "bring-up 1 (identified)") >= 0);
    gip_tick(&g, 1400, rec_send, &r);           /* the timeout no longer applies */
    CHECK_EQ(r.n, 7);
    RX(&g, 1410, &st, &r, 0x04, 0xA0, 0x06, 0x00, 0xBA, 0x01);   /* metadata now changes nothing */
    CHECK_EQ(r.n, 8);                                           /* (its ack only) */
    CHECK_EQ(g.bursts, 1);
    RX(&g, 1420, &st, &r, 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00);
    CHECK_EQ(g.state, GIP_ACTIVE);

    TEST("gip whole identification reply -> bring-up");
    gip_init(&g, "pad", 0x045E, 0x02DD, 0, 0);
    g.reads = 1;
    rec_clear(&r);
    rx_bytes(&g, 1000, &st, &r, hello, (int)sizeof hello);
    CHECK_EQ(r.n, 1);
    RX(&g, 1005, &st, &r, 0x04, 0x20, 0x01, 0x02, 0x01, 0x02);
    CHECK_EQ(r.n, 5);
    EXPECT_FRAME(&r, 1, 0x05, 0x20, 0x02, 0x01, 0x00);
    CHECK_EQ(g.bursts, 1);

    TEST("gip identifying: status, Guide and input skip nothing");
    gip_init(&g, "dock", 0x3537, 0x1004, 0, 0);
    g.reads = 1;
    xpad_state_reset(&st);
    rec_clear(&r);
    rx_bytes(&g, 1000, &st, &r, hello, (int)sizeof hello);
    CHECK_EQ(RX(&g, 1010, &st, &r, 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00), 0);
    CHECK_EQ(g.statuses, 1);
    CHECK_EQ(g.status, 0x87);
    CHECK_EQ(RX(&g, 1020, &st, &r, 0x07, 0x20, 0x01, 0x02, 0x01, 0x5b), GIP_RX_STATE | GIP_RX_INPUT);
    {
        const uint8_t p[14] = { 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        CHECK_EQ(rx_input(&g, 1030, &st, &r, 1, p, 14), GIP_RX_STATE | GIP_RX_INPUT);
    }
    CHECK_EQ(st.buttons, XPAD_BTN_A | XPAD_BTN_GUIDE);         /* published all the same */
    CHECK_EQ(g.state, GIP_WAIT_UP);
    CHECK_EQ(g.bursts, 0);
    CHECK_EQ(r.n, 1);
    RX(&g, 1040, &st, &r, 0x04, 0x00, 0x02, 0x02, 0x01, 0x02);   /* not a system frame: not the reply */
    RX(&g, 1050, &st, &r, 0x04, 0x21, 0x03, 0x02, 0x01, 0x02);   /* a module's: not the pad's */
    CHECK_EQ(r.n, 1);
    gip_tick(&g, 1100, rec_send, &r);
    CHECK_EQ(r.n, 5);                                           /* the bring-up, START first */
    EXPECT_FRAME(&r, 1, 0x05, 0x20, 0x02, 0x01, 0x00);
    CHECK_EQ(g.state, GIP_WAIT_UP);
    {
        const uint8_t p[14] = { 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        rx_input(&g, 1110, &st, &r, 2, p, 14);
    }
    CHECK_EQ(g.state, GIP_ACTIVE);
    CHECK_EQ(st.buttons, XPAD_BTN_B | XPAD_BTN_GUIDE);

    TEST("gip a reply that does not end: bring-up 500 ms after the request");
    gip_init(&g, "dock", 0x3537, 0x1004, 0, 0);
    g.reads = 1;
    rec_clear(&r);
    test_log_clear();
    rx_bytes(&g, 1000, &st, &r, hello, (int)sizeof hello);
    RX(&g, 1090, &st, &r, 0x04, 0xE0, 0x05, 0x01, 0xBA, 0x01, 0xaa);   /* a first chunk of 186, then */
    RX(&g, 1180, &st, &r, 0x04, 0xA0, 0x05, 0x01, 0x01, 0xbb);         /* one byte every 90 ms */
    gip_tick(&g, 1260, rec_send, &r);
    RX(&g, 1270, &st, &r, 0x04, 0xA0, 0x05, 0x01, 0x02, 0xcc);
    RX(&g, 1360, &st, &r, 0x04, 0xA0, 0x05, 0x01, 0x03, 0xdd);
    gip_tick(&g, 1440, rec_send, &r);
    RX(&g, 1450, &st, &r, 0x04, 0xA0, 0x05, 0x01, 0x04, 0xee);
    gip_tick(&g, 1499, rec_send, &r);
    CHECK_EQ(r.n, 1);                                           /* no ack asked, none sent */
    CHECK_EQ(g.bursts, 0);
    gip_tick(&g, 1500, rec_send, &r);
    CHECK_EQ(r.n, 5);
    EXPECT_FRAME(&r, 1, 0x05, 0x20, 0x02, 0x01, 0x00);
    CHECK_EQ(g.bursts, 1);
    CHECK(test_log_find(0, "bring-up 1 (identification reply not over in 500 ms)") >= 0);

    TEST("gip blind bring-ups do not identify");
    gip_init(&g, "pdp", 0x0E6F, 0x02A8, 0, 0);
    g.reads = 1;
    rec_clear(&r);
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 4);
    EXPECT_FRAME(&r, 0, 0x05, 0x20, 0x01, 0x01, 0x00);
    CHECK_EQ(g.bursts, 1);
}

static void test_retries_and_silence(void)
{
    gip_t g;
    xpad_state st;
    rec r;

    TEST("gip blind bring-up after 1 s");
    gip_init(&g, "hori", 0x0F0D, 0x0063, 0, 0);
    g.reads = 1;
    xpad_state_reset(&st);
    rec_clear(&r);
    gip_tick(&g, 999, rec_send, &r);
    CHECK_EQ(r.n, 0);
    gip_tick(&g, 1000, rec_send, &r);
    CHECK_EQ(r.n, 4);
    EXPECT_FRAME(&r, 0, 0x05, 0x20, 0x01, 0x01, 0x00);
    CHECK_EQ(g.state, GIP_WAIT_UP);

    TEST("gip retries every 2 s, 3 times");
    gip_tick(&g, 2999, rec_send, &r);
    CHECK_EQ(r.n, 4);
    gip_tick(&g, 3000, rec_send, &r);
    CHECK_EQ(r.n, 8);
    gip_tick(&g, 5000, rec_send, &r);
    CHECK_EQ(r.n, 12);
    CHECK_EQ(g.bursts, 3);
    gip_tick(&g, 7000, rec_send, &r);
    CHECK_EQ(r.n, 12);
    CHECK_EQ(g.state, GIP_SILENT);
    gip_tick(&g, 60000, rec_send, &r);
    CHECK_EQ(r.n, 12);                          /* silent: still read, never written */

    TEST("gip hello wakes a silent pad");
    RX(&g, 61000, &st, &r, 0x02, 0x20, 0x09, 0x00);
    CHECK_EQ(r.n, 13);                          /* identify, */
    EXPECT_FRAME(&r, 12, 0x04, 0x20, 0x07, 0x00);       /* after 3 x (START, LED) */
    CHECK_EQ(g.bursts, 0);
    CHECK_EQ(g.state, GIP_WAIT_UP);
    gip_tick(&g, 61100, rec_send, &r);
    CHECK_EQ(r.n, 17);                          /* then a fresh round of bring-ups */
    CHECK_EQ(g.bursts, 1);
    CHECK_EQ(g.state, GIP_WAIT_UP);

    TEST("gip input also brings it up");
    rec_clear(&r);
    {
        const uint8_t p[14] = { 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        CHECK_EQ(rx_input(&g, 61110, &st, &r, 1, p, 14), GIP_RX_STATE | GIP_RX_INPUT);
    }
    CHECK_EQ(g.state, GIP_ACTIVE);
    CHECK_EQ(st.buttons, XPAD_BTN_A);

    TEST("gip guide also brings it up");
    gip_init(&g, "hori", 0x0F0D, 0x0063, 0, 0);
    g.reads = 1;
    gip_tick(&g, 1000, rec_send, &r);
    CHECK_EQ(g.state, GIP_WAIT_UP);
    CHECK_EQ(RX(&g, 1010, &st, &r, 0x07, 0x20, 0x01, 0x02, 0x01, 0x5b), GIP_RX_STATE | GIP_RX_INPUT);
    CHECK_EQ(g.state, GIP_ACTIVE);

    TEST("gip a failing send still moves on");
    gip_init(&g, "hori", 0x0F0D, 0x0063, 0, 0);
    g.reads = 1;
    rec_clear(&r);
    r.fail = 1;
    test_log_clear();
    gip_tick(&g, 1000, rec_send, &r);
    CHECK_EQ(r.n, 4);
    CHECK_EQ(g.state, GIP_WAIT_UP);
    CHECK(test_log_find(0, "tx start: 05 20 01 01 00 -> failed") >= 0);
}

static void test_vendor_quirks(void)
{
    gip_t g;
    rec r;

    TEST("gip PDP/PowerA: no waiting");
    gip_init(&g, "pdp", 0x0E6F, 0x02A8, 0, 0);
    rec_clear(&r);
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 0);                           /* still nothing before the first read */
    g.reads = 1;
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 4);
    CHECK_EQ(g.state, GIP_WAIT_UP);
    gip_init(&g, "powera", 0x20D6, 0x2001, 0, 0);
    g.reads = 1;
    rec_clear(&r);
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 4);

    TEST("gip GIP_OPT_BLIND");
    gip_init(&g, "dock", 0x3537, 0x1004, GIP_OPT_BLIND, 0);
    g.reads = 1;
    rec_clear(&r);
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 4);

    TEST("gip 366c:0005 waits too");
    gip_init(&g, "dongle", 0x366C, 0x0005, 0, 0);
    g.reads = 1;
    rec_clear(&r);
    gip_tick(&g, 5000, rec_send, &r);
    CHECK_EQ(r.n, 0);

    TEST("gip Hori packet before START");
    gip_init(&g, "hori", 0x0F0D, 0x0067, GIP_OPT_BLIND, 0);
    g.reads = 1;
    rec_clear(&r);
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 5);
    EXPECT_FRAME(&r, 0, 0x01, 0x20, 0x01, 0x09, 0x00, 0x04, 0x20, 0x3a, 0x00, 0x00, 0x00, 0x80, 0x00);
    EXPECT_FRAME(&r, 1, 0x05, 0x20, 0x02, 0x01, 0x00);
    EXPECT_FRAME(&r, 2, 0x0a, 0x20, 0x03, 0x03, 0x00, 0x01, 0x14);
    gip_init(&g, "pdp-hori", 0x0E6F, 0x0165, 0, 0);        /* a PDP: also at once */
    g.reads = 1;
    rec_clear(&r);
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 5);
    EXPECT_FRAME(&r, 0, 0x01, 0x20, 0x01, 0x09, 0x00, 0x04, 0x20, 0x3a, 0x00, 0x00, 0x00, 0x80, 0x00);

    TEST("gip Xbox One S init");
    gip_init(&g, "one-s", 0x045E, 0x02EA, GIP_OPT_BLIND, 0);
    g.reads = 1;
    rec_clear(&r);
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 5);
    EXPECT_FRAME(&r, 0, 0x05, 0x20, 0x01, 0x01, 0x00);
    EXPECT_FRAME(&r, 1, 0x05, 0x20, 0x02, 0x0f, 0x06);
    EXPECT_FRAME(&r, 2, 0x0a, 0x20, 0x03, 0x03, 0x00, 0x01, 0x14);

    TEST("gip Elite 2 init");
    gip_init(&g, "elite2", 0x045E, 0x0B00, GIP_OPT_BLIND, 0);
    g.reads = 1;
    rec_clear(&r);
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 6);
    EXPECT_FRAME(&r, 1, 0x05, 0x20, 0x02, 0x0f, 0x06);
    EXPECT_FRAME(&r, 2, 0x4d, 0x10, 0x01, 0x02, 0x07, 0x00);
    EXPECT_FRAME(&r, 5, 0x09, 0x00, 0x02, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xeb);

    TEST("gip PowerA rumble kick");
    gip_init(&g, "powera", 0x24C6, 0x541A, 0, 0);
    g.reads = 1;
    rec_clear(&r);
    gip_tick(&g, 0, rec_send, &r);
    CHECK_EQ(r.n, 6);
    EXPECT_FRAME(&r, 3, 0x09, 0x00, 0x01, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00);
    EXPECT_FRAME(&r, 4, 0x09, 0x00, 0x02, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    EXPECT_FRAME(&r, 5, 0x09, 0x00, 0x03, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xeb);

    TEST("gip keepalive");
    {
        xpad_state st;
        long t;
        bring_up(&g, &st, &r, 0x3537, 0x1004, GIP_OPT_KEEPALIVE | GIP_OPT_FAKE_AUTH);
        for (t = 120; t < 20000; t += 20) gip_tick(&g, t, rec_send, &r);
        CHECK_EQ(r.n, 0);
        gip_tick(&g, 20000, rec_send, &r);
        CHECK_EQ(r.n, 3);
        EXPECT_FRAME(&r, 0, 0x05, 0x20, 0x04, 0x01, 0x00);       /* after identify 01, START 02, LED 03 */
        EXPECT_FRAME(&r, 1, 0x0a, 0x20, 0x05, 0x03, 0x00, 0x01, 0x14);
        EXPECT_FRAME(&r, 2, 0x06, 0x20, 0x02, 0x02, 0x01, 0x00);
        gip_tick(&g, 39999, rec_send, &r);
        CHECK_EQ(r.n, 3);
        gip_tick(&g, 40000, rec_send, &r);
        CHECK_EQ(r.n, 6);
    }
}

/* ---- input ------------------------------------------------------------------- */

static void test_input(void)
{
    gip_t g;
    xpad_state st;
    rec r;
    uint8_t p[64];
    int i;

    TEST("gip input, known values");
    bring_up(&g, &st, &r, 0x3537, 0x1004, 0);
    memset(p, 0, sizeof p);
    p[0] = 0x04 | 0x10;                         /* Menu, A */
    p[1] = 0x01 | 0x08 | 0x10 | 0x80;           /* Up, Right, LB, RS */
    p[2] = 0xFF; p[3] = 0x03;                   /* LT 1023 */
    p[4] = 0x00; p[5] = 0x02;                   /* RT 512 */
    p[6] = 0x00; p[7] = 0x80;                   /* LX -32768: full left */
    p[8] = 0xFF; p[9] = 0x7F;                   /* LY 32767: full up */
    p[10] = 0xFF; p[11] = 0x7F;                 /* RX 32767: full right */
    p[12] = 0x00; p[13] = 0x80;                 /* RY -32768: full down */
    p[14] = 0x01;                               /* Share: 18 from the end of 32 */
    CHECK_EQ(rx_input(&g, 200, &st, &r, 1, p, 32), GIP_RX_STATE | GIP_RX_INPUT);
    CHECK_EQ(st.buttons, XPAD_BTN_MENU | XPAD_BTN_A | XPAD_BTN_DPAD_UP | XPAD_BTN_DPAD_RIGHT | XPAD_BTN_LB |
                             XPAD_BTN_RS | XPAD_BTN_SHARE);
    CHECK_EQ(st.lt, 1023); CHECK_EQ(st.rt, 512);
    CHECK_EQ(st.lx, 0x0000); CHECK_EQ(st.ly, 0x0000);
    CHECK_EQ(st.rx, 0xFFFF); CHECK_EQ(st.ry, 0xFFFF);
    CHECK_EQ(g.has_share, 1);
    {
        static const uint8_t want[17] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0x03, 0x00, 0x02, 0x02, 0x41, 0x48, 0x01 };
        uint8_t rep[17];
        CHECK_EQ(xpad_xbox_build_input(&st, rep, sizeof rep), 17);
        CHECK_MEM(rep, want, 17);
    }
    CHECK_EQ(r.n, 0);

    TEST("gip input, mid values");
    memset(p, 0, sizeof p);
    p[2] = 0xFF; p[3] = 0xFF;                   /* only 10 bits count */
    p[4] = 0x34; p[5] = 0x01;                   /* 0x134 */
    p[6] = 0x34; p[7] = 0x12;                   /* LX 0x1234 */
    p[8] = 0x18; p[9] = 0xFC;                   /* LY -1000: a little down */
    p[10] = 0xC7; p[11] = 0xCF;                 /* RX -12345 */
    p[12] = 0xE8; p[13] = 0x03;                 /* RY 1000: a little up */
    rx_input(&g, 210, &st, &r, 2, p, 32);
    CHECK_EQ(st.buttons, 0);
    CHECK_EQ(st.lt, 0x3FF); CHECK_EQ(st.rt, 0x134);
    CHECK_EQ(st.lx, 0x9234);
    CHECK_EQ(st.ly, 0x83E8);
    CHECK_EQ(st.rx, 0x4FC7);
    CHECK_EQ(st.ry, 0x7C18);

    TEST("gip input, every bit");
    {
        static const uint32_t b0[8] = { 0, 0, XPAD_BTN_MENU, XPAD_BTN_VIEW, XPAD_BTN_A, XPAD_BTN_B,
                                        XPAD_BTN_X, XPAD_BTN_Y };
        static const uint32_t b1[8] = { XPAD_BTN_DPAD_UP, XPAD_BTN_DPAD_DOWN, XPAD_BTN_DPAD_LEFT,
                                        XPAD_BTN_DPAD_RIGHT, XPAD_BTN_LB, XPAD_BTN_RB, XPAD_BTN_LS,
                                        XPAD_BTN_RS };
        bring_up(&g, &st, &r, 0x0E6F, 0x02A8, 0);
        for (i = 0; i < 16; i++) {
            memset(p, 0, 14);
            p[i / 8] = (uint8_t)(1u << (i % 8));
            CHECK_EQ(rx_input(&g, 300 + i, &st, &r, (uint8_t)(i + 1), p, 14), GIP_RX_STATE | GIP_RX_INPUT);
            CHECK_EQ(st.buttons, i < 8 ? b0[i] : b1[i - 8]);
        }
        CHECK_EQ(g.has_share, 0);               /* a 14-byte report has none */
    }

    TEST("gip input, too short or not input");
    memset(p, 0, sizeof p);
    p[0] = 0x10;
    rx_input(&g, 400, &st, &r, 1, p, 14);
    CHECK_EQ(st.buttons, XPAD_BTN_A);
    CHECK_EQ(rx_input(&g, 401, &st, &r, 2, p + 1, 13), 0);           /* 13 bytes: ignored */
    CHECK_EQ(st.buttons, XPAD_BTN_A);
    CHECK_EQ(RX(&g, 402, &st, &r, 0x20, 0x20, 0x03, 0x0e, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(st.buttons, XPAD_BTN_A);                                /* a system 0x20 is not input */

    TEST("gip Share offset");
    bring_up(&g, &st, &r, 0x3537, 0x1004, 0);
    memset(p, 0, sizeof p);
    p[18] = 0x01;                               /* the wrong byte for a 32-byte report */
    rx_input(&g, 500, &st, &r, 1, p, 32);
    CHECK_EQ(st.buttons & XPAD_BTN_SHARE, 0);
    rx_input(&g, 501, &st, &r, 2, p, 36);       /* and the right one for 36 (Microsoft fw 5.x) */
    CHECK_EQ(st.buttons, XPAD_BTN_SHARE);
    p[18] = 0; p[22] = 0x01;
    rx_input(&g, 502, &st, &r, 3, p, 40);
    CHECK_EQ(st.buttons, XPAD_BTN_SHARE);
    p[22] = 0xFE;                               /* only bit 0 */
    rx_input(&g, 503, &st, &r, 4, p, 40);
    CHECK_EQ(st.buttons, 0);
    memset(p, 0, sizeof p);
    p[26] = 0x01;                               /* 44 bytes (DLI firmware): not 18 from the end */
    rx_input(&g, 506, &st, &r, 7, p, 44);
    CHECK_EQ(st.buttons, 0);
    p[26] = 0; p[18] = 0x01;                    /* but 26 from it, as SDL reads fw 5.5+ */
    rx_input(&g, 507, &st, &r, 8, p, 44);
    CHECK_EQ(st.buttons, XPAD_BTN_SHARE);
    memset(p, 0, sizeof p);
    p[28] = 0x01;                               /* 46 bytes (Victrix Gambit): 18 from the end again */
    rx_input(&g, 508, &st, &r, 9, p, 46);
    CHECK_EQ(st.buttons, XPAD_BTN_SHARE);
    bring_up(&g, &st, &r, 0x3537, 0x1004, 0);
    memset(p, 0xFF, sizeof p);
    memset(p, 0, 14);
    rx_input(&g, 504, &st, &r, 5, p, 31);       /* too short for the Series layout */
    CHECK_EQ(st.buttons, 0);
    CHECK_EQ(g.has_share, 0);
    bring_up(&g, &st, &r, 0x045E, 0x02E3, 0);   /* Elite 1: a long report, no Share */
    memset(p, 0, sizeof p);
    p[14] = 0x01;
    rx_input(&g, 505, &st, &r, 6, p, 32);
    CHECK_EQ(st.buttons, 0);
    CHECK_EQ(g.has_share, 0);
}

static void test_guide(void)
{
    gip_t g;
    xpad_state st;
    rec r;
    uint8_t p[14];

    TEST("gip guide");
    bring_up(&g, &st, &r, 0x3537, 0x1004, 0);
    CHECK_EQ(RX(&g, 200, &st, &r, 0x07, 0x20, 0x01, 0x02, 0x01, 0x5b), GIP_RX_STATE | GIP_RX_INPUT);
    CHECK_EQ(st.buttons, XPAD_BTN_GUIDE);
    memset(p, 0, sizeof p);
    p[0] = 0x10;
    rx_input(&g, 210, &st, &r, 1, p, 14);       /* input keeps it held */
    CHECK_EQ(st.buttons, XPAD_BTN_GUIDE | XPAD_BTN_A);
    CHECK_EQ(RX(&g, 220, &st, &r, 0x07, 0x20, 0x02, 0x02, 0x00, 0x5b), GIP_RX_STATE | GIP_RX_INPUT);
    CHECK_EQ(st.buttons, XPAD_BTN_A);
    RX(&g, 230, &st, &r, 0x07, 0x20, 0x03, 0x02, 0x02, 0x5b);        /* the Adaptive Controller's "down" */
    CHECK_EQ(st.buttons, XPAD_BTN_GUIDE | XPAD_BTN_A);
    CHECK_EQ(RX(&g, 240, &st, &r, 0x07, 0x00, 0x04, 0x02, 0x00, 0x5b), 0);   /* not a system frame */
    CHECK_EQ(st.buttons, XPAD_BTN_GUIDE | XPAD_BTN_A);
    CHECK_EQ(r.n, 0);

    TEST("gip guide asks for an ack");
    CHECK_EQ(RX(&g, 250, &st, &r, 0x07, 0x30, 0x05, 0x02, 0x00, 0x5b), GIP_RX_STATE | GIP_RX_INPUT);
    CHECK_EQ(r.n, 1);
    EXPECT_FRAME(&r, 0, 0x01, 0x20, 0x05, 0x09, 0x00, 0x07, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK_EQ(st.buttons, XPAD_BTN_A);

    TEST("gip re-announce resets the state");
    RX(&g, 260, &st, &r, 0x07, 0x20, 0x06, 0x02, 0x01, 0x5b);
    st.lt = 500;
    rec_clear(&r);
    CHECK_EQ(RX(&g, 270, &st, &r, 0x02, 0x20, 0x07, 0x00) & GIP_RX_STATE, GIP_RX_STATE);
    CHECK_EQ(st.buttons, 0);
    CHECK_EQ(st.lt, 0);
    CHECK_EQ(st.lx, 0x8000);
    CHECK_EQ(g.state, GIP_WAIT_UP);
    CHECK_EQ(r.n, 1);                           /* asked to identify itself again, */
    EXPECT_FRAME(&r, 0, 0x04, 0x20, 0x04, 0x00);
    CHECK_EQ(g.bursts, 0);
    gip_tick(&g, 370, rec_send, &r);
    CHECK_EQ(r.n, 5);                           /* then brought up again */
    EXPECT_FRAME(&r, 1, 0x05, 0x20, 0x05, 0x01, 0x00);
    CHECK_EQ(g.bursts, 1);
}

/* ---- acknowledgements, modules, chunks, several frames ------------------------ */

static void test_acks_and_modules(void)
{
    gip_t g;
    xpad_state st;
    rec r;

    TEST("gip ack, system frame");
    bring_up(&g, &st, &r, 0x3537, 0x1004, 0);
    RX(&g, 200, &st, &r, 0x04, 0x30, 0x09, 0x03, 0xaa, 0xbb, 0xcc);
    CHECK_EQ(r.n, 1);
    EXPECT_FRAME(&r, 0, 0x01, 0x20, 0x09, 0x09, 0x00, 0x04, 0x20, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK_EQ(g.acks, 1);

    TEST("gip ack, input frame");
    rec_clear(&r);
    CHECK_EQ(RX(&g, 210, &st, &r, 0x20, 0x10, 0x0a, 0x0e, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
             GIP_RX_STATE | GIP_RX_INPUT);
    CHECK_EQ(st.buttons, XPAD_BTN_B);
    CHECK_EQ(r.n, 1);
    EXPECT_FRAME(&r, 0, 0x01, 0x20, 0x0a, 0x09, 0x00, 0x20, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00);

    TEST("gip no ack unasked");
    rec_clear(&r);
    RX(&g, 220, &st, &r, 0x04, 0x20, 0x0b, 0x02, 0x01, 0x02);
    RX(&g, 221, &st, &r, 0x01, 0x20, 0x0c, 0x09, 0, 0, 0, 0, 0, 0, 0, 0, 0);   /* the pad's own ack */
    CHECK_EQ(r.n, 0);

    TEST("gip modules: acked, left alone");
    rec_clear(&r);
    CHECK_EQ(RX(&g, 230, &st, &r, 0x02, 0x31, 0x0d, 0x02, 0xaa, 0xbb), 0);    /* a headset's Hello */
    CHECK_EQ(r.n, 1);
    EXPECT_FRAME(&r, 0, 0x01, 0x21, 0x0d, 0x09, 0x00, 0x02, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK_EQ(g.state, GIP_ACTIVE);              /* not the pad starting over */
    CHECK_EQ(g.hellos, 1);
    CHECK_EQ(g.modules, 1);
    CHECK_EQ(RX(&g, 240, &st, &r, 0x20, 0x01, 0x0e, 0x0e, 0xF0, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(st.buttons, XPAD_BTN_B);           /* a module's "input" is not the pad's */
    CHECK_EQ(RX(&g, 250, &st, &r, 0x07, 0x22, 0x0f, 0x02, 0x01, 0x5b), 0);
    CHECK_EQ(st.buttons & XPAD_BTN_GUIDE, 0);
}

static void test_chunks(void)
{
    gip_t g;
    xpad_state st;
    rec r;

    TEST("gip chunks");
    bring_up(&g, &st, &r, 0x3537, 0x1004, 0);
    /* First chunk: total 186 (varint ba 01), 3 bytes, ack asked. */
    RX(&g, 200, &st, &r, 0x04, 0xF0, 0x07, 0x03, 0xBA, 0x01, 0xaa, 0xbb, 0xcc);
    CHECK_EQ(g.chunk_total, 186);
    CHECK_EQ(r.n, 1);
    EXPECT_FRAME(&r, 0, 0x01, 0x20, 0x07, 0x09, 0x00, 0x04, 0x20, 0x03, 0x00, 0x00, 0x00, 0xB7, 0x00);
    /* A middle chunk at offset 3, unasked: no ack. */
    RX(&g, 201, &st, &r, 0x04, 0xA0, 0x07, 0x03, 0x03, 0xdd, 0xee, 0xff);
    CHECK_EQ(r.n, 1);
    /* One at offset 6 that asks. */
    RX(&g, 202, &st, &r, 0x04, 0xB0, 0x07, 0x03, 0x06, 0x11, 0x22, 0x33);
    CHECK_EQ(r.n, 2);
    EXPECT_FRAME(&r, 1, 0x01, 0x20, 0x07, 0x09, 0x00, 0x04, 0x20, 0x09, 0x00, 0x00, 0x00, 0xB1, 0x00);
    /* A two-byte offset: 300 (ac 02) + 2 = 302 received, nothing left. */
    RX(&g, 203, &st, &r, 0x04, 0xB0, 0x07, 0x02, 0xAC, 0x02, 0x44, 0x55);
    CHECK_EQ(r.n, 3);
    EXPECT_FRAME(&r, 2, 0x01, 0x20, 0x07, 0x09, 0x00, 0x04, 0x20, 0x2E, 0x01, 0x00, 0x00, 0x00, 0x00);
    /* The last (empty) chunk is acknowledged even unasked. */
    RX(&g, 204, &st, &r, 0x04, 0xA0, 0x07, 0x00, 0xBA, 0x01);
    CHECK_EQ(r.n, 4);
    EXPECT_FRAME(&r, 3, 0x01, 0x20, 0x07, 0x09, 0x00, 0x04, 0x20, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK_EQ(g.state, GIP_ACTIVE);

    TEST("gip frames cut short");
    rec_clear(&r);
    test_log_clear();
    CHECK_EQ(RX(&g, 300, &st, &r, 0x04, 0xF0, 0x07, 0x03, 0xBA), 0);                 /* second varint unfinished */
    CHECK_EQ(RX(&g, 301, &st, &r, 0x20, 0x00, 0x01, 0x20, 0x10, 0x00), 0);           /* 32 announced, 2 there */
    CHECK_EQ(RX(&g, 302, &st, &r, 0x04, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01), 0); /* varint over 4 bytes */
    CHECK_EQ(RX(&g, 303, &st, &r, 0x04, 0x00, 0x01, 0x80), 0);                       /* varint runs out */
    CHECK_EQ(r.n, 0);
    CHECK_EQ(st.buttons, 0);
    CHECK(test_log_find(0, "rx frame cut short: 20 00 01 20 10 00") >= 0);

    TEST("gip nothing to read");
    CHECK_EQ(gip_receive(&g, NULL, 10, 400, &st, rec_send, &r), 0);
    CHECK_EQ(RX(&g, 401, &st, &r, 0x03, 0x20, 0x00), 0);                           /* under a header */
    CHECK_EQ(gip_receive(&g, (const uint8_t *)"\x03\x20\x00\x00", 0, 402, &st, rec_send, &r), 0);
    CHECK_EQ(gip_receive(&g, (const uint8_t *)"\x03\x20\x00\x00", -5, 403, &st, rec_send, &r), 0);
    CHECK_EQ(r.n, 0);
}

static void test_several_frames(void)
{
    gip_t g;
    xpad_state st;
    rec r;
    unsigned long statuses;

    TEST("gip several frames in one transfer");
    bring_up(&g, &st, &r, 0x3537, 0x1004, 0);
    statuses = g.statuses;
    CHECK_EQ(RX(&g, 200, &st, &r,
                0x03, 0x20, 0x00, 0x04, 0x86, 0x00, 0x00, 0x00,                 /* status */
                0x20, 0x00, 0x01, 0x0e, 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* A */
                0x07, 0x30, 0x02, 0x02, 0x01, 0x5b,                             /* Guide, ack asked */
                0x00, 0x00),                                                    /* trailing bytes */
             GIP_RX_STATE | GIP_RX_INPUT);
    CHECK_EQ(g.statuses, statuses + 1);
    CHECK_EQ(g.status, 0x86);
    CHECK_EQ(st.buttons, XPAD_BTN_A | XPAD_BTN_GUIDE);
    CHECK_EQ(r.n, 1);
    EXPECT_FRAME(&r, 0, 0x01, 0x20, 0x02, 0x09, 0x00, 0x07, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00);
}

/* ---- rumble -------------------------------------------------------------------- */

static void test_rumble(void)
{
    gip_t g;
    xpad_rumble m;
    uint8_t b[16];

    TEST("gip rumble frame");
    gip_init(&g, "dock", 0x3537, 0x1004, 0, 0);
    m.strong = 100; m.weak = 50; m.lt = 10; m.rt = 20;
    memset(b, 0xEE, sizeof b);
    CHECK_EQ(gip_rumble(&g, &m, 1234, b, (int)sizeof b), 13);
    {
        static const uint8_t want[13] = { 0x09, 0x00, 0x01, 0x09, 0x00, 0x0F, 0x0A, 0x14, 0x64, 0x32,
                                          0xFF, 0x00, 0xFF };
        CHECK_MEM(b, want, 13);
    }
    CHECK_EQ(b[13], 0xEE);
    CHECK_EQ(g.t_rumble, 1234);

    TEST("gip rumble stop");
    memset(&m, 0, sizeof m);
    CHECK_EQ(gip_rumble(&g, &m, 1300, b, 13), 13);
    {
        static const uint8_t want[13] = { 0x09, 0x00, 0x02, 0x09, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00,
                                          0xFF, 0x00, 0xFF };
        CHECK_MEM(b, want, 13);
    }

    TEST("gip rumble clamp and sequence");
    m.strong = 101; m.weak = 255; m.lt = 200; m.rt = 100;
    g.seq_vend = 0xFF;                          /* wraps past 0, which is reserved */
    gip_rumble(&g, &m, 1400, b, 13);
    CHECK_EQ(b[2], 0x01);
    CHECK_EQ(b[6], 100); CHECK_EQ(b[7], 100); CHECK_EQ(b[8], 100); CHECK_EQ(b[9], 100);

    TEST("gip rumble from GameController's report");
    {
        static const uint8_t gc[9] = { 0x03, 0x0F, 0x05, 0x06, 0x46, 0x1E, 0x0A, 0x00, 0x00 };
        static const uint8_t want[13] = { 0x09, 0x00, 0x02, 0x09, 0x00, 0x0F, 0x05, 0x06, 0x46, 0x1E,
                                          0xFF, 0x00, 0xFF };
        CHECK_EQ(xpad_xbox_parse_rumble(3, gc, sizeof gc, &m), 1);
        CHECK_EQ(gip_rumble(&g, &m, 1500, b, 13), 13);
        CHECK_MEM(b, want, 13);
    }

    TEST("gip rumble bounds");
    CHECK_EQ(gip_rumble(&g, &m, 1600, b, 12), 0);
    CHECK_EQ(gip_rumble(&g, NULL, 1600, b, 13), 0);
    CHECK_EQ(gip_rumble(&g, &m, 1600, NULL, 13), 0);
}

static void test_names(void)
{
    gip_t g;

    TEST("gip names");
    CHECK_STR(gip_state_name(GIP_WAIT_HELLO), "wait-hello");
    CHECK_STR(gip_state_name(GIP_WAIT_UP), "wait-up");
    CHECK_STR(gip_state_name(GIP_ACTIVE), "active");
    CHECK_STR(gip_state_name(GIP_SILENT), "silent");
    CHECK_STR(gip_state_name(-1), "?");
    CHECK_STR(gip_state_name(4), "?");
    gip_init(&g, NULL, 0x3537, 0x1004, 0, 0);
    CHECK_STR(g.name, "?");
    test_log_clear();
    gip_lost(&g, 5);
    CHECK(test_log_find(0, "gip ? +5 lost: wait-hello, 0 hello, 0 status, 0 input, 0 bring-up(s), last frame +-1") >= 0);
}

/* ---- fuzz ------------------------------------------------------------------------ */

static uint32_t rnd_state = 0x9E3779B9u;

static uint32_t rnd(void)
{
    uint32_t x = rnd_state;         /* xorshift32: deterministic, so a failure reproduces */

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rnd_state = x;
}

static void check_sane(const gip_t *g, const xpad_state *st, const rec *r, int from)
{
    int i;

    CHECK(g->state >= GIP_WAIT_HELLO && g->state <= GIP_SILENT);
    CHECK(st->lt <= XPAD_TRIGGER_MAX && st->rt <= XPAD_TRIGGER_MAX);
    CHECK((st->buttons & 0xFFFF0000u) == 0);
    CHECK(g->bursts <= 3);
    for (i = from; i < r->n && i < REC_MAX; i++) {
        int n = r->len[i];
        /* Only frames gip.c builds: those, and the handshake's (at most 64 bytes). */
        CHECK(n == 4 || n == 5 || n == 6 || n == 7 || n == 13 || (r->f[i][0] == 0x06 && n <= 64));
        CHECK(r->f[i][2] != 0 || r->f[i][0] == 0x01);            /* our sequence numbers are never 0 */
    }
}

static void test_fuzz(void)
{
    static const uint16_t vids[][2] = { { 0x3537, 0x1004 }, { 0x0E6F, 0x02A8 }, { 0x0F0D, 0x0067 },
                                        { 0x24C6, 0x541A }, { 0x045E, 0x0B00 }, { 0x1532, 0x0A00 } };
    static const uint8_t cmds[] = { 0x01, 0x02, 0x03, 0x04, 0x06, 0x07, 0x20, 0x1E };
    gip_t g;
    xpad_state st;
    rec r;
    long now = 0;
    int it;

    TEST("gip fuzz, 10000 random transfers");
    test_log_clear();
    for (it = 0; it < 10000; it++) {
        uint8_t buf[96];
        int len = 0, start;

        if (it % 500 == 0) {
            const uint16_t *v = vids[(it / 500) % (sizeof vids / sizeof vids[0])];
            gip_init(&g, "fuzz", v[0], v[1], (int)(rnd() % 8), now);
            g.reads = 1;
            xpad_state_reset(&st);
            rec_clear(&r);
        }
        if (r.n > REC_MAX - 16) r.n = 0;
        start = r.n;
        r.fail = (rnd() % 16) == 0;
        now += (long)(rnd() % 300);

        if (rnd() % 2) {                        /* pure noise */
            len = (int)(rnd() % (sizeof buf + 1));
            for (int i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
        } else {                                /* frames that look right, mostly */
            while (len < (int)sizeof buf - 8 && rnd() % 3) {
                int plen = (int)(rnd() % 40);
                uint8_t flags = (uint8_t)rnd();
                buf[len++] = cmds[rnd() % sizeof cmds];
                buf[len++] = flags;
                buf[len++] = (uint8_t)rnd();
                buf[len++] = (uint8_t)plen;
                if (flags & 0x80) buf[len++] = (uint8_t)(rnd() & 0x7F);
                for (int i = 0; i < plen && len < (int)sizeof buf; i++) buf[len++] = (uint8_t)rnd();
            }
            if (rnd() % 4 == 0 && len) len = (int)(rnd() % (unsigned)len);   /* cut anywhere */
        }
        rx_bytes(&g, now, &st, &r, buf, len);
        if (rnd() % 3 == 0) gip_tick(&g, now, rec_send, &r);
        if (rnd() % 7 == 0) {
            xpad_rumble m = { (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd() };
            uint8_t b[13];
            CHECK_EQ(gip_rumble(&g, &m, now, b, 13), 13);
            CHECK(b[6] <= 100 && b[7] <= 100 && b[8] <= 100 && b[9] <= 100);
        }
        check_sane(&g, &st, &r, start);
    }
    CHECK(test_log_total() > 0);

    TEST("gip every truncation of a real transfer");
    {
        static const uint8_t real[] = { 0x03, 0x20, 0x00, 0x04, 0x87, 0x00, 0x00, 0x00, 0x04, 0xF0, 0x07,
                                        0x03, 0xBA, 0x01, 0xaa, 0xbb, 0xcc, 0x20, 0x10, 0x01, 0x0e, 0x10,
                                        0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        int n;
        for (n = 0; n <= (int)sizeof real; n++) {
            gip_init(&g, "cut", 0x3537, 0x1004, 0, 0);
            g.reads = 1;
            xpad_state_reset(&st);
            rec_clear(&r);
            g.state = GIP_WAIT_UP;              /* brought up once, waiting for an answer */
            g.bursts = 1;
            rx_bytes(&g, 10, &st, &r, real, n);
            check_sane(&g, &st, &r, 0);
            CHECK_EQ(g.state, n >= 8 ? GIP_ACTIVE : GIP_WAIT_UP);
            CHECK_EQ(st.buttons, n == (int)sizeof real ? XPAD_BTN_A : 0u);
        }
    }
}

int main(int argc, char **argv)
{
    const char *research = argc > 1 ? argv[1] : "../docs/research";

    test_capture_replay(research);
    test_hello_starts_bring_up();
    test_identify();
    test_retries_and_silence();
    test_vendor_quirks();
    test_input();
    test_guide();
    test_acks_and_modules();
    test_chunks();
    test_several_frames();
    test_rumble();
    test_names();
    test_fuzz();
    return test_done("gip");
}
