#!/usr/bin/env python3
"""Check Driver/Info.plist's IOKitPersonalities.

IOUSBHostFamily matches a personality only when its USB keys form one of the
combinations of Apple's QA1076 ("Tips on USB driver matching"); any other
combination never matches, silently. Measured on macOS 27.2 by passing
dictionaries to IOServiceGetMatchingServices, which runs the same
IOUSBHost{Device,Interface}::matchPropertyTable a personality goes through:

  device:     idVendor + bDeviceClass 255         -> never matches
              idVendor + bDeviceSubClass [+ bDeviceProtocol] (+ bDeviceClass 255;
                  the device must be vendor class)                  -> matches
              idVendor + idProduct[Array] (+ bDeviceClass)          -> matches
  interface:  idVendor + bInterfaceSubClass + bInterfaceProtocol + bInterfaceNumber
                                                                    -> never matches
              idVendor + idProduct + bInterfaceNumber               -> never matches
              idVendor + idProduct[Array] + bConfigurationValue + bInterfaceNumber
                  (+ bInterfaceClass/SubClass/Protocol)             -> matches
              idVendor + bInterfaceSubClass [+ bInterfaceProtocol]
                  (+ idProduct, bInterfaceClass 255; no bConfigurationValue or
                  bInterfaceNumber)                                 -> matches
              bInterfaceClass + bInterfaceSubClass [+ bInterfaceProtocol], class
                  not 255, nothing else                             -> matches
  "*" (a string) matches any value of a key.

So this checks every USB personality against those rules, plus what the
design asks of each XPadMac personality (docs/DESIGN.md).

    check_personalities.py Driver/Info.plist     (or a built dext's Info.plist)

SPDX-License-Identifier: GPL-3.0-or-later
"""
import plistlib
import re
import sys

WILD = "*"

DEVICE_KEYS = {"idVendor", "idProduct", "idProductArray", "bcdDevice",
               "bDeviceClass", "bDeviceSubClass", "bDeviceProtocol"}
INTERFACE_KEYS = {"idVendor", "idProduct", "idProductArray", "bcdDevice",
                  "bConfigurationValue", "bInterfaceNumber",
                  "bInterfaceClass", "bInterfaceSubClass", "bInterfaceProtocol"}

# Third-party GIP / XInput vendors (docs/DESIGN.md): 8BitDo, GameSir, Hori,
# Hyperkin, Mad Catz, PDP, PowerA (two VIDs), Razer, Turtle Beach; and
# Microsoft's pads Apple does not drive.
GIP_VENDORS = {11720, 13623, 3853, 11812, 1848, 3695, 9414, 8406, 5426, 4341}
MICROSOFT = 1118
MICROSOFT_PIDS = [721, 733, 739]
APPLE_DRIVEN_PIDS = {654, 746, 2816, 2834}

failures = []


def fail(name, text):
    failures.append(f"{name}: {text}")


def has(p, *keys):
    return all(k in p for k in keys)


def product(p):
    return "idProduct" in p or "idProductArray" in p


def device_rule(p):
    """The QA1076 device rule the personality falls under, or None."""
    if has(p, "idVendor") and product(p):
        return "vendor+product"
    if has(p, "idVendor", "bDeviceSubClass") and not product(p) and "bcdDevice" not in p:
        if p.get("bDeviceClass", 255) != 255:
            return None          # the vendor rules are for vendor-class devices only
        return "vendor+subclass"
    if has(p, "bDeviceClass", "bDeviceSubClass") and not (p.keys() & {"idVendor", "idProduct", "idProductArray", "bcdDevice"}):
        return "class+subclass" if p["bDeviceClass"] not in (255, WILD) else None
    return None


def interface_rule(p):
    """The QA1076 interface rule the personality falls under, or None."""
    if has(p, "idVendor", "bConfigurationValue", "bInterfaceNumber") and product(p):
        return "vendor+product+config+interface"
    if p.keys() & {"bConfigurationValue", "bInterfaceNumber", "bcdDevice"}:
        return None              # honoured only with idVendor + idProduct + bConfigurationValue
    if has(p, "idVendor", "bInterfaceSubClass"):
        return "vendor+subclass" if p.get("bInterfaceClass", 255) == 255 else None
    if has(p, "bInterfaceClass", "bInterfaceSubClass") and not (p.keys() & {"idVendor", "idProduct", "idProductArray"}):
        return "class+subclass" if p["bInterfaceClass"] not in (255, WILD) else None
    return None


