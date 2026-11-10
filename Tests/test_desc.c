/*
 * The Xbox (045E:0B13, BLE) report descriptor the driver publishes:
 * byte for byte the captured one, and laid out the way xpad.h and Apple's
 * XboxOneHIDServicePlugin expect.
 *
 *   test_desc RESEARCH_DIR      (docs/research, for the captured .bin)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "hid_walk.h"
#include "test.h"
#include "xpad.h"

#include <stdio.h>

static uint16_t crc16_arc(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static void test_bytes(const char *research)
{
    char path[1024];
    uint8_t bin[XPAD_XBOX_DESC_LEN + 16];
    size_t got = 0;
    FILE *f;

    TEST("descriptor bytes");
    CHECK_EQ(sizeof xpad_xbox_desc, 283);
    CHECK_EQ(XPAD_XBOX_DESC_LEN, 283);
    CHECK_EQ(crc16_arc(xpad_xbox_desc, sizeof xpad_xbox_desc), 0x931D);
    CHECK_EQ(crc16_arc((const uint8_t *)"123456789", 9), 0xBB3D);     /* the CRC-16/ARC check value */

    /* Verbatim: the very bytes of the capture the C array was generated from. */
    snprintf(path, sizeof path, "%s/xbox_series_ble_0b13_descriptor.bin", research);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) {
        got = fread(bin, 1, sizeof bin, f);
        fclose(f);
    }
    CHECK_EQ(got, XPAD_XBOX_DESC_LEN);
    if (got == XPAD_XBOX_DESC_LEN) CHECK_MEM(xpad_xbox_desc, bin, XPAD_XBOX_DESC_LEN);
}

static void expect_field(const hid_layout *l, int type, uint16_t page, uint16_t usage, unsigned offset,
                         unsigned size, uint8_t id)
{
    const hid_field *f = hid_find_usage(l, type, page, usage);

    CHECK(f != NULL);
    if (!f) {
        fprintf(stderr, "    missing usage %04x:%04x\n", page, usage);
        return;
    }
    CHECK_EQ(f->report_id, id);
    CHECK_EQ(f->offset, offset);
    CHECK_EQ(f->size, size);
}

