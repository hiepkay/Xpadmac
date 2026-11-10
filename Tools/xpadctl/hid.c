/*
 * xpadctl hid — raw HID input reports plus the decoded xpad_state
 *
 * Goes through IOHIDManager. Matching is limited to the gamepad / joystick
 * top-level collections: many pads are composite devices with a keyboard or
 * mouse interface too, and opening keyboards needs the Input Monitoring
 * permission (and would capture typing).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "xpadctl.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HID usages: Generic Desktop page, and the application collections a game
 * controller declares. */
#define USAGE_PAGE_GENERIC_DESKTOP 0x01
static const int controller_usages[] = { 0x04 /* Joystick */, 0x05 /* Game Pad */,
                                         0x08 /* Multi-axis Controller */ };

/* ---- shared IOHIDManager plumbing ------------------------------------------- */

typedef struct {
    uint16_t vid, pid;
    int any_pid;
} hid_filter;

static void dict_set_int(CFMutableDictionaryRef d, CFStringRef key, int v)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}

/* One matching dictionary per (filter, controller usage). */
static CFArrayRef matching_for(const hid_filter *filters, size_t nfilters)
{
    CFMutableArrayRef arr = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    size_t i, u;

    for (i = 0; i < nfilters; i++) {
        for (u = 0; u < sizeof controller_usages / sizeof controller_usages[0]; u++) {
            CFMutableDictionaryRef d = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            dict_set_int(d, CFSTR(kIOHIDVendorIDKey), filters[i].vid);
            if (!filters[i].any_pid) dict_set_int(d, CFSTR(kIOHIDProductIDKey), filters[i].pid);
            dict_set_int(d, CFSTR(kIOHIDDeviceUsagePageKey), USAGE_PAGE_GENERIC_DESKTOP);
            dict_set_int(d, CFSTR(kIOHIDDeviceUsageKey), controller_usages[u]);
            CFArrayAppendValue(arr, d);
            CFRelease(d);
        }
    }
    return arr;
}

static long long dev_int(IOHIDDeviceRef dev, CFStringRef key, long long fallback)
{
    long long v;
    return xpadctl_cf_number(IOHIDDeviceGetProperty(dev, key), &v) ? v : fallback;
}

static void dev_str(IOHIDDeviceRef dev, CFStringRef key, char *out, size_t out_max)
{
    if (!xpadctl_cf_string(IOHIDDeviceGetProperty(dev, key), out, out_max)) out[0] = '\0';
}

/* Parses --vid/--pid (and the flags each command adds) out of argv. */
typedef struct {
    int have_vid, have_pid;
    uint16_t vid, pid;
    int all;
    double seconds;
} hid_args;

static int parse_hid_args(const char *cmd, int argc, char **argv, hid_args *a, int allow_extra)
{
    int i;

    memset(a, 0, sizeof *a);
    for (i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "--vid") || !strcmp(s, "--pid")) {
            uint16_t v;
            if (i + 1 >= argc) return xpadctl_usage_error(cmd, "%s needs a value (hexadecimal, e.g. 0e6f)", s);
            if (!xpadctl_parse_hex16(argv[++i], &v))
                return xpadctl_usage_error(cmd, "invalid value for %s: \"%s\" (hexadecimal, 0..FFFF)", s, argv[i]);
            if (s[2] == 'v') { a->vid = v; a->have_vid = 1; }
            else { a->pid = v; a->have_pid = 1; }
        } else if (allow_extra && !strcmp(s, "--all")) {
            a->all = 1;
        } else if (allow_extra && !strcmp(s, "--seconds")) {
            if (i + 1 >= argc || !xpadctl_parse_seconds(argv[i + 1], &a->seconds))
                return xpadctl_usage_error(cmd, "--seconds needs a non-negative number of seconds");
            i++;
        } else {
            return xpadctl_usage_error(cmd, "unknown option: %s", s);
        }
    }
    if (a->have_pid && !a->have_vid)
        return xpadctl_usage_error(cmd, "--pid needs --vid");
    return 0;
}

/* ---- xpadctl hid -------------------------------------------------------------- */

typedef enum { DECODE_RAW, DECODE_XBOX } decoder_kind;

