#!/usr/bin/env python3
"""Checks signed bundles against their embedded provisioning profiles.

AMFI lets code with restricted entitlements (the app's
com.apple.developer.system-extension.install, the dext's DriverKit ones)
run only if its provisioning profile

  1. lists this Mac by its Provisioning UDID (System Information >
     Hardware). On an Apple silicon Mac that is not the Hardware UUID; a
     team device record made from the Hardware UUID gives profiles AMFI
     rejects with "No matching profile found";
  2. grants every restricted entitlement the code is signed with. One the
     profile lacks (a DriverKit capability not yet turned on for the
     dext's App ID in the developer portal) makes AMFI refuse the whole
     binary.

Usage: check_profiles.py BUNDLE|PROFILE...

A BUNDLE (the .app or the .dext) gets both checks: its signed entitlements
(codesign -d --entitlements - --xml) against the Entitlements of its
embedded.provisionprofile. A PROFILE (a .provisionprofile file) gets the
device check only. Exit 1 if any check fails.

The com.apple.security.* entitlements (app-sandbox and the like) are not
restricted and never in a profile; every other key must be. Values are
compared too: a profile's "*" and "TEAM.*" match any value.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import fnmatch
import os
import plistlib
import re
import subprocess
import sys

UNRESTRICTED = "com.apple.security."

HINTS = {
    "com.apple.developer.driverkit.transport.usb":
        "turn on \"DriverKit USB Transport (development)\" for the dext's App ID in the developer portal",
    "com.apple.developer.driverkit.family.hid.device":
        "turn on \"DriverKit Family HID Device (development)\" for the dext's App ID in the developer portal",
}


def this_mac():
    out = subprocess.run(["system_profiler", "SPHardwareDataType"], capture_output=True, text=True).stdout
    m = re.search(r"Provisioning UDID:\s*(\S+)", out)
    return m.group(1) if m else None


def load_profile(path):
    der = subprocess.run(["security", "cms", "-D", "-i", path], capture_output=True)
    if der.returncode != 0:
        return None
    try:
        return plistlib.loads(der.stdout)
    except Exception:
        return None


def signed_entitlements(bundle):
    """The entitlements the bundle is signed with, {} if none, None if unsigned or unreadable."""
    r = subprocess.run(["codesign", "-d", "--entitlements", "-", "--xml", bundle], capture_output=True)
    if r.returncode != 0:
        return None
    if not r.stdout.strip():
        return {}
    try:
        return plistlib.loads(r.stdout)
    except Exception:
        return None


def embedded_profile(bundle):
    for rel in ("Contents/embedded.provisionprofile", "embedded.provisionprofile"):
        path = os.path.join(bundle, rel)
        if os.path.isfile(path):
            return path
    return None


def matches(signed, granted):
    """True if the profile's value grants the signed one."""
    if granted == "*":
        return True
    if isinstance(granted, str) and isinstance(signed, str):
        return fnmatch.fnmatchcase(signed, granted)
    if isinstance(granted, bool) or isinstance(signed, bool):
        return granted is True or granted == signed
    if isinstance(signed, list):
        pool = granted if isinstance(granted, list) else [granted]
        return all(any(matches(s, g) for g in pool) for s in signed)
    if isinstance(signed, dict) and isinstance(granted, dict):
        return all(k in granted and matches(v, granted[k]) for k, v in signed.items())
    if isinstance(granted, list):
        return any(matches(signed, g) for g in granted)
    return signed == granted


def check_entitlements(bundle, profile, name):
    """Error lines for the bundle's restricted entitlements its profile does not grant."""
    signed = signed_entitlements(bundle)
    if signed is None:
        return [f"{bundle}: not signed, or its entitlements are unreadable (codesign -d --entitlements)"]
    granted = profile.get("Entitlements") or {}
    errors = []
    for key in sorted(signed):
        if key.startswith(UNRESTRICTED) and key != UNRESTRICTED + "get-task-allow":
            continue
        if key not in granted:
            line = f"{bundle}: signed with {key}, which \"{name}\" does not grant; AMFI will refuse it"
        elif not matches(signed[key], granted[key]):
            line = (f"{bundle}: {key} is {signed[key]!r} in the signature but {granted[key]!r} in \"{name}\"; "
                    f"AMFI will refuse it")
        else:
            continue
        if key in HINTS:
            line += f"\n  {HINTS[key]}"
        errors.append(line)
    return errors


def main(paths):
    if not paths:
        sys.exit(__doc__)
    udid = this_mac()
    if not udid:
        print("profiles: no Provisioning UDID in System Information; device lists not checked", file=sys.stderr)
    errors = []
    entitled = 0
    for path in paths:
        bundle = path if os.path.isdir(path) else None
        profile_path = embedded_profile(path) if bundle else path
        if not profile_path:
            errors.append(f"{path}: no embedded.provisionprofile")
            continue
        profile = load_profile(profile_path)
        if profile is None:
            errors.append(f"{profile_path}: no readable provisioning profile")
            continue
        name = profile.get("Name", "?")
        devs = True if profile.get("ProvisionsAllDevices") else profile.get("ProvisionedDevices", [])
        if udid and devs is not True and udid not in devs:
            errors.append(f"{profile_path}: \"{name}\" does not list this Mac ({udid}); macOS will refuse to run it.\n"
                          f"  register {udid} as a macOS device of the team and see Config.xcconfig (APP_PROFILE).")
        if bundle:
            errors += check_entitlements(bundle, profile, name)
            entitled += 1
    if errors:
        for line in errors:
            print("error: " + line, file=sys.stderr)
        return 1
    where = f"list this Mac ({udid})" if udid else "are readable"
    print(f"profiles: {len(paths)} profile(s) {where}; {entitled} bundle(s) signed only with entitlements "
          f"their profile grants")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
