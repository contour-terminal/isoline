# Fuzz targets

Three libFuzzer targets, and the module generator the differential one is driven by. What each target does
with one input lives in `FuzzTargets.hpp`, a library the unit tests share, so the entry points
(`DifferentialFuzzer.cpp`, `LoadFuzzer.cpp`, `TextFuzzer.cpp`) are one call each.

| Target | Input | Finds |
|---|---|---|
| `isoline_differential_fuzzer` | an argument seed (8 bytes, little-endian), then the generator's entropy | any observable on which an engine differs from the reference, in every active lane (`DifferentialRunner.hpp`) |
| `isoline_load_fuzzer` | a binary module | crashes in the decoder, validator and lowering; a lowering past its published bounds (code words, work, the arena's peak); a lowering that is not linear, as a timeout or past the RSS limit |
| `isoline_text_fuzzer` | a module in the text format | crashes in the text reader; what it compiles goes on to the load target |

## The generator

`generateModule()` (`ModuleGenerator.hpp`) builds a module that validates under `GeneratorFeatures` (WebAssembly
3.0, wide arithmetic and custom page sizes) from any bytes.
It writes a function body one step at a time: a step picks a group of instructions by weight, then a row of
the group. The rows are every `OpcodeTable` row of the cut: the `OpcodeSignature` rows, typed by
`SignatureTable`; the loads and stores, typed by `MemoryAccessTable`; and a table of special rows (control,
calls, variables, tables, memory, references, exceptions), each with its emitter. A new instruction lands with
its row: a row of the cut that none of the three types stops the generator.

The writer tracks each block's operand types. An operand an instruction does not find on top of the stack is
made: a constant (an edge value of its type a quarter of the time), or a read of a local or a global. An
address, a bulk length and a table index are biased toward values that do not trap at once. Every block
closes with exactly its results; an unconditional branch closes its block next. A loop sits in a block of its
own and tests and decrements a counter local at its head, leaving through that block at zero, so every branch
to it is bounded: its back edge, and a br, br_if (a comparison right before it, which the interpreter fuses,
half the time), br_table, br_on_null or catch clause from inside it. Calls may still recurse, and
`FuzzLimits.maxSteps` bounds them. A step that would take the body past its budget is rolled back, and the
open blocks are closed, so a body holds at most `maxFunctionInstructions()` instructions and a module at most
`maxModuleBytes()` bytes.

The module has three memories: memory 0 (cached by the interpreter), a 64-bit memory 1, and memory 2 of 1-byte
pages. `GeneratorMode::Oracle` builds modules that import nothing and whose functions take numbers, which
Wasmtime can run: `GeneratedOracle.agreesWithWasmtime` (label `oracle-nightly`) holds them against it in the
nightly differential run, since the engines share their numeric kernels and only the oracle checks those.

Two program shapes reach what a random body does not: a recursion hundreds of frames deep inside eight blocks
that throws at the bottom, one unwind past `UnwindPollItems` labels; and a loop that throws and catches more
exceptions than the store holds, which sweeps.

`ModuleGenerator_test.cpp` checks that 5'000 fixed-seed buffers all validate, that the same entropy builds the
same module, that the generated modules reach every row of the cut, every `OperationForm` the interpreter
lowers to and every `PollSite`, that each kind of branch reaches a loop, that the oracle mode imports nothing,
and the bound on the output.

## Lanes

`DifferentialLanes` (`FuzzTargets.hpp`) says which lanes are fuzzed. Lane 2 (Production) lowers exactly as lane
1 until P5's compiler lowers it otherwise, so it is inactive until then: no campaign hours, no CI smoke run.
`ISOLINE_FUZZ_LANE=production` still runs it on request.

## Running

The targets build with the `clang-fuzz` (optimized) and `clang-fuzz-asan` (with AddressSanitizer and
UndefinedBehaviorSanitizer) presets, which set `ISOLINE_FUZZING`: every Isoline target and vendored core-cpp
are instrumented for coverage, and the targets alone link libFuzzer's main. Every other Clang build links them
with `FuzzReplayMain.cpp` instead, which runs each input file or directory it is given: clang-tidy checks the
entry points, `<target>.replay` runs the committed inputs through them, and a finding replays offline.

```sh
cmake --preset clang-fuzz-asan && cmake --build --preset clang-fuzz-asan
out/clang-fuzz-asan/tests/fuzz/isoline-fuzz-seeds load out/fuzz/seeds/load 262144
mkdir -p out/fuzz/load
out/clang-fuzz-asan/tests/fuzz/isoline_load_fuzzer -max_total_time=300 -rss_limit_mb=2048 -timeout=10 \
    -max_len=262144 out/fuzz/load tests/fuzz/regressions/load out/fuzz/seeds/load
```

`ISOLINE_FUZZ_LANE` (`deterministic` or `production`) runs one lane; unset or empty, every active one.
`isoline-fuzz-seeds` writes the load target's seeds (linear lowering's hostile shapes scaled down, and
generated modules) and the text target's (generated modules as text), refusing any longer than the `-max_len`
it is given. CI's `linux-clang-fuzz-smoke` job runs each target and active lane for five minutes under
`clang-fuzz-asan`, from the committed inputs, the generated seeds, the spec suite's binaries (load) and the
differential edge modules (text); its run table holds each target's `-max_len`. Run campaigns niced, one at a
time, with `-jobs` and `-workers` for more cores.

## Findings

Every finding becomes a committed input and a unit test in the task that owns the code.

- **Inputs.** `regressions/<target>/` holds each target's committed inputs: seeds and minimized findings.
  `FuzzRegressions.everyInputPasses` runs every one of them in the unit run, and the smoke job starts from them.
- **A divergence.** `isoline-fuzz-module <input> tests/differential/regressions` (which never overwrites) writes the module the input
  generates as `<seed>.wasm`, the form of a differential regression, which `isoline-differential-replay`
  replays and `CorpusReplay.everyRegressionAgrees` keeps checking. Minimize it with `shrink-predicate.py`
  (its docstring has the command line).
