/*
 * XPadMac driver: helpers shared by the driver classes (see XPadCommon.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/OSCollections.h>
#include <HIDDriverKit/IOHIDDeviceKeys.h>

#include "XPadCommon.h"

/* ---- memory descriptors ----------------------------------------------------- */

void XPadMappingReset(XPadMapping *m)
{
    if (!m) return;
    OSSafeReleaseNULL(m->map);
    OSSafeReleaseNULL(m->descriptor);
    m->bytes = nullptr;
    m->length = 0;
}

bool XPadMappingGet(XPadMapping *m, IOMemoryDescriptor *md)
{
    if (!m || !md) return false;
    if (m->descriptor == md && m->bytes) return true;

    XPadMappingReset(m);

    /* A buffer created in this process already has an address here. */
    IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, md);
    if (buffer) {
        IOAddressSegment range = {};
        if (buffer->GetAddressRange(&range) == kIOReturnSuccess && range.address && range.length) {
            m->bytes = reinterpret_cast<uint8_t *>(range.address);
            m->length = range.length;
        }
    }

    /* Anything else (a kernel buffer handed to setReport, say) is mapped. */
    if (!m->bytes) {
        IOMemoryMap *map = nullptr;
        kern_return_t ret = md->CreateMapping(0, 0, 0, 0, 0, &map);
        if (ret != kIOReturnSuccess || !map) {
            OSSafeReleaseNULL(map);
            return false;
        }
        if (!map->GetAddress() || !map->GetLength()) {
            map->release();
            return false;
        }
        m->map = map;
        m->bytes = reinterpret_cast<uint8_t *>(map->GetAddress());
        m->length = map->GetLength();
    }

    md->retain();
    m->descriptor = md;
    return true;
}

/* The descriptor's valid length, which may be shorter than its mapping. */
static uint64_t descriptorLength(IOMemoryDescriptor *md, uint64_t mapped)
{
    uint64_t length = 0;
    if (md->GetLength(&length) != kIOReturnSuccess || length == 0 || length > mapped) return mapped;
    return length;
}

size_t XPadCopyFromDescriptor(IOMemoryDescriptor *md, void *dst, size_t max)
{
    XPadMapping m = {};
    size_t n = 0;

    if (!dst || !max || !XPadMappingGet(&m, md)) return 0;
    n = (size_t)descriptorLength(md, m.length);
    if (n > max) n = max;
    memcpy(dst, m.bytes, n);
    XPadMappingReset(&m);
    return n;
}

size_t XPadCopyToDescriptor(IOMemoryDescriptor *md, const void *src, size_t len)
{
    XPadMapping m = {};
    size_t n = 0;

    if (!src || !len || !XPadMappingGet(&m, md)) return 0;
    if (descriptorLength(md, m.length) >= len) {
        memcpy(m.bytes, src, len);
        n = len;
    }
    XPadMappingReset(&m);
    return n;
}

kern_return_t XPadCreateBuffer(uint64_t direction, uint64_t length,
                               IOBufferMemoryDescriptor **buffer, uint8_t **bytes)
{
    IOBufferMemoryDescriptor *md = nullptr;
    IOAddressSegment range = {};
    kern_return_t ret;

    if (!buffer || !bytes || !length) return kIOReturnBadArgument;
    *buffer = nullptr;
    *bytes = nullptr;

    ret = IOBufferMemoryDescriptor::Create(direction, length, 0, &md);
    if (ret != kIOReturnSuccess) return ret;
    ret = md->SetLength(length);
    if (ret == kIOReturnSuccess) ret = md->GetAddressRange(&range);
    if (ret == kIOReturnSuccess && (!range.address || range.length < length)) ret = kIOReturnNoMemory;
    if (ret != kIOReturnSuccess) {
        md->release();
        return ret;
    }
    memset(reinterpret_cast<void *>(range.address), 0, (size_t)length);
    *buffer = md;
    *bytes = reinterpret_cast<uint8_t *>(range.address);
    return kIOReturnSuccess;
}

/* ---- teardown ----------------------------------------------------------------- */

void XPadCancelTimer(IOTimerDispatchSource **timer)
{
    IOTimerDispatchSource *source = timer ? *timer : nullptr;

    if (!source) return;
    *timer = nullptr;
    /* The reference moves into the handler. */
    if (source->Cancel(^{ source->release(); }) != kIOReturnSuccess) source->release();
}

void XPadCancelAction(OSAction **action)
{
    OSAction *a = action ? *action : nullptr;

    if (!a) return;
    *action = nullptr;
    if (a->Cancel(^{ a->release(); }) != kIOReturnSuccess) a->release();
}

/* ---- the published identity ------------------------------------------------- */

