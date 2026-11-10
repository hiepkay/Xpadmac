/* Xbox One and Series controllers over USB: the host's side of the Gaming
 * Input Protocol (MS-GIPUSB), as Linux's xpad and xone and SDL speak it.
 *
 * Adapted for XPadMac from a GIP module (src/gip.c/gip.h) written on top of
 * AnyPad PS4 and never published there (GPL-3.0-or-later; originals kept in
 * docs/research/anypad_gip.c.orig and anypad_gip.h.orig; see NOTICE). Changes: input fills an
 * xpad_state with full precision instead of a PS-style pad_state; rumble
 * takes an xpad_rumble (0..100, incl. trigger motors); logging goes through
 * xpad_log() so the same file builds for DriverKit and for the host.
 *
 * A frame is: command, flags, sequence number, then the payload's length as
 * a varint (7 bits a byte, bit 7 meaning more), and for a chunked frame a
 * second varint (the total on the first chunk, the offset on the others).
 * Flags: 0x0F client, 0x10 acknowledgement asked, 0x20 system message,
 * 0x40 first chunk, 0x80 chunked. One USB transfer may hold several frames.
 *
 * Bringing a pad up: it announces itself with a Hello (0x02) until the host
 * answers. The host asks it to identify itself (0x04; the pad answers with
 * its metadata) and, once that answer is in or after 100 ms without one,
 * sends START (0x05 power on), then the Guide LED (0x0A), security (0x06)
 * and a rumble stop (0x09); then the pad sends its status (0x03) and input
 * (0x20). Security is either "complete" (06 20 SS 02 01 00) at once, or,
 * for VID 0x3537 pads (a vendor quirk) and with GIP_OPT_AUTH, xone's real
 * authentication handshake (gip_auth.h), whose "complete" follows the pad's
 * verified Finished value; if the pad does not answer it, or the handshake
 * fails, the plain "complete" is sent instead, and the bring-ups after it
 * send that "complete" at once until the pad identifies itself again (a
 * Hello in GIP_WAIT_HELLO or GIP_SILENT, or a re-announce while ACTIVE).
 * GIP_OPT_SERIAL adds xone's serial-number request after security. A pad
 * brought up without a
 * Hello gets START and the rest only. While identifying, the state is
 * GIP_WAIT_UP. Pure: it builds and reads bytes; the caller moves them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XPAD_GIP_H
#define XPAD_GIP_H

#include "xpad.h"
#include "gip_auth.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { GIP_WAIT_HELLO, GIP_WAIT_UP, GIP_ACTIVE, GIP_SILENT };

#define GIP_OPT_BLIND     2     /* bring the pad up without waiting for its Hello */
#define GIP_OPT_KEEPALIVE 4     /* resend START, LED and security every 20 s while up */
#define GIP_OPT_AUTH      8     /* authenticate (xone's handshake) whatever the vendor */
#define GIP_OPT_FAKE_AUTH 16    /* never authenticate: "security complete" at once, VID 0x3537 too */
#define GIP_OPT_SERIAL    32    /* ask for the serial number (1E 20 SS 01 04) after security, as xone does */

/* gip_receive's result. */
#define GIP_RX_STATE 1          /* *st changed: publish it */
#define GIP_RX_INPUT 2          /* the pad sent input or its Guide button */

/* Sends p[0..len) to the pad (OUT endpoint, in order); returns 1 if queued. */
typedef int (*gip_send_fn)(void *ctx, const uint8_t *p, int len);

typedef struct {
    const char *name;           /* for the log */
    uint16_t vid, pid;
    int opts;
    int state;
    uint8_t seq_sys, seq_sec, seq_vend;         /* 05/0A/04, 06, 09/4D: never 0 */
    long t_open, t_state, t_burst, t_keep, t_rumble, t_rx;
    int reads;                  /* set to 1 by the caller once reads are queued: no write before */
    int bursts;
    int hint;
    long chunk_total;
    uint8_t status;
    int has_share;              /* the pad's input report carries a Share byte (Series layout) */
    unsigned long hellos, statuses, inputs, acks, frames, other, modules;
    /* Authentication (0x06): the handshake, a long host message being sent
     * chunk by chunk as the pad acknowledges it, a long pad message being
     * reassembled. */
    gip_auth_t auth;
    uint8_t auth_out[GIP_AUTH_MSG_MAX];
    int auth_out_len;           /* 0: nothing waiting for the pad's acknowledgement */
    uint8_t auth_out_seq;
    uint8_t auth_in[GIP_AUTH_IN_MAX];
    long auth_in_total;
    int auth_in_on;             /* a chunked message from the pad is coming in */
    int auth_skip;              /* given up since the last identification: bring-ups send "complete" */
    unsigned long auth_done, auth_fallbacks;
} gip_t;

/* `now` is a monotonic time in milliseconds everywhere below. */
void gip_init(gip_t *g, const char *name, uint16_t vid, uint16_t pid, int opts, long now);

/* One USB transfer from the pad: every frame in it. Acknowledges what asks
 * for it, moves the bring-up on, and updates *st from input and the Guide
 * button (and resets it when the pad announces itself again). */
int  gip_receive(gip_t *g, const uint8_t *buf, int len, long now, xpad_state *st,
                 gip_send_fn send, void *ctx);

/* Timers, on every pass (call every 10..50 ms). */
void gip_tick(gip_t *g, long now, gip_send_fn send, void *ctx);

int  gip_up(const gip_t *g);

/* A rumble frame (13 bytes: 09 00 SS 09 00 0F LT RT strong weak FF 00 FF)
 * for *r, with the next sequence number. Returns its length, or 0. */
int  gip_rumble(gip_t *g, const xpad_rumble *r, long now, uint8_t *b, int max);

const char *gip_state_name(int state);
void gip_lost(const gip_t *g, long now);

/* Logging hook, implemented once per platform (driver: os_log; host: stderr). */
void xpad_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Random bytes for authentication, implemented once per platform (driver
 * and tool: arc4random_buf; tests: a deterministic source). */
void xpad_random(void *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
