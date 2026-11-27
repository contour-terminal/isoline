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

Isoline's first consumer is [Contour](https://github.com/contour-terminal/contour), the terminal emulator,
which runs its extensions as WebAssembly. Contour switching its extension host to Isoline — and dropping
the two runtimes it carries today — is the first milestone that proves the runtime in production.

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

The API as it stands (it may still change before 1.0). This is real code: the test suite compiles and
runs it (`src/isoline/api/ReadmeExample_test.cpp`).

```cpp
#include <isoline/api/Engine.hpp>
#include <isoline/api/Store.hpp>

auto const engine =
    isoline::api::Engine { isoline::api::EngineConfig { /* features, engine, limits */ } };

auto const module = engine.load(wasmBytes); // std::expected<Module, LoadError>
if (!module)
    return report(module.error()); // invalid module, or over a configured limit

auto store = isoline::api::Store { engine, memoryAllocator };
auto imports = isoline::api::Imports {};
imports.define("env", "log", logType, logFunction); // a host function: its type and implementation

auto const instance = store.instantiate(*module, imports); // std::expected<Instance, InstantiateError>
if (!instance)
    return report(instance.error());

auto const args = std::array<isoline::api::Value, 2> { isoline::api::I32 { 2 }, isoline::api::I32 { 3 } };
auto results = std::array<isoline::api::Value, 1> {};
auto const called =
    instance->call(*instance->function("add"), args, results); // traps come back as values
```

[docs/embedding.md](docs/embedding.md) is the embedder's contract as it stands: the life cycle, the thread
and lifetime rules, every limit with its default, every place a running guest can be interrupted, and
every way a load, a call or an instantiation fails.

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

Other presets: `clang-release`, `clang-coverage`, `gcc-debug`, `gcc-release`, `macos-clang` and `msvc-debug`
(from a Visual Studio Developer Command Prompt). Every preset builds into `out/<preset>/`.

