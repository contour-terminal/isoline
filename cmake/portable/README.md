# Portable CMake modules

Files in this directory are **verbatim copies** from another repository. Do not
edit them here. When the upstream copy changes, re-sync the whole file and update
the provenance below in the same commit. A fix goes upstream first.

| File | Upstream | Commit |
| --- | --- | --- |
| `CompileCache.cmake` | [Pace](https://github.com/LASTRADA-Software/pace) `cmake/portable/CompileCache.cmake` | `46b563b064fcdc1650f547b52f083bdb5089f871` (2026-10-18) |

The commit is the one whose blob this file equals, the last upstream commit that
touched it, and the same blob core-cpp carries (`cmake/portable/CompileCache.cmake`
there). When it was taken here, Pace's `origin/master` was
`a401af88e88bb6f71528c5f35dce8ab413929f18` and had moved one commit past it:
`bf5e734f` adds a prebuilt `pace-cc` for Linux aarch64 hosts to `PACE_AUTO_INSTALL`
and changes nothing else. The two copies are kept equal; re-sync both together.

To re-sync, read the upstream file as a blob so no line-ending conversion touches
it, and check that it contains no CR byte:

```sh
git -C <pace> fetch origin
git -C <pace> -c core.autocrlf=false -c core.eol=lf \
    show origin/master:cmake/portable/CompileCache.cmake > cmake/portable/CompileCache.cmake
grep -c $'\r' cmake/portable/CompileCache.cmake   # must print 0
```

## `CompileCache.cmake`

The top-level `CMakeLists.txt` includes it only when Isoline is the top-level
project, after `project()` and before `IsolineDependencies`, so a fetched Catch2 is
compiled through the cache too. As a subproject Isoline sets no launcher, and its
targets inherit whatever the embedder chose. The selected launcher is printed as
`[isoline] Compiler launcher: …` and recorded as the internal cache entry
`ISOLINE_CXX_COMPILER_LAUNCHER`, because the module sets
`CMAKE_CXX_COMPILER_LAUNCHER` only as a normal variable.

The module picks the first launcher that is installed and usable:

1. `pace-cc`, when a Pace daemon accepts its probe compile at `PACE_ADDR`
   (default `127.0.0.1:6674`);
2. `sccache`, only with `-DALLOW_SCCACHE_FALLBACK=ON`;
3. `ccache`;
4. none: the build compiles uncached.

It leaves a launcher that is already set (by `-D`, a preset or a toolchain file)
untouched, and it never fails a configure. Its status lines start with `[cache]`.
How a developer turns on dispatch, and what it does to `-j`, is in
[docs/compile-cache.md](../../docs/compile-cache.md).

- **Opt out:** `-DUSE_COMPILER_CACHE=OFF`. The `clang-coverage` preset does this:
  a cache hit replays an object whose embedded coverage mapping names the checkout
  that produced it, so the report would describe another tree.
- **No daemon yet:** `-DPACE_AUTO_INSTALL=ON` downloads `pace-cc`, and
  `-DPACE_AUTO_START=ON` starts a daemon in the background.

Its options (`USE_COMPILER_CACHE`, `ALLOW_SCCACHE_FALLBACK`, `PACE_*`) are
unprefixed because the organisation's projects share them.
