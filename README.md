# Isoline

Isoline is a general-purpose, modern WebAssembly runtime for the
[contour-terminal](https://github.com/contour-terminal) organisation, **written in C++23 only — no Rust
in its source or build** — and engineered to Contour's quality bar. Contour runs its extensions as
WebAssembly and is Isoline's first consumer: today it carries two backends, WAMR (small, but built
interpreter-only and too slow) and Wasmtime (excellent, but a Rust toolchain and a very large
dependency for a terminal emulator's build). Isoline's measure of success is that Contour drops both
and runs every extension on Isoline alone.

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

## Conformance

Isoline implements the WebAssembly 3.0 core language without garbage collection, plus the wide
arithmetic, custom page sizes and compact import section proposals. It runs the official 3.0 spec
suite and each proposal's suite; [docs/conformance.md](docs/conformance.md) pins every suite and lists
each excluded file and skipped assertion with its reason.

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
