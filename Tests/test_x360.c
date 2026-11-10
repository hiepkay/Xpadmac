/*
 * Xbox 360 wired (XInput) messages: every button bit, the axis and trigger
 * extremes, the idle message giving the exact idle Xbox report, the
 * messages that are not input, short buffers, and the rumble and LED bytes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "xpad.h"

static const uint8_t idle_msg[20] = { 0x00, 0x14 };
static const uint8_t idle_xbox[17] = { 0x01, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static void put_s16(uint8_t *p, int v)
{
    unsigned u = (unsigned)(v < 0 ? v + 65536 : v);
    p[0] = (uint8_t)(u & 0xFFu);
    p[1] = (uint8_t)(u >> 8);
}

static void test_idle(void)
{
    xpad_state s;
    uint8_t r[17];

    TEST("x360 idle");
    s.buttons = 0xFFFFFFFFu;
    CHECK_EQ(xpad_x360_parse(idle_msg, sizeof idle_msg, &s), 1);
    CHECK_EQ(s.buttons, 0);
    CHECK_EQ(s.lx, XPAD_AXIS_CENTER);
    CHECK_EQ(s.ly, XPAD_AXIS_CENTER);
    CHECK_EQ(s.rx, XPAD_AXIS_CENTER);
    CHECK_EQ(s.ry, XPAD_AXIS_CENTER);
    CHECK_EQ(s.lt, 0);
    CHECK_EQ(s.rt, 0);
    CHECK_EQ(xpad_xbox_build_input(&s, r, sizeof r), 17);
    CHECK_MEM(r, idle_xbox, 17);

    /* Longer transfers (the endpoint's packet is 32 bytes) parse the same. */
    {
        uint8_t big[32] = { 0x00, 0x14 };
        CHECK_EQ(xpad_x360_parse(big, sizeof big, &s), 1);
        CHECK_EQ(xpad_xbox_build_input(&s, r, sizeof r), 17);
        CHECK_MEM(r, idle_xbox, 17);
    }
}

static void test_buttons(void)
{
    static const struct { unsigned byte, bit; uint32_t button; } map[] = {
        { 2, 0, XPAD_BTN_DPAD_UP }, { 2, 1, XPAD_BTN_DPAD_DOWN }, { 2, 2, XPAD_BTN_DPAD_LEFT },
        { 2, 3, XPAD_BTN_DPAD_RIGHT }, { 2, 4, XPAD_BTN_MENU }, { 2, 5, XPAD_BTN_VIEW },
        { 2, 6, XPAD_BTN_LS }, { 2, 7, XPAD_BTN_RS },
        { 3, 0, XPAD_BTN_LB }, { 3, 1, XPAD_BTN_RB }, { 3, 2, XPAD_BTN_GUIDE }, { 3, 3, 0 },
        { 3, 4, XPAD_BTN_A }, { 3, 5, XPAD_BTN_B }, { 3, 6, XPAD_BTN_X }, { 3, 7, XPAD_BTN_Y },
    };
    uint32_t all = 0;
    xpad_state s;
    uint8_t m[20], r[17];
    size_t i;

    TEST("x360 each button");
    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        memcpy(m, idle_msg, sizeof m);
        m[map[i].byte] = (uint8_t)(1u << map[i].bit);
        CHECK_EQ(xpad_x360_parse(m, sizeof m, &s), 1);
        CHECK_EQ(s.buttons, map[i].button);
        CHECK_EQ(s.lx, XPAD_AXIS_CENTER);
        CHECK_EQ(s.lt, 0);
        all |= map[i].button;
    }

    TEST("x360 all buttons");
    memcpy(m, idle_msg, sizeof m);
    m[2] = m[3] = 0xFF;
    CHECK_EQ(xpad_x360_parse(m, sizeof m, &s), 1);
    CHECK_EQ(s.buttons, all);
    CHECK_EQ(s.buttons & XPAD_BTN_SHARE, 0);                 /* the 360 pad has none */

    /* Into the Xbox report: A, B, X, Y, LB, RB; View, Menu, Guide, LS, RS. */
    memcpy(m, idle_msg, sizeof m);
    m[3] = 0xF3;                                              /* A B X Y LB RB */
    m[2] = 0xF0;                                              /* Menu View LS RS */
    CHECK_EQ(xpad_x360_parse(m, sizeof m, &s), 1);
    CHECK_EQ(xpad_xbox_build_input(&s, r, sizeof r), 17);
    CHECK_EQ(r[13], 0);
    CHECK_EQ(r[14], 0xDB);                                    /* b0 b1 b3 b4 b6 b7 */
    CHECK_EQ(r[15], 0x6C);                                    /* b2 b3 b5 b6 */
    CHECK_EQ(r[16], 0);
    m[2] = 0; m[3] = 0x04;                                    /* Guide */
    CHECK_EQ(xpad_x360_parse(m, sizeof m, &s), 1);
    CHECK_EQ(xpad_xbox_build_input(&s, r, sizeof r), 17);
    CHECK_EQ(r[15], 0x10);

    TEST("x360 d-pad to hat");
    {
        static const struct { uint8_t bits, hat; } hats[] = {
            { 0x01, 1 }, { 0x09, 2 }, { 0x08, 3 }, { 0x0A, 4 },
            { 0x02, 5 }, { 0x06, 6 }, { 0x04, 7 }, { 0x05, 8 },
        };
        for (i = 0; i < sizeof hats / sizeof hats[0]; i++) {
            memcpy(m, idle_msg, sizeof m);
            m[2] = hats[i].bits;
            CHECK_EQ(xpad_x360_parse(m, sizeof m, &s), 1);
            CHECK_EQ(xpad_xbox_build_input(&s, r, sizeof r), 17);
            CHECK_EQ(r[13], hats[i].hat);
        }
    }
}

