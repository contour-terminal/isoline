# Cross-compiling

A Linux host builds Isoline for Windows x64 and for Linux aarch64 and runs the whole test suite there,
under wine and qemu-user respectively, so a portability failure shows up locally instead of on CI.
Both are ordinary presets; `ctest` runs each test binary through the toolchain's
`CMAKE_CROSSCOMPILING_EMULATOR`. The Python tests (label `lint`) name the host interpreter by its path
and run on the host in every tree.

| Preset | Target | Toolchain file | Tests run under |
|---|---|---|---|
| `clangcl-cross-debug` | Windows x64, Debug | `cmake/toolchains/windows-clangcl-cross.cmake` | wine |
| `clang-aarch64-cross` | Linux aarch64, Debug | `cmake/toolchains/aarch64-linux-gnu-clang.cmake` | `qemu-aarch64-static` |
| `clang-aarch64-cross-release` | Linux aarch64, Release | the same | `qemu-aarch64-static` |

## Windows x64: clang-cl and wine

Needs the host's clang-cl, lld-link, llvm-lib, llvm-rc and llvm-mt (Fedora: `clang lld llvm`), wine,
and the MSVC toolset plus Windows SDK as [msvc-wine](https://github.com/mstorsjo/msvc-wine) installs
them:

```sh
git clone https://github.com/mstorsjo/msvc-wine && cd msvc-wine
./vsdownload.py --accept-license --dest /opt/msvc && ./install.sh /opt/msvc
```

The install is found at `MSVC_WINE_ROOT` (cache or environment variable, default `/opt/msvc`); the
newest toolset and SDK in it are used unless `MSVC_WINE_TOOLSET_VERSION` / `MSVC_WINE_SDK_VERSION` name
one. The system headers are passed as `/imsvc`, so the warning set and `/WX` apply to Isoline's code only.

```sh
cmake --preset clangcl-cross-debug
cmake --build --preset clangcl-cross-debug
ctest --preset clangcl-cross-debug
```

The emulator is `out/clangcl-cross-debug/wine-run.sh`, which the toolchain file writes. It runs with the
MSVC C++ runtime (release and debug) and the UCRT on `WINEPATH`, and in a **wine prefix of the build's
own**, `out/clangcl-cross-debug/wineprefix` (cache variable `MSVC_WINE_PREFIX`), never the user's
`~/.wine`. The first test initializes it. `WINEDEBUG` defaults to `-all`. Run a binary by hand the same
way, e.g. `out/clangcl-cross-debug/wine-run.sh out/clangcl-cross-debug/src/isoline/isoline_test.exe`. The
prefix's wineserver lingers a few seconds after the last test; stop it at once with
`WINEPREFIX=$PWD/out/clangcl-cross-debug/wineprefix wineserver -k`.

## Linux aarch64: clang and qemu-user

Needs the host's clang and lld, `qemu-user-static-aarch64` (it also registers the binfmt handler that
running an arm64 container needs), and podman to create the sysroot once:

```sh
scripts/make-aarch64-sysroot.sh            # a no-op when the sysroot is complete; --refresh recreates it
cmake --preset clang-aarch64-cross
cmake --build --preset clang-aarch64-cross
QEMU_CPU=max ctest --preset clang-aarch64-cross
QEMU_CPU=cortex-a53 ctest --preset clang-aarch64-cross
```

The sysroot is the C and C++ development files (`libc6-dev libstdc++-14-dev libgcc-14-dev
linux-libc-dev`) of a Debian trixie arm64 container. The image is pinned by digest, so the base system
is reproducible; `apt-get` still installs the packages current in the trixie archive, so a `--refresh`
can bring newer point releases. The sysroot lives at `ISOLINE_AARCH64_SYSROOT` (cache or environment
variable, default `~/.cache/isoline/sysroot-aarch64-trixie`). The Debug preset keeps assertions on;
`clang-aarch64-cross-release` shows what the optimizer does with the same code. `QEMU_CPU` picks the emulated CPU: `max`
enables every extension qemu implements, `cortex-a53` is a baseline ARMv8.0 core without them, so the
two runs cover both sides of any feature detection.

## What an emulator cannot show

- **wine is not Windows.** It reimplements the Win32 API: a pass under wine is strong evidence about the
  compiler, the MSVC STL and the ABI, but not proof about kernel32 or ntdll behaviour (virtual memory,
  exceptions, file and console I/O, timing). Windows CI stays the authority there.
- **qemu-user does not reorder memory.** It runs arm64 code on x86's strong memory model, so a missing
  acquire/release that real arm64 hardware would expose passes under qemu. Threading bugs of that kind
  need real arm64 hardware (or the TSan preset, which checks the C++ memory model rather than a CPU).
- **No performance numbers.** Both run translated or emulated code; timings and benchmarks from these
  trees mean nothing.
