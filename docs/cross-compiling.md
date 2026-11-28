# Cross-compiling

A Linux host builds Isoline for Windows x64, for Linux aarch64 and for big-endian Linux s390x and runs the
test suite there, under wine and qemu-user, so a portability failure shows up locally instead of on CI.
Both are ordinary presets; `ctest` runs each test binary through the toolchain's
`CMAKE_CROSSCOMPILING_EMULATOR`. The Python tests (label `lint`) name the host interpreter by its path
and run on the host in every tree.

| Preset | Target | Toolchain file | Tests run under |
|---|---|---|---|
| `clangcl-cross-debug` | Windows x64, Debug | `cmake/toolchains/windows-clangcl-cross.cmake` | wine |
| `clangcl-cross-release` | Windows x64, Release | the same | wine |
| `msvc-wine-debug` | Windows x64, Debug, real MSVC | `cmake/toolchains/windows-msvc-wine-cross.cmake` | wine |
| `msvc-wine-modules` | Windows x64, Debug, real MSVC, C++ modules | the same | wine |
| `clang-aarch64-cross` | Linux aarch64, Debug | `cmake/toolchains/aarch64-linux-gnu-clang.cmake` | `qemu-aarch64-static` |
| `clang-aarch64-cross-release` | Linux aarch64, Release | the same | `qemu-aarch64-static` |
| `gcc-s390x` | Linux s390x (big-endian), Release | `cmake/toolchains/s390x-linux-gnu.cmake` | `qemu-s390x` |

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

`clangcl-cross-release` builds the same with the release STL. MSVC's debug STL allocates inside
`noexcept` container constructors, so the Debug tree leaves the memory suite's `[host-memory]` cases out
(a refusal there terminates); the release tree runs them, the only Windows build that does.

The emulator is `out/clangcl-cross-debug/wine-run.sh`, which the toolchain file writes. It runs with the
MSVC C++ runtime (release and debug) and the UCRT on `WINEPATH`, and in a **wine prefix of the build's
own**, `out/clangcl-cross-debug/wineprefix` (cache variable `MSVC_WINE_PREFIX`), never the user's
`~/.wine`. The first test initializes it. `WINEDEBUG` defaults to `-all`. Run a binary by hand the same
way, e.g. `out/clangcl-cross-debug/wine-run.sh out/clangcl-cross-debug/src/isoline/isoline_test.exe`. The
prefix's wineserver lingers a few seconds after the last test; stop it at once with
`WINEPREFIX=$PWD/out/clangcl-cross-debug/wineprefix wineserver -k`.

## Windows x64: MSVC under wine

clang-cl does not raise every warning MSVC does, and the two differ on where a calling convention may
stand (`__cdecl` in `WASM_API_EXTERN`'s position is C2143 on MSVC and accepted by clang-cl). Nor does
clang-cl build C++ modules: CMake has no module dependency scanning for it, so
`cmake --preset clangcl-cross-debug -DISOLINE_MODULES=ON` fails at configure time, naming clang-cl
(`cmake/IsolineModuleSupport.cmake`). The clang-cl trees instead run `module-scanning-tripwire` (label
`lint`), which configures a probe project of one module (`tests/cmake/module-scanning-probe/`) with the
same toolchain and passes while CMake refuses it. Once CMake scans modules for clang-cl it fails, and its
refused row should become a lane.

So two presets build with Microsoft's own compiler: msvc-wine's `cl.exe`, `link.exe` and `lib.exe`, run
under wine with Ninja. `msvc-wine-debug` is the plain build; `msvc-wine-modules` inherits it and turns on
`ISOLINE_MODULES`, the module lane. Both need the same msvc-wine install as clang-cl, including the
wrappers its `install.sh` writes into `<MSVC_WINE_ROOT>/bin/x64/`:

```sh
cmake --preset msvc-wine-debug      # or msvc-wine-modules
cmake --build --preset msvc-wine-debug
ctest --preset msvc-wine-debug
```

- **The compiler runs in the build's own prefix.** msvc-wine's wrappers would run in the user's
  `~/.wine`. The toolchain file calls each one through a runner written into the build,
  `out/<preset>/msvc-wine-bin/<tool>`, which runs it in `out/<preset>/wineprefix`, the prefix the tests
  run in.
- **The static CRT.** The presets set `CMAKE_MSVC_RUNTIME_LIBRARY` to
  `MultiThreaded$<$<CONFIG:Debug>:Debug>`, so a test binary needs no MSVC runtime DLL under wine.
- **Debug information in the objects (`/Z7`).** In a new prefix, the first `/Zi` compile fails with C1902
  ("Program database manager mismatch") because `mspdbsrv.exe` is not up yet. `/Z7` needs no PDB server,
  so the toolchain file sets `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` to `Embedded`.

The two Windows toolchain files share `cmake/toolchains/msvc-wine-common.cmake`: the install and its
versions, the prefix, and the `wine-run.sh` emulator.

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

## Linux s390x: GCC 14 and qemu-user, big-endian

The one big-endian target: Wasm memory stays little-endian, so every load, store and slot access of the
interpreter byte-swaps there, and a read that only works in the host's order fails. It needs the
Debian/Ubuntu cross compiler `g++-14-s390x-linux-gnu` (its target libraries in `/usr/s390x-linux-gnu`,
`ISOLINE_S390X_PREFIX`) and `qemu-user`, as CI's `linux-gcc-s390x-qemu` job installs them:

```sh
cmake --preset gcc-s390x
cmake --build --preset gcc-s390x
ctest --preset gcc-s390x
```

A host without those packages (Fedora has no s390x cross g++) runs the same three steps in an Ubuntu 24.04
container with `scripts/s390x-podman.sh`; arguments after the options go to `ctest`, e.g.
`scripts/s390x-podman.sh --jobs 4 -R 'LittleEndian|SlotCodec'`. The test preset runs what CI runs, the
`unit` and `interpreter` labels without `perf` and `wallclock`. The preset is Release with neither
sanitizers nor clang-tidy, and multiplies every area's timeout by 10 (`ISOLINE_TEST_TIMEOUT_FACTOR`): s390x
emulation is slower than aarch64's.

## What an emulator cannot show

- **wine is not Windows.** It reimplements the Win32 API: a pass under wine is strong evidence about the
  compiler, the MSVC STL and the ABI, but not proof about kernel32 or ntdll behaviour (virtual memory,
  exceptions, file and console I/O, timing). Windows CI stays the authority there.
- **qemu-user does not reorder memory.** It runs arm64 code on x86's strong memory model, so a missing
  acquire/release that real arm64 hardware would expose passes under qemu. Threading bugs of that kind
  need real arm64 hardware (or the TSan preset, which checks the C++ memory model rather than a CPU).
- **No performance numbers.** Both run translated or emulated code; timings and benchmarks from these
  trees mean nothing.
