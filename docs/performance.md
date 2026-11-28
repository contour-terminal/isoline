# Performance

How fast Isoline's interpreter runs WebAssembly against the engine Contour ships today, WAMR's fast
interpreter, and how that is measured. The numbers between the markers below are written by
`scripts/benchmark.py report` from a `benchmark.json` that `scripts/benchmark.py run` measured; nothing
else edits them.

## The bar

On the reference machine (the owner's Linux x86-64 desktop): the geomean, over the 19 shootout guests, of
`isoline-interpreter-<best dispatch>`'s time per iteration over `wamr-contour`'s is at most 1.00. The secondary target:
`isoline-interpreter-switch`, built by Clang and by GCC, at most 1.25 over `wamr-contour`. Both are
published, not blocking: a miss is tuned in a follow-up. The micro-kernels are reported apart, and the SIMD
micro-kernel has no WAMR column, as Contour's WAMR is built without SIMD. The bar line states how many
shootout guests it is computed over, and a run with ratios over fewer than all 19
(`BAR_MINIMUM_SHOOTOUT_GUESTS`) gets no bar verdict, only the reason. Wasmtime's numbers are
information: being at or above Wasmtime is the compiler's bar, from P5.

## The guests

`benchmarks/guests/` holds 25 guests, each a kernel exporting `run(i32 iterations) -> i64 checksum` and
importing nothing: the 19 Sightglass `shootout-*` kernels and 6 micro-kernels (`loop`, `call`,
`call_indirect`, `memory`, `float`, `simd`). Their sources are in `benchmarks/sources/`
(`PROVENANCE.md` there has each one's licence chain), rebuilt
freestanding against `benchmarks/shim/benchmark.h`: a bump allocator over a static arena, the `mem*` and
`str*` functions, no output, and a defined checksum. `run()` repeats its kernel `iterations` times; `run(0)`
does its setup alone. `benchmarks/build-guests.sh` builds them with the pinned wasi-sdk and a pinned feature
list (the MVP, mutable globals, sign extension, saturating conversions and bulk memory; SIMD for the SIMD
guest alone), validates each with wasm-tools and pins its SHA-256 in `benchmarks/guests.lock`, which
`benchmark.py` and the `isoline_benchmark_test` suite check. That suite also runs every guest on every engine
of the build and compares the checksums, and loads every non-SIMD guest under the features of Contour's WAMR
build.

## The runners

A table in `scripts/benchmark.py` (`RUNNERS`):

| runner | what it runs |
|---|---|
| `isoline-interpreter-tailcall`, `isoline-interpreter-switch` | `isoline-benchmark`, Clang, the interpreter under each dispatch |
| `isoline-interpreter-switch-gcc` | `isoline-benchmark`, GCC, the Switch dispatch |
| `isoline-reference` | `isoline-benchmark`, Clang, the reference interpreter (information) |
| `wamr-contour` | `isoline-benchmark-wamr`: WAMR 2.4.5 at 25bd7eb with Contour's two patches, built with exactly Contour's options |
| `wamr-default` | `isoline-benchmark-wamr`: the same WAMR, unpatched, with WAMR's default options (information) |
| `wasmtime` | the pinned Wasmtime's `wasmtime run --invoke` (information) |

`wamr-contour` is what Contour ships: the fast interpreter alone, with the thread manager on, so that it
checks for termination as Isoline polls its interrupt; no hardware bounds checks; bulk memory, reference
types and extended constant expressions on; SIMD, tail calls, memory64, multi-memory, GC and exception
handling off (`benchmarks/wamr/ContourOptions.cmake`). Its patches are copied from Contour with their SHA-256
and the Contour commit they came from (`benchmarks/wamr/patches.lock`), and are refused if they differ. WAMR
has no Spectre mitigation of its bounds checks; Isoline's explicit bounds checks select a zero address when
out of bounds, which every Isoline number below pays for.

Every C and C++ build of the runners takes one flag table (`FLAG_TABLE`): the release optimization flags
(`-O2 -DNDEBUG`, LTO off, no `-march=native`) and Contour's production hardening of a Linux build (`-fPIE
-pie`, `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3`, `-fstack-protector-strong`, `-fstack-clash-protection`,
`-fcf-protection` on x86-64, `-Wl,-z,relro -Wl,-z,now`). `scripts/benchmark.py build` configures
`out/benchmark-clang` and `out/benchmark-gcc` with it (and builds WAMR at its `--jobs`), and
`benchmark.json` records it with every compiler's version.

## The method

`benchmark.py run` times each runner's whole process with its own monotonic clock, at `n = N` and `n = 0`,
in 10 rounds of pairs, pinned to one core (`taskset`). Each round times every runner's pair in turn, so that
a change in the machine between rounds falls on every runner alike, and alternates which of `N` and `0`
goes first. Each runner has its own `N`, calibrated so that its `t(N) - t(0)` takes about a second. A
kernel's time is the median of the paired differences over `N`: the median time per iteration, which every
table and every ratio uses, so that numbers compare across runs, runners and subsets of runners whatever
their `N`. Before the pairs, every runner runs the guest at 3 iterations, and their checksums must agree.

`--write-pinned-n FILE` writes the `N` of every guest and runner, and `--pinned-n FILE` runs with those `N`
instead of calibrating, refusing a guest or runner the file has none for. When the baseline is taken, its
`N` are committed as `benchmarks/pinned-iterations.json`, so that a later run measures the same work.

`t(0)` is the cold-start row: process start, load, lowering and instantiation, which Contour pays at every
extension start. Every series is timed twice (A/A); a spread of 2 % or more between a runner's two medians
means the machine was not quiet, and the run is repeated (exit status 3).

`benchmark.py calls` measures the host→guest call round trip: `run(0)` of `micro-loop` called 100'000 times
in one process, by the runner's own clock.

`benchmark.py instructions --update|--compare` counts the instructions of `isoline-benchmark` on every guest
under callgrind, at 1 iteration less at 0 (so the count is the kernel's, without the start, load and
lowering), kept in `benchmarks/baseline-instructions.json` and compared before a merge, with a
tolerance of 1 % per kernel: counters, not hosted CI. `isoline-benchmark-simd-lanes` counts the reference
interpreter's SIMD lane kernels alone (`--toggle-collect='*applySimd*'`).

## The numbers

<!-- benchmark:begin -->
Not measured yet: the baseline is taken on the reference machine once it is quiet.
<!-- benchmark:end -->
