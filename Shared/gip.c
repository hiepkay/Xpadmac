/*
 * The host's side of the Gaming Input Protocol (Xbox One / Series pads on
 * USB): bring-up, acknowledgements, chunked frames, input. See gip.h.
 *
 * Derived from a GIP module (src/gip.c) written on top of AnyPad PS4
 * (https://github.com/Vld439/AnyPad-PS4, a port of AnyPad PS5 by
 * elmonomalvad0, https://codeberg.org/elmonomalva0/Any-Pad-ps5) and never
 * published there; the original is kept verbatim in
 * docs/research/anypad_gip.c.orig (see NOTICE). AnyPad is GPL-3.0-or-later,
 * and so is this file. The protocol knowledge it encodes
 * comes from Linux's xpad and xone and from SDL.
 *
 * Changes for XPadMac:
 *  - input fills an xpad_state with full precision (16-bit sticks, 10-bit
 *    triggers) instead of an 8-bit PS-style pad_state, and reads the Share
 *    button of Series-layout reports;
 *  - rumble takes an xpad_rumble (0..100, the trigger motors too);
 *  - logging goes through xpad_log(), and the hex dumps no longer need
 *    stdio, so the file builds for DriverKit as for the host;
 *  - a pad's Hello is answered with an identification request first, as
 *    SDL, xone and Windows do (AnyPad and Linux's xpad skip it), and the
 *    bring-up follows the pad's answer or 100 ms of silence: a third-party
 *    dock dropped out of GIP 1.35 s after AnyPad's bring-up
 *    alone (docs/research/gip_bringup_capture.log);
 *  - pads of VID 0x3537 (a vendor quirk), and any with GIP_OPT_AUTH, are
 *    authenticated as xone does it (gip_auth.c): its Host Hello takes the place of the "security
 *    complete" frame in the bring-up, gip.c sends the long host messages in
 *    chunks as the pad acknowledges them and reassembles the pad's, and
 *    "complete" follows the pad's verified Finished value, or a failure or
 *    2 s without an answer (then as before: that "complete", T_UP for the
 *    pad to come up, and the plain "complete" in the bring-ups that follow
 *    until it identifies itself again); a Hello again while the handshake
 *    moves on leaves it alone (xone never restarts it);
 *  - GIP_OPT_SERIAL sends xone's serial-number request (1E 20 SS 01 04)
 *    after security, as xone's gip_init_input does;
 *  - chunked 0x06 frames are acknowledged by xone's rule: asked, or the last
 *    chunk with data; never the empty one that closes the message.
 * Otherwise the state machine, the vendor quirks, the acknowledgements, the
 * chunk handling and the module filtering are AnyPad's.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * General Public License (LICENSE at the top of this repository).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gip.h"

#include <string.h>

#define T_IDENTIFY      100     /* ms without any of the identification reply: bring it up anyway (SDL) */
#define T_IDENTIFY_MAX  500     /* ms of a reply that does not end: bring it up anyway */
#define T_HELLO_WAIT   1000     /* ms without a Hello before bringing the pad up blind */
#define T_HINT        10000     /* ms waiting for a Hello before the hint */
#define T_UP           2000     /* ms after a bring-up without status or input: again */
#define BURSTS            3     /* bring-ups tried before giving up (the pad stays read) */
#define T_REANNOUNCE    500     /* ms between two bring-ups for a pad that announces again */
#define T_KEEPALIVE   20000
#define T_AUTH_STEP    2000     /* ms without a word on the handshake: give it up, "complete" as before */
#define PKT_MAX          58     /* payload bytes in one frame (xone's GIP_PKT_MAX_LENGTH) */
#define CHUNKS_PER_ACK    5     /* chunks sent for one acknowledgement (GIP_PKT_COALESCE_COUNT) */
#define LOG_ACKS         20     /* acknowledgements logged, then only counted */
#define LOG_INPUTS        5
#define LOG_OTHER        50

/* Input 0x20: the fixed part every pad sends (buttons, triggers, sticks). */
#define INPUT_BASE_LEN   14
/* Series X|S layout: the Share byte sits 18 bytes from the payload's end
 * (Linux xpad's MAP_SHARE_BUTTON and xone's GIP_GP_BTN_SHARE_OFFSET: p14
 * for a 32-byte payload, p18 for 36), or 26
 * from it on firmware that appends dynamic latency input (xone's DLI
 * offset). gip.c reads no metadata, so the DLI case is told by its one
 * known length: 44 bytes (Series X|S firmware 5.5+), Share at p18, as SDL
 * reads it. A payload too short to hold it after the fixed part has none. */
#define SHARE_FROM_END   18
#define SHARE_FROM_END_DLI 26
#define SHARE_DLI_LEN    44
#define SHARE_MIN_LEN    (INPUT_BASE_LEN + SHARE_FROM_END)

/* ---- small things ------------------------------------------------------------ */

static uint8_t next_seq(uint8_t *c)
{
    if (++*c == 0) ++*c;        /* 0 is reserved */
    return *c;
}

