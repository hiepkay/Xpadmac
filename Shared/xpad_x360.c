/*
 * Xbox 360 wired (XInput) pads: the 20-byte input message of interface 0
 * (FF/5D/01), the rumble message and the LED message. The layout is in
 * xpad.h; it is the one Linux's xpad.c (XTYPE_XBOX360) and SDL's
 * SDL_hidapi_xbox360.c read.
 *
 * Pure C99 with no OS headers: it builds for DriverKit and for the host.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "xpad.h"

/* Bit i of byte 2, then of byte 3 -> XPAD_BTN_*. Byte 3 bit 3 is unused. */
static const uint32_t x360_buttons[16] = {
    XPAD_BTN_DPAD_UP, XPAD_BTN_DPAD_DOWN, XPAD_BTN_DPAD_LEFT, XPAD_BTN_DPAD_RIGHT,
    XPAD_BTN_MENU, XPAD_BTN_VIEW, XPAD_BTN_LS, XPAD_BTN_RS,
    XPAD_BTN_LB, XPAD_BTN_RB, XPAD_BTN_GUIDE, 0,
    XPAD_BTN_A, XPAD_BTN_B, XPAD_BTN_X, XPAD_BTN_Y,
};

static int16_t get_s16(const uint8_t *p)
{
    uint16_t u = (uint16_t)((unsigned)p[0] | (unsigned)p[1] << 8);
    /* Two's complement without implementation-defined conversion. */
    return u >= 0x8000u ? (int16_t)(-(int32_t)(0x10000u - u)) : (int16_t)u;
}

int xpad_x360_parse(const uint8_t *msg, size_t len, xpad_state *s)
{
    unsigned raw, i;
    uint32_t b = 0;

    if (!msg || !s || len < XPAD_X360_INPUT_LEN) return 0;
    /* Only the type byte decides, as in Linux's xpad360_process_packet: the
     * length byte (0x14) is not checked. */
    if (msg[0] != XPAD_X360_MSG_INPUT) return 0;

    raw = (unsigned)msg[2] | (unsigned)msg[3] << 8;
    for (i = 0; i < 16; i++)
        if (raw & (1u << i)) b |= x360_buttons[i];

    s->buttons = b;
    s->lt = xpad_trigger_from_u8(msg[4]);
    s->rt = xpad_trigger_from_u8(msg[5]);
    s->lx = xpad_axis_from_s16(get_s16(msg + 6));
    s->ly = xpad_axis_from_s16_inverted(get_s16(msg + 8));     /* up positive -> 0 = up */
    s->rx = xpad_axis_from_s16(get_s16(msg + 10));
    s->ry = xpad_axis_from_s16_inverted(get_s16(msg + 12));
    return 1;
}

/* 0..100 (GameController's scale) -> 0..255, rounded; above 100 is 100. */
static uint8_t x360_motor(uint8_t v)
{
    if (v >= 100u) return 0xFFu;
    return (uint8_t)(((unsigned)v * 255u + 50u) / 100u);
}

size_t xpad_x360_build_rumble(const xpad_rumble *r, uint8_t *out, size_t max)
{
    if (!r || !out || max < XPAD_X360_RUMBLE_LEN) return 0;
    out[0] = 0x00;
    out[1] = (uint8_t)XPAD_X360_RUMBLE_LEN;
    out[2] = 0x00;
    out[3] = x360_motor(r->strong);         /* left, low frequency */
    out[4] = x360_motor(r->weak);           /* right, high frequency */
    out[5] = out[6] = out[7] = 0x00;
    return XPAD_X360_RUMBLE_LEN;
}

size_t xpad_x360_build_led(uint8_t n, uint8_t *out, size_t max)
{
    if (!out || max < XPAD_X360_LED_LEN) return 0;
    out[0] = 0x01;
    out[1] = (uint8_t)XPAD_X360_LED_LEN;
    out[2] = n;
    return XPAD_X360_LED_LEN;
}
