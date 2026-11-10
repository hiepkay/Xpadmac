#!/bin/sh
# Builds every Shared/*.c for DriverKit, the way the dext target will
# (arm64e, the DriverKit SDK's own headers only: a host-only header fails),
# with warnings as errors, and checks that each object needs nothing beyond
# the other Shared objects, xpad_log() and xpad_random() (the driver's
# hooks) and what DriverKit's libSystem exports.
#
#   driverkit_check.sh SHARED_DIR BUILD_DIR [DEPLOYMENT_TARGET]
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

shared=$1
out=$2
target=${3:-21.0}

sdk=$(xcrun --sdk driverkit --show-sdk-path 2>/dev/null) || {
    echo "driverkit: no DriverKit SDK (xcrun --sdk driverkit); skipped"
    exit 0
}
tbd=$sdk/System/DriverKit/usr/lib/libSystem.tbd
mkdir -p "$out"

for c in "$shared"/*.c; do
    o=$out/$(basename "$c" .c).o
    xcrun clang -target "arm64e-apple-driverkit$target" -isysroot "$sdk" \
        -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Wconversion -Wsign-conversion \
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
        -I"$shared" -c "$c" -o "$o"
done

# Symbols the objects define between them.
defined=$(xcrun nm -g -U -j "$out"/*.o 2>/dev/null | grep -v ':$' | sort -u)
status=0
for o in "$out"/*.o; do
    for sym in $(xcrun nm -u -j "$o"); do
        case $sym in _xpad_log|_xpad_random) continue ;; esac
        if printf '%s\n' "$defined" | grep -qx -- "$sym"; then continue; fi
        if grep -q -- "'$sym'" "$tbd" || grep -qw -- "$sym" "$tbd"; then continue; fi
        echo "driverkit: $(basename "$o") needs $sym, which DriverKit does not export" >&2
        status=1
    done
done
[ $status -eq 0 ] && echo "driverkit: Shared/*.c build for arm64e-apple-driverkit$target ($(basename "$sdk")), symbols resolved"
exit $status
