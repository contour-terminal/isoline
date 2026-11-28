# Conformance

Isoline implements the WebAssembly core language of the 3.0 release without garbage collection, plus
three proposals beyond 3.0. This page pins, for each feature, the proposal and specification revision it
follows and the spec test suite that proves it. The suites are imported by
`scripts/import-spec-tests.py` into `tests/spec/<source>/<path>/` and run by CTest (label `spec`), one
test per file; `tests/spec/files.txt` names every imported file and pins its skip count.

## The runner

Every file is imported natively: the `.wast` script verbatim, and an oracle manifest beside it.
`isoline_spec_runner` reads the script itself with Isoline's text format (`tests/spec/WastReader.cpp`,
on the reader's S-expression tree), compiles every module given as text, `quote` included, with
`text::compileText`, and runs each command against the decoder, the validator and the reference
interpreter, as the spec interpreter runs a `.wast` file. Nothing in the run needs `wasm-tools` or the
network.

The manifest, `<name>.oracle`, is keyed by command ordinal, the index of the command in
`wasm-tools json-from-wast`'s output, not by line: its first line counts the script's commands,
`commands <n>`, and every other is `<ordinal> <line> <kind> <value>`, the line for diagnostics only. A
reader that split or merged a command would count differently, or land an entry on a command it does not
fit (a `wasm` entry on an action, say); either fails the whole script as a harness failure before any
command runs, rather than checking one command against another's binary. The kinds:

- `wasm`: the oracle's binary, `<name>.<ordinal>.wasm`, of a command whose module is given as text. A
  module given as bytes (`module binary`) has none: the runner takes its bytes from the script.
- `skip`: a command that is never compiled nor run, with its reason. Only the GC cut (below) writes
  them; each file pins its count in `tests/spec/files.txt`, which CTest passes the runner.
- `message` and `parity`: the divergences `tests/spec/divergences.txt` records (below).

### Parity

Isoline's encoding of a module given as text must mean the same module as the oracle's binary: the two
are compared by their canonical encodings, `binary::canonicalize`, which is
`encodeModule(normalize(decodeModule(bytes)))`, byte for byte. The normalizations are the encoding choices
that do not change a module's meaning:

- LEB128 widths, segment flag forms, and a compact import group, which encodes as one plain entry per
  import;
- a data count section that is redundant (its count is the data segments' and no function body holds a
  `memory.init` or `data.drop`), which is dropped, as the text format writes one only when a body needs
  it;
- local runs: runs of no locals are dropped and adjacent runs of one type merged;
- custom sections: one anchored to a section the encoder does not write is anchored to the last one
  before it that it writes, and all of them ordered as the encoder writes them;
- a branch hint section the decoder read whole, which is dropped and written again from the hints at the
  offsets the encoding gives the hinted instructions (one it refused stays verbatim);