A top-level build installs as the CMake package `isoline` (`-DISOLINE_INSTALL=OFF` turns that off), with
the components `core` (`isoline::core`) and `text` (`isoline::text`, the text format). Its one dependency,
[core-cpp](https://github.com/contour-terminal/core-cpp), is not installed with it: install core-cpp
first, into the same prefix, built by the same compiler and standard library as the consumer.

```sh
cmake -S vendor/core-cpp -B out/core-cpp -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCORE_CPP_INSTALL=ON -DCORE_CPP_TESTING=OFF -DCORE_CPP_CATCH2_MAIN=OFF \
      -DCORE_CPP_BUILD_EXAMPLES=OFF -DCORE_CPP_FETCH_DEPS=OFF \
      -DCORE_CPP_WITH_TUI=OFF -DCORE_CPP_WITH_TUI_OUTPUT=OFF -DCORE_CPP_WITH_TUI_CORE=OFF \
      -DCORE_CPP_WITH_TLS=OFF -DCORE_CPP_WITH_SYSTEMD=OFF
cmake --build out/core-cpp
cmake --install out/core-cpp --prefix <prefix> --component core-cpp
cmake --install out/clang-release --prefix <prefix> --component isoline
```

A consumer configured with `-DCMAKE_PREFIX_PATH=<prefix>` then finds it:

```cmake
find_package(isoline 0.1 REQUIRED COMPONENTS core)
target_link_libraries(app PRIVATE isoline::core)
```

From a Linux host, Isoline also cross-compiles for Windows (clang-cl, tests under wine) and arm64
(clang, tests under qemu); see [docs/cross-compiling.md](docs/cross-compiling.md).

With `-DISOLINE_MODULES=ON` (CMake 4.3 or newer, a Ninja generator for the installed package), or one of
the module presets `clang-modules`, `clang-modules-release`, `gcc-modules`, `msvc-modules` and
`msvc-wine-modules`, the build also offers the C++ module `isoline`: a consumer writes `import isoline;`
instead of including the headers, and links `isoline::core_module` instead of `isoline::core`. The module
re-exports every public name of the headers, which stay the one source of truth, and installs as source,
built in the consumer with its own flags. [docs/modules.md](docs/modules.md) has the supported
toolchains, `import std;`, and the recipe by which each later public library adds its own module.

Compiles go through a compiler cache when one is installed (Pace's `pace-cc`, else `ccache`). With a
local `pace-node`, `cmake --preset clang-asan -DPACE_SCHEDULER=127.0.0.1:6674` also dispatches misses
through its admission queue; see [docs/compile-cache.md](docs/compile-cache.md).

## Command-line tool

The build makes `isoline` (`out/<preset>/src/isoline/cli/isoline`), a thin client of the library:

```sh
isoline compile     app.wat  -o app.wasm   # a text module to a binary
isoline disassemble app.wasm -o app.wat    # a binary module to text
```

`-` names standard input as the input and standard output as the output; `compile` refuses to
write a binary to a terminal, and both verbs refuse an output that names their input. Options may
come before or after the input. `--features wasm3` accepts WebAssembly 3.0 alone; the default,
`--features all`, also accepts every proposal Isoline implements. `compile` decodes and validates what
it compiles; `disassemble` writes any module that decodes, valid or not. A component, as text or as a
binary, is refused until Isoline has the component model. Diagnostics go to standard error, one line
each, as `app.wat:3:7: error: …` for text, `app.wasm: error at offset 0x1f: …` for a binary, and
`app.wat: error: function 0, instruction 2: …` for a module that does not validate.

An option's name is an option even without its dashes (`o`, `output`, `features`), so an input of
that name follows `--`: `isoline compile -o out.wasm -- output`. A `--` ends the options, so nothing
may follow the input after it. On Windows the tool takes its command line in UTF-16 and reads it as
UTF-8, so a path of any characters can be named, whatever the ANSI code page.

The inputs are bounded by the text format's published limits (`text::TextLimits`, see
[docs/conformance.md](docs/conformance.md#text-format-limits)): `compile` reads at most 1 GiB of text,
nested at most 10 000 lists deep, and refuses a module whose binary would pass 64 MiB; `disassemble`
reads at most 64 MiB of binary and writes at most 1 GiB of text, so that what it writes reads back.
Past a limit the run is refused, having held about the limit, never the whole input.

The output is written atomically: to a new file beside it, which replaces it only once it is whole
and on the disk, so a run that fails leaves an existing output as it was. The new file keeps the
mode of the file it replaces. A device such as `/dev/null` is written in place. A symbolic link is
replaced itself, by a file of the mode of the file it named, which is left untouched.

The exit status says why a run failed:

| Status | Refusal |
| --- | --- |
| 0 | none: the output was written |
| 2 | the command line is wrong |
| 2 | the output names the input |
| 2 | a binary would be written to a terminal |
| 2 | the input cannot be read |
| 2 | the output cannot be written |
| 2 | the host ran out of memory |
| 3 | the tool failed unexpectedly |
| 1 | the input is too large |
| 1 | the text is malformed |
| 1 | the binary is malformed |
| 1 | the module is invalid |
| 1 | the binary is a component |
| 1 | the module's text would be too large |
| 1 | a page size the text format cannot say |

## Conformance

Isoline implements the WebAssembly 3.0 core language without garbage collection, plus the wide
arithmetic, custom page sizes and compact import section proposals. It runs the official 3.0 spec
suite and each proposal's suite, every script read by Isoline's own text format: only the assertions
about garbage collection are skipped. [docs/conformance.md](docs/conformance.md) pins every suite and
lists each excluded file and skipped assertion with its reason.

## Contributing

Isoline follows the contour-terminal C++ guidelines in [AGENT.md](AGENT.md): C++23, `std::expected` for
fallible APIs, `enum class` over `bool`, dependency injection for every external resource, configuration
at construction, data-driven design, and a zero-warning build. New code lands with tests.

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