/* "02 20 01 ..." into out (at most max - 1 characters and a NUL); stops,
 * without a half-written byte, when out is full. */
static void hex(char *out, size_t max, const uint8_t *p, int n)
{
    static const char digits[] = "0123456789abcdef";
    size_t k = 0;
    int i;

    if (!max) return;
    for (i = 0; i < n && k + 3 < max; i++) {
        if (k) out[k++] = ' ';
        out[k++] = digits[p[i] >> 4];
        out[k++] = digits[p[i] & 0x0F];
    }
    out[k] = '\0';
}

const char *gip_state_name(int state)
{
    static const char *const names[] = { "wait-hello", "wait-up", "active", "silent" };

    return state >= 0 && state <= GIP_SILENT ? names[state] : "?";
}

static void set_state(gip_t *g, int state, long now, const char *why)
{
    if (g->state == state) return;
    xpad_log("gip %s +%ld %s -> %s (%s)", g->name, now - g->t_open, gip_state_name(g->state),
             gip_state_name(state), why);
    g->state = state;
    g->t_state = now;
}

static int tx(gip_t *g, const char *what, const uint8_t *p, int len, long now, gip_send_fn send, void *ctx)
{
    char h[3 * 32 + 1];
    int ok = send(ctx, p, len);

    hex(h, sizeof h, p, len);
    if (len > 32)
        xpad_log("gip %s +%ld tx %s: %s ... (%d bytes) -> %s", g->name, now - g->t_open, what, h, len,
                 ok ? "ok" : "failed");
    else
        xpad_log("gip %s +%ld tx %s: %s -> %s", g->name, now - g->t_open, what, h, ok ? "ok" : "failed");
    return ok;
}

/* ---- the vendors that need more ------------------------------------------------ */

static int is(const gip_t *g, uint16_t vid, uint16_t pid) { return g->vid == vid && g->pid == pid; }

/* Vendor quirk: VID 0x3537 receivers (and the 366c:0005) announce only
 * once their controller is linked: the host waits for them however long it
 * takes (xone; xpad's FLAG_DELAY_INIT). */
static int waits_for_hello(const gip_t *g)
{
    return g->vid == 0x3537 || is(g, 0x366C, 0x0005);
}

/* PDP, PowerA and these others may never announce: brought up at once
 * (SDL's legacy Xbox One driver). */
static int never_hello(const gip_t *g)
{
    return g->vid == 0x0E6F || g->vid == 0x24C6 || g->vid == 0x20D6;
}

/* ---- authentication (0x06) ---------------------------------------------------------
 *
 * The handshake itself is gip_auth.c's; here it is framed. A host message
 * of up to 58 bytes is one frame (06 30 SS len, ack asked). A longer one
 * goes as xone sends it (bus/protocol.c gip_send_pkt and
 * gip_handle_pkt_acknowledge): a first chunk 06 F0 SS 58 total, then,
 * when the pad acknowledges (01 .. 06 .. got), the rest from `got` on, 58
 * bytes a chunk (06 A0 SS len offset; the last one asks for an ack: B0),
 * and once it acknowledges the whole, an empty chunk (06 A0 SS 0 total).
 * Every chunk carries the first one's sequence number. */

static int uses_auth(const gip_t *g)
{
    if (g->opts & GIP_OPT_FAKE_AUTH) return 0;
    return (g->opts & GIP_OPT_AUTH) || g->vid == 0x3537;
}

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

/* xone's gip_encode_header: command, options, sequence, varint length, and
 * for a chunk the varint offset; a header of odd length gets its length
 * varint padded with a 0x00 byte (the byte before it marked "more"). */
static int put_header(uint8_t *b, uint8_t cmd, uint8_t opts, uint8_t seq, long plen, long off)
{
    uint8_t v[8];
    int n = 3;

    b[0] = cmd;
    b[1] = opts;
    b[2] = seq;
    n += put_varint(b + n, plen);
    if (((opts & 0x80) ? n + put_varint(v, off) : n) % 2) {
        b[n - 1] |= 0x80;
        b[n++] = 0x00;
    }
    if (opts & 0x80) n += put_varint(b + n, off);
    return n;
}

static int auth_frame(gip_t *g, const char *what, uint8_t opts, uint8_t seq, const uint8_t *p, int n, long off,
                      long now, gip_send_fn send, void *ctx)
{
    uint8_t b[8 + PKT_MAX];
    int h = put_header(b, 0x06, opts, seq, n, off);

    if (n > 0) memcpy(b + h, p, (size_t)n);
    return tx(g, what, b, h + n, now, send, ctx);
}

static const char *auth_what(const uint8_t *m)
{
    if (m[1] & 0x02) return "auth request";
    switch (m[3]) {
    case 0x01: return "auth host-hello";
    case 0x21: return "auth host-hello-v2";
    case 0x05: return "auth host-secret";
    case 0x25: return "auth host-pubkey";
    default:   return "auth host-finish";
    }
}

