/*
 * A small HID report-descriptor walker for the tests: it lays out every
 * report the way a host's HID parser does (global and local items, usage
 * ranges, report IDs, constant padding) and lists each field's usage, bit
 * offset and size, so the tests can check the descriptor and the report
 * builder against each other instead of against hand-copied offsets.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XPAD_HID_WALK_H
#define XPAD_HID_WALK_H

#include <stddef.h>
#include <stdint.h>

enum { HID_INPUT, HID_OUTPUT, HID_FEATURE };

typedef struct {
    int type;               /* HID_INPUT / HID_OUTPUT / HID_FEATURE */
    uint8_t report_id;
    uint16_t page, usage;   /* 0, 0 for constant padding */
    unsigned offset, size;  /* in bits, from the start of the report's data (after its ID) */
    unsigned flags;         /* the main item's data: bit 0 constant, bit 1 variable, bit 6 null state */
    int32_t logical_min, logical_max;
} hid_field;

typedef struct {
    int type;
    uint8_t report_id;
    unsigned bits;          /* data, without the ID byte */
} hid_report;

#define HID_MAX_FIELDS  128
#define HID_MAX_REPORTS 8

typedef struct {
    hid_field fields[HID_MAX_FIELDS];
    size_t nfields;
    hid_report reports[HID_MAX_REPORTS];
    size_t nreports;
    int collections_open;   /* 0 after a balanced descriptor */
    int items;              /* items walked */
} hid_layout;

/* Walks d[0..n). Returns 0 on success, or -(byte offset + 1) of the item it
 * could not take (cut short, unsupported, out of room). */
int hid_walk(const uint8_t *d, size_t n, hid_layout *out);

/* The report of that type and ID, or NULL. */
const hid_report *hid_find_report(const hid_layout *l, int type, uint8_t id);

/* The first field with that usage, or NULL. */
const hid_field *hid_find_usage(const hid_layout *l, int type, uint16_t page, uint16_t usage);

#endif