typedef struct {
    IOHIDDeviceRef dev;
    int index;                  /* #n in the output */
    uint16_t vid, pid;
    decoder_kind kind;
    uint8_t *buf;               /* IOHIDManager writes reports here */
    CFIndex buf_len;
    uint8_t last[256];
    size_t last_len;
    int have_last;
    unsigned long reports, printed, undecoded;
} hid_dev;

#define HID_MAX_DEVS 8

static struct {
    hid_dev *devs[HID_MAX_DEVS];
    int next_index;
    int print_all;
    unsigned long total_reports;
    int ever_seen;
} H;

static const char *decoder_name(decoder_kind k)
{
    switch (k) {
    case DECODE_XBOX:    return "Xbox Wireless Controller, report 1";
    default:             return "no decoder (raw bytes only)";
    }
}

static void on_input(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id,
                     uint8_t *report, CFIndex len)
{
    hid_dev *d = ctx;
    char hex[3 * 64 + 8], decoded[160];
    xpad_state st;
    size_t n = len > 0 ? (size_t)len : 0;
    int changed;

    (void)sender; (void)type; (void)report_id;
    if (result != kIOReturnSuccess || !d) return;
    d->reports++;
    H.total_reports++;

    if (n > sizeof d->last) {
        changed = 1;                    /* too long to remember: always shown */
    } else {
        changed = !d->have_last || n != d->last_len || memcmp(report, d->last, n) != 0;
        memcpy(d->last, report, n);
        d->last_len = n;
        d->have_last = 1;
    }
    if (!changed && !H.print_all) return;
    d->printed++;

    xpadctl_hex(hex, sizeof hex, report, n);
    decoded[0] = '\0';
    xpad_state_reset(&st);
    if (d->kind == DECODE_XBOX) {
        if (xpadctl_xbox_decode(report, n, &st)) xpadctl_format_state(&st, decoded, sizeof decoded);
        else { d->undecoded++; snprintf(decoded, sizeof decoded, "(not report 1, %u bytes)", XPAD_XBOX_INPUT_LEN); }
    }

    printf("[%7.3f] #%d %04x:%04x (%2zu B) %s\n", xpadctl_now_ms() / 1000.0, d->index, d->vid, d->pid, n, hex);
    if (decoded[0]) printf("          %s\n", decoded);
}

static void on_matched(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef dev)
{
    char product[128], transport[64], source[128];
    hid_dev *d;
    long long max_in;
    int slot;

    (void)ctx; (void)result; (void)sender;
    for (slot = 0; slot < HID_MAX_DEVS; slot++)
        if (H.devs[slot] && H.devs[slot]->dev == dev) return;   /* already known (matched by two usages) */
    for (slot = 0; slot < HID_MAX_DEVS && H.devs[slot]; slot++) {}  /* first free slot */
    if (slot == HID_MAX_DEVS) {
        fprintf(stderr, "xpadctl hid: too many devices, ignoring one.\n");
        return;
    }

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->dev = dev;
    d->index = ++H.next_index;
    d->vid = (uint16_t)dev_int(dev, CFSTR(kIOHIDVendorIDKey), 0);
    d->pid = (uint16_t)dev_int(dev, CFSTR(kIOHIDProductIDKey), 0);
    dev_str(dev, CFSTR(kIOHIDProductKey), product, sizeof product);
    dev_str(dev, CFSTR(kIOHIDTransportKey), transport, sizeof transport);
    dev_str(dev, CFSTR("XPadSource"), source, sizeof source);

    if (source[0] || (d->vid == XPAD_XBOX_VID && d->pid == XPAD_XBOX_PID)) d->kind = DECODE_XBOX;
    else d->kind = DECODE_RAW;

    max_in = dev_int(dev, CFSTR(kIOHIDMaxInputReportSizeKey), 64);
    d->buf_len = max_in > 0 && max_in <= 4096 ? (CFIndex)max_in : 64;
    d->buf = calloc(1, (size_t)d->buf_len);
    if (!d->buf) { free(d); return; }
    H.devs[slot] = d;
    H.ever_seen = 1;

    printf("[%7.3f] #%d connected: %04x:%04x \"%s\" transport=%s%s%s%s, max input report %ld B\n",
           xpadctl_now_ms() / 1000.0, d->index, d->vid, d->pid, product[0] ? product : "?",
           transport[0] ? transport : "?", source[0] ? ", XPadSource=\"" : "", source, source[0] ? "\"" : "",
           (long)d->buf_len);
    printf("          decoder: %s\n", decoder_name(d->kind));
    IOHIDDeviceRegisterInputReportCallback(dev, d->buf, d->buf_len, on_input, d);
}