static void auth_send(gip_t *g, const uint8_t *m, int len, long now, gip_send_fn send, void *ctx)
{
    uint8_t seq = next_seq(&g->seq_sec);

    g->auth_out_len = 0;
    if (len <= PKT_MAX) {
        auth_frame(g, auth_what(m), 0x30, seq, m, len, 0, now, send, ctx);
        return;
    }
    memcpy(g->auth_out, m, (size_t)len);
    g->auth_out_len = len;
    g->auth_out_seq = seq;
    auth_frame(g, auth_what(m), 0xF0, seq, m, PKT_MAX, len, now, send, ctx);
}

/* The pad acknowledged `got` bytes of a 0x06 message. */
static void auth_acked(gip_t *g, long got, long now, gip_send_fn send, void *ctx)
{
    long len = g->auth_out_len, off = got > 0 ? got : 0;
    int k;

    if (g->auth.state == GIP_AUTH_RUNNING) g->auth.t_step = now;
    if (!len) return;
    if (got >= len) {
        auth_frame(g, "auth chunks end", 0xA0, g->auth_out_seq, NULL, 0, len, now, send, ctx);
        g->auth_out_len = 0;
        return;
    }
    for (k = 0; k < CHUNKS_PER_ACK && off < len; k++) {
        int n = len - off > PKT_MAX ? PKT_MAX : (int)(len - off);
        uint8_t opts = (uint8_t)(len - off <= PKT_MAX ? 0xB0 : 0xA0);

        auth_frame(g, "auth chunk", opts, g->auth_out_seq, g->auth_out + off, n, off, now, send, ctx);
        off += n;
    }
}

static void security_complete(gip_t *g, const char *what, long now, gip_send_fn send, void *ctx)
{
    const uint8_t p[] = { 0x06, 0x20, next_seq(&g->seq_sec), 0x02, 0x01, 0x00 };

    tx(g, what, p, (int)sizeof p, now, send, ctx);
}

static void auth_reset(gip_t *g)
{
    g->auth_out_len = 0;
    g->auth_in_on = 0;
}

/* Given up: the plain "complete", and the pad gets a whole T_UP from it to
 * come up before a bring-up again, which then sends the plain "complete"
 * at once (auth_skip) until the pad identifies itself again: a pad that
 * passes with fake authentication is not handed a new Host Hello after it. */
static void auth_fallback(gip_t *g, long now, gip_send_fn send, void *ctx)
{
    xpad_log("gip %s +%ld auth given up (%s): security complete as without it", g->name, now - g->t_open,
             g->auth.why ? g->auth.why : "?");
    auth_reset(g);
    g->auth_fallbacks++;
    g->auth_skip = 1;
    g->t_burst = now;
    security_complete(g, "security-complete", now, send, ctx);
}

/* The handshake starts with the bring-up, where "security complete" was. */
static void auth_begin(gip_t *g, long now, gip_send_fn send, void *ctx)
{
    uint8_t m[64];
    int n;

    auth_reset(g);
    n = gip_auth_start(&g->auth, now, m, (int)sizeof m);
    if (n > 0) auth_send(g, m, n, now, send, ctx);
    else auth_fallback(g, now, send, ctx);
}

/* A whole auth message from the pad. */
static void auth_message(gip_t *g, const uint8_t *m, int n, long now, gip_send_fn send, void *ctx)
{
    uint8_t out[GIP_AUTH_MSG_MAX];
    int olen = 0;

    if (g->auth.state != GIP_AUTH_RUNNING) {
        if (uses_auth(g) && g->other <= LOG_OTHER)      /* show() counted it */
            xpad_log("gip %s +%ld auth message (%d bytes) while %s: ignored", g->name, now - g->t_open, n,
                     gip_auth_state_name(g->auth.state));
        return;
    }
    switch (gip_auth_rx(&g->auth, m, n, now, out, (int)sizeof out, &olen)) {
    case GIP_AUTH_SEND:
        auth_send(g, out, olen, now, send, ctx);
        break;
    case GIP_AUTH_COMPLETE:
        g->auth_done++;
        auth_reset(g);
        security_complete(g, "auth complete", now, send, ctx);
        break;
    case GIP_AUTH_FAIL:
        auth_fallback(g, now, send, ctx);
        break;
    default:
        break;
    }
}

/* A chunk of a long auth message from the pad: the first gives the total
 * (at most GIP_AUTH_IN_MAX), the others their offset; the empty one that
 * closes it hands the whole over (xone's gip_process_pkt_chunked). */
static void auth_chunk(gip_t *g, uint8_t flags, long second, const uint8_t *p, int n, long now,
                       gip_send_fn send, void *ctx)
{
    long off = second;

    if (flags & 0x40) {
        g->auth_in_on = 0;
        if (second <= 0 || second > GIP_AUTH_IN_MAX) {
            xpad_log("gip %s +%ld auth: a %ld-byte message from the pad, over %d: dropped", g->name,
                     now - g->t_open, second, GIP_AUTH_IN_MAX);
            return;
        }
        memset(g->auth_in, 0, (size_t)second);
        g->auth_in_on = 1;
        g->auth_in_total = second;
        off = 0;
    }
    if (n > 0) {
        if (!g->auth_in_on) return;
        if (off < 0 || off + n > g->auth_in_total) {
            xpad_log("gip %s +%ld auth: chunk at %ld (%d bytes) past the message's %ld: dropped", g->name,
                     now - g->t_open, off, n, g->auth_in_total);
            g->auth_in_on = 0;
            return;
        }
        memcpy(g->auth_in + off, p, (size_t)n);
        if (g->auth.state == GIP_AUTH_RUNNING) g->auth.t_step = now;
    } else if (!(flags & 0x40) && g->auth_in_on) {
        g->auth_in_on = 0;
        auth_message(g, g->auth_in, (int)g->auth_in_total, now, send, ctx);
    }
}

