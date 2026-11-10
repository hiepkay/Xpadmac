/*
 * XPadMac driver: helpers shared by the driver classes.
 *
 *  - XPadLog: os_log with the "XPad: " prefix every driver message carries.
 *  - XPadNowMs: the monotonic millisecond clock gip.c runs on.
 *  - XPadMapping: CPU access to an IOMemoryDescriptor, mapped once and kept
 *    for as long as the same descriptor comes back (the HID superclass reuses
 *    its interrupt-IN buffer for every report).
 *  - XPadCreateXboxDescription: the "Xbox Wireless Controller" identity
 *    (045E:0B13, Transport BluetoothLowEnergy) both HID classes publish.
 *  - XPadServeInputReport / XPadParseRumble: the parts of getReport and
 *    setReport the two HID classes share.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XPadCommon_h
#define XPadCommon_h

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <os/log.h>

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSDictionary.h>
#include <HIDDriverKit/IOHIDDeviceTypes.h>

#include "xpad.h"

#define XPadLog(fmt, ...) os_log(OS_LOG_DEFAULT, "XPad: " fmt, ##__VA_ARGS__)

/* Monotonic milliseconds (does not advance while the Mac sleeps, like
 * mach_absolute_time): the time base of gip.c's timers. */
static inline long XPadNowMs(void)
{
    return (long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull);
}

/* ---- memory descriptors ----------------------------------------------------- */

struct XPadMapping {
    IOMemoryDescriptor *descriptor;     /* retained while cached */
    IOMemoryMap        *map;            /* retained; NULL when GetAddressRange gave the address */
    uint8_t            *bytes;
    uint64_t            length;
};

/* Points m at md's bytes. Reuses the cached mapping when md is the
 * descriptor mapped last time; otherwise drops it and maps md. */
bool XPadMappingGet(XPadMapping *m, IOMemoryDescriptor *md);

/* Releases the cached mapping (safe on a zeroed or already reset one). */
void XPadMappingReset(XPadMapping *m);

/* Copies min(max, length of md) bytes out of md. Returns the count, 0 on failure. */
size_t XPadCopyFromDescriptor(IOMemoryDescriptor *md, void *dst, size_t max);

/* Copies len bytes into md if it can hold them. Returns len, or 0. */
size_t XPadCopyToDescriptor(IOMemoryDescriptor *md, const void *src, size_t len);

/* A buffer of exactly `length` valid bytes, zeroed, with its CPU address. */
kern_return_t XPadCreateBuffer(uint64_t direction, uint64_t length,
                               IOBufferMemoryDescriptor **buffer, uint8_t **bytes);

/* ---- teardown ----------------------------------------------------------------- */

/* Cancel a timer source or an action, and drop the caller's reference only
 * once the cancellation has run. Cancel() completes asynchronously on the
 * object's queue; releasing the object before that (in free(), say) makes
 * the cancel callout touch freed memory, which crashes the
 * driver process (TimerDispatchSourceWasCanceled) whenever it outlives
 * the stopped service. *object is set to NULL;
 * safe on NULL. */
void XPadCancelTimer(IOTimerDispatchSource **timer);
void XPadCancelAction(OSAction **action);

/* ---- the published identity ------------------------------------------------- */

struct XPadIdentity {
    const char *source;             /* diagnostics: "GIP vvvv:pppp", "X360 vvvv:pppp" */
    uint64_t    locationID;         /* 0 if unknown */
    const char *serial;             /* the pad's own serial number, or NULL */
    uint32_t    reportIntervalUs;   /* used only when `base` has no ReportInterval; 0 = none */
};

/* A new dictionary for newDeviceDescription(): `base` (may be NULL; not
 * consumed) with VendorID, ProductID, VersionNumber, Transport, Product and
 * Manufacturer replaced by the Xbox Wireless Controller's, a SerialNumber
 * (the pad's, else "XPAD-<locationID>"), and XPadSource. ReportInterval,
 * LocationID and the other keys of `base` are kept. NULL on failure. */
OSDictionary *XPadCreateXboxDescription(OSDictionary *base, const XPadIdentity *identity);

/* ---- reports ---------------------------------------------------------------- */

/* getReport for the Xbox descriptor: input report 1 is answered with the
 * latest translated report (`current`, XPAD_XBOX_INPUT_LEN bytes); the
 * descriptor has no other input and no feature report. Returns the status
 * and sets *count to the bytes written. The caller completes the action. */
kern_return_t XPadServeInputReport(IOMemoryDescriptor *report, IOHIDReportType type,
                                   IOOptionBits options, const uint8_t *current, uint32_t *count);

/* setReport for the Xbox descriptor: 1 and *rumble set when `report` is
 * the rumble output report (ID 3, the ID taken from options' low byte or
 * the report's first byte), else 0. *count gets the report's length, and
 * *playMs (if not NULL) how long a real pad would play it on its own
 * (xpad_xbox_rumble_play_ms; 0 for a stop). */
int XPadParseRumble(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options,
                    xpad_rumble *rumble, uint32_t *count, uint32_t *playMs);

/* Slack added to a rumble report's own play time before a driver stops the
 * motors itself: GameController re-sends its 100 ms report every 100 ms
 * while a haptic plays, so a gap of up to 100 + kXPadRumbleSlackMs between
 * two reports does not cut it, and a client that dies mid-haptic leaves
 * the pad rumbling for at most that long. */
static constexpr long kXPadRumbleSlackMs = 200;

#endif /* XPadCommon_h */
