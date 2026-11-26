#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Creates the aarch64 Linux sysroot that cmake/toolchains/aarch64-linux-gnu-clang.cmake builds against:
# the C and C++ development files of a Debian trixie arm64 container, exported with podman.
#
# Usage: scripts/make-aarch64-sysroot.sh [--refresh] [<sysroot>]
#
#   <sysroot>   where to create it; default $ISOLINE_AARCH64_SYSROOT, else
#               ~/.cache/isoline/sysroot-aarch64-trixie (the toolchain file's default)
#   --refresh   recreate it even when it is already there
#
# Idempotent: a complete sysroot is left alone. The path of the sysroot is the last line printed.
# Needs podman, and qemu-user binfmt support for running the arm64 container (qemu-user-static-aarch64).
#
# @see docs/cross-compiling.md

set -eu

image="docker.io/library/debian:trixie"
packages="libc6-dev libstdc++-14-dev libgcc-14-dev linux-libc-dev"

refresh=no
sysroot="${ISOLINE_AARCH64_SYSROOT:-$HOME/.cache/isoline/sysroot-aarch64-trixie}"
for arg in "$@"; do
    case "$arg" in
        --refresh) refresh=yes ;;
        -h|--help) sed -n '4,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "make-aarch64-sysroot: unknown option $arg" >&2; exit 2 ;;
        *) sysroot="$arg" ;;
    esac
done

# What the toolchain file and the C++ build need from it: the C library and libstdc++'s headers.
isComplete() {
    [ -f "$1/usr/lib/aarch64-linux-gnu/libc.so" ] && [ -d "$1/usr/include/c++/14" ] \
        && [ -d "$1/usr/include/aarch64-linux-gnu" ]
}

if [ "$refresh" = no ] && isComplete "$sysroot"; then
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
trap cleanup EXIT INT TERM

container=$(podman create --platform linux/arm64 "$image" \
    sh -c "apt-get update && apt-get install -y --no-install-recommends $packages")
podman start --attach "$container" >&2

rm -rf "$staging"
mkdir -p "$staging"
podman export "$container" | tar -x -C "$staging" --anchored --exclude=dev --exclude=proc --exclude=sys \
    usr lib etc/ld.so.conf etc/ld.so.conf.d
isComplete "$staging" || { echo "make-aarch64-sysroot: the export lacks the C/C++ development files" >&2; exit 1; }

rm -rf "$sysroot"
mv "$staging" "$sysroot"
echo "make-aarch64-sysroot: created" >&2
echo "$sysroot"
