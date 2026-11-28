#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# The fallback route to running the suite on arm64 from an x86-64 Linux host: a Debian trixie arm64
# container, in which everything is arm64 and runs under the host's qemu binfmt registration. It builds
# natively inside with GCC 14 (gcc-release) and runs ctest. Every compile runs emulated, so it is far
# slower than scripts/arm64-cross.sh; it exists for when the host's cross toolchain is unavailable or
# suspected.
#
# Usage: scripts/arm64-podman.sh [--cpu max|cortex-a53] [-- <ctest arguments>]
#
# The build image is made once from the sysroot's pinned Debian image with the build packages installed, and
# kept as a local image named after both (localhost/isoline-arm64-build:<hash>); the package versions it got
# are recorded in it and printed on every run. Later runs reuse it, so they need no network once it and the
# build's fetched dependencies exist. The tree is mounted read-only; the build is out/gcc-arm64-podman, which
# persists between runs. Exits with ctest's status. Environment: ISOLINE_BUILD_JOBS (default: every core).

set -eu

cpu=max
while [ $# -gt 0 ]; do
    case "$1" in
        --cpu) [ $# -ge 2 ] || { echo "arm64-podman: --cpu needs a value" >&2; exit 2; }; cpu="$2"; shift 2 ;;
        --) shift; break ;;
        -h|--help) sed -n '4,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "arm64-podman: unknown option $1" >&2; exit 2 ;;
    esac
done

# The emulated CPUs: the same rows as scripts/arm64-cross.sh's.
case "$cpu" in
    max) qemuCpu="max,pauth-impdef=on" ;;
    cortex-a53) qemuCpu="cortex-a53" ;;
    *) echo "arm64-podman: unknown CPU $cpu (max or cortex-a53)" >&2; exit 2 ;;
esac

command -v podman >/dev/null || { echo "arm64-podman: podman is not installed" >&2; exit 1; }
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] \
    || { echo "arm64-podman: no qemu-aarch64 binfmt registration (qemu-user-static-aarch64)" >&2; exit 1; }

source=$(cd "$(dirname "$0")/.." && pwd)
build="$source/out/gcc-arm64-podman"
mkdir -p "$build"
jobs="${ISOLINE_BUILD_JOBS:-$(nproc)}"

# The image scripts/make-aarch64-sysroot.sh pins, so that both routes build against the same system, and the
# packages the build needs in it.
base=$("$source/scripts/make-aarch64-sysroot.sh" --image)
packages="g++-14 cmake ninja-build python3 git ca-certificates"
image="localhost/isoline-arm64-build:$(printf '%s %s' "$base" "$packages" | sha256sum | cut -c1-16)"
if ! podman image exists "$image"; then
    context=$(mktemp -d)
    printf '%s\n' "FROM $base" \
        "RUN apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -qy --no-install-recommends $packages \\" \
        "    && dpkg-query -W -f '\${Package} \${Version}\\n' $packages > /isoline-build-packages.txt \\" \
        "    && rm -rf /var/lib/apt/lists/*" > "$context/Containerfile"
    status=0
    podman build --platform linux/arm64 -t "$image" "$context" >&2 || status=$?
    rm -rf "$context"
    [ "$status" -eq 0 ] || { echo "arm64-podman: building $image failed" >&2; exit "$status"; }
fi
echo "arm64-podman: $image, from $base:"
podman run --rm --platform linux/arm64 "$image" cat /isoline-build-packages.txt | sed 's/^/arm64-podman:   /'

# The ctest arguments, each quoted for the container's shell.
quoted=""
for argument in "$@"; do
    quoted="$quoted '$(printf '%s' "$argument" | sed "s/'/'\\\\''/g")'"
done

# SELinux labels keep a container from reading the host's tree; the mounts are the container's whole view of
# it, so label separation is turned off rather than the tree relabelled.
exec podman run --rm --platform linux/arm64 --security-opt label=disable \
    -e QEMU_CPU="$qemuCpu" \
    -v "$source:/src:ro" -v "$build:/build" \
    "$image" sh -c "set -eu
        cd /src && cmake --preset gcc-release -B /build -DCMAKE_CXX_COMPILER=g++-14 -DISOLINE_FETCH_DEPS=ON
        cmake --build /build -j $jobs
        ctest --test-dir /build -j $jobs -LE 'perf|differential' --output-on-failure $quoted"
