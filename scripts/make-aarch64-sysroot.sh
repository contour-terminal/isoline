#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Creates the aarch64 Linux sysroot that cmake/toolchains/aarch64-linux-gnu-clang.cmake builds against:
# the C and C++ development files of a Debian trixie arm64 container, exported with podman.
#
# Usage: scripts/make-aarch64-sysroot.sh [--refresh | --check | --image] [<sysroot>]
#
#   <sysroot>   where to create it; default $ISOLINE_AARCH64_SYSROOT, else
#               ~/.cache/isoline/sysroot-aarch64-trixie (the toolchain file's default)
#   --refresh   recreate it even when it is already there
#   --check     create nothing: exit 0 when it is complete and made from the pinned image, else 1, naming
#               what it lacks
#   --image     print the pinned image, by digest, and exit
#
# Idempotent: a complete sysroot made from the pinned image is left alone; one made from another image (its
# stamp file, .isoline-sysroot-image, names the image) is recreated. The path of the sysroot is the last line
# printed.
# Needs podman, and qemu-user binfmt support for running the arm64 container (qemu-user-static-aarch64).
#
# @see docs/cross-compiling.md

set -eu

# The image is pinned by its multi-arch manifest digest (debian:trixie as of 2026-11-26), so the base
# system is reproducible; apt-get still installs the packages current in the trixie archive.
image="docker.io/library/debian:trixie@sha256:3cda3ca472ce45507a5109c6a3907093e24d9ae67d55954c9a07a45665dafba2"
packages="libc6-dev libstdc++-14-dev libgcc-14-dev linux-libc-dev"

refresh=no
sysroot="${ISOLINE_AARCH64_SYSROOT:-$HOME/.cache/isoline/sysroot-aarch64-trixie}"
for arg in "$@"; do
    case "$arg" in
        --refresh) refresh=yes ;;
        --check) refresh=check ;;
        --image) echo "$image"; exit 0 ;;
        -h|--help) sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "make-aarch64-sysroot: unknown option $arg" >&2; exit 2 ;;
        *) sysroot="$arg" ;;
    esac
done

# What the toolchain file, the C++ build and the tests under qemu need from it, one entry per line (a
# trailing / names a directory): the C library, libstdc++ (its headers, and the library the test binaries
# load), the target's system headers, and GCC's crt files and libgcc, which clang's driver finds through
# usr/lib/gcc. sys/types.h stands for the target's sys/ headers, which an unanchored `--exclude=sys` drops.
required="usr/lib/aarch64-linux-gnu/libc.so
usr/lib/aarch64-linux-gnu/libstdc++.so.6
usr/include/c++/14/
usr/include/aarch64-linux-gnu/sys/types.h
usr/lib/gcc/aarch64-linux-gnu/14/"

# The file that records which image a sysroot was exported from.
stamp=".isoline-sysroot-image"

# Whether $1 holds every required entry and was made from the pinned image; names what is wrong on stderr.
isComplete() {
    complete=yes
    for entry in $required; do
        case "$entry" in
            */) [ -d "$1/$entry" ] ;;
            *) [ -e "$1/$entry" ] ;;
        esac || { echo "make-aarch64-sysroot: $1 lacks $entry" >&2; complete=no; }
    done
    if [ ! -f "$1/$stamp" ]; then
        echo "make-aarch64-sysroot: $1 lacks $stamp, the image it was made from" >&2
        complete=no
    elif [ "$(cat "$1/$stamp")" != "$image" ]; then
        echo "make-aarch64-sysroot: $1 was made from $(cat "$1/$stamp"), not the pinned $image" >&2
        complete=no
    fi
    [ "$complete" = yes ]
}

if [ "$refresh" = check ]; then
    isComplete "$sysroot"
    exit
fi

if [ "$refresh" = no ] && isComplete "$sysroot" 2>/dev/null; then
    echo "make-aarch64-sysroot: up to date" >&2
    echo "$sysroot"
    exit 0
fi

command -v podman >/dev/null || { echo "make-aarch64-sysroot: podman is not installed" >&2; exit 1; }

staging="$sysroot.new"
container=""
cleanup() {
    [ -n "$container" ] && podman rm -f "$container" >/dev/null 2>&1 || true
    rm -rf "$staging"
}
# POSIX sh runs an EXIT trap on exit, not on a signal: INT and TERM exit, which then runs it.
trap cleanup EXIT
trap 'exit 130' INT TERM

container=$(podman create --platform linux/arm64 "$image" \
    sh -c "apt-get update && apt-get install -y --no-install-recommends $packages")
podman start --attach "$container" >&2

rm -rf "$staging"
mkdir -p "$staging"
podman export "$container" | tar -x -C "$staging" --anchored --exclude=dev --exclude=proc --exclude=sys \
    usr lib etc/ld.so.conf etc/ld.so.conf.d
echo "$image" > "$staging/$stamp"
isComplete "$staging" || { echo "make-aarch64-sysroot: the export lacks the C/C++ development files" >&2; exit 1; }

rm -rf "$sysroot"
mv "$staging" "$sysroot"
echo "make-aarch64-sysroot: created" >&2
echo "$sysroot"
