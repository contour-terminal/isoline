# Compile caching and dispatch

When Isoline is the top-level project, every compile goes through a compiler cache if one is
installed: [Pace](https://github.com/LASTRADA-Software/pace)'s `pace-cc` when a Pace daemon answers,
else `ccache`, else none. Nothing has to be set for that, and nothing can fail: on a CI runner, on
macOS, under MSVC, or in a cross tree where no launcher works, the configure prints
`[isoline] Compiler launcher: none (compiles uncached)` and the build proceeds as it would without
the module. The module and its options are described in
[cmake/portable/README.md](../cmake/portable/README.md).

## Turning dispatch on

Caching only stores and replays objects. **Dispatch** additionally hands every cache miss to the
compile fleet's scheduler, which *admits* it: the compile waits for a free slot on a machine of the
fleet, so parallel builds stop competing for the same cores and memory. On a machine running
`pace-node`, the scheduler answers on the same port as the cache, and this is the one line every
developer and every lane uses to configure a tree:

```sh
cmake --preset clang-asan -DPACE_SCHEDULER=127.0.0.1:6674
```

The endpoint is held in the build tree's cache and baked into the launcher, so the build's own
environment does not change it, and re-running the line above on an existing tree turns dispatch on
there too. `-DPACE_SCHEDULER=` turns it off again. A fresh tree also takes the endpoint from the
environment (`export PACE_SCHEDULER=127.0.0.1:6674` before the first `cmake --preset`), but an
existing tree keeps what it holds, which is why the `-D` form is the one to use.

The address is deliberately in no committed preset: it names a service on the developer's machine,
and CI runners have none. To make it a default for yourself, put it in a `CMakeUserPresets.json`,
which `.gitignore` excludes:

```json
{
    "version": 6,
    "configurePresets": [
        {
            "name": "clang-asan-pace",
            "inherits": "clang-asan",
            "binaryDir": "${sourceDir}/out/clang-asan",
            "cacheVariables": { "PACE_SCHEDULER": "127.0.0.1:6674" }
        }
    ]
}
```

The configure says which state it is in, on a line starting `[cache] dispatch:`. Each compile records
its outcome in `~/.local/state/pace-cc/invocations.log`: column 2 is `HIT`/`MISS`, column 13 is
`DISPATCHED`, `DECLINED` or `NOT_ATTEMPTED` (a hit needs no dispatch), column 14 says why a compile was
declined, and column 17 is how long it waited for a slot, in milliseconds. `pace-cc --show-stats`
summarizes the same log, and `pacectl node` reports the node's `slots-available` and what bounds them.

## Admission and `-j`

**Keep `-j8` per build; do not raise it to `-j$(nproc)`.** Admission is not a queue in front of
Ninja that makes any `-j` safe: a compile the fleet has no slot for is *declined*, and `pace-cc` then
compiles it on this machine at once, outside the node's memory ceiling. A higher `-j` therefore turns
into unadmitted local compiles, which is exactly the overload admission exists to prevent.

Measured on a 32-thread, single-node fleet whose node had 13 to 15 slots available (bound by external
CPU and by memory, both of which `pace-node` re-samples as the machine's load changes), on Isoline's
80 compiles:

| Build | `DISPATCHED` | `DECLINED` (*the fleet was full of its own work*) |
| --- | --- | --- |
| `-j8`, every compile a miss | 80 | 0 |
| `-j32`, every compile a miss | 52 | 28, compiled locally without the node's ceiling |

The slots are the *machine's*, shared by every lane building on it, so the per-lane `-j` should leave
the lanes' sum near the node's `slots-available` rather than near the core count. With several heavy
builds at once, `-j8` already reaches that sum; lowering it costs a lane little, because the cache
answers most of its compiles anyway.

Two things in the build are not compiles and are never admitted: linking, and clang-tidy, which the
`clang-*` presets run beside every compile (`ISOLINE_CLANG_TIDY`). A cache hit skips the compiler but
not clang-tidy, so on those presets a fully cached rebuild still takes most of the time a cold one
does.

## Where it stays off

- **`clang-coverage`** sets `USE_COMPILER_CACHE=OFF`: a replayed object's coverage mapping names the
  checkout that stored it.
- **The Windows cross trees** (`clangcl-cross-*`, and `cl.exe` under wine) configure with no
  launcher: the module's probe compile reports no cache outcome, because `pace-cc` takes an absolute
  POSIX source path such as `/home/…/probe.cpp` on an MSVC-style command line for a switch. The module
  forces `/Z7` only when a launcher is in use, so these trees keep CMake's default debug format.
- **Embedding builds**: as a subproject Isoline selects no launcher at all.
