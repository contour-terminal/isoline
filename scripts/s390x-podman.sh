#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Configures, builds and tests the gcc-s390x preset -- big-endian s390x, GCC 14's cross compiler, the tests
# under qemu-user -- in an Ubuntu 24.04 container, the packages CI's linux-gcc-s390x-qemu job installs, so that
# a host without them (Fedora has no s390x cross g++) runs what CI runs.
#
# Usage: scripts/s390x-podman.sh [--jobs <n>] [--build-only] [--] [<ctest argument>...]
#
#   --jobs <n>     build and test with n jobs; default $ISOLINE_S390X_JOBS, else every processor
#   --build-only   configure and build, run no test
#   <ctest ...>    passed to `ctest --preset gcc-s390x` after the preset's own selection (the CI one,
#                  -L "unit|interpreter" -LE "perf|wallclock"), e.g. -R LittleEndian
#
# The source tree is mounted read-only at its own path, out/gcc-s390x writable, so that the build tree's
# paths are the host's. The toolchain image is built once and kept as localhost/isoline-s390x:<tag>; a
# change to the package list below makes a new tag. Needs podman.
#
# @see docs/cross-compiling.md

set -eu

# The image is pinned by its multi-arch manifest digest (ubuntu:24.04 as of 2026-08-17), so the base
# system is reproducible; apt-get still installs the packages current in the noble archive.
base="docker.io/library/ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517"
packages="g++-14-s390x-linux-gnu qemu-user cmake ninja-build python3 git ca-certificates"
tag="noble-gcc14-1"
image="localhost/isoline-s390x:$tag"

jobs="${ISOLINE_S390X_JOBS:-$(nproc)}"
buildOnly=no
while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) [ $# -ge 2 ] || { echo "s390x-podman: --jobs needs a number" >&2; exit 2; }
                jobs="$2"; shift 2 ;;
        --build-only) buildOnly=yes; shift ;;
        -h|--help) sed -n '4,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; break ;;
        *) break ;;
    esac
done

command -v podman >/dev/null || { echo "s390x-podman: podman is not installed" >&2; exit 1; }

source="$(cd "$(dirname "$0")/.." && pwd)"
build="$source/out/gcc-s390x"
mkdir -p "$build"

if ! podman image exists "$image"; then
    printf 'FROM %s\nRUN apt-get update && apt-get install -y --no-install-recommends %s && rm -rf /var/lib/apt/lists/*\n' \
        "$base" "$packages" | podman build --tag "$image" --file - >&2
fi

script='cmake --preset gcc-s390x && cmake --build --preset gcc-s390x -j "$0"'
if [ "$buildOnly" = no ]; then
    script="$script"' && ctest --preset gcc-s390x -j "$0" "$@"'
fi

# keep-id: the build tree belongs to the caller, not to a subordinate uid. No SELinux label separation: a
# relabel (:z) would rewrite the labels of the caller's whole source tree, and without one the container
# cannot read it on an enforcing host (Fedora).
exec podman run --rm --userns=keep-id --security-opt label=disable --env HOME=/tmp \
    --volume "$source:$source:ro" --volume "$build:$build" --workdir "$source" \
    "$image" sh -c "$script" "$jobs" "$@"
