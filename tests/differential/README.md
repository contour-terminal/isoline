# Differential testing

The differential harness runs one module on every engine of the build: the reference interpreter, and the
fast interpreter under each compiled dispatch. It compares everything each engine observes, exactly. The
first participant is the expected one.

For each call it compares the following, in this order (`Comparison.hpp`'s `ObservationFields`):

- the exception the call threw, and the call's outcome, with NaN bits compared;
- the steps charged, and the polls of each site;
- a digest of every memory (a divergence names the first differing offset), then every global and every
  table element;
- the refused grows;
- every live exception, with its tag and payload;
- which data and element segments are dropped;
- the stack values still charged;
- the calls into the recording host.

The instantiation is compared the same way. Each participant runs twice: once unobserved, through the
engines' fast paths, and once with a poll observer. An `InterruptionPlan` adds one more call to the
observed run, after every complete one: an export the seed picks, interrupted at its k-th poll. The
interrupt is never lowered, so that call is the last, and every call before it keeps its polls.

The recording host (`RecordingHost.hpp`) binds every import alike in each store. Its functions derive
their results from their arguments, fail one call in 64, re-enter the guest through an export of the
caller one call in 16, and write a byte of the caller's memory one call in 4. Imports named `reenter`
and `reenterDeep` re-enter at every call, so that the edge modules reach the nested-invocation limit
and the call depth across nested invocations.

`Reach.hpp` counts what a run reached: traps by code, refused grows, thrown exceptions, host failures,
bulk granules, unwinds, sweeps, re-entries and host writes. `Differential.reachesTheEdges` asserts that
the edge modules reach each of them, and the corpus replay prints what the generated modules reached.

Wasmtime is the oracle of results and traps for import-free modules whose exports take numbers
(`WasmtimeOracle.hpp`, its traps mapped by `TrapMapping.hpp`).

## Inputs

| Input | Where | Run by |
|---|---|---|
| Every committed spec binary | `tests/spec/**/*.wasm` | `Differential.agreesOnTheSpecSelftests` (unit) |
| The hard edges, by hand | `edges/*.wat` | `Differential.agreesOnTheEdges`, `Differential.reachesTheEdges` (unit) |
| Divergences once found | `regressions/<seed>.wasm` | `CorpusReplay.everyRegressionAgrees` (unit) |
| 10'000 `wasm-tools smith` modules | `out/corpus/differential/` (never committed) | `ctest -L differential` |

The unit run (`isoline_differential_test`, label `unit`) compares every spec binary on every engine, both
lanes and both observation modes. ThreadSanitizer is too slow for all of them within the test's timeout, so
the `clang-tsan` preset sets `ISOLINE_DIFFERENTIAL_SPEC_SAMPLE=8`: it compares the binaries whose index is
divisible by 8, the same eighth lane 2 samples on a pull request. Every other preset, ASan's among them,
compares them all; the corpus replay is never run under ThreadSanitizer.

## Running the corpus

The replay is `ISOLINE_DIFFERENTIAL_SHARDS` ctest entries (8 by default), one per contiguous run of seeds,
for `ctest -j`. Lane 1 replays every module. Lane 2 and Wasmtime replay the seeds divisible by
`ISOLINE_DIFFERENTIAL_SAMPLE`: 8 on a pull request, 1 in the nightly run (`differential-nightly.yml`). The
reference is observed once per module and reused by lane 2. Each shard prints what it counted: modules by
whether they are compared, calls, the oracle's verdicts and why it had no answer, and what was reached. It
fails on a divergence, a module Isoline refuses (wasm-tools validated each), fewer compared modules, calls
or oracle agreements than measured, and any fault of the harness (the oracle's process, module file,
unmapped trap, unparsed output, or an error of Wasmtime's that is neither a trap nor one of its limits in
`WasmtimeLimitRows`). Calls Wasmtime refused for one of its limits while Isoline returned results are
counted and printed.

```sh
python3 -I scripts/make-differential-corpus.py            # out/corpus/differential/<seed>.wasm
ISOLINE_WASMTIME=$(python3 -I scripts/fetch-tools.py --tool wasmtime) \
    ctest --test-dir out/clang-release -L differential -j4 --output-on-failure
```

## Reproducing and minimizing a divergence

Every divergence is reported with a reproducer. The replay prints each engine's observations and the
lowered listing, and it exits with 1 on a divergence:

```sh
out/clang-release/tests/differential/isoline-differential-replay out/corpus/differential/<seed>.wasm <seed>
```

`wasm-tools shrink` minimizes the module while it still diverges, with `tests/fuzz/shrink-predicate.py`
as the predicate. Set `ISOLINE_FIELD` to keep the minimization on one field:

```sh
ISOLINE_REPLAY=out/clang-release/tests/differential/isoline-differential-replay ISOLINE_SEED=<seed> \
    ISOLINE_FIELD=steps wasm-tools shrink tests/fuzz/shrink-predicate.py <seed>.wasm -o regressions/<seed>.wasm
```

The fix lands in the code that owns it, with a unit test of its own. The minimized module is committed
to `regressions/`.
