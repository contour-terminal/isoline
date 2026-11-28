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
| `wamr-classic` | `isoline-benchmark-wamr`: Contour's WAMR with its classic interpreter instead of the fast one (information) |
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

`benchmark.py instructions --update|--compare` counts the instructions of `isoline-benchmark` and of both
WAMR runners on every guest under callgrind (Wasmtime, whose count is mostly its compiler's, has no row), at
1 iteration less at 0 (so the count is the kernel's, without the start, load and lowering), kept in
`benchmarks/baseline-instructions.json` and compared before a merge, with a tolerance of 1 % per kernel:
counters, not hosted CI. `isoline-benchmark-simd-lanes` counts the reference interpreter's SIMD lane
kernels alone (`--toggle-collect='*applySimd*'`).

## The numbers

### Run notes (2026-11-28)

- **Background load.** The baseline ran on the owner's desktop with the runner pinned to core 2. From
  10:00 another lane's builds (P6, arm64) shared the machine (load average 9–17 on 32 threads); before
  that the load was 2–4. Nine guests whose A/A spread was 2 % or more were measured again with the same
  `N` (`--pinned-n benchmarks/pinned-iterations.json`), and per guest the measurement with the smaller
  worst spread is the one below; six guests stay just over the tolerance (2.3–2.8 %: micro-call,
  micro-call_indirect, micro-loop, shootout-ed25519, shootout-minicsv, shootout-random), so their A/A rows
  say "repeat". The bar's margin (0.79 against 1.00) is far wider than that spread. A wall-clock run on a
  quiet machine is owed, to replace these rows.
- **The instruction counts** are callgrind's and independent of the load.
- **Code size per dispatch backend** (`size`, text bytes of the dispatch objects in `libisoline_core`):
  Clang 22: switch 254 KB (`SwitchDispatch` 130 KB, `SwitchDispatchCold` 124 KB), tail calls 391 KB
  (control 121 KB, memory 156 KB, SIMD 89 KB, numeric 24 KB, exceptions 1 KB); GCC 16: switch 361 KB, tail
  calls 461 KB. `libisoline_core.a`'s whole text: 3.40 MB (Clang), 4.33 MB (GCC).
- **The GCC switch dispatch** misses its secondary target (2.09 against 1.25): issue #21.
- **Calls** are where the interpreter trails WAMR (micro-call 2.2×, micro-call_indirect 2.7×): the
  instruction counts are close to WAMR's, so the time goes to the call path's dependent loads and calls,
  which callgrind does not price. The follow-up PR tunes it.


<!-- benchmark:begin -->
Measured 2026-11-28 on x86_64, 10 pairs per runner and guest, by `scripts/benchmark.py run`.

**Bar** (geomean over the 19 shootout guests of the best dispatch over wamr-contour, at most 1.00): isoline-interpreter-tailcall at 0.7933, met.
**Secondary target** (over 19 shootout guests, at most 1.25): isoline-interpreter-switch at 1.0338, met.
**Secondary target** (over 19 shootout guests, at most 1.25): isoline-interpreter-switch-gcc at 2.0859, missed.

Each cell is the median time per iteration (the median of t(N) - t(0), over the runner's own N, in parentheses), and its ratio over wamr-contour's.

### Shootout guests

| guest | isoline-interpreter-tailcall | isoline-interpreter-switch | isoline-interpreter-switch-gcc | isoline-reference | wamr-contour | wamr-classic | wamr-default | wasmtime |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| shootout-ackermann | 16.5 ms (N 60), 1.49× | 13.8 ms (N 74), 1.25× | 19.5 ms (N 51), 1.76× | 46.6 ms (N 22), 4.21× | 11.1 ms (N 91) | 14.2 ms (N 70), 1.28× | 4.5 ms (N 226), 0.41× | 535 µs (N 1888), 0.05× |
| shootout-base64 | 78.2 µs (N 12669), 0.61× | 133 µs (N 6681), 1.04× | 294 µs (N 3367), 2.29× | 1.99 ms (N 503), 15.48× | 129 µs (N 7731) | 257 µs (N 3859), 2.00× | 94 µs (N 10626), 0.73× | 5.91 µs (N 169808), 0.05× |
| shootout-ctype | 963 µs (N 1160), 0.56× | 1.26 ms (N 679), 0.73× | 3.58 ms (N 277), 2.09× | 24.7 ms (N 40), 14.41× | 1.72 ms (N 583) | 3.4 ms (N 290), 1.98× | 1.1 ms (N 900), 0.64× | 92.2 µs (N 10927), 0.05× |
| shootout-ed25519 | 4.58 ms (N 226), 1.56× | 4.64 ms (N 219), 1.58× | 8.32 ms (N 123), 2.84× | 31.5 ms (N 33), 10.75× | 2.93 ms (N 350) | 5.07 ms (N 205), 1.73× | 2.3 ms (N 442), 0.79× | 149 µs (N 6898), 0.05× |
| shootout-fib2 | 6.12 ms (N 164), 1.66× | 5.45 ms (N 186), 1.48× | 7.31 ms (N 137), 1.98× | 19.1 ms (N 53), 5.18× | 3.69 ms (N 275) | 5.04 ms (N 206), 1.36× | 1.7 ms (N 591), 0.46× | 132 µs (N 7592), 0.04× |
| shootout-gimli | 844 ns (N 1168098), 0.64× | 1.62 µs (N 602061), 1.22× | 3.45 µs (N 280247), 2.60× | 24.6 µs (N 39901), 18.55× | 1.33 µs (N 743622) | 3.24 µs (N 291435), 2.44× | 1.08 µs (N 905468), 0.81× | 85.1 ns (N 11550151), 0.06× |
| shootout-heapsort | 2.61 ms (N 356), 0.54× | 3.35 ms (N 305), 0.69× | 7.33 ms (N 126), 1.51× | 43.4 ms (N 17), 8.97× | 4.84 ms (N 140) | 7.86 ms (N 85), 1.63× | 2.86 ms (N 223), 0.59× | 556 µs (N 1794), 0.11× |
| shootout-keccak | 2.52 µs (N 403164), 0.78× | 4.19 µs (N 237738), 1.30× | 8.54 µs (N 117604), 2.65× | 60.4 µs (N 16462), 18.73× | 3.22 µs (N 314597) | 16.4 µs (N 61014), 5.10× | 3.24 µs (N 310938), 1.01× | 585 ns (N 1723668), 0.18× |
| shootout-matrix | 4.29 µs (N 237349), 1.28× | 5.42 µs (N 185008), 1.62× | 13.7 µs (N 74878), 4.10× | 57.8 µs (N 17632), 17.29× | 3.34 µs (N 285266) | 7.34 µs (N 140440), 2.20× | 2.9 µs (N 349681), 0.87× | 342 ns (N 2998017), 0.10× |
| shootout-memmove | 764 µs (N 1314), 1.24× | 1.01 ms (N 990), 1.63× | 1.33 ms (N 753), 2.15× | 2.73 ms (N 371), 4.41× | 619 µs (N 1620) | 685 µs (N 1478), 1.11× | 576 µs (N 1740), 0.93× | 463 µs (N 2187), 0.75× |
| shootout-minicsv | 3.97 µs (N 250600), 0.65× | 4.57 µs (N 215310), 0.75× | 8.46 µs (N 113682), 1.38× | 39.6 µs (N 24919), 6.46× | 6.13 µs (N 161177) | 9.74 µs (N 100507), 1.59× | 3.35 µs (N 295827), 0.55× | 160 ns (N 6051728), 0.03× |
| shootout-nestedloop | 3.76 ms (N 263), 0.69× | 4.75 ms (N 206), 0.87× | 12.3 ms (N 82), 2.25× | 59.9 ms (N 17), 10.95× | 5.47 ms (N 179) | 8.51 ms (N 119), 1.56× | 2.99 ms (N 340), 0.55× | 151 µs (N 6659), 0.03× |
| shootout-random | 311 µs (N 3323), 0.85× | 434 µs (N 2323), 1.19× | 956 µs (N 1051), 2.62× | 5.34 ms (N 187), 14.66× | 364 µs (N 2718) | 692 µs (N 1476), 1.90× | 285 µs (N 3591), 0.78× | 240 µs (N 4243), 0.66× |
| shootout-ratelimit | 126 µs (N 7995), 0.91× | 170 µs (N 5301), 1.23× | 337 µs (N 2945), 2.44× | 1.79 ms (N 552), 12.97× | 138 µs (N 6555) | 244 µs (N 3799), 1.77× | 136 µs (N 6850), 0.99× | 5.76 µs (N 173825), 0.04× |
| shootout-seqhash | 8.53 ms (N 146), 0.60× | 11.6 ms (N 106), 0.82× | 23.8 ms (N 43), 1.68× | 130 ms (N 12), 9.21× | 14.1 ms (N 80) | 21 ms (N 49), 1.49× | 13.6 ms (N 87), 0.97× | 691 µs (N 1429), 0.05× |
| shootout-sieve | 103 µs (N 9685), 0.43× | 139 µs (N 7204), 0.59× | 287 µs (N 3490), 1.21× | 1.82 ms (N 530), 7.70× | 236 µs (N 4218) | 408 µs (N 2440), 1.73× | 100 µs (N 10215), 0.42× | 7.97 µs (N 128315), 0.03× |
| shootout-switch | 40.5 µs (N 24650), 0.62× | 42 µs (N 23843), 0.64× | 90.7 µs (N 11051), 1.38× | 49.4 ms (N 21), 753.55× | 65.6 µs (N 6879) | 49 ms (N 21), 747.36× | 41.9 µs (N 23942), 0.64× | 28.4 µs (N 35492), 0.43× |
| shootout-xblabla20 | 4.48 µs (N 223694), 0.68× | 8.31 µs (N 120878), 1.26× | 15.9 µs (N 62799), 2.42× | 93.1 µs (N 10767), 14.15× | 6.58 µs (N 152283) | 13 µs (N 72360), 1.97× | 6.16 µs (N 162058), 0.94× | 360 ns (N 2784795), 0.05× |
| shootout-xchacha20 | 8.92 µs (N 109710), 0.58× | 12.1 µs (N 76279), 0.79× | 30.3 µs (N 31940), 1.97× | 179 µs (N 5461), 11.58× | 15.4 µs (N 62990) | 25.3 µs (N 38357), 1.64× | 13 µs (N 72969), 0.85× | 682 ns (N 1425718), 0.04× |

### Micro-kernels

| guest | isoline-interpreter-tailcall | isoline-interpreter-switch | isoline-interpreter-switch-gcc | isoline-reference | wamr-contour | wamr-classic | wamr-default | wasmtime |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| micro-call | 5.12 ms (N 190), 2.21× | 4.4 ms (N 216), 1.90× | 6.07 ms (N 159), 2.63× | 11.8 ms (N 79), 5.12× | 2.31 ms (N 415) | 3.03 ms (N 320), 1.31× | 1.26 ms (N 772), 0.55× | 134 µs (N 7394), 0.06× |
| micro-call_indirect | 6.86 ms (N 144), 2.70× | 6.22 ms (N 164), 2.45× | 8.39 ms (N 123), 3.30× | 14.8 ms (N 69), 5.82× | 2.54 ms (N 385) | 3.47 ms (N 287), 1.37× | 1.88 ms (N 550), 0.74× | 511 µs (N 1969), 0.20× |
| micro-float | 494 µs (N 2017), 1.18× | 624 µs (N 1602), 1.49× | 1.58 ms (N 636), 3.77× | 6.11 ms (N 165), 14.59× | 419 µs (N 2385) | 1.41 ms (N 709), 3.37× | 386 µs (N 2599), 0.92× | 184 µs (N 5475), 0.44× |
| micro-loop | 248 µs (N 3953), 0.48× | 561 µs (N 1761), 1.09× | 1.25 ms (N 796), 2.44× | 9.15 ms (N 108), 17.83× | 513 µs (N 1980) | 1.06 ms (N 933), 2.06× | 452 µs (N 2217), 0.88× | 109 µs (N 9205), 0.21× |
| micro-memory | 676 µs (N 1318), 1.03× | 759 µs (N 1313), 1.16× | 2.12 ms (N 471), 3.23× | 11.2 ms (N 90), 17.03× | 656 µs (N 1533) | 1.31 ms (N 744), 1.99× | 572 µs (N 1746), 0.87× | 63.5 µs (N 15847), 0.10× |
| micro-simd | 1.72 ms (N 592) | 4.11 ms (N 248) | 5.04 ms (N 199) | 4.92 ms (N 205) | – | – | – | 75.9 µs (N 13206) |

### Cold start

t(0): process start, load, lowering and instantiation, the median over every guest's pairs.

| runner | t(0) |
|---|---:|
| isoline-interpreter-tailcall | 1.7 ms |
| isoline-interpreter-switch | 1.7 ms |
| isoline-interpreter-switch-gcc | 1.7 ms |
| isoline-reference | 1.6 ms |
| wamr-contour | 2.6 ms |
| wamr-classic | 2.3 ms |
| wamr-default | 4.3 ms |
| wasmtime | 3.9 ms |

### A/A

| runner | worst spread | verdict |
|---|---:|---|
| isoline-interpreter-switch | 1.88 % | ok |
| isoline-interpreter-switch-gcc | 2.85 % | repeat on a quieter machine |
| isoline-interpreter-tailcall | 2.61 % | repeat on a quieter machine |
| isoline-reference | 1.44 % | ok |
| wamr-classic | 2.79 % | repeat on a quieter machine |
| wamr-contour | 2.61 % | repeat on a quieter machine |
| wamr-default | 2.26 % | repeat on a quieter machine |
| wasmtime | 2.72 % | repeat on a quieter machine |

### Host → guest call round trip

| runner | per call |
|---|---:|
| isoline-interpreter-switch | 223 ns |
| isoline-interpreter-switch-gcc | 304 ns |
| isoline-interpreter-tailcall | 214 ns |
| isoline-reference | 295 ns |
| wamr-classic | 86 ns |
| wamr-contour | 68 ns |
| wamr-default | 51 ns |

### Instructions

callgrind's instruction count of one run() iteration (1 iteration less 0), and its ratio over wamr-contour's: deterministic, whatever the machine's load (`benchmark.py instructions`).

| guest | isoline-interpreter-switch | isoline-interpreter-switch-gcc | isoline-interpreter-tailcall | isoline-reference | wamr-classic | wamr-contour |
|---|---:|---:|---:|---:|---:|---:|
| micro-call | 70'200'684 (1.17×) | 79'900'767 (1.33×) | 65'200'697 (1.08×) | 271'103'578 (4.51×) | 93'799'701 (1.56×) | 60'100'536 |
| micro-call_indirect | 94'849'521 (1.60×) | 112'050'684 (1.89×) | 92'729'084 (1.57×) | 318'129'720 (5.38×) | 91'368'120 (1.54×) | 59'176'660 |
| micro-float | 13'250'797 (1.91×) | 17'925'992 (2.59×) | 8'550'868 (1.23×) | 97'005'411 (14.01×) | 14'899'349 (2.15×) | 6'925'580 |
| micro-loop | 14'375'658 (1.25×) | 16'025'751 (1.40×) | 8'400'695 (0.73×) | 177'728'573 (15.49×) | 22'648'833 (1.97×) | 11'475'512 |
| micro-memory | 21'500'658 (1.52×) | 30'400'730 (2.15×) | 18'450'671 (1.31×) | 250'753'807 (17.75×) | 35'948'320 (2.54×) | 14'125'515 |
| micro-simd | 44'995'193 | 59'918'521 | 32'457'711 | 95'874'070 | – | – |
| shootout-ackermann | 229'885'135 (0.84×) | 255'900'516 (0.94×) | 217'364'521 (0.80×) | 1'145'578'315 (4.21×) | 400'365'227 (1.47×) | 272'313'881 |
| shootout-base64 | 2'909'655 (1.27×) | 3'491'102 (1.52×) | 1'696'840 (0.74×) | 35'566'869 (15.51×) | 6'171'772 (2.69×) | 2'292'678 |
| shootout-ctype | 38'050'606 (1.03×) | 48'700'617 (1.32×) | 25'700'594 (0.70×) | 481'602'557 (13.09×) | 98'699'317 (2.68×) | 36'800'518 |
| shootout-ed25519 | 84'735'945 (1.49×) | 102'657'202 (1.81×) | 70'729'779 (1.25×) | 628'655'091 (11.07×) | 116'554'191 (2.05×) | 56'793'611 |
| shootout-fib2 | 87'888'087 (1.00×) | 98'199'908 (1.12×) | 82'787'078 (0.94×) | 426'540'880 (4.86×) | 126'810'712 (1.45×) | 87'702'202 |
| shootout-gimli | 36'328 (1.47×) | 41'255 (1.67×) | 21'943 (0.89×) | 510'256 (20.68×) | 59'504 (2.41×) | 24'668 |
| shootout-heapsort | 70'582'886 (0.79×) | 95'814'305 (1.07×) | 53'979'769 (0.60×) | 896'533'579 (10.01×) | 171'123'563 (1.91×) | 89'555'597 |
| shootout-keccak | 89'135 (1.56×) | 100'822 (1.76×) | 49'834 (0.87×) | 1'093'494 (19.14×) | 158'610 (2.78×) | 57'133 |
| shootout-matrix | 141'921 (1.79×) | 205'870 (2.60×) | 130'134 (1.64×) | 1'300'620 (16.44×) | 151'444 (1.91×) | 79'121 |
| shootout-memmove | 34'739'927 (1.17×) | 36'606'438 (1.23×) | 33'433'344 (1.13×) | 74'928'475 (2.52×) | 33'954'399 (1.14×) | 29'712'058 |
| shootout-minicsv | 86'324 (0.77×) | 110'459 (0.98×) | 73'146 (0.65×) | 897'622 (7.99×) | 216'998 (1.93×) | 112'384 |
| shootout-nestedloop | 127'622'344 (1.00×) | 183'311'260 (1.44×) | 120'522'321 (0.95×) | 1'343'370'071 (10.54×) | 245'974'570 (1.93×) | 127'455'745 |
| shootout-random | 9'875'210 (1.55×) | 12'925'248 (2.03×) | 7'600'137 (1.19×) | 82'179'328 (12.89×) | 23'849'574 (3.74×) | 6'375'612 |
| shootout-ratelimit | 3'573'019 (1.33×) | 4'388'007 (1.63×) | 2'663'008 (0.99×) | 38'939'767 (14.44×) | 5'618'153 (2.08×) | 2'696'021 |
| shootout-seqhash | 119'188'374 (1.42×) | 149'080'106 (1.77×) | 83'821'994 (1.00×) | 1'560'155'448 (18.52×) | 183'761'449 (2.18×) | 84'221'891 |
| shootout-sieve | 3'687'957 (0.72×) | 4'723'870 (0.92×) | 2'882'781 (0.56×) | 40'967'572 (7.98×) | 11'620'335 (2.26×) | 5'136'956 |
| shootout-switch | 982'341 (0.64×) | 1'283'802 (0.83×) | 893'556 (0.58×) | 1'219'976'888 (793.11×) | 1'392'874'930 (905.51×) | 1'538'225 |
| shootout-xblabla20 | 153'129 (1.33×) | 183'943 (1.60×) | 104'517 (0.91×) | 2'008'016 (17.51×) | 237'171 (2.07×) | 114'707 |
| shootout-xchacha20 | 286'868 (1.32×) | 352'298 (1.63×) | 198'337 (0.91×) | 3'782'769 (17.45×) | 441'266 (2.04×) | 216'766 |


The flag table of every C and C++ build: `-O2` `-DNDEBUG` `-fno-lto` `-fPIE` `-U_FORTIFY_SOURCE` `-D_FORTIFY_SOURCE=3` `-fstack-protector-strong` `-fstack-clash-protection` `-fcf-protection` `-pie` `-Wl,-z,relro` `-Wl,-z,now`.

<!-- benchmark:end -->
