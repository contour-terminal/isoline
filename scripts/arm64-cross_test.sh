#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Tests scripts/arm64-cross.sh's refusals, offline: without the qemu binfmt registration it refuses before
# building, naming the package that installs it; an unknown CPU and an unknown option are refused too.

set -eu

script="$(dirname "$0")/arm64-cross.sh"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
failures=0

expect() { # <name> <expected text> <command...>
    name="$1"; text="$2"; shift 2
    status=0
    "$@" > "$scratch/out" 2>&1 || status=$?
    if [ "$status" -eq 0 ] || ! grep -qF "$text" "$scratch/out"; then
        echo "FAIL $name: exited $status, expected a refusal naming '$text'" >&2; cat "$scratch/out" >&2
        failures=$((failures + 1))
    else
        echo "ok $name"
    fi
}

# The binfmt refusal names the registration it looked for; the package name alone would also match the
# refusal for a missing qemu.
expect "no binfmt" "$scratch/missing is absent" env ISOLINE_BINFMT_PATH="$scratch/missing" "$script" -- -N
expect "no binfmt, the package" "qemu-user-static-aarch64 (Fedora)" \
    env ISOLINE_BINFMT_PATH="$scratch/missing" "$script" -- -N
expect "unknown cpu" "unknown CPU" "$script" --cpu pentium -- -N
expect "unknown option" "unknown option" "$script" --frobnicate

[ "$failures" -eq 0 ] || { echo "$failures failed" >&2; exit 1; }