def check_common(name, p, bundle_id):
    # bundle_id is "$(PRODUCT_BUNDLE_IDENTIFIER)" in the source, expanded in a build.
    for key, want in (("CFBundleIdentifier", bundle_id),
                      ("IOUserServerName", bundle_id),
                      ("CFBundleIdentifierKernel", "com.apple.kpi.iokit")):
        if p.get(key) != want:
            fail(name, f"{key} is {p.get(key)!r}, not {want!r}")
    for key in ("IOClass", "IOUserClass", "IOProviderClass"):
        if not isinstance(p.get(key), str) or not p[key]:
            fail(name, f"no {key}")
    for key, value in p.items():
        if key in DEVICE_KEYS | INTERFACE_KEYS and key != "idProductArray":
            if not (isinstance(value, int) or value == WILD):
                fail(name, f"{key} = {value!r}: must be an integer or \"*\"")


def check_usb_rules(name, p):
    provider = p.get("IOProviderClass")
    if provider == "IOUSBHostDevice":
        stray = (p.keys() & INTERFACE_KEYS) - DEVICE_KEYS
        if stray:
            fail(name, f"interface keys on a device personality: {sorted(stray)}")
        if device_rule(p) is None:
            fail(name, "IOUSBHostFamily never matches this combination of device keys (QA1076)")
    elif provider == "IOUSBHostInterface":
        stray = (p.keys() & DEVICE_KEYS) - INTERFACE_KEYS
        if stray:
            fail(name, f"device keys on an interface personality: {sorted(stray)}")
        if interface_rule(p) is None:
            fail(name, "IOUSBHostFamily never matches this combination of interface keys (QA1076)")