static void on_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef dev)
{
    int slot;

    (void)ctx; (void)result; (void)sender;
    for (slot = 0; slot < HID_MAX_DEVS; slot++) {
        hid_dev *d = H.devs[slot];
        if (!d || d->dev != dev) continue;
        printf("[%7.3f] #%d disconnected (%lu reports)\n", xpadctl_now_ms() / 1000.0, d->index, d->reports);
        IOHIDDeviceRegisterInputReportCallback(dev, d->buf, d->buf_len, NULL, NULL);
        free(d->buf);
        free(d);
        H.devs[slot] = NULL;
    }
}

int xpadctl_cmd_hid(int argc, char **argv)
{
    hid_args a;
    hid_filter filters[1];
    size_t nfilters;
    IOHIDManagerRef mgr;
    CFArrayRef match;
    IOReturn r;
    int rc, hinted = 0, slot;

    if ((rc = parse_hid_args("hid", argc, argv, &a, 1))) return rc;
    if (a.have_vid) {
        filters[0].vid = a.vid;
        filters[0].pid = a.pid;
        filters[0].any_pid = !a.have_pid;
        nfilters = 1;
    } else {
        /* What the XPad driver publishes. */
        filters[0].vid = XPAD_XBOX_VID; filters[0].pid = XPAD_XBOX_PID; filters[0].any_pid = 0;
        nfilters = 1;
    }
    H.print_all = a.all;

    mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) { fprintf(stderr, "xpadctl hid: could not create an IOHIDManager.\n"); return 1; }
    match = matching_for(filters, nfilters);
    IOHIDManagerSetDeviceMatchingMultiple(mgr, match);
    CFRelease(match);
    IOHIDManagerRegisterDeviceMatchingCallback(mgr, on_matched, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(mgr, on_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    /* Opens every matching device now and later (hot-plug). */
    r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "xpadctl hid: IOHIDManagerOpen failed: 0x%08x.\n", r);
        if (r == kIOReturnNotPermitted)
            fprintf(stderr, "  → Give Terminal the Input Monitoring permission in "
                            "System Settings › Privacy & Security, then run this again.\n");
    }

    if (a.have_vid) {
        if (a.have_pid) printf("Listening to HID %04x:%04x", a.vid, a.pid);
        else printf("Listening to HID from vendor %04x (any PID)", a.vid);
    } else {
        printf("Listening to HID 045e:0b13 (XPad / Xbox Wireless Controller)");
    }
    printf("%s. Press Ctrl-C to stop.\n", a.all ? ", printing every report" : ", printing only when a report changes (--all for every report)");

    xpadctl_set_deadline(a.seconds);
    while (!xpadctl_should_stop()) {
        xpadctl_run_loop_for(0.25);
        if (!hinted && !H.ever_seen && xpadctl_now_ms() > 3000) {
            hinted = 1;
            printf("No device yet. A GIP or Xbox 360 controller only shows up as HID once the XPadMac driver has it:\n"
                   "  run `xpadctl doctor` to see what macOS sees.\n");
        }
    }

    printf("\nReceived %lu reports.\n", H.total_reports);
    for (slot = 0; slot < HID_MAX_DEVS; slot++) {
        hid_dev *d = H.devs[slot];
        if (!d) continue;
        printf("  #%d %04x:%04x: %lu reports, %lu printed%s\n", d->index, d->vid, d->pid, d->reports, d->printed,
               d->undecoded ? ", some reports could not be decoded" : "");
        IOHIDDeviceRegisterInputReportCallback(d->dev, d->buf, d->buf_len, NULL, NULL);
    }
    IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);
    for (slot = 0; slot < HID_MAX_DEVS; slot++) {
        if (!H.devs[slot]) continue;
        free(H.devs[slot]->buf);
        free(H.devs[slot]);
        H.devs[slot] = NULL;
    }
    return 0;
}
