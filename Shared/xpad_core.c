/*
 * XPadMac shared core: the canonical state, the scaling helpers, and the
 * "Xbox Wireless Controller" (045E:0B13, Bluetooth LE layout) input and
 * rumble reports. See xpad.h for the contract and docs/DESIGN.md for why
 * this identity and this layout.
 *
 * Pure C99 with no OS headers: it builds for DriverKit and for the host.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "xpad.h"

/* ---- state ------------------------------------------------------------------ */

void xpad_state_reset(xpad_state *s)
{
    if (!s) return;
    s->buttons = 0;
    s->lx = s->ly = s->rx = s->ry = (uint16_t)XPAD_AXIS_CENTER;
    s->lt = s->rt = 0;
}

/* ---- scaling ------------------------------------------------------------------
 *
 * Every stick mapping is anchored at three points: full left/up -> 0,
 * centre -> 0x8000 exactly, full right/down -> 0xFFFF. An idle pad then
 * produces the very same report as xpad_state_reset(), whatever it speaks. */

uint16_t xpad_axis_from_u8(uint8_t v)
{
    /* 0x00..0x80 -> 0x0000..0x8000 by 256 a step; 0x80..0xFF -> 0x8000..0xFFFF
     * by 0x7FFF/0x7F (about 258) a step. Strictly increasing. */
    if (v <= 0x80u) return (uint16_t)((unsigned)v << 8);
    return (uint16_t)(0x8000u + ((unsigned)v - 0x80u) * 0x7FFFu / 0x7Fu);
}

uint16_t xpad_axis_from_s16(int16_t v)
{
    return (uint16_t)((int32_t)v + 32768);      /* -32768 -> 0, 0 -> 0x8000, 32767 -> 0xFFFF */
}

uint16_t xpad_axis_from_s16_inverted(int16_t v)
{
    /* GIP's Y axes grow upwards; ours grow downwards (0 = up). A plain
     * 32767 - v would put the centre at 0x7FFF, so each half is scaled on
     * its own: 32767 -> 0, 0 -> 0x8000, -32768 -> 0xFFFF, rounded to the
     * nearest and monotonic. */
    if (v >= 0) return (uint16_t)(0x8000u - ((uint32_t)v * 0x8000u + 16383u) / 32767u);
    return (uint16_t)(0x8000u + ((uint32_t)(-(int32_t)v) * 0x7FFFu + 16384u) / 32768u);
}

uint16_t xpad_trigger_from_u8(uint8_t v)
{
    /* Bit replication: 0x00 -> 0, 0x80 -> 514, 0xFF -> 1023, strictly increasing. */
    return (uint16_t)(((unsigned)v << 2) | ((unsigned)v >> 6));
}

/* ---- the Xbox input report (report ID 1, 17 bytes) ----------------------------- */

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t trigger_clamp(uint16_t v)
{
    return v > XPAD_TRIGGER_MAX ? (uint16_t)XPAD_TRIGGER_MAX : v;
}

/* The hat switch from the four D-pad bits. Opposing directions cancel each
 * other per axis (as Linux's xpad reports its hat axes), so Up+Down is no
 * direction and Up+Down+Left is West. Index: Up 1, Down 2, Left 4, Right 8.
 * Values: 0 none, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW. */
static const uint8_t hat_from_dpad[16] = {
    /* -     U  D  UD  L  UL DL UDL  R  UR DR UDR LR ULR DLR UDLR */
       0,    1, 5, 0,  7, 8, 6, 7,   3, 2, 4, 3,  0, 1,  5,  0,
};

static uint8_t hat_of(uint32_t b)
{
    unsigned i = ((b & XPAD_BTN_DPAD_UP) ? 1u : 0u) | ((b & XPAD_BTN_DPAD_DOWN) ? 2u : 0u) |
                 ((b & XPAD_BTN_DPAD_LEFT) ? 4u : 0u) | ((b & XPAD_BTN_DPAD_RIGHT) ? 8u : 0u);

    return hat_from_dpad[i];
}

/* One button: the bit it sets in the byte it lives in, if pressed. */
static uint8_t bit_if(uint32_t buttons, uint32_t which, unsigned bit)
{
    return (buttons & which) ? (uint8_t)(1u << bit) : 0u;
}