static void test_axes(void)
{
    static const struct { int raw; unsigned x, y; } axes[] = {
        { -32768, 0x0000, 0xFFFF },     /* full left / full down */
        { 0, 0x8000, 0x8000 },
        { 32767, 0xFFFF, 0x0000 },      /* full right / full up */
        { -1, 0x7FFF, 0x8001 },
        { 1, 0x8001, 0x7FFF },
    };
    xpad_state s;
    uint8_t m[20], r[17];
    size_t i;

    TEST("x360 sticks");
    for (i = 0; i < sizeof axes / sizeof axes[0]; i++) {
        memcpy(m, idle_msg, sizeof m);
        put_s16(m + 6, axes[i].raw);
        put_s16(m + 8, axes[i].raw);
        put_s16(m + 10, axes[i].raw);
        put_s16(m + 12, axes[i].raw);
        CHECK_EQ(xpad_x360_parse(m, sizeof m, &s), 1);
        CHECK_EQ(s.lx, axes[i].x);
        CHECK_EQ(s.ly, axes[i].y);
        CHECK_EQ(s.rx, axes[i].x);
        CHECK_EQ(s.ry, axes[i].y);
        CHECK_EQ(s.buttons, 0);
    }
    /* Each axis lands in its own place. */
    memcpy(m, idle_msg, sizeof m);
    put_s16(m + 6, 32767);              /* LX right */
    put_s16(m + 8, 32767);              /* LY up */
    put_s16(m + 10, -32768);            /* RX left */
    put_s16(m + 12, -32768);            /* RY down */
    CHECK_EQ(xpad_x360_parse(m, sizeof m, &s), 1);
    CHECK_EQ(xpad_xbox_build_input(&s, r, sizeof r), 17);
    {
        static const uint8_t want[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF };
        CHECK_MEM(r + 1, want, 8);
    }

    TEST("x360 triggers");
    memcpy(m, idle_msg, sizeof m);
    m[4] = 0xFF;
    m[5] = 0x80;
    CHECK_EQ(xpad_x360_parse(m, sizeof m, &s), 1);
    CHECK_EQ(s.lt, XPAD_TRIGGER_MAX);
    CHECK_EQ(s.rt, xpad_trigger_from_u8(0x80));
    CHECK_EQ(s.lx, XPAD_AXIS_CENTER);
    CHECK_EQ(xpad_xbox_build_input(&s, r, sizeof r), 17);
    CHECK_EQ(r[9] | r[10] << 8, 1023);
    CHECK_EQ(r[11] | r[12] << 8, 514);
    m[4] = 0x01;
    m[5] = 0x00;
    CHECK_EQ(xpad_x360_parse(m, sizeof m, &s), 1);
    CHECK_EQ(s.lt, xpad_trigger_from_u8(1));
    CHECK_EQ(s.rt, 0);
}