/* Still waiting on an answer that is not overdue. */
static int auth_busy(const gip_t *g, long now)
{
    return g->auth.state == GIP_AUTH_RUNNING && now - g->auth.t_step < T_AUTH_STEP;
}

/* ---- bringing the pad up --------------------------------------------------------
 *
 * In xpad's and SDL's order, with the vendor packets from xpad's init list.
 * Security is the one step that differs: "complete" at once, or the start
 * of the handshake (xone's place for it: after the LED, before the rumble
 * stop; "complete" at once after a fallback, see auth_fallback). A
 * bring-up while a handshake runs starts it over; gip_tick and hello()
 * hold the bring-ups back while it moves on. With GIP_OPT_SERIAL, xone's
 * serial-number request follows security (gip_init_input). */
static void burst(gip_t *g, long now, gip_send_fn send, void *ctx, const char *why)
{
    uint8_t p[16];

    xpad_log("gip %s +%ld bring-up %d (%s)", g->name, now - g->t_open, g->bursts + 1, why);
    if (is(g, 0x0E6F, 0x0165) || is(g, 0x0F0D, 0x0067)) {          /* Hori: before START */
        const uint8_t q[] = { 0x01, 0x20, next_seq(&g->seq_sys), 0x09, 0x00, 0x04, 0x20, 0x3A,
                              0x00, 0x00, 0x00, 0x80, 0x00 };
        tx(g, "hori", q, (int)sizeof q, now, send, ctx);
    }
    p[0] = 0x05; p[1] = 0x20; p[2] = next_seq(&g->seq_sys); p[3] = 0x01; p[4] = 0x00;
    tx(g, "start", p, 5, now, send, ctx);
    if (is(g, 0x045E, 0x02EA) || is(g, 0x045E, 0x0B00)) {          /* Xbox One S, Elite 2 */
        p[0] = 0x05; p[1] = 0x20; p[2] = next_seq(&g->seq_sys); p[3] = 0x0F; p[4] = 0x06;
        tx(g, "s-init", p, 5, now, send, ctx);
    }
    if (is(g, 0x045E, 0x0B00)) {                                     /* Elite 2: extra input */
        p[0] = 0x4D; p[1] = 0x10; p[2] = next_seq(&g->seq_vend); p[3] = 0x02; p[4] = 0x07; p[5] = 0x00;
        tx(g, "extra-input", p, 6, now, send, ctx);
    }
    p[0] = 0x0A; p[1] = 0x20; p[2] = next_seq(&g->seq_sys); p[3] = 0x03; p[4] = 0x00; p[5] = 0x01; p[6] = 0x14;
    tx(g, "led", p, 7, now, send, ctx);
    if (uses_auth(g) && !g->auth_skip) auth_begin(g, now, send, ctx);
    else security_complete(g, "security-complete", now, send, ctx);
    if (g->opts & GIP_OPT_SERIAL) {
        p[0] = 0x1E; p[1] = 0x20; p[2] = next_seq(&g->seq_sys); p[3] = 0x01; p[4] = 0x04;
        tx(g, "serial", p, 5, now, send, ctx);
    }
    if (is(g, 0x24C6, 0x541A) || is(g, 0x24C6, 0x542A) || is(g, 0x24C6, 0x543A)) {   /* PowerA */
        const uint8_t on[] = { 0x09, 0x00, next_seq(&g->seq_vend), 0x09, 0x00, 0x0F, 0x00, 0x00,
                               0x1D, 0x1D, 0xFF, 0x00, 0x00 };
        uint8_t off[] = { 0x09, 0x00, 0, 0x09, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        tx(g, "rumble-begin", on, (int)sizeof on, now, send, ctx);
        off[2] = next_seq(&g->seq_vend);
        tx(g, "rumble-end", off, (int)sizeof off, now, send, ctx);
    }
    {
        /* "Required for some exotic gamepads to start input" (xone, SDL). */
        const uint8_t stop[] = { 0x09, 0x00, next_seq(&g->seq_vend), 0x09, 0x00, 0x0F, 0x00, 0x00,
                                 0x00, 0x00, 0xFF, 0x00, 0xEB };
        tx(g, "rumble-stop", stop, (int)sizeof stop, now, send, ctx);
    }
    g->bursts++;
    g->t_burst = now;
    set_state(g, GIP_WAIT_UP, now, "brought up");
}

/* ---- asking the pad who it is ---------------------------------------------------
 *
 * What SDL (UpdateInitState: ANNOUNCED -> IDENTIFYING), xone and Windows
 * answer a Hello with: 04 20 SS 00. The pad replies with its metadata
 * (command 04, usually chunked, the chunks acknowledged as any others), and
 * the bring-up above follows once the reply has ended (a whole frame, or
 * the empty chunk that closes a chunked one), after T_IDENTIFY ms without
 * any part of it (SDL's CONTROLLER_IDENTIFY_TIMEOUT_MS), or T_IDENTIFY_MAX
 * ms after the request whatever comes. The metadata itself is not read. A
 * bring-up without a Hello (blind, or a vendor that never announces) has
 * nothing to identify and goes straight to burst().
 *
 * gip.h has no state of its own for this: it is GIP_WAIT_UP before any
 * bring-up (bursts == 0, which nothing else produces), t_state the time of
 * the request, t_burst that of the request or of the reply's latest part.
 * Nothing the pad sends meanwhile skips the bring-up: status, Guide and
 * input are taken in, but the pad goes ACTIVE only once START and the rest
 * have gone out. So every frame that brought the pad up before is still
 * sent, in the same order; the request comes first, and START up to
 * T_IDENTIFY_MAX ms later (T_IDENTIFY when the pad says nothing). */
static int identifying(const gip_t *g)
{
    return g->state == GIP_WAIT_UP && g->bursts == 0;
}

static void identify(gip_t *g, long now, gip_send_fn send, void *ctx, const char *why)
{
    const uint8_t p[] = { 0x04, 0x20, next_seq(&g->seq_sys), 0x00 };

    tx(g, "identify", p, (int)sizeof p, now, send, ctx);
    if (g->auth.state == GIP_AUTH_RUNNING) gip_auth_end(&g->auth, GIP_AUTH_IDLE, NULL);   /* starts over */
    auth_reset(g);
    g->auth_skip = 0;                   /* a new start: the handshake is tried again */
    g->bursts = 0;
    g->t_burst = now;
    set_state(g, GIP_WAIT_UP, now, why);
}

/* A metadata frame (04) from the pad while identifying: the end of the reply
 * brings the pad up, any other part of it buys it T_IDENTIFY more. */
static void identified(gip_t *g, uint8_t flags, int n, long now, gip_send_fn send, void *ctx)
{
    if (!(flags & 0x20) || !identifying(g)) return;
    if (!(flags & 0x80) || (n == 0 && !(flags & 0x40))) burst(g, now, send, ctx, "identified");
    else g->t_burst = now;
}

/* ---- what the pad sends --------------------------------------------------------- */

static int16_t les16(const uint8_t *p)
{
    unsigned v = (unsigned)p[0] | (unsigned)p[1] << 8;

    return (int16_t)(v >= 0x8000u ? (int)v - 0x10000 : (int)v);   /* no implementation-defined cast */
}

static uint16_t trigger10(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] | (unsigned)p[1] << 8) & 0x3FFu);
}

