#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Rebuilds clock.wasm (plan P11 Task 22): Contour's bundled clock extension (extensions/bundled/clock) at the commit
# scripts/tools.lock records, built as Contour builds it (cargo, release, wasm32-wasip1, Contour's Cargo.lock), then
# componentized with the preview-1 reactor adapter by `wasm-tools component new --adapt`.
#
# No build of Isoline runs it: clock.wasm is committed, with its SHA-256 in README.md, so that the tests need none of
# these tools. The tools are the versions scripts/tools.lock pins, each given by its path (scripts/fetch-tools.py
# fetches every one): $WASM_TOOLS, the installers $RUST_INSTALLER and $RUST_STD_INSTALLER (each archive's install.sh),
# and the adapter module $WASI_ADAPTER. The Contour sources are fetched from the lock's repository, or from
# $CONTOUR_REPOSITORY (a local clone, say), and the crates from crates.io into $CARGO_HOME (a temporary one if unset).
#
# The build is reproducible: every path of this machine it would embed (the work directory, the Rust installation,
# the cargo home) is remapped to a fixed one, and cargo hashes the workspace's crates by their paths in it.
#
# Usage: build-clock.sh [--check]
#   --check  rebuilds into a temporary directory and compares with the committed clock.wasm instead.

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

fail() {
    echo "build-clock.sh: $*" >&2
    exit 1
}

check=0
if [[ "${1:-}" == "--check" ]]; then
    check=1
fi

wasm_tools="${WASM_TOOLS:-wasm-tools}"
rust_installer="${RUST_INSTALLER:?set RUST_INSTALLER to the install.sh of the Rust $(pinned rust.version) archive}"
rust_std_installer="${RUST_STD_INSTALLER:?set RUST_STD_INSTALLER to the install.sh of rust-std-wasm32-wasip1}"
adapter="${WASI_ADAPTER:?set WASI_ADAPTER to the preview-1 reactor adapter (wasi-preview1-adapter)}"
repository="${CONTOUR_REPOSITORY:-$(pinned extension.contour.repository)}"
commit="$(pinned extension.contour.commit)"

python3 -I "${scripts}/fetch-tools.py" --check-version "${wasm_tools}" --tool wasm-tools > /dev/null
# The adapter is a module, which names no version: it is the pinned one if it has the pinned digest.
if [[ "$(digest "${adapter}")" != "$(pinned wasi-preview1-adapter.sha256.x86_64-linux)" ]]; then
    fail "${adapter} is not the adapter of Wasmtime $(pinned wasi-preview1-adapter.version) (scripts/tools.lock)"
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# The toolchain: the host's rustc, cargo and standard library (which build scripts and wit-bindgen's macros run
# on), and the wasm32-wasip1 standard library beside them.
host_std="$(cd "$(dirname "${rust_installer}")" && ls -d rust-std-*)"
"${rust_installer}" --prefix="${work}/rust" --disable-ldconfig --components="rustc,cargo,${host_std}" > /dev/null
"${rust_std_installer}" --prefix="${work}/rust" --disable-ldconfig > /dev/null
python3 -I "${scripts}/fetch-tools.py" --check-version "${work}/rust/bin/rustc" --tool rust > /dev/null

# The sources: the crate's workspace (extensions/) and the WIT it generates its bindings from (wit/), at the commit.
git init -q "${work}/contour"
git -C "${work}/contour" sparse-checkout set --no-cone /extensions/ /wit/
git -C "${work}/contour" fetch -q --depth 1 --filter=blob:none "${repository}" "${commit}"
git -C "${work}/contour" -c advice.detachedHead=false checkout -q FETCH_HEAD

# The crate's bindings are those of the wit-bindgen release the lock pins, whose C generator built guest.c.
locked="$(sed -n '/^name = "wit-bindgen"$/{n;s/^version = "\(.*\)"$/\1/p;}' "${work}/contour/extensions/Cargo.lock")"
if [[ "${locked}" != "$(pinned wit-bindgen.version)" ]]; then
    fail "Contour's Cargo.lock has wit-bindgen ${locked}, scripts/tools.lock $(pinned wit-bindgen.version)"
fi

cargo_home="${CARGO_HOME:-${work}/cargo-home}"
remaps=(
    "--remap-path-prefix=${work}/contour=/contour"
    "--remap-path-prefix=${cargo_home}=/cargo"
    "--remap-path-prefix=${work}/rust=/rust"
)
(
    cd "${work}/contour/extensions"
    env -i PATH="${work}/rust/bin:/usr/bin:/bin" HOME="${work}" CARGO_HOME="${cargo_home}" \
        CARGO_TARGET_DIR="${work}/target" RUSTFLAGS="${remaps[*]}" \
        cargo build --quiet --release --locked --target wasm32-wasip1 -p clock
)

"${wasm_tools}" component new "${work}/target/wasm32-wasip1/release/clock.wasm" \
    --adapt "wasi_snapshot_preview1=${adapter}" -o "${work}/clock.wasm"
"${wasm_tools}" validate --features component-model "${work}/clock.wasm"

status=0
if [[ ${check} -eq 1 ]]; then
    if ! cmp -s "${work}/clock.wasm" "${here}/clock.wasm"; then
        echo "build-clock.sh: clock.wasm differs from what Contour ${commit} builds" >&2
        status=1
    fi
else
    cp "${work}/clock.wasm" "${here}/clock.wasm"
fi
echo "$(digest "${work}/clock.wasm")  clock.wasm"
exit ${status}