static bool hasNonEmptyString(OSDictionary *d, const char *key)
{
    const char *s = d ? OSDictionaryGetStringValue(d, key) : nullptr;
    return s && s[0];
}

OSDictionary *XPadCreateXboxDescription(OSDictionary *base, const XPadIdentity *identity)
{
    OSDictionary *d;
    char text[64];

    if (base) {
        d = OSDictionary::withDictionary(base, OSDictionaryGetCount(base) + 8);
    } else {
        d = OSDictionary::withCapacity(16);
    }
    if (!d) return nullptr;

    /* Who we say we are: an Xbox Series X|S controller on Bluetooth LE. Apple's
     * XboxOneHIDServicePlugin requires VendorID 0x045E, a PID it knows and a
     * Transport other than "USB"; the GameController personality for 0x0B13
     * is the one that accepts a device from a third-party driver. */
    OSDictionarySetUInt64Value(d, kIOHIDVendorIDKey, XPAD_XBOX_VID);
    OSDictionarySetUInt64Value(d, kIOHIDProductIDKey, XPAD_XBOX_PID);
    OSDictionarySetUInt64Value(d, kIOHIDVersionNumberKey, XPAD_XBOX_VERSION);
    OSDictionarySetStringValue(d, kIOHIDTransportKey, XPAD_XBOX_TRANSPORT);
    OSDictionarySetStringValue(d, kIOHIDProductKey, XPAD_XBOX_PRODUCT);
    OSDictionarySetStringValue(d, kIOHIDManufacturerKey, XPAD_XBOX_MANUFACTURER);
    if (!OSDictionaryGetValue(d, kIOHIDCountryCodeKey)) OSDictionarySetUInt64Value(d, kIOHIDCountryCodeKey, 0);

    if (identity && identity->locationID && !OSDictionaryGetValue(d, kIOHIDLocationIDKey))
        OSDictionarySetUInt64Value(d, kIOHIDLocationIDKey, identity->locationID);
    if (identity && identity->reportIntervalUs && !OSDictionaryGetValue(d, kIOHIDReportIntervalKey))
        OSDictionarySetUInt64Value(d, kIOHIDReportIntervalKey, identity->reportIntervalUs);

    /* A serial number tells two pads apart: the pad's own if it has one,
     * else one made from its USB location (stable while it stays plugged
     * into the same port). */
    if (identity && identity->serial && identity->serial[0]) {
        OSDictionarySetStringValue(d, kIOHIDSerialNumberKey, identity->serial);
    } else if (!hasNonEmptyString(d, kIOHIDSerialNumberKey)) {
        uint64_t location = identity && identity->locationID ? identity->locationID
                                                             : OSDictionaryGetUInt64Value(d, kIOHIDLocationIDKey);
        snprintf(text, sizeof text, "XPAD-%08llX", (unsigned long long)location);
        OSDictionarySetStringValue(d, kIOHIDSerialNumberKey, text);
    }

    /* For xpadctl doctor and the app: which pad is really behind this one. */
    OSDictionarySetStringValue(d, "XPadSource", identity && identity->source ? identity->source : "XPad");
    return d;
}

/* ---- reports ---------------------------------------------------------------- */

kern_return_t XPadServeInputReport(IOMemoryDescriptor *report, IOHIDReportType type,
                                   IOOptionBits options, const uint8_t *current, uint32_t *count)
{
    *count = 0;
    if (type != kIOHIDReportTypeInput || (options & 0xFF) != XPAD_XBOX_INPUT_ID || !current || !report)
        return kIOReturnUnsupported;
    if (XPadCopyToDescriptor(report, current, XPAD_XBOX_INPUT_LEN) != XPAD_XBOX_INPUT_LEN)
        return kIOReturnNoSpace;
    *count = XPAD_XBOX_INPUT_LEN;
    return kIOReturnSuccess;
}

int XPadParseRumble(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options,
                    xpad_rumble *rumble, uint32_t *count, uint32_t *playMs)
{
    uint8_t bytes[XPAD_XBOX_RUMBLE_LEN];
    uint8_t id = (uint8_t)(options & 0xFF);
    size_t n;

    *count = 0;
    if (playMs) *playMs = 0;
    if (type != kIOHIDReportTypeOutput || !report) return 0;
    n = XPadCopyFromDescriptor(report, bytes, sizeof bytes);
    if (!n) return 0;
    *count = (uint32_t)n;
    /* xpad_xbox_parse_rumble takes the report with or without its ID byte:
     * 9 bytes start with 0x03; 8 bytes are the payload of report options&0xFF. */
    if (!xpad_xbox_parse_rumble(id, bytes, n, rumble)) return 0;
    if (playMs) *playMs = xpad_xbox_rumble_play_ms(id, bytes, n);
    return 1;
}