/* Input 0x20, payload: [0] b2 Menu, b3 View, b4 A, b5 B, b6 X, b7 Y
 * [1] b0 up, b1 down, b2 left, b3 right, b4 LB, b5 RB, b6 LS, b7 RS
 * [2..3] LT and [4..5] RT, 10 bits  [6..13] LX LY RX RY, s16 LE, Y up
 * [n-18] b0 Share, in a Series-layout report (n >= 32; [18] when n is 44,
 * see SHARE_FROM_END_DLI). The Guide button
 * comes in its own frame (0x07) and is kept across input frames. */
static int input(gip_t *g, const uint8_t *p, int n, long now, xpad_state *st)
{
    uint32_t b = 0;

    if (n < INPUT_BASE_LEN) return 0;
    if (p[0] & 0x04) b |= XPAD_BTN_MENU;
    if (p[0] & 0x08) b |= XPAD_BTN_VIEW;
    if (p[0] & 0x10) b |= XPAD_BTN_A;
    if (p[0] & 0x20) b |= XPAD_BTN_B;
    if (p[0] & 0x40) b |= XPAD_BTN_X;
    if (p[0] & 0x80) b |= XPAD_BTN_Y;
    if (p[1] & 0x01) b |= XPAD_BTN_DPAD_UP;
    if (p[1] & 0x02) b |= XPAD_BTN_DPAD_DOWN;
    if (p[1] & 0x04) b |= XPAD_BTN_DPAD_LEFT;
    if (p[1] & 0x08) b |= XPAD_BTN_DPAD_RIGHT;
    if (p[1] & 0x10) b |= XPAD_BTN_LB;
    if (p[1] & 0x20) b |= XPAD_BTN_RB;
    if (p[1] & 0x40) b |= XPAD_BTN_LS;
    if (p[1] & 0x80) b |= XPAD_BTN_RS;
    /* Microsoft's own pads that reach this code (02D1/02DD/02E3: Xbox One and
     * Elite 1; Apple drives the Series ones) have no Share; the Elite 1's
     * long report carries paddles and a copy of its mapping instead. */
    if (n >= SHARE_MIN_LEN && g->vid != 0x045E) {
        int share = n - (n == SHARE_DLI_LEN ? SHARE_FROM_END_DLI : SHARE_FROM_END);

        if (!g->has_share) {
            g->has_share = 1;
            xpad_log("gip %s +%ld %d-byte input: Series layout, Share at byte %d", g->name,
                     now - g->t_open, n, share);
        }
        if (p[share] & 0x01) b |= XPAD_BTN_SHARE;
    }
    st->lt = trigger10(p + 2);
    st->rt = trigger10(p + 4);
    st->lx = xpad_axis_from_s16(les16(p + 6));
    st->ly = xpad_axis_from_s16_inverted(les16(p + 8));
    st->rx = xpad_axis_from_s16(les16(p + 10));
    st->ry = xpad_axis_from_s16_inverted(les16(p + 12));
    st->buttons = b | (st->buttons & XPAD_BTN_GUIDE);
    return 1;
}