- a name section that decodes, which is written again in subsection order, by ascending index, without
  what the codec skips (the GC proposal's field names, unknown, out-of-order and repeated subsections)
  and without empty subsections; one that does not decode is compared byte for byte.

Two binaries that do not decode are compared byte for byte; one that decodes and one that does not
differ. A difference fails the command at the first canonical offset the two differ at, unless the
oracle records it as a parity divergence.

## Text format limits

Reading text is bounded by `text::TextLimits`, every cap a published default an embedder may lower or
raise (`src/isoline/text/TextLimits.hpp`). Nothing reading or writing text recurses on its input: the
lexer, the tree, folded instructions, labels, float literals, the writer and the script reader are loops
over explicit stacks. Reading and writing take time linear in the text but for a logarithmic factor:
identifiers, labels, implicit types and the writer's names are looked up in ordered maps, never in hash
tables, which a text could collide on purpose, and an error deferred to a module's end is located once,
when it is reported. `isoline_text_test`'s `LinearTime` and `FunctionTypes` cases hold crafted inputs
(names one string hash maps to one value, types one type hash did) to their controls, and growing inputs
to linear growth.

| Limit | Default | What it bounds |
|---|---|---|
| `maxSourceBytes` | 1 GiB | the source, below 2^32 as tokens and nodes keep 32-bit offsets |
| `maxNesting` | 10 000 | the nesting of parenthesized lists, checked before the tree is reserved, so that a text of open lists past it holds nothing for them; flat `block … end` nests without it |
| `maxNumberLength` | 4 096 bytes | a numeric token, and so the exact arithmetic of its literal |
| `maxModuleBytes` | 64 MiB (`api::LoadLimits::maxModuleBytes`) | the binary a text compiles to, which a compact import group can make far larger than its source |

Within these, reading text holds memory linear in the source: at most `BytesPerSourceByte` bytes per
source byte beyond a fixed 64 KiB, measured with a budget-refusing allocator by `isoline_text_memory_test`
(`tests/memory/TextMemoryBounds_test.cpp`) on hostile inputs:

| `BytesPerSourceByte` | Bound | Measured densest case |
|---|---|---|
| the S-expression tree | 20 | 20.0, one-byte punctuation tokens |
| a module compiled from text | 20, plus each defined function's fixed cost | 15.6 under MSVC's debug runtime and 15.2 under Clang, a compact import group of short names |
| a module of one dense function body | 23 | 22.9, folded blocks nested to the cap |

The output a compact import group asks for is bounded apart, by `maxModuleBytes`: compiling holds at
most three copies of it. Writing text (`text::ModuleWriter`, `isoline disassemble`) is bounded by
`WriterOptions::maxTextBytes`, which defaults to `maxSourceBytes`, so that what it writes reads back:
writing holds at most 3 bytes per byte of text and 64 per byte of the module, and decides whether a
branch hint section can be written as annotations by encoding the hinted bodies alone, never the module.
That decision is bounded by what it allocates in all, freed or not: writing a hinted module allocates
less than one encoding of the module more than writing it without the hint
(`TextMemoryBounds.writtenHintSectionIsNotReencoded`), so no whole-module encoding fits in it.

## Suite sources

The suites are pinned in `scripts/tools.lock` as `source.<name>.repository` and
`source.<name>.commit`.

| Source | Repository | Commit |
|---|---|---|
| `wasm3` | [WebAssembly/spec](https://github.com/WebAssembly/spec), tag `wg-3.0` | `9d36019973201a19f9c9ebb0f10828b2fe2374aa` |
| `wide-arithmetic` | [WebAssembly/wide-arithmetic](https://github.com/WebAssembly/wide-arithmetic) | `024d4ce1b28cee7fd006d9c5175e3b7ece52b429` |
| `custom-page-sizes` | [WebAssembly/custom-page-sizes](https://github.com/WebAssembly/custom-page-sizes) | `61c873a1860acad2772539164f284cc5110f1b1b` |
| `compact-imports` | [WebAssembly/compact-import-section](https://github.com/WebAssembly/compact-import-section) | `0624de829e2663da799e708444dec80337a3fe54` |

Every `.wast` file of the pinned `wasm3` `test/core` and `test/custom` trees has a row in
`tests/spec/files.txt`: a row that imports it, or an `exclude` row with its reason. Checked against a
checkout of `WebAssembly/spec` (any commit containing the pinned one), in a scratch directory, where
`comm` printing nothing means the two lists are equal:

```sh
cd "$(mktemp -d)" && repo=<this checkout> && spec=<spec checkout>   # absolute paths
git -C "$spec" ls-tree -r --name-only 9d36019973201a19f9c9ebb0f10828b2fe2374aa test/core test/custom \
  | sed -n 's|^test/\(.*\)\.wast$|\1|p' | LC_ALL=C sort > pinned.txt
awk '$1 == "wasm3" { print $2 }' "$repo/tests/spec/files.txt" | LC_ALL=C sort > listed.txt
comm -3 pinned.txt listed.txt
```

### WASI

Isoline's WASI host interfaces target WASI 0.3, pinned to one tag of its WIT definitions. The core-cpp
services that will serve them (`core::fs`, `core::net`, `core::http`, vendored from core-cpp v0.24.0)
were mapped function by function against that tag.

| Interface | Repository | Tag | Commit |
|---|---|---|---|
| WASI 0.3 WIT (`proposals/<package>/wit/*.wit`, every package `@0.3.2`) | [WebAssembly/WASI](https://github.com/WebAssembly/WASI) | `v0.3.2` | `bdadbbb8cadd287cc5014c4b5a0bf76efc60291c` |

## The component suites

The component model's suites (P11, D12) are imported by `scripts/import-spec-tests.py --suite component`
into `tests/<source>/<path>/`, one directory per `.wast` file, and run by CTest (label `component-spec`) on
the reference engine under the component suites' rules: an assertion's message is found as a substring,
of Isoline's message or of one of the suites' wordings of it (`tests/spec/ComponentWording.hpp`: Wasmtime's
and wasm-tools' texts for the same failure, by stage), and a core trap is worded as Wasmtime words it.
`tests/component-files.txt` names every file, pins its skip count, and the core and component features it
runs with, which the import derives from the file (wasm-tools' `;; RUN:` line, Wasmtime's `;;!`
directives, or the component model's `test/README.md`); wasm-tools' own tests run short of executing
anything, as its `wast` does: a component is validated, never instantiated, and an action is read.

| Source | Repository | Commit |
|---|---|---|
| `component-model` | [WebAssembly/component-model](https://github.com/WebAssembly/component-model) `test/` | `f78ac03c178ad0f51b307852087792f1f164dc34` |
| `component-wasm-tools` | [bytecodealliance/wasm-tools](https://github.com/bytecodealliance/wasm-tools) 1.263.0 `tests/cli/component-model/` | `7a2fa422cdf9aeeaf488a2acac8bc94a078bfa32` |
| `component-wasmtime` | [bytecodealliance/wasmtime](https://github.com/bytecodealliance/wasmtime) 51.0.0 `tests/misc_testsuite/component-model/`, a curated list | `32656dc4873fcec5c8662aa937bd01acda3b2e85` |

The oracle is wasm-tools 1.263.0, whose component model submodule is at `1a743de6`. A command is skipped
only for a reason of `SkipReasons` (`tests/spec/SpecCheck.hpp`), which the runner refuses any other of:

- a rule of the importer's `FILE_SKIPS` skips a whole file: `async (P12)` for the async tests, `gated
  <emoji>` for a gate Isoline refuses (🧵②, 🐘, 📝, 🔧, 🪙), `core feature cut <name>` for GC and custom
  descriptors, `legacy text indices` for the legacy text syntax;
- the component cut (`component.cut.features` in `scripts/tools.lock`: the core cut less shared memories,
  with the component model and the gates Isoline implements) changes a command's verdict: it is skipped
  for the gate or core feature that gives the suite's verdict back, as wasm-tools validates it;
- the pinned wasm-tools fails the directive: `upstream nyi.txt` in a file the suite's `nyi.txt` lists,
  else `oracle unavailable since <commit>`, recorded in `tests/component-skips.txt` with the suite's commit
  that introduced the expectation;
- `wasmtime restriction (restrictions.wast:<line>)`, recorded there too: Wasmtime refuses what the component
  model admits (a root import or export of a component, a re-exported host function), as the assert_invalid
  at that line of its `restrictions.wast` does with the same message (the names it quotes aside), and the
  pinned wasm-tools admits it;
- `diverges from CM pin (definitions.py:<line>)`, recorded there too: the suite asserts what the pinned
  canonical ABI does otherwise at that line;
- and every command that uses a skipped module, for its root's reason.

Two Wasmtime tests are not imported: `same-instance-call.wast` expects an entry check the pin's
`definitions.py` does not make, and `big-strings.wast` strings past its `MAX_STRING_BYTE_LENGTH`.

<!-- component-exclusions:begin -->
```text
component-wasmtime big-strings: it sends strings of 2^31 bytes, past the pin's MAX_STRING_BYTE_LENGTH of 2^28 - 1 (definitions.py:1337, trapped at definitions.py:1360), raises Wasmtime's own hostcall fuel through a `wasmtime` instance no other runtime has, and needs about 4 GiB of memory
component-wasmtime same-instance-call: it expects a call from an instance into a function it lifted itself to trap, an entry check the pin's synchronous entry (definitions.py:425-445, canon_lift at definitions.py:2056) does not make
```
<!-- component-exclusions:end -->

Skips by file and reason:

<!-- component-skips:begin -->
```text
component-model async/async-calls-sync: 3 × async (P12)
component-model async/big-interleaving-test: 57 × async (P12)
component-model async/builtin-trap-poisons-instance: 8 × async (P12)
component-model async/cancel-and-exclusive-lock: 2 × async (P12)
component-model async/cancel-delivery: 4 × async (P12)
component-model async/cancel-not-delivered: 2 × async (P12)
component-model async/cancel-resumed-callback-switch: 4 × async (P12)
component-model async/cancel-stream: 15 × async (P12)
component-model async/cancel-subtask: 4 × async (P12)
component-model async/cancel-targeted-resume: 10 × async (P12)
component-model async/closed-stream: 3 × async (P12)
component-model async/cross-abi-calls: 49 × async (P12)
component-model async/cross-task-future: 2 × async (P12)
component-model async/deadlock: 2 × async (P12)
component-model async/dont-block-start: 2 × async (P12)
component-model async/drop-cross-task-borrow: 7 × async (P12)
component-model async/drop-stream: 5 × async (P12)
component-model async/drop-subtask: 3 × async (P12)
component-model async/drop-waitable-set: 2 × async (P12)
component-model async/during-sync-call-exclusive-resume: 6 × async (P12)
component-model async/during-sync-call-may-block-if-other-ready-threads: 9 × async (P12)
component-model async/during-sync-call-no-sibling-resume: 6 × async (P12)
component-model async/during-sync-scheduling-candidates: 31 × async (P12)
component-model async/empty-wait: 2 × async (P12)
component-model async/forward: 95 × async (P12)
component-model async/future-completion-order: 5 × async (P12)
component-model async/futures-must-write: 14 × async (P12)
component-model async/idle-drop: 38 × async (P12)
component-model async/partial-stream-copies: 9 × async (P12)
component-model async/passing-resources: 3 × async (P12)
component-model async/reentrance: 25 × async (P12)
component-model async/same-component-stream-future: 9 × async (P12)
component-model async/self-switch-traps: 9 × async (P12)
component-model async/switch-to-ready-callback: 17 × async (P12)
component-model async/sync-barges-in: 3 × async (P12)
component-model async/sync-streams: 2 × async (P12)
component-model async/trap-if-block-and-sync: 48 × async (P12)
component-model async/trap-if-done: 31 × async (P12)
component-model async/trap-if-sync-and-waitable-set: 29 × async (P12)
component-model async/trap-if-transfer-in-waitable-set: 5 × async (P12)
component-model async/validate-no-async-abi-for-sync-type: 3 × async (P12)
component-model async/validate-no-stream-char: 1 × async (P12)
component-model async/validate-task-return: 3 × async (P12)
component-model async/wait-during-callback: 2 × async (P12)
component-model async/zero-length: 27 × async (P12)
component-model binary/binary: 4 × async (P12)
component-model binary/binary: 3 × core feature cut gc
component-model binary/binary: 1 × gated 🔧
component-model binary/binary: 11 × upstream nyi.txt
component-model validation/indicies: 2 × async (P12)
component-model validation/indicies: 1 × gated 🧵
component-model validation/instantiation: 1 × core feature cut threads
component-model validation/kebab: 1 × upstream nyi.txt
component-model validation/max-value-size: 1 × async (P12)
component-model validation/max-value-size: 7 × gated 🔧
component-model values/post-return: 5 × async (P12)
component-model values/post-return: 61 × upstream nyi.txt
component-model values/variants: 5 × async (P12)
component-wasm-tools async/abi: 2 × async (P12)
component-wasm-tools async/async-builtins: 19 × async (P12)
component-wasm-tools async/futures: 24 × async (P12)
component-wasm-tools async/lift: 11 × async (P12)
component-wasm-tools async/lower: 6 × async (P12)
component-wasm-tools async/names: 4 × async (P12)
component-wasm-tools async/stackful: 8 × async (P12)
component-wasm-tools async/streams: 23 × async (P12)
component-wasm-tools async/task-builtins: 49 × async (P12)
component-wasm-tools async/threading: 29 × async (P12)
component-wasm-tools async/threading-and-memory64: 7 × async (P12)
component-wasm-tools attribute-order: 5 × gated 🔗
component-wasm-tools canon-names: 21 × gated 🔗
component-wasm-tools custom-descriptors: 7 × core feature cut custom-descriptors
component-wasm-tools error-context/error-context: 9 × gated 📝
component-wasm-tools fixed-length-lists: 7 × gated 🔧
component-wasm-tools gc/enum-types: 2 × core feature cut gc
component-wasm-tools gc/error-context: 2 × core feature cut gc
component-wasm-tools gc/flag-types: 2 × core feature cut gc
component-wasm-tools gc/futures: 2 × core feature cut gc
component-wasm-tools gc/gc: 14 × core feature cut gc
component-wasm-tools gc/lift-func-many-params: 1 × core feature cut gc
component-wasm-tools gc/lift-func-no-params: 1 × core feature cut gc
component-wasm-tools gc/list-types: 2 × core feature cut gc
component-wasm-tools gc/option-types: 6 × core feature cut gc
component-wasm-tools gc/primitive-types: 60 × core feature cut gc
component-wasm-tools gc/record-types: 4 × core feature cut gc
component-wasm-tools gc/resource-types: 4 × core feature cut gc
component-wasm-tools gc/result-types: 6 × core feature cut gc
component-wasm-tools gc/tuple-types: 4 × core feature cut gc
component-wasm-tools gc/variant-types: 6 × core feature cut gc
component-wasm-tools implements-version-suffix: 7 × gated 🔗
component-wasm-tools indices: 2 × async (P12)
component-wasm-tools indices: 1 × gated 🧵
component-wasm-tools instance-type: 2 × core feature cut threads
component-wasm-tools instantiate: 1 × core feature cut threads
component-wasm-tools legacy-indices: 8 × legacy text indices
component-wasm-tools map: 3 × async (P12)
component-wasm-tools map: 1 × gated 📝
component-wasm-tools max-value-size: 1 × async (P12)
component-wasm-tools max-value-size: 8 × gated 🔧
component-wasm-tools memory64/async: 8 × gated 🐘
component-wasm-tools memory64/context: 9 × gated 🐘
component-wasm-tools memory64/list: 1 × gated 🐘
component-wasm-tools memory64/memory64: 1 × gated 🐘
component-wasm-tools memory64/realloc: 3 × gated 🐘
component-wasm-tools memory64/resources: 7 × gated 🐘
component-wasm-tools memory64/string: 1 × gated 🐘
component-wasm-tools memory64/waitable-set: 6 × gated 🐘
component-wasm-tools naming: 2 × async (P12)
component-wasm-tools naming: 22 × gated 📡
component-wasm-tools nested-names: 1 × gated 🪺
component-wasm-tools options: 1 × core feature cut threads
component-wasm-tools shared-everything-threads/globals: 3 × gated 🧵②
component-wasm-tools shared-everything-threads/not-accepted: 3 × gated 🧵②
component-wasm-tools tags: 3 × core feature cut gc
component-wasm-tools values/start: 9 × gated 🪙
component-wasm-tools values/values: 18 × gated 🪙
component-wasmtime core-import-func-subtype: 2 × core feature cut gc
component-wasmtime core-type-out-of-group-deps: 3 × core feature cut gc
component-wasmtime empty-rec-groups: 3 × core feature cut gc
component-wasmtime resources: 4 × async (P12)
```
<!-- component-skips:end -->

Recorded skips, by line:

<!-- component-recorded-skips:begin -->
```text
component-model validation/attributes:88: oracle unavailable since fb27002f
component-model validation/attributes:92: oracle unavailable since fb27002f
component-wasmtime adapter:98: diverges from CM pin (definitions.py:425)
component-wasmtime import:2: wasmtime restriction (restrictions.wast:9)
component-wasmtime resources:1318: diverges from CM pin (definitions.py:1432)
component-wasmtime restrictions:2: wasmtime restriction (restrictions.wast:1)
component-wasmtime restrictions:6: wasmtime restriction (restrictions.wast:5)
component-wasmtime restrictions:10: wasmtime restriction (restrictions.wast:9)
component-wasmtime restrictions:17: wasmtime restriction (restrictions.wast:16)
component-wasmtime simple:25: wasmtime restriction (restrictions.wast:1)
component-wasmtime simple:31: wasmtime restriction (restrictions.wast:5)
component-wasmtime types:524: diverges from CM pin (definitions.py:1493)
component-wasmtime types:525: diverges from CM pin (definitions.py:1503)
```
<!-- component-recorded-skips:end -->

Divergences from the oracle (`tests/component-divergences.txt`), each checked by the runner:

<!-- component-divergences:begin -->
```text
component-wasm-tools very-nested:1871: message unexpected token
```
<!-- component-divergences:end -->

## Features

Suite paths are relative to the source's `test/` directory.

| Feature | Proposal | Spec revision | Suite source | Suite files |
|---|---|---|---|---|
| Wasm 1.0 (MVP, mutable-global import/export) | — | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/*.wast` |
| Sign-extension operators | [sign-extension-ops](https://github.com/WebAssembly/sign-extension-ops) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/i32.wast`, `core/i64.wast` |
| Non-trapping float-to-int | [nontrapping-float-to-int-conversions](https://github.com/WebAssembly/nontrapping-float-to-int-conversions) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/conversions.wast` |
| Multi-value | [multi-value](https://github.com/WebAssembly/multi-value) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/block.wast`, `core/func.wast`, `core/if.wast`, `core/loop.wast`, … |
| Reference types | [reference-types](https://github.com/WebAssembly/reference-types) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/ref_*.wast`, `core/table*.wast`, `core/select.wast` |
| Bulk memory | [bulk-memory-operations](https://github.com/WebAssembly/bulk-memory-operations) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/bulk-memory/*.wast` |
| 128-bit SIMD | [simd](https://github.com/WebAssembly/simd) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/simd/*.wast` |
| Tail calls | [tail-call](https://github.com/WebAssembly/tail-call) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/return_call*.wast` |
| Extended constant expressions | [extended-const](https://github.com/WebAssembly/extended-const) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/data.wast`, `core/elem.wast`, `core/global.wast` |
| Typed function references | [function-references](https://github.com/WebAssembly/function-references) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/call_ref.wast`, `core/br_on_*null.wast`, `core/ref_as_non_null.wast`, `core/local_init.wast`, `core/type-equivalence.wast`, … |
| Multiple memories | [multi-memory](https://github.com/WebAssembly/multi-memory) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/multi-memory/*.wast` |
| Relaxed SIMD | [relaxed-simd](https://github.com/WebAssembly/relaxed-simd) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/relaxed-simd/*.wast` |
| Exception handling (`exnref`) | [exception-handling](https://github.com/WebAssembly/exception-handling) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/exceptions/*.wast` |
| memory64 and table64 | [memory64](https://github.com/WebAssembly/memory64) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/memory64/*.wast` |
| Branch hinting | [branch-hinting](https://github.com/WebAssembly/branch-hinting) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `custom/metadata.code.branch_hint/branch_hint.wast` |
| Custom annotations (text format) | [annotations](https://github.com/WebAssembly/annotations) | WebAssembly 3.0 (`wg-3.0`): `@custom` and `@name`, and branch hinting's `@metadata.code.branch_hint`; every other annotation is ignored | `wasm3` | `core/annotations.wast`, `custom/custom/custom_annot.wast`, `custom/name/name_annot.wast` |
| Wide arithmetic | [wide-arithmetic](https://github.com/WebAssembly/wide-arithmetic) (phase 4) | the proposal at the pinned commit | `wide-arithmetic` | `core/wide-arithmetic.wast` |
| Custom page sizes | [custom-page-sizes](https://github.com/WebAssembly/custom-page-sizes) (phase 3) | the proposal at the pinned commit | `custom-page-sizes` | `core/custom-page-sizes/*.wast` |
| Compact import section | [compact-import-section](https://github.com/WebAssembly/compact-import-section) (phase 4) | the proposal at the pinned commit | `compact-imports` | `core/compact-import-section/*.wast` |

## The GC cut

Isoline has typed function references but no garbage collection. The language it accepts is exactly
what the pinned oracle accepts:

```sh
wasm-tools validate --features=wasm3,-gc,wide-arithmetic,compact-imports,custom-page-sizes <module.wasm>
```

with `wasm-tools` at the version `scripts/tools.lock` pins, and the features its `cut.features` names.
The importer runs the oracle on the module of every command of every suite source, a quote module's
included. A module it rejects with a message of GC's becomes a `gc:` skip entry, together with every
command that uses that module, and a whole file is excluded where all of it is about GC. Any other
disagreement between the oracle and the suite fails the import, so the cut never hides a difference
that is not GC's. These are the only skips: every module the suites give as text is read, and every
assertion about text (`assert_malformed` of a quote module, `assert_malformed_custom`, the annotations
of `annotations.wast`, `custom_annot.wast`, `name_annot.wast` and `branch_hint.wast`) runs.

## Exclusions

Generated by `scripts/check-conformance.py --update` from the `exclude` rows of
`tests/spec/files.txt` and the `skip` entries of the imported oracles, all of them the cut's (any other
fails the check); the CTest test
`conformance-doc-current` fails when it is not current. Each line is an excluded file,
`<source> <path>: <reason>`, or an assertion the cut skips, `<source> <path>:<wast line>: <oracle
message>`; a command skipped because it uses a cut module names its root, `(root wast line N)`.

<!-- exclusions:begin -->
```text
wasm3 core/data:89: constant expression required: global.get of locally defined global
wasm3 core/data:90: constant expression required: global.get of locally defined global
wasm3 core/elem:178: constant expression required: global.get of locally defined global
wasm3 core/elem:182: constant expression required: global.get of locally defined global
wasm3 core/exceptions/tag:30: rec group usage requires `gc` proposal to be enabled
wasm3 core/exceptions/tag:38: rec group usage requires `gc` proposal to be enabled (root wast line 30)
wasm3 core/exceptions/tag:40: rec group usage requires `gc` proposal to be enabled (root wast line 30)
wasm3 core/exceptions/tag:49: rec group usage requires `gc` proposal to be enabled (root wast line 30)
wasm3 core/exceptions/tag:60: rec group usage requires `gc` proposal to be enabled (root wast line 30)
wasm3 core/gc/array: GC: the GC proposal's own tests
wasm3 core/gc/array_copy: GC: the GC proposal's own tests
wasm3 core/gc/array_fill: GC: the GC proposal's own tests
wasm3 core/gc/array_init_data: GC: the GC proposal's own tests
wasm3 core/gc/array_init_elem: GC: the GC proposal's own tests
wasm3 core/gc/array_new_data: GC: the GC proposal's own tests
wasm3 core/gc/array_new_elem: GC: the GC proposal's own tests
wasm3 core/gc/binary-gc: GC: the GC proposal's own tests
wasm3 core/gc/br_on_cast: GC: the GC proposal's own tests
wasm3 core/gc/br_on_cast_fail: GC: the GC proposal's own tests
wasm3 core/gc/extern: GC: the GC proposal's own tests
wasm3 core/gc/i31: GC: the GC proposal's own tests
wasm3 core/gc/ref_cast: GC: the GC proposal's own tests
wasm3 core/gc/ref_eq: GC: the GC proposal's own tests
wasm3 core/gc/ref_test: GC: the GC proposal's own tests
wasm3 core/gc/struct: GC: the GC proposal's own tests
wasm3 core/gc/type-subtyping: GC: the GC proposal's own tests
wasm3 core/global:373: constant expression required: global.get of locally defined global
wasm3 core/global:374: constant expression required: global.get of locally defined global
wasm3 core/global:634: constant expression required: global.get of locally defined global
wasm3 core/global:659: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/global:660: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/global:661: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/global:663: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/global:664: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/ref_null: GC: the nulls of GC's heap types and anyref
wasm3 core/type-canon: GC: canonicalization of recursive type groups
wasm3 core/type-equivalence:30: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:38: unknown type 0: type index out of bounds because the GC proposal is disabled
wasm3 core/type-equivalence:49: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:136: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:156: rec group usage requires `gc` proposal to be enabled (root wast line 136)
wasm3 core/type-equivalence:161: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:188: rec group usage requires `gc` proposal to be enabled (root wast line 161)
wasm3 core/type-equivalence:233: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:237: rec group usage requires `gc` proposal to be enabled (root wast line 233)
wasm3 core/type-equivalence:238: rec group usage requires `gc` proposal to be enabled (root wast line 233)
wasm3 core/type-equivalence:246: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:256: rec group usage requires `gc` proposal to be enabled (root wast line 246)
wasm3 core/type-equivalence:257: rec group usage requires `gc` proposal to be enabled (root wast line 246)
wasm3 core/type-equivalence:268: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:278: rec group usage requires `gc` proposal to be enabled (root wast line 268)
wasm3 core/type-equivalence:279: rec group usage requires `gc` proposal to be enabled (root wast line 268)
wasm3 core/type-equivalence:290: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:307: rec group usage requires `gc` proposal to be enabled (root wast line 290)
wasm3 core/type-equivalence:308: rec group usage requires `gc` proposal to be enabled (root wast line 290)
wasm3 core/type-rec: GC: recursive type groups
```
<!-- exclusions:end -->

## Expected divergences

Where Isoline reports another error of a malformed text module first than the reference interpreter
does, `tests/spec/divergences.txt` records Isoline's message for that `assert_malformed` or
`assert_malformed_custom`, the file's oracle carries it as a `message` entry, and the runner requires exactly that message instead of the suite's: the
assertion still runs, and the divergence is listed here rather than hidden as a skip. Generated and
checked the same way, from the `message` entries of the `.oracle` manifests,
`<source> <path>:<wast line>: <Isoline's message>`.

<!-- divergences:begin -->
```text
```
<!-- divergences:end -->

### Parity divergences

Where the oracle's binary of a module given as text is known to be wrong, so that Isoline's canonical
encoding differs from it, `tests/spec/divergences.txt` records the first offset the two differ at, the
SHA-256 of Isoline's canonical encoding and why, and the file's oracle carries it as a `parity` entry,
`0x<offset> sha256:<digest> <reason>`. The runner then requires the parity check to fail at exactly
that offset, with Isoline's encoding having exactly that digest: a divergence that goes away or moves
fails as stale, and so does any later change to Isoline's encoding, behind the offset as well as before
it, and the command still runs. What the row cannot see is a second difference already behind the
offset when it is recorded: checking there is none is the job of whoever records the row. Generated
and checked the same way, from the `parity` entries, `<source> <path>:<wast line>: 0x<offset>
sha256:<digest> <reason>`.

<!-- parity-divergences:begin -->
```text
```
<!-- parity-divergences:end -->

### Other divergences

Two more places where Isoline reads text other than one of its references does, neither reached by an
assertion of the suites:

- **An implicit type spelled two ways.** `(func (param (ref $t)))` and `(func (param (ref 0)))`, where
  `$t` is type 0, use one implicit type in Isoline, as in the reference interpreter (`parser.mly`'s
  `inline_functype` resolves the identifier before it looks the type up). The pinned wasm-tools
  1.263.0 appends a second, identical type for the second spelling. Only `ref.wast` mixes the
  spellings, never two in one module, so its parity holds.
- **Instances in scripts.** The script reader refuses `(module instance)` that names no definition
  ("a module instance must name its definition"), where the reference instantiates the latest one, and
  an assertion about a module instance (`assert_trap`, `assert_unlinkable` of `(module instance …)`:
  "an assertion on a module instance is not supported"); it accepts `(module definition …)` there. The
  suites use none of these forms.

### Annotations beyond the reference

The reference interpreter's `@name` handler (`interpreter/custom/handler_name.ml` at `wg-3.0`) names
the module, functions and tags only, and refuses the rest as "not yet supported". Isoline reads `@name`
where wasm-tools does, directly after an entity's optional identifier, and so names what the reference
refuses or places differently. No assertion of the suite reaches any of these:

- globals, memories, tables, element and data segments, and types, which the reference refuses;
- parameters and locals, `(param $x? (@name "…") t)`, which it refuses as "local names not yet
  supported", and block labels, which it does not name;
- a function's `@name` only right after its identifier, where the reference takes one anywhere before
  its type use, `(func (export "e") (@name "f"))` included;
- a tag's `@name` right after its identifier, where the reference takes one anywhere before the tag's
  end.

Every `@name` Isoline does not place is "misplaced @name annotation", where the reference reports some
of those as "@name annotation: misplaced annotation". Beyond names: a branch hint annotation with no
contents is "invalid hint value", where the reference fails on it, and an unknown annotation outside
the module form is ignored, where the reference refuses every annotation it leaves outside a module as
"misplaced annotation", as Isoline does a known one.
