/*
 * The scaling helpers' edges, the Xbox input report (xpad_xbox_build_input)
 * and rumble report (xpad_xbox_parse_rumble). Button and hat positions are
 * taken from the published descriptor itself (hid_walk), so the report and
 * the descriptor are checked against each other, for every one of the
 * 65536 button sets.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "hid_walk.h"
#include "test.h"
#include "xpad.h"

static const uint8_t idle_report[17] = { 0x01, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* Which descriptor usage each canonical button is (the Android layout
 * Apple's plugin reads; xpad.h): page 9 buttons, and Consumer Record. */
static const struct {
    uint32_t bit;
    uint16_t page, usage;
} button_usage[] = {
    { XPAD_BTN_A, 0x09, 1 },     { XPAD_BTN_B, 0x09, 2 },      { XPAD_BTN_X, 0x09, 4 },
    { XPAD_BTN_Y, 0x09, 5 },     { XPAD_BTN_LB, 0x09, 7 },     { XPAD_BTN_RB, 0x09, 8 },
    { XPAD_BTN_VIEW, 0x09, 11 }, { XPAD_BTN_MENU, 0x09, 12 },  { XPAD_BTN_GUIDE, 0x09, 13 },
    { XPAD_BTN_LS, 0x09, 14 },   { XPAD_BTN_RS, 0x09, 15 },    { XPAD_BTN_SHARE, 0x0C, 0xB2 },
};
#define NBUTTONS (sizeof button_usage / sizeof button_usage[0])

/* The hat, worked out independently of the code's table: each axis is
 * -1/0/+1 with opposing directions cancelling, then the 8 compass points. */
static unsigned expected_hat(uint32_t b)
{
    int dx = ((b & XPAD_BTN_DPAD_RIGHT) ? 1 : 0) - ((b & XPAD_BTN_DPAD_LEFT) ? 1 : 0);
    int dy = ((b & XPAD_BTN_DPAD_DOWN) ? 1 : 0) - ((b & XPAD_BTN_DPAD_UP) ? 1 : 0);
    static const unsigned compass[3][3] = {   /* [dy + 1][dx + 1] */
        { 8, 1, 2 },                          /* up:   NW N NE */
        { 7, 0, 3 },                          /*       W  -  E */
        { 6, 5, 4 },                          /* down: SW S SE */
    };

    return compass[dy + 1][dx + 1];
}

static void test_scaling(void)
{
    unsigned v;
    long i;

    TEST("scale u8 axis");
    CHECK_EQ(xpad_axis_from_u8(0x00), 0x0000);
    CHECK_EQ(xpad_axis_from_u8(0x80), 0x8000);
    CHECK_EQ(xpad_axis_from_u8(0xFF), 0xFFFF);
    CHECK_EQ(xpad_axis_from_u8(0x7F), 0x7F00);
    CHECK_EQ(xpad_axis_from_u8(0x81), 0x8102);
    for (v = 1; v < 256; v++) CHECK(xpad_axis_from_u8((uint8_t)v) > xpad_axis_from_u8((uint8_t)(v - 1)));

    TEST("scale u8 trigger");
    CHECK_EQ(xpad_trigger_from_u8(0x00), 0);
    CHECK_EQ(xpad_trigger_from_u8(0x80), 514);
    CHECK_EQ(xpad_trigger_from_u8(0xFF), XPAD_TRIGGER_MAX);
    for (v = 1; v < 256; v++) CHECK(xpad_trigger_from_u8((uint8_t)v) > xpad_trigger_from_u8((uint8_t)(v - 1)));

    TEST("scale s16 axis");
    CHECK_EQ(xpad_axis_from_s16(-32768), 0x0000);
    CHECK_EQ(xpad_axis_from_s16(0), 0x8000);
    CHECK_EQ(xpad_axis_from_s16(32767), 0xFFFF);
    CHECK_EQ(xpad_axis_from_s16(-1), 0x7FFF);
    for (i = -32767; i <= 32767; i++)
        if (xpad_axis_from_s16((int16_t)i) != xpad_axis_from_s16((int16_t)(i - 1)) + 1) {
            CHECK_EQ(xpad_axis_from_s16((int16_t)i), xpad_axis_from_s16((int16_t)(i - 1)) + 1);
            break;
        }

    TEST("scale s16 axis inverted");
    CHECK_EQ(xpad_axis_from_s16_inverted(32767), 0x0000);     /* full up */
    CHECK_EQ(xpad_axis_from_s16_inverted(0), 0x8000);         /* centre, exactly */
    CHECK_EQ(xpad_axis_from_s16_inverted(-32768), 0xFFFF);    /* full down */
    CHECK_EQ(xpad_axis_from_s16_inverted(1), 0x7FFF);
    CHECK_EQ(xpad_axis_from_s16_inverted(-1), 0x8001);
    for (i = -32767; i <= 32767; i++)
        if (xpad_axis_from_s16_inverted((int16_t)i) > xpad_axis_from_s16_inverted((int16_t)(i - 1))) {
            CHECK(!"inverted axis not monotonic");
            break;
        }
    /* Symmetric about the centre to within two steps (the halves differ by
     * one value: 32767 above zero, 32768 below). */
    for (i = 1; i <= 32767; i++) {
        long up = 0x8000 - (long)xpad_axis_from_s16_inverted((int16_t)i);
        long down = (long)xpad_axis_from_s16_inverted((int16_t)-i) - 0x8000;
        if (up - down > 2 || down - up > 2) {
            CHECK_EQ(up, down);
            break;
        }
    }
}