/* The acknowledgement: 01 20 SS 09 | 00 CC FF Rl Rh 00 00 Ml Mh, SS the
 * frame's sequence number, CC its command, FF its system flag, R the bytes
 * received so far and M those still to come (SDL, xone). */
static void ack(gip_t *g, uint8_t cmd, uint8_t flags, uint8_t seq, long got, long left, long now,
                gip_send_fn send, void *ctx)
{
    /* Addressed to whoever sent the frame: the pad (0), or a module on it. */
    uint8_t a[13] = { 0x01, (uint8_t)(0x20 | (flags & 0x0F)), seq, 0x09, 0x00, cmd, (uint8_t)(flags & 0x20),
                      (uint8_t)got, (uint8_t)(got >> 8), 0x00, 0x00, (uint8_t)left, (uint8_t)(left >> 8) };
    int ok = send(ctx, a, (int)sizeof a);

    if (g->acks++ < LOG_ACKS)
        xpad_log("gip %s +%ld ack cmd %02x seq %02x got %ld left %ld -> %s", g->name, now - g->t_open, cmd,
                 seq, got, left, ok ? "ok" : "failed");
}

static void hello(gip_t *g, const uint8_t *p, int n, long now, xpad_state *st, int *ret,
                  gip_send_fn send, void *ctx)
{
    g->hellos++;
    if (n >= 28)
        xpad_log("gip %s +%ld hello %lu: %04x:%04x firmware %u.%u.%u.%u hardware %u.%u", g->name,
                 now - g->t_open, g->hellos, (unsigned)(p[8] | p[9] << 8), (unsigned)(p[10] | p[11] << 8),
                 (unsigned)(p[12] | p[13] << 8), (unsigned)(p[14] | p[15] << 8),
                 (unsigned)(p[16] | p[17] << 8), (unsigned)(p[18] | p[19] << 8), (unsigned)p[20],
                 (unsigned)p[21]);
    switch (g->state) {
    case GIP_WAIT_HELLO:
    case GIP_SILENT:
        identify(g, now, send, ctx, "hello");
        break;
    case GIP_WAIT_UP:                   /* announcing again: the bring-up (or identification) did not take */
        if (auth_busy(g, now)) {        /* but not a handshake that moves on: xone never restarts it */
            xpad_log("gip %s +%ld hello again while the auth handshake moves on: left alone", g->name,
                     now - g->t_open);
            break;
        }
        if (now - g->t_burst >= T_REANNOUNCE && g->bursts < BURSTS) burst(g, now, send, ctx, "hello again");
        break;
    case GIP_ACTIVE:                    /* it started over: nothing held, up again */
        xpad_log("gip %s +%ld re-announce after %ld ms up", g->name, now - g->t_open, now - g->t_state);
        xpad_state_reset(st);
        *ret |= GIP_RX_STATE;
        identify(g, now, send, ctx, "re-announce");
        break;
    default:
        break;
    }
}

/* A frame gip.c does not act on (or acts on only in passing): shown. */
static void show(gip_t *g, uint8_t cmd, uint8_t flags, uint8_t seq, const uint8_t *p, int n, long now)
{
    char h[3 * 64 + 1];

    if (g->other++ < LOG_OTHER) {
        hex(h, sizeof h, p, n < 64 ? n : 64);
        xpad_log("gip %s +%ld rx cmd %02x flags %02x seq %02x len %d: %s", g->name, now - g->t_open, cmd, flags,
                 seq, n, h);
    }
}

