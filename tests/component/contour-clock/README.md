# Contour's clock extension as a component

Contour's migration path (plan P11 Task 22): one of the status-line extensions Contour bundles, built exactly as
Contour builds it, turned into a component, and hosted on Isoline as Contour's extension host (P9) would host it.
The host side is `ContourClock_test.cpp`; the WASI 0.2 the component imports is bound by a test double,
`Wasi02Subset`, because Isoline ships no WASI 0.2.

| File | What it is |
|---|---|
| `clock.wasm` | The component: Contour's `extensions/bundled/clock` crate, a `wasm32-wasip1` core module, componentized with the preview-1 reactor adapter. Prebuilt. |
| `build-clock.sh` | Rebuilds `clock.wasm` from Contour's sources; `--check` compares instead. |
| `Wasi02Subset.hpp`, `Wasi02Subset.cpp` | The test double of the WASI 0.2 interfaces `clock.wasm` imports. |
| `wasi-subset.wat` | A component that imports those interfaces and re-exports each, so that the tests call the double as a guest does. |
| `ContourClock_test.cpp` | The tests (`isoline_component_contour_test`). |

## How it is built

The crate is Contour's at commit `54387ba8140a81e62e92a2b1beb9a76b591be04f` (`extension.contour.commit` in
`scripts/tools.lock`), built as Contour's `cmake/ContourCargo.cmake` builds it: `cargo build --release --locked
--target wasm32-wasip1 -p clock` in its `extensions/` workspace, with Contour's `Cargo.lock` (whose `wit-bindgen` crate is
0.63.0, the release `wit-bindgen.version` pins) and Contour's Rust, 1.100.0 (`scripts/install-rust-wasm.sh` there).
Rust builds a reactor: a core module that exports `_initialize` and imports `wasi_snapshot_preview1`. `wasm-tools
component new --adapt wasi_snapshot_preview1=<adapter>` turns it into a component, the adapter being Wasmtime
51.0.0's `wasi_snapshot_preview1.reactor.wasm`: it implements the preview-1 functions the module imports
(`environ_get`, `environ_sizes_get`, `clock_time_get`, `fd_write`, `proc_exit`) over WASI 0.2, and wit-component keeps
only what those reach.

The build is reproducible: `build-clock.sh` remaps the paths of the machine it runs on (the work directory, the Rust
installation, the cargo home), and cargo hashes the workspace's crates by their paths in it, so a build anywhere
gives the same bytes. No build of Isoline runs it, and no test needs its tools. After a change of a pin, rebuild it
with the tools `scripts/tools.lock` pins, and update the digest below:

```sh
fetch() { python3 -I scripts/fetch-tools.py --tool "$1"; }
WASM_TOOLS="$(fetch wasm-tools)" RUST_INSTALLER="$(fetch rust)" \
RUST_STD_INSTALLER="$(fetch rust-std-wasm32-wasip1)" WASI_ADAPTER="$(fetch wasi-preview1-adapter)" \
    tests/component/contour-clock/build-clock.sh
```

It fetches the Contour sources at the commit from `extension.contour.repository` (or `$CONTOUR_REPOSITORY`, a local
clone, say) and the crates from crates.io, into `$CARGO_HOME` if set. CI runs it with `--check` (the
`extension-guests` job).

| File | Built by | SHA-256 |
|---|---|---|
| `clock.wasm` | Rust 1.100.0, `wasm-tools component new` 1.263.0, Wasmtime 51.0.0's reactor adapter | `f2e2555d97a71117de3775c917d036c177bf490190780ddc857e254438d897b5` |

## What the component is

- **Four core modules:** the crate's, the adapter, and wit-component's shim and fixup modules.
- **One memory.** The adapter imports the main module's memory instead of defining one, so the component fits
  Contour's provision of one memory per extension (A9: `ContourClock.embedderMemory`). At its first preview-1 call
  that needs it (here `fd_write`) the adapter allocates a 64 KiB stack and its 64 KiB state through the main module's
  `cabi_realloc`.
- **`_initialize` runs once, at instantiation.** wit-component makes the reactor's `_initialize` the start function of
  its fixup module, the last core instance the component makes, so instantiating the component runs it before any
  export can be called; no export reaches it. wasi-libc's guard word (at 1,054,608 in the main module's memory) is set
  by it and traps a second run (`ContourClock.initializeRunsOnce`).
- **The features it needs** of Contour's `EngineFeatures` On rows: reference-types (and so bulk-memory, which it is
  built on) and sign-extension. It loads without saturating-float-to-int, multi-value or extended-const, and needs
  none of the component model's gates. Contour's own `component-model` row is Off while it runs core modules;
  hosting components turns it On for its Wasmtime engine (P9).
- **Its stderr.** Rust writes nothing to a standard stream in normal operation. An allocation of its own that fails
  is reported on stderr (`fd_write`, which the adapter writes to `wasi:cli/stderr`'s stream), and the guest aborts. An
  allocation of wit-bindgen's `cabi_realloc` that fails (the canonical ABI's, for an argument or a result) is a bare
  `unreachable` in a release build and writes nothing.

## P9's to-do: the WASI 0.2 Contour's extension host binds

`clock.wasm` imports these eleven interfaces of WASI 0.2.12, besides the world's
`terminal-extensions:statusline/types@0.1.0` and `/host@0.1.0` (whose `request-frame` and `state` it calls). Contour's
extension host binds them in its own `Linker`, as it keeps its preview-1 shim today; `Wasi02Subset` is the test-double
version of the same rows, and `ContourClock.importsAreTheAdapterSubset` pins the list.

| Interface | What the adapter imports of it |
|---|---|
| `wasi:cli/environment` | `get-environment` |
| `wasi:cli/exit` | `exit` |
| `wasi:io/error` | the resource `error` |
| `wasi:io/streams` | the resources `output-stream` and `input-stream`; `output-stream`'s `check-write`, `write`, `blocking-write-and-flush`, `blocking-flush` |
| `wasi:cli/stdin`, `wasi:cli/stdout`, `wasi:cli/stderr` | `get-stdin`, `get-stdout`, `get-stderr` |
| `wasi:clocks/monotonic-clock`, `wasi:clocks/wall-clock` | `now` each |
| `wasi:filesystem/types` | the resource `descriptor`; its `write-via-stream`, `append-via-stream`, `get-type`, `stat`; `filesystem-error-code` |
| `wasi:filesystem/preopens` | `get-directories` |

The double: output streams capture what is written to them, stdout and stderr apart; both clocks read a fake clock
the test advances; `get-environment` answers what it is given; `exit` fails its call, which traps the guest and locks
the store down; no directory is preopened, and every descriptor method answers `bad-descriptor`. The adapter imports
no random source.
