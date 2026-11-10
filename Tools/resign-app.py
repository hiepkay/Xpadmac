#!/usr/bin/env python3
"""Re-signs the built XPadMac.app (its outer bundle only) with a portal profile.

Why: AMFI runs the app (it has the restricted entitlement
com.apple.developer.system-extension.install) only with a profile that
lists this Mac's Provisioning UDID, or a Developer ID profile, which lists
no devices. A Mac whose team record is its Hardware UUID gets neither from
Xcode's automatic signing (see Config.xcconfig). Xcode will not build an
app signed with another certificate than its embedded dext ("Embedded
binary is not signed with the same certificate as the parent app"), but
macOS only asks for the same team; so the app and the dext are built and
signed as usual, then this re-signs the app's outer bundle: the dext keeps
its own signature and profile, and the app's signature seals it as is.

Usage: resign-app.py APP PROFILE_NAME TEAM

  - finds the installed profile named PROFILE_NAME (Xcode's profile folders),
  - picks the keychain identity whose certificate the profile lists,
  - embeds the profile, and signs APP with the entitlements the app was
    built with, minus get-task-allow (a Developer ID profile grants none of
    it), with the hardened runtime.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import hashlib
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

PROFILE_DIRS = [
    os.path.expanduser("~/Library/Developer/Xcode/UserData/Provisioning Profiles"),
    os.path.expanduser("~/Library/MobileDevice/Provisioning Profiles"),
]
DROPPED = {"com.apple.security.get-task-allow", "get-task-allow"}


def run(args, **kw):
    return subprocess.run(args, capture_output=True, **kw)


def decode(path):
    r = run(["security", "cms", "-D", "-i", path])
    return plistlib.loads(r.stdout) if r.returncode == 0 and r.stdout else None


def find_profile(name, team):
    best = None
    for d in PROFILE_DIRS:
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            path = os.path.join(d, f)
            p = decode(path)
            if not p or p.get("Name") != name or team not in p.get("TeamIdentifier", []):
                continue
            if best is None or p["CreationDate"] > best[1]["CreationDate"]:
                best = (path, p)
    return best


def identities():
    out = run(["security", "find-identity", "-v", "-p", "codesigning"], text=True).stdout
    return {m.group(1): m.group(2) for m in re.finditer(r"\)\s+([0-9A-F]{40})\s+\"([^\"]+)\"", out)}


def die(msg):
    print(f"resign-app: {msg}", file=sys.stderr)
    sys.exit(1)


def main(app, name, team):
    if not os.path.isdir(app):
        die(f"no {app}")
    found = find_profile(name, team)
    if not found:
        die(f"no installed profile named \"{name}\" for team {team}: download it from "
            f"developer.apple.com and double-click it (or copy it to {PROFILE_DIRS[0]})")
    path, profile = found

    ids = identities()
    sha1 = None
    for cert in profile.get("DeveloperCertificates", []):
        h = hashlib.sha1(cert).hexdigest().upper()
        if h in ids:
            sha1 = h
            break
    if not sha1:
        die(f"none of the certificates in \"{name}\" has its private key in the keychain")

    r = run(["codesign", "-d", "--entitlements", "-", "--xml", app])
    current = plistlib.loads(r.stdout) if r.returncode == 0 and r.stdout.strip() else {}
    allowed = profile.get("Entitlements", {})
    ents = {k: v for k, v in current.items() if k not in DROPPED}
    missing = [k for k in ents if k not in allowed]
    if missing:
        die(f"\"{name}\" does not grant {', '.join(missing)}")

    shutil.copyfile(path, os.path.join(app, "Contents", "embedded.provisionprofile"))
    with tempfile.NamedTemporaryFile(suffix=".plist", delete=False) as t:
        plistlib.dump(ents, t)
        ent_path = t.name
    try:
        r = run(["codesign", "--force", "--sign", sha1, "--entitlements", ent_path, "--options", "runtime",
                 "--timestamp=none", "--generate-entitlement-der", app], text=True)
        if r.returncode != 0:
            die("codesign failed:\n" + r.stderr)
    finally:
        os.unlink(ent_path)
    r = run(["codesign", "--verify", "--deep", "--strict", app], text=True)
    if r.returncode != 0:
        die("signature does not verify:\n" + r.stderr)
    print(f"resign-app: {os.path.basename(app)} signed by {ids[sha1]} with \"{name}\"; "
          f"the dext keeps its own signature")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        die("usage: resign-app.py APP PROFILE_NAME TEAM")
    main(*sys.argv[1:])