static void test_idle_and_axes(void)
{
    xpad_state s;
    uint8_t r[32];

    TEST("xbox idle report");
    memset(&s, 0xA5, sizeof s);
    xpad_state_reset(&s);
    CHECK_EQ(s.buttons, 0);
    CHECK_EQ(s.lx, 0x8000); CHECK_EQ(s.ly, 0x8000); CHECK_EQ(s.rx, 0x8000); CHECK_EQ(s.ry, 0x8000);
    CHECK_EQ(s.lt, 0); CHECK_EQ(s.rt, 0);
    memset(r, 0xEE, sizeof r);
    CHECK_EQ(xpad_xbox_build_input(&s, r, sizeof r), 17);
    CHECK_MEM(r, idle_report, 17);
    CHECK_EQ(r[17], 0xEE);                     /* nothing written past the report */

    TEST("xbox axes and triggers");
    s.lx = 0x1234; s.ly = 0xFEDC; s.rx = 0x0001; s.ry = 0xFFFF; s.lt = 0x3FF; s.rt = 0x155;
    CHECK_EQ(xpad_xbox_build_input(&s, r, 17), 17);
    {
        static const uint8_t want[17] = { 0x01, 0x34, 0x12, 0xDC, 0xFE, 0x01, 0x00, 0xFF, 0xFF,
                                          0xFF, 0x03, 0x55, 0x01, 0x00, 0x00, 0x00, 0x00 };
        CHECK_MEM(r, want, 17);
    }
    s.lt = 1024; s.rt = 0xFFFF;                /* beyond 10 bits: clamped, never wrapped */
    xpad_xbox_build_input(&s, r, 17);
    CHECK_EQ(r[9] | r[10] << 8, 1023);
    CHECK_EQ(r[11] | r[12] << 8, 1023);

    TEST("xbox build bounds");
    CHECK_EQ(xpad_xbox_build_input(&s, r, 16), 0);
    CHECK_EQ(xpad_xbox_build_input(NULL, r, 17), 0);
    CHECK_EQ(xpad_xbox_build_input(&s, NULL, 17), 0);
}

