#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Builds the aarch64 cross tree once and runs its tests under qemu-user on one emulated CPU: the local
# route to executing arm64 code on an x86-64 Linux host (docs/cross-compiling.md).
#
# Usage: scripts/arm64-cross.sh [--cpu max|cortex-a53] [--preset <preset>] [-- <ctest arguments>]
#
#   --cpu      the emulated CPU (default max): max is every extension qemu implements (PAC, BTI, CSSC,
#              FEAT_AFP, ...), with pauth-impdef so that PAC costs a few host instructions, not QARMA5's
#              hundreds; cortex-a53 is a baseline ARMv8.0 core, on which the hint-space instructions are NOPs
#   --preset   the configure, build and test preset (default clang-aarch64-cross-release)
#
# Prints the qemu version, QEMU_CPU and the guest's AT_HWCAP and AT_HWCAP2, then exits with ctest's status.
# Programs a test starts itself run through the kernel's binfmt registration, which honours the exported
# QEMU_CPU and QEMU_LD_PREFIX, so the script refuses to run without it. ctest runs under a 6 GiB
# address-space cap.
#
# Environment: ISOLINE_AARCH64_SYSROOT (the sysroot, as the toolchain file reads it), ISOLINE_CONFIGURE_ARGS
# (added to a first configure, e.g. -DPACE_SCHEDULER=...), ISOLINE_BUILD_JOBS (default: every core),
# ISOLINE_BINFMT_PATH (default /proc/sys/fs/binfmt_misc/qemu-aarch64; tests point it elsewhere).

set -eu

cpu=max
preset=clang-aarch64-cross-release
while [ $# -gt 0 ]; do
    case "$1" in
        --cpu) [ $# -ge 2 ] || { echo "arm64-cross: --cpu needs a value" >&2; exit 2; }; cpu="$2"; shift 2 ;;
        --preset) [ $# -ge 2 ] || { echo "arm64-cross: --preset needs a value" >&2; exit 2; }; preset="$2"; shift 2 ;;
        --) shift; break ;;
        -h|--help) sed -n '4,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "arm64-cross: unknown option $1" >&2; exit 2 ;;
    esac
done

# The emulated CPUs, one row each: the name the script takes, and qemu's QEMU_CPU for it.
case "$cpu" in
    max) qemuCpu="max,pauth-impdef=on" ;;
    cortex-a53) qemuCpu="cortex-a53" ;;
    *) echo "arm64-cross: unknown CPU $cpu (max or cortex-a53)" >&2; exit 2 ;;
esac

binfmt="${ISOLINE_BINFMT_PATH:-/proc/sys/fs/binfmt_misc/qemu-aarch64}"
if [ ! -e "$binfmt" ]; then
    echo "arm64-cross: $binfmt is absent: aarch64 programs a test starts would not run. Install" \
         "qemu-user-static-aarch64 (Fedora) or qemu-user-static (Debian, Ubuntu), which registers it." >&2
    exit 1
fi
qemu=$(command -v qemu-aarch64-static || command -v qemu-aarch64 || true)
[ -n "$qemu" ] || { echo "arm64-cross: qemu-aarch64-static is not installed (qemu-user-static-aarch64)" >&2; exit 1; }

source=$(cd "$(dirname "$0")/.." && pwd)
sysroot="${ISOLINE_AARCH64_SYSROOT:-$HOME/.cache/isoline/sysroot-aarch64-trixie}"
"$source/scripts/make-aarch64-sysroot.sh" --check "$sysroot" \
    || { echo "arm64-cross: create the sysroot with scripts/make-aarch64-sysroot.sh" >&2; exit 1; }
export ISOLINE_AARCH64_SYSROOT="$sysroot"
export QEMU_LD_PREFIX="$sysroot"
export QEMU_CPU="$qemuCpu"

echo "arm64-cross: $("$qemu" --version | head -n 1)"
echo "arm64-cross: QEMU_CPU=$QEMU_CPU"
# The guest's hardware capabilities, as glibc's loader reports them for any program of the sysroot.
LD_SHOW_AUXV=1 "$qemu" "$sysroot/usr/bin/true" | grep -E '^AT_HWCAP2?:' | sed 's/^/arm64-cross: /'

cd "$source"
# A configured tree keeps the sysroot it was configured with; the tests must load the one checked above.
if [ -f "out/$preset/CMakeCache.txt" ]; then
    configured=$(sed -n 's/^ISOLINE_AARCH64_SYSROOT:PATH=//p' "out/$preset/CMakeCache.txt")
    if [ "$configured" != "$sysroot" ]; then
        echo "arm64-cross: out/$preset was configured with the sysroot $configured, not $sysroot;" \
             "remove the tree or set ISOLINE_AARCH64_SYSROOT" >&2
        exit 1
    fi
else
    cmake --preset "$preset" ${ISOLINE_CONFIGURE_ARGS:-}
fi
cmake --build --preset "$preset" -j "${ISOLINE_BUILD_JOBS:-$(nproc)}"

status=0
prlimit --as=$((6 * 1024 * 1024 * 1024)) -- ctest --preset "$preset" -j "${ISOLINE_BUILD_JOBS:-$(nproc)}" "$@" \
    || status=$?
exit "$status"