def check_design(pers):
    known = {"XPadGIPDevice", "XPadGIPGamepad", "XPadGIPAudio", "XPad360Gamepad"}
    for name, p in pers.items():
        if p.get("IOUserClass") not in known:
            fail(name, f"IOUserClass {p.get('IOUserClass')!r} is not one of {sorted(known)}")
        # Every service runs in a process of its own: a crash takes one pad
        # down, not every pad. Nothing in the driver shares memory between
        # services, so nothing needs one process.
        if "IOUserServerOneProcess" in p:
            fail(name, "IOUserServerOneProcess is set: each service should get its own process")
        if p.get("IOProviderClass") not in ("IOUSBHostDevice", "IOUSBHostInterface"):
            fail(name, f"IOProviderClass {p.get('IOProviderClass')!r}: the driver matches USB only")

    devices = {p.get("idVendor"): (n, p) for n, p in pers.items() if p.get("IOUserClass") == "XPadGIPDevice"}
    pads = {p.get("idVendor"): (n, p) for n, p in pers.items() if p.get("IOUserClass") == "XPadGIPGamepad"}
    for vid in sorted(GIP_VENDORS | {MICROSOFT}):
        for kind, table in (("XPadGIPDevice", devices), ("XPadGIPGamepad", pads)):
            if vid not in table:
                fail(kind, f"no personality for vendor {vid}")
    for vid, (name, p) in devices.items():
        if p.get("IOProviderClass") != "IOUSBHostDevice" or p.get("IOClass") != "IOUserService":
            fail(name, "must be IOUSBHostDevice / IOUserService")
        if p.get("bDeviceClass") != 255:
            fail(name, "must be limited to vendor-class devices (bDeviceClass 255)")
        if not isinstance(p.get("XPadWindowsDescriptors"), bool):
            fail(name, "XPadWindowsDescriptors must be a boolean")
    for vid, (name, p) in pads.items():
        if p.get("IOProviderClass") != "IOUSBHostInterface" or p.get("IOClass") != "AppleUserHIDDevice":
            fail(name, "must be IOUSBHostInterface / AppleUserHIDDevice")
        for key, value in (("bInterfaceClass", 255), ("bInterfaceSubClass", 71),
                           ("bInterfaceProtocol", 208), ("bInterfaceNumber", 0)):
            if p.get(key) != value:
                fail(name, f"{key} is {p.get(key)!r}, not {value!r} (GIP data interface)")

    # Xbox 360 (XInput) pads: interface 0 FF/5D/01 of every third-party GIP
    # vendor, never Microsoft's (Apple's XboxGamepad.dext drives 045E:028E).
    x360 = {p.get("idVendor"): (n, p) for n, p in pers.items() if p.get("IOUserClass") == "XPad360Gamepad"}
    for vid in sorted(GIP_VENDORS):
        if vid not in x360:
            fail("XPad360Gamepad", f"no personality for vendor {vid}")
    if len(x360) != sum(p.get("IOUserClass") == "XPad360Gamepad" for p in pers.values()):
        fail("XPad360Gamepad", "two personalities for one vendor")
    for vid, (name, p) in x360.items():
        if vid == MICROSOFT or vid not in GIP_VENDORS:
            fail(name, f"vendor {vid!r} is not a third-party GIP vendor (never Microsoft {MICROSOFT})")
        if p.get("IOProviderClass") != "IOUSBHostInterface" or p.get("IOClass") != "AppleUserHIDDevice":
            fail(name, "must be IOUSBHostInterface / AppleUserHIDDevice")
        for key, value in (("bInterfaceClass", 255), ("bInterfaceSubClass", 93),
                           ("bInterfaceProtocol", 1), ("bInterfaceNumber", 0)):
            if p.get(key) != value:
                fail(name, f"{key} is {p.get(key)!r}, not {value!r} (XInput gamepad interface)")
        if p.get("IOProbeScore", 0) < 100000:
            fail(name, "IOProbeScore must be at least 100000")

    # Microsoft: only the pads Apple's XboxGamepad.dext does not drive.
    for name, p in pers.items():
        if p.get("idVendor") != MICROSOFT:
            continue
        pids = p.get("idProductArray", [p["idProduct"]] if "idProduct" in p else None)
        if pids != MICROSOFT_PIDS:
            fail(name, f"Microsoft PIDs are {pids!r}, not {MICROSOFT_PIDS!r}")
        elif set(pids) & APPLE_DRIVEN_PIDS:
            fail(name, "matches a PID Apple's XboxGamepad.dext drives")


def main():
    args = sys.argv[1:]
    if len(args) != 1:
        sys.exit(__doc__)
    path = args[0]
    with open(path, "rb") as f:
        plist = plistlib.load(f)
    pers = plist.get("IOKitPersonalities") or {}
    bundle_id = plist.get("CFBundleIdentifier")
    if not bundle_id:
        fail("Info.plist", "no CFBundleIdentifier")
    if not pers:
        fail("Info.plist", "no IOKitPersonalities")
    for name, p in sorted(pers.items()):
        check_common(name, p, bundle_id)
        check_usb_rules(name, p)
    check_design(pers)
    if not plist.get("OSBundleUsageDescription"):
        fail("Info.plist", "no OSBundleUsageDescription")
    # kernelmanagerd refuses a dext whose CFBundleVersion is not a kext
    # version ("Property 'CFBundleVersion' must be a valid kext version"):
    # 1-4 digits, then up to two .components of 1-2 digits, then optionally
    # a stage (d, a, b, fc) and a number up to 255.
    # The source Info.plist has $(CURRENT_PROJECT_VERSION) here; only the
    # built one (make check-product) carries the real value.
    version = str(plist.get("CFBundleVersion", ""))
    if version.startswith("$("):
        version = "1"
    m = re.fullmatch(r"(\d{1,4})(?:\.(\d{1,2})(?:\.(\d{1,2}))?)?(?:(d|a|b|fc)(\d{1,3}))?", version)
    if not m or (m.group(5) and int(m.group(5)) > 255):
        fail("Info.plist", f"CFBundleVersion {version!r} is not a kext version (at most 9999.99.99)")

    if failures:
        for line in failures:
            print("personalities: " + line, file=sys.stderr)
        sys.exit(1)
    print(f"personalities: {len(pers)} in {path}, "
          f"every one a combination IOUSBHostFamily matches")


if __name__ == "__main__":
    main()
