#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Builds the benchmark guests (benchmarks/sources/*.c) into benchmarks/guests/*.wasm and pins them in
# benchmarks/guests.lock, which scripts/benchmark.py checks before it times one. Maintainer tooling: the
# guests are committed, so neither a build nor a test runs this.
#
# Each guest is one kernel and the shim (benchmarks/shim/), compiled freestanding for wasm32 by the
# pinned wasi-sdk's clang and linked without libc: it imports nothing and exports run(i32) -> i64 and its
# memory. The feature list is pinned, and so is what it must stay within: every guest but the SIMD one
# validates under the features of Contour's WAMR build (benchmarks/wamr/ContourOptions.cmake), which
# BenchmarkGuests.validateUnderContoursWamrFeatures checks again on Isoline's validator.
#
# Usage: benchmarks/build-guests.sh [<wasi-sdk directory>]
#        (default: $WASI_SDK_PATH, else ~/usr/opt/wasi-sdk-34.0-x86_64-linux)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
wasiSdk="${1:-${WASI_SDK_PATH:-${HOME}/usr/opt/wasi-sdk-34.0-x86_64-linux}}"
clang="${wasiSdk}/bin/clang"
# The wasi-sdk release the guests are pinned to; another one is refused, as it would change the guests.
pinnedClang="clang version 23.1.0-wasi-sdk"

if [[ ! -x "${clang}" ]]; then
    echo "error: no clang at ${clang}; pass the wasi-sdk directory" >&2
    exit 2
fi
clangVersion="$("${clang}" --version | head -n 1)"
if [[ "${clangVersion}" != "${pinnedClang}"* ]]; then
    echo "error: ${clang} is '${clangVersion}', not the pinned '${pinnedClang}'" >&2
    exit 2
fi
wasmTools="$(python3 -I "${root}/scripts/fetch-tools.py" --tool wasm-tools)"

# The pinned feature list: the MVP plus what Contour's WAMR is built with and every engine accepts.
# Multi-value, reference types and the rest of clang's default CPU are off, so that a newer clang does
# not quietly start using them. The SIMD guest adds simd128 alone.
features=(-mcpu=mvp -mmutable-globals -msign-ext -mnontrapping-fptoint -mbulk-memory)
simdFeatures=(-msimd128)
# What wasm-tools validates each guest under: the same list, by its names.
validateFeatures="mvp,mutable-global,sign-extension,saturating-float-to-int,bulk-memory"
validateSimdFeatures="${validateFeatures},simd"

common=(--target=wasm32-unknown-unknown -O2 -std=c17 -nostdlib -I "${here}/shim")
# The kernels keep the compiler's builtins (a struct copy may become memory.copy); the shim is
# freestanding, so that its own mem functions are not compiled into calls of themselves.
link=(-Wl,--no-entry -Wl,--export=run -Wl,-z,stack-size=65536 -Wl,--strip-debug)
# compiler-rt's builtins, for what wasm32 has no instruction for (ed25519's 128-bit multiply): wasi-sdk
# ships them for its WASI targets only, and they are plain computation, which no WASI import enters.
# The validation below holds them to the feature list too.
builtins="$("${clang}" --target=wasm32-unknown-wasip1 --print-libgcc-file-name)"

scratch="$(mktemp -d)"
trap 'rm -rf "${scratch}"' EXIT

lock="${here}/guests.lock"
{
    echo "# The benchmark guests, as benchmarks/build-guests.sh built them: <guest>.wasm = <SHA-256>, and"
    echo "# <guest>.wasm.features = the wasm-tools feature list it was built and validated with."
    echo "# scripts/benchmark.py refuses a guest whose bytes differ. Rebuild with build-guests.sh."
    echo "# compiler = ${clangVersion}"
    echo "# features = ${features[*]} (simd: ${simdFeatures[*]})"
} >"${scratch}/guests.lock"

for source in "${here}"/sources/*.c; do
    name="$(basename "${source}" .c)"
    if [[ "${name}" == *simd* ]]; then
        guestFeatures=("${features[@]}" "${simdFeatures[@]}")
        guestValidate="${validateSimdFeatures}"
    else
        guestFeatures=("${features[@]}")
        guestValidate="${validateFeatures}"
    fi
    "${clang}" "${common[@]}" "${guestFeatures[@]}" -ffreestanding -c "${here}/shim/benchmark.c" \
        -o "${scratch}/${name}-shim.o"
    "${clang}" "${common[@]}" "${guestFeatures[@]}" -c "${source}" -o "${scratch}/${name}.o"
    "${clang}" "${common[@]}" "${guestFeatures[@]}" "${link[@]}" "${scratch}/${name}.o" \
        "${scratch}/${name}-shim.o" "${builtins}" -o "${scratch}/${name}.wasm"
    "${wasmTools}" validate --features "${guestValidate}" "${scratch}/${name}.wasm"
    if "${wasmTools}" print "${scratch}/${name}.wasm" | grep -q '(import '; then
        echo "error: ${name}.wasm imports something; a guest imports nothing" >&2
        exit 1
    fi
    install -m 644 "${scratch}/${name}.wasm" "${here}/guests/${name}.wasm"
    echo "${name}.wasm = $(sha256sum "${here}/guests/${name}.wasm" | cut -d ' ' -f 1)" >>"${scratch}/guests.lock"
    echo "${name}.wasm.features = ${guestValidate}" >>"${scratch}/guests.lock"
done
cp "${scratch}/guests.lock" "${lock}"
echo "built $(ls "${here}"/guests/*.wasm | wc -l) guests; pinned in ${lock}"
