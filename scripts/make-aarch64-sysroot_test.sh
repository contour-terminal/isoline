#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Tests scripts/make-aarch64-sysroot.sh --check on fixture trees: a complete one made from the pinned image
# passes; each file the cross build needs, taken away, fails it by name, and so does a missing stamp or one
# naming another image. usr/include/aarch64-linux-gnu/sys/types.h is the one an
# unanchored `--exclude=sys` drops silently (the anchored excludes keep it); libstdc++.so.6 is what the
# test binaries load under qemu.
#
# Usage: scripts/make-aarch64-sysroot_test.sh [<sysroot>]
#   <sysroot>   also check a real sysroot, which must pass

set -eu

script="$(dirname "$0")/make-aarch64-sysroot.sh"
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
failures=0

# Every file and directory --check requires, one per line.
required="usr/lib/aarch64-linux-gnu/libc.so
usr/lib/aarch64-linux-gnu/libstdc++.so.6
usr/include/c++/14/
usr/include/aarch64-linux-gnu/sys/types.h
usr/lib/gcc/aarch64-linux-gnu/14/"

image=$("$script" --image)

makeTree() {
    rm -rf "$fixture/tree"
    mkdir -p "$fixture/tree"
    echo "$image" > "$fixture/tree/.isoline-sysroot-image"
    echo "$required" | while read -r entry; do
        case "$entry" in
            */) mkdir -p "$fixture/tree/$entry" ;;
            *) mkdir -p "$fixture/tree/$(dirname "$entry")" && : > "$fixture/tree/$entry" ;;
        esac
    done
}

expect() { # <name> <expected status> <expected text or ""> <sysroot>
    status=0
    "$script" --check "$4" > "$fixture/out" 2>&1 || status=$?
    if [ "$status" -ne "$2" ]; then
        echo "FAIL $1: --check exited $status, expected $2" >&2; cat "$fixture/out" >&2
        failures=$((failures + 1))
    elif [ -n "$3" ] && ! grep -qF "$3" "$fixture/out"; then
        echo "FAIL $1: --check did not name $3" >&2; cat "$fixture/out" >&2
        failures=$((failures + 1))
    else
        echo "ok $1"
    fi
}

makeTree
expect complete 0 "" "$fixture/tree"
for entry in $required; do
    makeTree
    rm -rf "${fixture:?}/tree/$entry"
    expect "without $entry" 1 "${entry%/}" "$fixture/tree"
done
makeTree
rm "$fixture/tree/.isoline-sysroot-image"
expect "without its stamp" 1 ".isoline-sysroot-image" "$fixture/tree"
makeTree
echo "docker.io/library/debian:trixie@sha256:0000" > "$fixture/tree/.isoline-sysroot-image"
expect "from another image" 1 "not the pinned $image" "$fixture/tree"
expect missing 1 "" "$fixture/none"
if [ $# -gt 0 ]; then
    expect "the sysroot $1" 0 "" "$1"
fi

[ "$failures" -eq 0 ] || { echo "$failures failed" >&2; exit 1; }
