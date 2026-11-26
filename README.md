# Isoline

**A complete, fast and safe WebAssembly runtime and toolchain, written in modern C++23.**

> [!WARNING]
> Isoline is under active development and not ready for production use. We are working at full pace
> toward the first stable release. Stay tuned!

## Mission

Isoline aims to be the WebAssembly runtime C++ developers reach for first — and to hold its own against
the best in any language, with Wasmtime's performance as the bar, measured and published at equal safety
settings. It is standards-complete, safe by construction, and written entirely in C++23: no Rust, no
foreign toolchain, nothing in the build but a C++ compiler and CMake.

Whether it is a plugin system in a desktop application, sandboxed extensions in a terminal, a serverless
host, or a command-line tool that runs `.wasm` programs directly, Isoline is meant to be the production-grade
choice for running WebAssembly from C++.

## Why Isoline

- **One language, one toolchain.** Pure C++23 from decoder to code generator. It builds warning-free with
  GCC, Clang, Apple Clang and MSVC, links as a small set of independent libraries, and drops into an
  existing C++ project like any other dependency — no second compiler, no 100 MB of vendored runtime.
- **Fast.** An optimizing ahead-of-time compiler (own SSA IR, linear-scan register allocation,
  table-driven instruction selection) for **x86-64 and arm64**, with cachable `.isoline` objects. The bar
  is **at least Wasmtime's performance**, measured and published at equal safety settings. Where there is
  no code generator, a **fast interpreter** runs on every other 64-bit architecture, little- and big-endian.
- **Safe by construction.** Every limit is enforced and deterministic: memory, tables, call depth,
  execution steps, compile time and memory. A runaway guest can always be interrupted from another
  thread. Traps never escape as crashes, a guest can never exhaust the host's memory, executable memory is
  never writable at the same time, and cached compiled code is authenticated before it runs. Untrusted
  input is decoded without recursion and with published bounds.
- **Modern standards, nothing legacy.** WebAssembly 3.0 (minus garbage collection), the **component
  model** with async, and **100 % WASI 0.3** — and deliberately no WASI preview 1, no WASI 0.2, no
  superseded proposal drafts.
- **Built to be embedded.** A small, `std::expected`-based C++ API with no exceptions for recoverable
  failures, every OS resource reached through an injectable interface, configuration fixed at
  construction, no hidden threads, and C++ coroutines for async components and WASI I/O.
- **Engineered for quality.** The official spec suites on every engine, differential fuzzing of every
  engine against a reference interpreter, 100 % line and branch coverage as a ratchet, mutation testing,
  and ASan/UBSan/TSan builds in CI.

## What's in the suite

| Part | What it does |
|---|---|
| **Runtime library** | Decoder, validator and the embedding API: engines, modules, stores, instances, host functions, limits, interrupts |
| **Three engines** | A *reference interpreter* (spec fidelity, the fuzzing oracle), a *fast interpreter* (portable production engine), and an *optimizing compiler* for x86-64 and arm64 — chosen per engine at construction, never silently swapped |
| **AOT cache** | Cachable, authenticated `.isoline` objects, with an engine-free verifier a host process can link without the compiler |
| **Component model** | Component binaries, the canonical ABI, resources, linking — and async streams and futures |
| **WASI 0.3** | Every 0.3 package (`cli`, `clocks`, `random`, `filesystem`, `sockets`, `http`), capability-based and sandboxed, with a C++ binding generator for WIT |
| **Text format** | A `.wat` reader and writer for modules and components, and the `.wast` script runner the spec suites need |
| **`isoline` CLI** | The suite on the command line |

```sh
isoline compile      app.wat      -o app.wasm      # text → binary
isoline disassemble  app.wasm     -o app.wat       # binary → text
isoline aot          app.wasm     -o app.isoline   # binary → cachable native object
isoline run          app.wasm     [args…]          # run a WASI 0.3 command or a module's export
isoline serve        app.wasm     --listen <addr>  # host a wasi:http server component
```

`run` grants arguments, standard I/O, clocks and randomness by default; directories, environment
variables and network access are explicit opt-ins, and a granted directory can never be escaped.

