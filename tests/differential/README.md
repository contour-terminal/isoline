# Differential testing

The differential harness runs one module on every engine of the build: the reference interpreter, and the
fast interpreter under each compiled dispatch. It compares everything each engine observes, exactly. The
first participant is the expected one.

For each call it compares the following, in this order (`Comparison.hpp`'s `ObservationFields`):

- the exception the call threw, and the call's outcome, with NaN bits compared;
- the steps charged, and the polls of each site;
- a digest of every memory, then every global and every table element;
- the refused grows;
- the live exceptions;
- the stack values still charged;
- the calls into the recording host.

The instantiation is compared the same way. Each participant runs twice: once unobserved, through the
engines' fast paths, and once with a poll observer. An `InterruptionPlan` can raise the interrupt at the
k-th poll of each call.

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

## Running the corpus

```sh
python3 -I scripts/make-differential-corpus.py            # out/corpus/differential/<seed>.wasm
ISOLINE_WASMTIME=$(python3 -I scripts/fetch-tools.py --tool wasmtime) \
    ctest --test-dir out/clang-release -L differential --output-on-failure
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