size_t xpad_xbox_build_input(const xpad_state *s, uint8_t *out, size_t out_max)
{
    uint32_t b;

    if (!s || !out || out_max < XPAD_XBOX_INPUT_LEN) return 0;
    b = s->buttons;

    out[0] = (uint8_t)XPAD_XBOX_INPUT_ID;
    put_le16(out + 1, s->lx);
    put_le16(out + 3, s->ly);
    put_le16(out + 5, s->rx);
    put_le16(out + 7, s->ry);
    put_le16(out + 9, trigger_clamp(s->lt));
    put_le16(out + 11, trigger_clamp(s->rt));
    out[13] = hat_of(b);
    /* HID buttons 1..15 = bits 0..14 of bytes 14..15 (byte 15 bit 7 is padding). */
    out[14] = (uint8_t)(bit_if(b, XPAD_BTN_A, 0) | bit_if(b, XPAD_BTN_B, 1) | bit_if(b, XPAD_BTN_X, 3) |
                        bit_if(b, XPAD_BTN_Y, 4) | bit_if(b, XPAD_BTN_LB, 6) | bit_if(b, XPAD_BTN_RB, 7));
    out[15] = (uint8_t)(bit_if(b, XPAD_BTN_VIEW, 2) | bit_if(b, XPAD_BTN_MENU, 3) |
                        bit_if(b, XPAD_BTN_GUIDE, 4) | bit_if(b, XPAD_BTN_LS, 5) | bit_if(b, XPAD_BTN_RS, 6));
    out[16] = bit_if(b, XPAD_BTN_SHARE, 0);        /* Consumer Record */
    return XPAD_XBOX_INPUT_LEN;
}

/* ---- the Xbox rumble report (report ID 3, 9 bytes) ------------------------------
 *
 * The "DC Enable Actuators" nibble names the motors the report drives, with
 * the bits xpadneo uses: 0x01 weak (right), 0x02 strong (left), 0x04 right
 * trigger, 0x08 left trigger. GameController sends 0x0F while a haptic plays
 * and 0xFF (with zero magnitudes) to stop. */

#define ACT_WEAK   0x01u
#define ACT_STRONG 0x02u
#define ACT_RT     0x04u
#define ACT_LT     0x08u
#define MAGNITUDE_MAX 100u

static uint8_t magnitude(const uint8_t *d, unsigned enable, unsigned which, int at)
{
    uint8_t v = d[at];

    if (!(enable & which)) return 0;
    return v > MAGNITUDE_MAX ? (uint8_t)MAGNITUDE_MAX : v;
}

int xpad_xbox_parse_rumble(uint8_t report_id, const uint8_t *rep, size_t len, xpad_rumble *out)
{
    const uint8_t *d;           /* the report's body: d[0] is the enable byte */
    unsigned enable;

    if (!rep || !out) return 0;
    if (len >= XPAD_XBOX_RUMBLE_LEN) {              /* starts with the report ID */
        report_id = rep[0];
        d = rep + 1;
    } else if (len == XPAD_XBOX_RUMBLE_LEN - 1u) {  /* the ID came separately */
        d = rep;
    } else {
        return 0;
    }
    if (report_id != XPAD_XBOX_RUMBLE_ID) return 0;

    /* The high nibble is padding, except that GameController's stop is 0xFF. */
    enable = d[0] == 0xFFu ? 0u : (d[0] & 0x0Fu);
    if (!enable) {                                  /* stop, or nothing enabled */
        out->strong = out->weak = out->lt = out->rt = 0;
        return 1;
    }
    /* d[5..7] (on, off, loop count) are not forwarded: every consumer here
     * holds a level until the next report changes it, and the drivers stop
     * it themselves after xpad_xbox_rumble_play_ms. */
    out->lt = magnitude(d, enable, ACT_LT, 1);
    out->rt = magnitude(d, enable, ACT_RT, 2);
    out->strong = magnitude(d, enable, ACT_STRONG, 3);
    out->weak = magnitude(d, enable, ACT_WEAK, 4);
    return 1;
}

uint32_t xpad_xbox_rumble_play_ms(uint8_t report_id, const uint8_t *rep, size_t len)
{
    xpad_rumble r;
    const uint8_t *d;

    if (!xpad_xbox_parse_rumble(report_id, rep, len, &r)) return 0;
    if (!(r.strong | r.weak | r.lt | r.rt)) return 0;
    d = len >= XPAD_XBOX_RUMBLE_LEN ? rep + 1 : rep;
    /* At most (255 + 255) x 256 x 10 ms = 1 305 600 ms: no overflow. */
    return ((uint32_t)d[5] + d[6]) * ((uint32_t)d[7] + 1u) * 10u;
}