static void frame(gip_t *g, uint8_t cmd, uint8_t flags, uint8_t seq, const uint8_t *p, int n, long second,
                  long now, xpad_state *st, int *ret, gip_send_fn send, void *ctx)
{
    char h[3 * 64 + 1];

    switch (cmd) {
    case 0x01:                          /* the pad acknowledges: 00 CC FF Rl Rh 00 00 Ml Mh */
        if ((flags & 0x20) && n == 9 && p[1] == 0x06)
            auth_acked(g, (long)p[3] | (long)p[4] << 8, now, send, ctx);
        break;
    case 0x02:
        if (flags & 0x20) hello(g, p, n, now, st, ret, send, ctx);
        break;
    case 0x03:                          /* status: power and battery */
        if (!(flags & 0x20)) break;
        g->statuses++;
        if (n >= 1 && (g->statuses == 1 || p[0] != g->status))
            xpad_log("gip %s +%ld status %02x", g->name, now - g->t_open, p[0]);
        if (n >= 1) g->status = p[0];
        if (g->state == GIP_WAIT_UP && !identifying(g)) set_state(g, GIP_ACTIVE, now, "status");
        break;
    case 0x04:                          /* metadata: the answer to identify() */
        show(g, cmd, flags, seq, p, n, now);
        identified(g, flags, n, now, send, ctx);
        break;
    case 0x07:                          /* the Guide button: {down, 0x5B}; the Adaptive Controller says 2 */
        if ((flags & 0x20) && n >= 1) {
            st->buttons = (st->buttons & ~(uint32_t)XPAD_BTN_GUIDE) | (p[0] & 0x03 ? (uint32_t)XPAD_BTN_GUIDE : 0u);
            *ret |= GIP_RX_STATE | GIP_RX_INPUT;
            if (g->state == GIP_WAIT_UP && !identifying(g)) set_state(g, GIP_ACTIVE, now, "guide");
        }
        break;
    case 0x20:
        if (flags & 0x20) break;
        if (input(g, p, n, now, st)) {
            *ret |= GIP_RX_STATE | GIP_RX_INPUT;
            if (g->inputs++ < LOG_INPUTS) {
                hex(h, sizeof h, p, n < 64 ? n : 64);
                xpad_log("gip %s +%ld input %lu: %s", g->name, now - g->t_open, g->inputs, h);
            }
            if (g->state != GIP_ACTIVE && !identifying(g)) set_state(g, GIP_ACTIVE, now, "input");
        }
        break;
    case 0x06:                          /* security: the handshake */
        show(g, cmd, flags, seq, p, n, now);
        if (!(flags & 0x20)) break;
        if (flags & 0x80) auth_chunk(g, flags, second, p, n, now, send, ctx);
        else auth_message(g, p, n, now, send, ctx);
        break;
    default:                            /* the rest: shown */
        show(g, cmd, flags, seq, p, n, now);
        break;
    }
}

/* At most 4 bytes (28 bits); 0 if it runs past len or is longer. */
static int varint(const uint8_t *b, int len, int *pos, long *v)
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

int gip_receive(gip_t *g, const uint8_t *buf, int len, long now, xpad_state *st, gip_send_fn send, void *ctx)
{
    int off = 0, ret = 0;

    if (!buf) len = 0;
    g->t_rx = now;
    while (len - off >= 4) {
        const uint8_t *f = buf + off;
        uint8_t cmd = f[0], flags = f[1], seq = f[2];
        int pos = 3;
        long plen, second = 0;

        if (!varint(f, len - off, &pos, &plen) || ((flags & 0x80) && !varint(f, len - off, &pos, &second)) ||
            plen > len - off - pos) {
            char h[3 * 32 + 1];
            hex(h, sizeof h, f, len - off < 32 ? len - off : 32);
            xpad_log("gip %s +%ld rx frame cut short: %s", g->name, now - g->t_open, h);
            break;
        }
        g->frames++;
        if (flags & 0x80) {             /* chunked: the first chunk gives the total, the others the offset */
            long got, left;
            if (flags & 0x40) g->chunk_total = second;
            got = (flags & 0x40 ? 0 : second) + plen;
            left = g->chunk_total > got ? g->chunk_total - got : 0;
            if (cmd == 0x06 && !(flags & 0x0F)) {
                /* Security: xone's rule (bus/protocol.c gip_process_pkt_chunked):
                 * asked, or the last chunk with data, never the empty one. */
                if (plen > 0 && ((flags & 0x10) || got == g->chunk_total))
                    ack(g, cmd, flags, seq, got, left, now, send, ctx);
            } else if ((flags & 0x10) || (plen == 0 && !(flags & 0x40))) {
                /* The last chunk is acknowledged even unasked (as xone's fork does). */
                ack(g, cmd, flags, seq, got, left, now, send, ctx);
            }
        } else if (flags & 0x10) {
            ack(g, cmd, flags, seq, plen, 0, now, send, ctx);
        }
        /* The low bits name a module on the pad (a headset or chatpad in its
         * jack): acknowledged, but not the pad's own; its Hello must not be
         * taken for the pad starting over (SDL skips them too). */
        if (flags & 0x0F) {
            if (g->modules++ < LOG_OTHER)
                xpad_log("gip %s +%ld module %d: cmd %02x, left alone", g->name, now - g->t_open, flags & 0x0F, cmd);
        } else {
            frame(g, cmd, flags, seq, f + pos, (int)plen, second, now, st, &ret, send, ctx);
        }
        off += pos + (int)plen;
    }
    return ret;
}

/* ---- timers -------------------------------------------------------------------- */