static void test_every_button_set(void)
{
    hid_layout l;
    unsigned offset[NBUTTONS];
    unsigned set, i, bad = 0;
    xpad_state s;
    uint8_t r[17];

    TEST("xbox buttons x hat, all 65536");
    CHECK_EQ(hid_walk(xpad_xbox_desc, XPAD_XBOX_DESC_LEN, &l), 0);
    for (i = 0; i < NBUTTONS; i++) {
        const hid_field *f = hid_find_usage(&l, HID_INPUT, button_usage[i].page, button_usage[i].usage);
        CHECK(f != NULL && f->size == 1);
        offset[i] = f ? f->offset + 8 : 0;     /* + the report ID byte */
    }
    {
        const hid_field *hat = hid_find_usage(&l, HID_INPUT, 0x01, 0x39);
        CHECK(hat != NULL && hat->offset + 8 == 13 * 8 && hat->size == 4);
    }

    xpad_state_reset(&s);
    s.lx = 0x0102; s.ly = 0x0304; s.rx = 0x0506; s.ry = 0x0708; s.lt = 0x109; s.rt = 0x20B;
    for (set = 0; set < 0x10000u; set++) {
        uint8_t want[17];

        s.buttons = set | 0xFFFF0000u;          /* bits xpad.h does not define are ignored */
        if (xpad_xbox_build_input(&s, r, sizeof r) != 17) { bad++; continue; }

        memset(want, 0, sizeof want);
        want[0] = 0x01;
        want[1] = 0x02; want[2] = 0x01; want[3] = 0x04; want[4] = 0x03;
        want[5] = 0x06; want[6] = 0x05; want[7] = 0x08; want[8] = 0x07;
        want[9] = 0x09; want[10] = 0x01; want[11] = 0x0B; want[12] = 0x02;
        want[13] = (uint8_t)expected_hat(set);
        for (i = 0; i < NBUTTONS; i++)
            if (set & button_usage[i].bit) want[offset[i] / 8] |= (uint8_t)(1u << (offset[i] % 8));
        if (memcmp(r, want, 17) != 0) {
            if (bad++ < 5) {
                char what[64];
                snprintf(what, sizeof what, "buttons %04x", set);
                CHECK_MEM(r, want, 17);
                test_fail(__FILE__, __LINE__, what);
            }
        }
    }
    CHECK_EQ(bad, 0);

    /* The hat, spelled out (xpad.h: 0 none, 1 N ... 8 NW). */
    {
        static const struct { uint32_t b; unsigned hat; } h[] = {
            { 0, 0 },
            { XPAD_BTN_DPAD_UP, 1 },
            { XPAD_BTN_DPAD_UP | XPAD_BTN_DPAD_RIGHT, 2 },
            { XPAD_BTN_DPAD_RIGHT, 3 },
            { XPAD_BTN_DPAD_DOWN | XPAD_BTN_DPAD_RIGHT, 4 },
            { XPAD_BTN_DPAD_DOWN, 5 },
            { XPAD_BTN_DPAD_DOWN | XPAD_BTN_DPAD_LEFT, 6 },
            { XPAD_BTN_DPAD_LEFT, 7 },
            { XPAD_BTN_DPAD_UP | XPAD_BTN_DPAD_LEFT, 8 },
            { XPAD_BTN_DPAD_UP | XPAD_BTN_DPAD_DOWN, 0 },
            { XPAD_BTN_DPAD_LEFT | XPAD_BTN_DPAD_RIGHT, 0 },
            { XPAD_BTN_DPAD_UP | XPAD_BTN_DPAD_DOWN | XPAD_BTN_DPAD_LEFT | XPAD_BTN_DPAD_RIGHT, 0 },
        };
        TEST("xbox hat values");
        for (i = 0; i < sizeof h / sizeof h[0]; i++) {
            s.buttons = h[i].b;
            xpad_xbox_build_input(&s, r, sizeof r);
            CHECK_EQ(r[13], h[i].hat);
        }
    }

    /* And the documented bytes for each button (xpad.h), so the walker
     * itself is checked too. */
    {
        static const struct { uint32_t b; unsigned byte, mask; } d[] = {
            { XPAD_BTN_A, 14, 0x01 },     { XPAD_BTN_B, 14, 0x02 },    { XPAD_BTN_X, 14, 0x08 },
            { XPAD_BTN_Y, 14, 0x10 },     { XPAD_BTN_LB, 14, 0x40 },   { XPAD_BTN_RB, 14, 0x80 },
            { XPAD_BTN_VIEW, 15, 0x04 },  { XPAD_BTN_MENU, 15, 0x08 }, { XPAD_BTN_GUIDE, 15, 0x10 },
            { XPAD_BTN_LS, 15, 0x20 },    { XPAD_BTN_RS, 15, 0x40 },   { XPAD_BTN_SHARE, 16, 0x01 },
        };
        TEST("xbox button bytes");
        for (i = 0; i < sizeof d / sizeof d[0]; i++) {
            uint8_t want[17];
            s.buttons = d[i].b;
            xpad_xbox_build_input(&s, r, sizeof r);
            memcpy(want, r, 13);
            memset(want + 13, 0, 4);
            want[d[i].byte] = (uint8_t)d[i].mask;
            CHECK_MEM(r, want, 17);
        }
    }
}

