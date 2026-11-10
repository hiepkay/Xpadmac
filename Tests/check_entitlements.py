#!/usr/bin/env python3
"""Checks the dext's entitlements against what the driver needs.

The dext is signed with Driver/XPadDriver.entitlements. It must carry the
DriverKit development entitlements the driver uses (driverkit,
transport.usb for any vendor, family.hid.device) plus the sandbox, and
nothing else: every restricted entitlement has to be granted by the
dext's provisioning profile, and AMFI refuses the whole dext when one is
not. In particular it must not ask for transport.hid, since
Driver/Info.plist matches no IOHIDInterface.

    check_entitlements.py Driver/XPadDriver.entitlements Driver/Info.plist

SPDX-License-Identifier: GPL-3.0-or-later
"""
import plistlib
import sys

WANT = {
    "com.apple.developer.driverkit": True,
    "com.apple.developer.driverkit.transport.usb": [{"idVendor": "*"}],
    "com.apple.developer.driverkit.family.hid.device": True,
    "com.apple.security.app-sandbox": True,
}


def load(path):
    with open(path, "rb") as f:
        return plistlib.load(f)


def main(args):
    if len(args) != 2:
        sys.exit(__doc__)
    ents, info = (load(p) for p in args)
    errors = []
    for key, value in WANT.items():
        if ents.get(key) != value:
            errors.append(f"{args[0]}: {key} is {ents.get(key)!r}, not {value!r}")
    extra = sorted(set(ents) - set(WANT))
    if extra:
        errors.append(f"{args[0]}: unexpected entitlements {extra} (each must be in the profile)")
    hid = sorted(n for n, p in (info.get("IOKitPersonalities") or {}).items()
                 if p.get("IOProviderClass") == "IOHIDInterface")
    if hid:
        errors.append(f"{args[1]}: IOHIDInterface personalities {hid} need transport.hid")
    if errors:
        for line in errors:
            print("entitlements: " + line, file=sys.stderr)
        return 1
    print(f"entitlements: {len(WANT)} keys, as the driver needs")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