void gip_init(gip_t *g, const char *name, uint16_t vid, uint16_t pid, int opts, long now)
{
    memset(g, 0, sizeof *g);
    g->name = name ? name : "?";
    g->vid = vid;
    g->pid = pid;
    g->opts = opts;
    g->state = GIP_WAIT_HELLO;
    g->t_open = g->t_state = now;
    gip_auth_init(&g->auth, g->name, now);
}

void gip_tick(gip_t *g, long now, gip_send_fn send, void *ctx)
{
    if (!g->reads) return;              /* nothing is written before the first read */
    if (g->auth.state == GIP_AUTH_RUNNING && now - g->auth.t_step >= T_AUTH_STEP) {
        xpad_log("gip %s +%ld auth v%d: no answer in %d ms", g->name, now - g->t_open, g->auth.version,
                 T_AUTH_STEP);
        gip_auth_end(&g->auth, GIP_AUTH_FAILED, "no answer");
        auth_fallback(g, now, send, ctx);
    }
    switch (g->state) {
    case GIP_WAIT_HELLO:
        if ((g->opts & GIP_OPT_BLIND) || never_hello(g)) {
            burst(g, now, send, ctx, "without a hello");
        } else if (waits_for_hello(g)) {
            if (!g->hint && now - g->t_open >= T_HINT) {
                g->hint = 1;
                xpad_log("gip %s +%ld no hello yet: press the controller's Xbox button", g->name, now - g->t_open);
            }
        } else if (now - g->t_open >= T_HELLO_WAIT) {
            burst(g, now, send, ctx, "no hello in 1 s");
        }
        break;
    case GIP_WAIT_UP:
        if (identifying(g)) {
            if (now - g->t_burst >= T_IDENTIFY) burst(g, now, send, ctx, "no identification reply in 100 ms");
            else if (now - g->t_state >= T_IDENTIFY_MAX)
                burst(g, now, send, ctx, "identification reply not over in 500 ms");
            break;
        }
        if (now - g->t_burst < T_UP || auth_busy(g, now)) break;   /* not while a handshake moves on */
        if (g->bursts < BURSTS) {
            burst(g, now, send, ctx, "no status or input in 2 s");
        } else {
            set_state(g, GIP_SILENT, now, "no answer to 3 bring-ups");
            xpad_log("gip %s: the pad does not come up; it is still read, and a hello brings it up again",
                     g->name);
        }
        break;
    case GIP_ACTIVE:
        if ((g->opts & GIP_OPT_KEEPALIVE) && now - g->t_keep >= T_KEEPALIVE) {
            uint8_t p[7];
            g->t_keep = now;
            p[0] = 0x05; p[1] = 0x20; p[2] = next_seq(&g->seq_sys); p[3] = 0x01; p[4] = 0x00;
            tx(g, "keepalive start", p, 5, now, send, ctx);
            p[0] = 0x0A; p[1] = 0x20; p[2] = next_seq(&g->seq_sys); p[3] = 0x03; p[4] = 0x00; p[5] = 0x01; p[6] = 0x14;
            tx(g, "keepalive led", p, 7, now, send, ctx);
            /* Never in the middle of a handshake (xone never resends it). */
            if (g->auth.state != GIP_AUTH_RUNNING) security_complete(g, "keepalive security", now, send, ctx);
        }
        break;
    default:
        break;
    }
}

int gip_up(const gip_t *g)
{
    return g->state == GIP_ACTIVE;
}

/* 09 00 SS 09 | 00 0F LT RT strong weak FF 00 FF: every motor enabled,
 * magnitudes 0..100 (GameController's scale, which GIP shares), on for
 * 2.55 s, no delay, repeated 255 times: in effect until the next frame.
 * A stop is the same frame with zero magnitudes. Some firmwares choke on
 * rumble frames closer than about 10 ms: g->t_rumble lets the caller pace
 * them. */
static uint8_t pct(uint8_t v) { return v > 100u ? (uint8_t)100u : v; }

int gip_rumble(gip_t *g, const xpad_rumble *r, long now, uint8_t *b, int max)
{
    if (!r || !b || max < 13) return 0;
    memset(b, 0, 13);
    b[0] = 0x09;
    b[2] = next_seq(&g->seq_vend);
    b[3] = 0x09;
    b[5] = 0x0F;
    b[6] = pct(r->lt);
    b[7] = pct(r->rt);
    b[8] = pct(r->strong);
    b[9] = pct(r->weak);
    b[10] = 0xFF;
    b[12] = 0xFF;
    g->t_rumble = now;
    return 13;
}

void gip_lost(const gip_t *g, long now)
{
    xpad_log("gip %s +%ld lost: %s, %lu hello, %lu status, %lu input, %d bring-up(s), last frame +%ld", g->name,
             now - g->t_open, gip_state_name(g->state), g->hellos, g->statuses, g->inputs, g->bursts,
             g->t_rx ? g->t_rx - g->t_open : -1);
    if (uses_auth(g))
        xpad_log("gip %s auth: %s, %lu authenticated, %lu given up", g->name, gip_auth_state_name(g->auth.state),
                 g->auth_done, g->auth_fallbacks);
}