static void test_ignored(void)
{
    static const uint8_t led[3] = { 0x01, 0x03, 0x06 };
    static const uint8_t m02[3] = { 0x02, 0x03, 0x00 };
    static const uint8_t rumble[3] = { 0x03, 0x03, 0x03 };
    static const uint8_t headset[3] = { 0x08, 0x03, 0x00 };
    uint8_t padded[20] = { 0x01, 0x03, 0x06, 0xFF, 0xFF, 0xFF, 0xFF };
    xpad_state s, before;

    TEST("x360 other messages ignored");
    xpad_state_reset(&s);
    s.buttons = XPAD_BTN_A;
    s.lx = 0x1234;
    before = s;
    CHECK_EQ(xpad_x360_parse(led, sizeof led, &s), 0);
    CHECK_EQ(xpad_x360_parse(m02, sizeof m02, &s), 0);
    CHECK_EQ(xpad_x360_parse(rumble, sizeof rumble, &s), 0);
    CHECK_EQ(xpad_x360_parse(headset, sizeof headset, &s), 0);
    CHECK_EQ(xpad_x360_parse(padded, sizeof padded, &s), 0);     /* long, but not type 0 */
    padded[0] = 0x08;
    CHECK_EQ(xpad_x360_parse(padded, sizeof padded, &s), 0);
    CHECK_MEM(&s, &before, sizeof s);

    TEST("x360 short and null");
    CHECK_EQ(xpad_x360_parse(idle_msg, 19, &s), 0);
    CHECK_EQ(xpad_x360_parse(idle_msg, 0, &s), 0);
    CHECK_EQ(xpad_x360_parse(NULL, 20, &s), 0);
    CHECK_EQ(xpad_x360_parse(idle_msg, 20, NULL), 0);
    CHECK_MEM(&s, &before, sizeof s);
}

static void test_output(void)
{
    xpad_rumble m = { 0, 0, 0, 0 };
    uint8_t out[16];
    unsigned v;

    TEST("x360 rumble bytes");
    m.strong = 100;
    m.weak = 50;
    m.lt = 100;                         /* no trigger motors: ignored */
    m.rt = 100;
    memset(out, 0xAA, sizeof out);
    CHECK_EQ(xpad_x360_build_rumble(&m, out, sizeof out), 8);
    {
        static const uint8_t want[9] = { 0x00, 0x08, 0x00, 0xFF, 0x80, 0x00, 0x00, 0x00, 0xAA };
        CHECK_MEM(out, want, 9);        /* nothing written past 8 bytes */
    }
    m.strong = 0;
    m.weak = 100;
    CHECK_EQ(xpad_x360_build_rumble(&m, out, 8), 8);
    CHECK_EQ(out[3], 0x00);
    CHECK_EQ(out[4], 0xFF);
    m.strong = m.weak = 0;
    CHECK_EQ(xpad_x360_build_rumble(&m, out, 8), 8);
    {
        static const uint8_t stop[8] = { 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        CHECK_MEM(out, stop, 8);
    }
    m.strong = 200;                     /* above 100 is 100 */
    m.weak = 1;
    CHECK_EQ(xpad_x360_build_rumble(&m, out, 8), 8);
    CHECK_EQ(out[3], 0xFF);
    CHECK_EQ(out[4], 3);                /* 2.55, rounded */
    for (v = 1; v <= 100; v++) {
        uint8_t prev;
        m.strong = (uint8_t)(v - 1);
        xpad_x360_build_rumble(&m, out, 8);
        prev = out[3];
        m.strong = (uint8_t)v;
        xpad_x360_build_rumble(&m, out, 8);
        if (out[3] <= prev) {
            CHECK(out[3] > prev);
            break;
        }
    }

    TEST("x360 rumble from xbox report");
    {
        static const uint8_t gc[9] = { 0x03, 0x0F, 0x00, 0x00, 0x64, 0x32, 0x0A, 0x00, 0x00 };
        CHECK_EQ(xpad_xbox_parse_rumble(3, gc, sizeof gc, &m), 1);
        CHECK_EQ(xpad_x360_build_rumble(&m, out, 8), 8);
        CHECK_EQ(out[3], 0xFF);
        CHECK_EQ(out[4], 0x80);
    }

    TEST("x360 rumble bounds");
    CHECK_EQ(xpad_x360_build_rumble(&m, out, 7), 0);
    CHECK_EQ(xpad_x360_build_rumble(NULL, out, 8), 0);
    CHECK_EQ(xpad_x360_build_rumble(&m, NULL, 8), 0);

    TEST("x360 led bytes");
    memset(out, 0xAA, sizeof out);
    CHECK_EQ(xpad_x360_build_led(XPAD_X360_LED_PLAYER1_FLASH, out, sizeof out), 3);
    {
        static const uint8_t want[4] = { 0x01, 0x03, 0x02, 0xAA };
        CHECK_MEM(out, want, 4);
    }
    CHECK_EQ(xpad_x360_build_led(9, out, 3), 3);
    CHECK_EQ(out[2], 9);
    CHECK_EQ(xpad_x360_build_led(2, out, 2), 0);
    CHECK_EQ(xpad_x360_build_led(2, NULL, 3), 0);
}

int main(void)
{
    test_idle();
    test_buttons();
    test_axes();
    test_ignored();
    test_output();
    return test_done("x360");
}