static void expect_rumble(const uint8_t *rep, size_t len, uint8_t id, int ok, unsigned strong, unsigned weak,
                          unsigned lt, unsigned rt)
{
    xpad_rumble m;

    memset(&m, 0xCC, sizeof m);
    CHECK_EQ(xpad_xbox_parse_rumble(id, rep, len, &m), ok);
    if (!ok) {
        CHECK(m.strong == 0xCC && m.weak == 0xCC && m.lt == 0xCC && m.rt == 0xCC);   /* untouched */
        return;
    }
    CHECK_EQ(m.strong, strong);
    CHECK_EQ(m.weak, weak);
    CHECK_EQ(m.lt, lt);
    CHECK_EQ(m.rt, rt);
}

static void test_rumble(void)
{
    /* What GameController sends while a haptic plays: 0x0F, LT RT strong weak, 100 ms. */
    static const uint8_t active[9] = { 0x03, 0x0F, 0x00, 0x00, 0x64, 0x32, 0x0A, 0x00, 0x00 };
    static const uint8_t triggers[9] = { 0x03, 0x0F, 0x1E, 0x28, 0x05, 0x06, 0x0A, 0x00, 0x00 };
    static const uint8_t stop[9] = { 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t stop_dirty[9] = { 0x03, 0xFF, 0x64, 0x64, 0x64, 0x64, 0x0A, 0x00, 0x00 };
    static const uint8_t none[9] = { 0x03, 0x00, 0x64, 0x64, 0x64, 0x64, 0x0A, 0x00, 0x00 };
    static const uint8_t mains[9] = { 0x03, 0x03, 0x1E, 0x28, 0x64, 0x32, 0x0A, 0x00, 0x00 };
    static const uint8_t trigs[9] = { 0x03, 0x0C, 0x1E, 0x28, 0x64, 0x32, 0x0A, 0x00, 0x00 };
    static const uint8_t weak_only[9] = { 0x03, 0x01, 0x1E, 0x28, 0x64, 0x32, 0x0A, 0x00, 0x00 };
    static const uint8_t pad_bits[9] = { 0x03, 0x1F, 0x01, 0x02, 0x03, 0x04, 0x0A, 0x00, 0x00 };
    static const uint8_t over[9] = { 0x03, 0x0F, 0x65, 0xFF, 0xC8, 0x80, 0x0A, 0x00, 0x00 };
    static const uint8_t other_id[9] = { 0x02, 0x0F, 0x00, 0x00, 0x64, 0x32, 0x0A, 0x00, 0x00 };
    xpad_rumble m;

    TEST("rumble parse");
    expect_rumble(active, 9, 0, 1, 100, 50, 0, 0);
    expect_rumble(active, 9, 3, 1, 100, 50, 0, 0);
    expect_rumble(triggers, 9, 3, 1, 5, 6, 30, 40);
    expect_rumble(active + 1, 8, 3, 1, 100, 50, 0, 0);      /* the ID given apart */
    expect_rumble(triggers + 1, 8, 3, 1, 5, 6, 30, 40);
    TEST("rumble stop");
    expect_rumble(stop, 9, 3, 1, 0, 0, 0, 0);
    expect_rumble(stop_dirty, 9, 3, 1, 0, 0, 0, 0);
    expect_rumble(none, 9, 3, 1, 0, 0, 0, 0);
    expect_rumble(stop + 1, 8, 3, 1, 0, 0, 0, 0);
    TEST("rumble enable mask");
    expect_rumble(mains, 9, 3, 1, 100, 50, 0, 0);
    expect_rumble(trigs, 9, 3, 1, 0, 0, 30, 40);
    expect_rumble(weak_only, 9, 3, 1, 0, 50, 0, 0);
    expect_rumble(pad_bits, 9, 3, 1, 3, 4, 1, 2);           /* high nibble is padding */
    TEST("rumble clamp");
    expect_rumble(over, 9, 3, 1, 100, 100, 100, 100);
    TEST("rumble not ours");
    expect_rumble(other_id, 9, 3, 0, 0, 0, 0, 0);           /* the byte in the report wins */
    expect_rumble(active + 1, 8, 2, 0, 0, 0, 0, 0);
    expect_rumble(active + 1, 8, 0, 0, 0, 0, 0, 0);
    expect_rumble(active, 7, 3, 0, 0, 0, 0, 0);
    expect_rumble(active, 0, 3, 0, 0, 0, 0, 0);
    CHECK_EQ(xpad_xbox_parse_rumble(3, NULL, 9, &m), 0);
    CHECK_EQ(xpad_xbox_parse_rumble(3, active, 9, NULL), 0);
    {
        uint8_t longer[12];
        memcpy(longer, active, 9);
        memset(longer + 9, 0x77, 3);
        expect_rumble(longer, sizeof longer, 0, 1, 100, 50, 0, 0);   /* a padded buffer */
    }

    TEST("rumble play time");
    {
        static const uint8_t xpadneo[9] = { 0x03, 0x0F, 0x00, 0x00, 0x64, 0x64, 0xFF, 0x00, 0xEB };
        static const uint8_t longest[9] = { 0x03, 0x0F, 0x01, 0x01, 0x01, 0x01, 0xFF, 0xFF, 0xFF };
        static const uint8_t pulses[9] = { 0x03, 0x02, 0x00, 0x00, 0x32, 0x00, 0x05, 0x03, 0x02 };
        static const uint8_t no_time[9] = { 0x03, 0x0F, 0x00, 0x00, 0x64, 0x32, 0x00, 0x00, 0x00 };
        static const uint8_t zeros[9] = { 0x03, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00 };
        static const uint8_t masked[9] = { 0x03, 0x0C, 0x00, 0x00, 0x64, 0x32, 0x0A, 0x00, 0x00 };

        CHECK_EQ(xpad_xbox_rumble_play_ms(0, active, 9), 100);           /* GameController */
        CHECK_EQ(xpad_xbox_rumble_play_ms(3, active + 1, 8), 100);
        CHECK_EQ(xpad_xbox_rumble_play_ms(3, triggers, 9), 100);
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, xpadneo, 9), 255u * 236u * 10u);
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, longest, 9), 510u * 256u * 10u);
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, pulses, 9), (5u + 3u) * 3u * 10u);
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, no_time, 9), 0);
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, stop, 9), 0);
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, stop_dirty, 9), 0);
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, none, 9), 0);
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, zeros, 9), 0);
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, masked, 9), 0);             /* enabled motors at 0 */
        CHECK_EQ(xpad_xbox_rumble_play_ms(0, other_id, 9), 0);
        CHECK_EQ(xpad_xbox_rumble_play_ms(2, active + 1, 8), 0);
        CHECK_EQ(xpad_xbox_rumble_play_ms(3, active, 7), 0);
        CHECK_EQ(xpad_xbox_rumble_play_ms(3, NULL, 9), 0);
    }
}

int main(void)
{
    test_scaling();
    test_idle_and_axes();
    test_every_button_set();
    test_rumble();
    return test_done("xbox");
}