static void test_layout(void)
{
    hid_layout l;
    const hid_report *in1, *out3;
    unsigned b;
    size_t i;

    TEST("descriptor layout");
    CHECK_EQ(hid_walk(xpad_xbox_desc, XPAD_XBOX_DESC_LEN, &l), 0);
    CHECK_EQ(l.collections_open, 0);

    /* Exactly two reports: input 1 (16 bytes + ID) and output 3 (8 bytes + ID). */
    CHECK_EQ(l.nreports, 2);
    in1 = hid_find_report(&l, HID_INPUT, XPAD_XBOX_INPUT_ID);
    out3 = hid_find_report(&l, HID_OUTPUT, XPAD_XBOX_RUMBLE_ID);
    CHECK(in1 != NULL);
    CHECK(out3 != NULL);
    if (in1) {
        CHECK_EQ(in1->bits, 16 * 8);
        CHECK_EQ(in1->bits / 8 + 1, XPAD_XBOX_INPUT_LEN);
    }
    if (out3) {
        CHECK_EQ(out3->bits, 8 * 8);
        CHECK_EQ(out3->bits / 8 + 1, XPAD_XBOX_RUMBLE_LEN);
    }

    /* Input 1, bit offsets after the ID byte; xpad.h's byte k is bit (k-1)*8. */
    expect_field(&l, HID_INPUT, 0x01, 0x30, 0, 16, 1);       /* X: bytes 1..2 */
    expect_field(&l, HID_INPUT, 0x01, 0x31, 16, 16, 1);      /* Y: 3..4 */
    expect_field(&l, HID_INPUT, 0x01, 0x32, 32, 16, 1);      /* Z (right X): 5..6 */
    expect_field(&l, HID_INPUT, 0x01, 0x35, 48, 16, 1);      /* Rz (right Y): 7..8 */
    expect_field(&l, HID_INPUT, 0x02, 0xC5, 64, 10, 1);      /* Brake (LT): 9..10 */
    expect_field(&l, HID_INPUT, 0x02, 0xC4, 80, 10, 1);      /* Accelerator (RT): 11..12 */
    expect_field(&l, HID_INPUT, 0x01, 0x39, 96, 4, 1);       /* hat: 13 low nibble */
    for (b = 1; b <= 15; b++)                                /* buttons 1..15: 14..15 */
        expect_field(&l, HID_INPUT, 0x09, (uint16_t)b, 104 + b - 1, 1, 1);
    expect_field(&l, HID_INPUT, 0x0C, 0xB2, 120, 1, 1);      /* Record (Share): 16 bit 0 */

    {
        const hid_field *x = hid_find_usage(&l, HID_INPUT, 0x01, 0x30);
        const hid_field *lt = hid_find_usage(&l, HID_INPUT, 0x02, 0xC5);
        const hid_field *hat = hid_find_usage(&l, HID_INPUT, 0x01, 0x39);
        if (x) { CHECK_EQ(x->logical_min, 0); CHECK_EQ(x->logical_max, 65535); }
        if (lt) { CHECK_EQ(lt->logical_min, 0); CHECK_EQ(lt->logical_max, XPAD_TRIGGER_MAX); }
        if (hat) {
            CHECK_EQ(hat->logical_min, 1);               /* 1..8, anything else (0) = null */
            CHECK_EQ(hat->logical_max, 8);
            CHECK(hat->flags & 0x40);                    /* has a null state */
        }
    }

    /* What Apple's XboxOneHIDServicePlugin looks for: Brake and Accelerator
     * (Android layout), no Consumer AC Back (View in the button field), and
     * Consumer Record (Share present). */
    CHECK(hid_find_usage(&l, HID_INPUT, 0x02, 0xC5) != NULL);
    CHECK(hid_find_usage(&l, HID_INPUT, 0x02, 0xC4) != NULL);
    CHECK(hid_find_usage(&l, HID_INPUT, 0x0C, 0x224) == NULL);
    CHECK(hid_find_usage(&l, HID_INPUT, 0x0C, 0xB2) != NULL);

    /* Output 3 (PID page): enable nibble, 4 magnitudes 0..100, duration, delay, loop. */
    expect_field(&l, HID_OUTPUT, 0x0F, 0x97, 0, 4, 3);
    expect_field(&l, HID_OUTPUT, 0x0F, 0x50, 40, 8, 3);
    expect_field(&l, HID_OUTPUT, 0x0F, 0xA7, 48, 8, 3);
    expect_field(&l, HID_OUTPUT, 0x0F, 0x7C, 56, 8, 3);
    {
        unsigned mags = 0;
        for (i = 0; i < l.nfields; i++) {
            const hid_field *f = &l.fields[i];
            if (f->type == HID_OUTPUT && f->page == 0x0F && f->usage == 0x70) {
                CHECK_EQ(f->offset, 8 + 8 * mags);       /* bytes 2..5: LT RT strong weak */
                CHECK_EQ(f->size, 8);
                CHECK_EQ(f->logical_max, 100);
                mags++;
            }
        }
        CHECK_EQ(mags, 4);
    }

    /* Every data bit belongs to exactly one field, padding included. */
    {
        unsigned covered[2] = { 0, 0 };
        for (i = 0; i < l.nfields; i++) {
            const hid_field *f = &l.fields[i];
            covered[f->type == HID_INPUT ? 0 : 1] += f->size;
        }
        CHECK_EQ(covered[0], 128);
        CHECK_EQ(covered[1], 64);
    }
}

int main(int argc, char **argv)
{
    test_bytes(argc > 1 ? argv[1] : "../docs/research");
    test_layout();
    return test_done("descriptor");
}
