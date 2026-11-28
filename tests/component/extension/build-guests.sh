#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Rebuilds the extension world's two prebuilt guests (plan P11 Task 21) from their sources:
#
#   extension.wasm  from extension.wat, by `wasm-tools parse`
#   guest.wasm      from guest.c, by `wit-bindgen c`, wasi-sdk's clang (wasm32-unknown-unknown, no libc)
#                   and `wasm-tools component new`
#
# No build of Isoline runs it: both outputs are committed, with their SHA-256 in README.md, so that the
# tests need none of these tools. Run it after changing a source, then update README.md's digests.
# The tools are the versions scripts/tools.lock pins; scripts/fetch-tools.py fetches wasm-tools and
# wit-bindgen, and wasi-sdk is found at $WASI_SDK.
#
# Usage: build-guests.sh [--check]
#   --check  rebuilds into a temporary directory and compares with the committed files instead.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
scripts="${here}/../../../scripts"
lock="${scripts}/tools.lock"

pinned() {
    sed -n "s/^$1 = //p" "${lock}"
}

# The SHA-256 of a file, by whichever of the two common tools the host has.
digest() {
    if command -v sha256sum > /dev/null; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

wasm_tools="${WASM_TOOLS:-wasm-tools}"
wit_bindgen="${WIT_BINDGEN:-wit-bindgen}"
wasi_sdk="${WASI_SDK:?set WASI_SDK to a wasi-sdk $(pinned wasi-sdk.version) installation}"
clang="${wasi_sdk}/bin/clang"

# Each tool must be the version the lock pins, as fetch-tools.py checks a pinned tool's version line; wasi-sdk's
# clang reports its LLVM, so its own VERSION file is read instead.
python3 -I "${scripts}/fetch-tools.py" --check-version "${wasm_tools}" --tool wasm-tools > /dev/null
python3 -I "${scripts}/fetch-tools.py" --check-version "${wit_bindgen}" --tool wit-bindgen > /dev/null
if [[ "$(head -n 1 "${wasi_sdk}/VERSION")" != "$(pinned wasi-sdk.version)" ]]; then
    echo "build-guests.sh: ${wasi_sdk} is not wasi-sdk $(pinned wasi-sdk.version) (scripts/tools.lock)" >&2
    exit 1
fi

check=0
if [[ "${1:-}" == "--check" ]]; then
    check=1
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

"${wasm_tools}" parse "${here}/extension.wat" -o "${work}/extension.wasm"
"${wasm_tools}" validate --features component-model "${work}/extension.wasm"

"${wit_bindgen}" c "${here}/extension.wit" --out-dir "${work}/bindings" > /dev/null
# The bindings include two C library headers; guest.c defines what they declare.
mkdir -p "${work}/libc"
cat > "${work}/libc/stdlib.h" <<'HEADER'
#pragma once
#include <stddef.h>
void abort(void);
void* malloc(size_t size);
void* realloc(void* pointer, size_t size);
void free(void* pointer);
HEADER
cat > "${work}/libc/string.h" <<'HEADER'
#pragma once
#include <stddef.h>
void* memcpy(void* destination, void const* source, size_t count);
size_t strlen(char const* text);
HEADER
"${clang}" --target=wasm32-unknown-unknown -nostdlib -O2 -Wall -Wextra -Werror \
    -I "${work}/bindings" -I "${work}/libc" \
    -Wl,--no-entry \
    -o "${work}/guest.core.wasm" \
    "${here}/guest.c" "${work}/bindings/extension.c" "${work}/bindings/extension_component_type.o"
"${wasm_tools}" component new "${work}/guest.core.wasm" -o "${work}/guest.wasm"
"${wasm_tools}" validate --features component-model "${work}/guest.wasm"

status=0
for output in extension.wasm guest.wasm; do
    if [[ ${check} -eq 1 ]]; then
        if ! cmp -s "${work}/${output}" "${here}/${output}"; then
            echo "build-guests.sh: ${output} differs from what its source builds" >&2
            status=1
        fi
    else
        cp "${work}/${output}" "${here}/${output}"
    fi
    echo "$(digest "${work}/${output}")  ${output}"
done
exit ${status}