## A taste of the embedding API

A sketch of the shape the API is settling into (it will still change before 1.0):

```cpp
#include <isoline/api/Engine.hpp>
#include <isoline/api/Store.hpp>

auto const engine = isoline::api::Engine { isoline::api::EngineConfig { /* features, engine, limits */ } };

auto module = engine.load(wasmBytes);               // std::expected<Module, LoadError>
if (!module)
    return report(module.error());                  // invalid module, or over a configured limit

auto store = isoline::api::Store { engine, memoryAllocator };
auto imports = isoline::api::Imports {};
imports.define("env", "log", logFunction);          // a host function

auto instance = store.instantiate(*module, imports); // std::expected<Instance, InstantiateError>
auto result = instance->call("add", { 2, 3 });       // traps, limits and interrupts come back as values
```

## Roadmap

Isoline is built in phases, each landing as reviewed, fully tested pull requests.

| Phase | | Status |
|---|---|---|
| P1 | Foundation: build and CI, decoder, validator and reference interpreter for WebAssembly 1.0 and 2.0 | done |
| P2 | Full core language: SIMD, tail calls, exceptions, memory64, multiple memories and the rest of WebAssembly 3.0 minus GC | done |
| P3 | Runtime and embedding API: limits, interrupts, deterministic budgets, the public API | in progress |
| P4 | Fast interpreter | in progress |
| P10 | Text format: `.wat` / `.wast` | in progress |
| P5 | Optimizing compiler, x86-64 | planned |
| P6 | arm64 code generation | planned |
| P7 | Code memory, guard pages and fault handling on every OS | planned |
| P8 | AOT cache and CLI | planned |
| P11 | Component model | planned |
| P12 | Component model async | planned |
| P14 | WASI 0.3 | planned |
| P15 | 1.0 hardening: fuzzing campaigns, mutation testing, published benchmarks, release | planned |

Isoline's first consumer is [Contour](https://github.com/contour-terminal/contour), the terminal emulator,
which runs its extensions as WebAssembly. Contour switching its extension host to Isoline — and dropping
the two runtimes it carries today — is the first milestone that proves the runtime in production.

## Platforms

| Tier | Platforms | Engines |
|---|---|---|
| 1 (release-gating) | Linux x86-64 and arm64, macOS arm64, Windows x86-64 | compiler + interpreters |
| 2 (tested in CI) | Windows arm64, macOS x86-64, FreeBSD x86-64 and arm64, s390x (emulated) | compiler where available, interpreters everywhere |
| Everything else 64-bit | ppc64le, riscv64, s390x, … | fast interpreter |

## Building

Requirements: CMake 3.25 or newer, Ninja, a C++23 compiler (GCC, Clang, Apple Clang or MSVC) and
Catch2 v3 for the tests (or `-DISOLINE_FETCH_DEPS=ON` to fetch it).

```sh
cmake --preset clang-asan          # Clang, Debug, ASan + UBSan, clang-tidy
cmake --build --preset clang-asan
ctest --preset clang-asan
```

Other presets: `clang-release`, `clang-coverage`, `gcc-debug`, `macos-clang` and `msvc-debug`
(from a Visual Studio Developer Command Prompt). Every preset builds into `out/<preset>/`.

From a Linux host, Isoline also cross-compiles for Windows (clang-cl, tests under wine) and arm64
(clang, tests under qemu); see [docs/cross-compiling.md](docs/cross-compiling.md).

## Conformance

Isoline implements the WebAssembly 3.0 core language without garbage collection, plus the wide
arithmetic, custom page sizes and compact import section proposals. It runs the official 3.0 spec
suite and each proposal's suite; [docs/conformance.md](docs/conformance.md) pins every suite and lists
each excluded file and skipped assertion with its reason.

## Contributing

Isoline follows the contour-terminal C++ guidelines in [AGENT.md](AGENT.md): C++23, `std::expected` for
fallible APIs, `enum class` over `bool`, dependency injection for every external resource, configuration
at construction, data-driven design, and a zero-warning build. New code lands with tests.

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
