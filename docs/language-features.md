# C++26 language features

Isoline is a C++23 library. Where a compiler supports C++26 reflection (P2996), annotations (P3394) and
contracts (P2900), Isoline uses them. Reflection adds compile-time checks of its hand-maintained tables,
and contracts carry preconditions. Every other compiler builds the same library through a C++23
fallback. The installed headers stay C++23 on every compiler, and consumers compile as C++23.

`cmake/IsolineLanguageFeatures.cmake` decides each feature's path at configure time, and prints one line
per feature:

```text
-- [isoline] C++26 reflection: native (GNU 16.2 meets GNU 16.2, and cmake/probes/reflection.cpp compiles)
-- [isoline] C++26 reflection: fallback (no measured row for Clang 22.1)
-- [isoline] C++26 consumer floor: pending Clang, AppleClang, MSVC
```

## Options

| Option | Values | Default | Meaning |
|---|---|---|---|
| `ISOLINE_REFLECTION` | `AUTO`, `ON`, `OFF` | `AUTO` | reflection checks of the tables |
| `ISOLINE_ANNOTATIONS` | `AUTO`, `ON`, `OFF` | `AUTO` | annotations, read by reflection; needs reflection native |
| `ISOLINE_CONTRACTS` | `AUTO`, `ON`, `OFF` | `AUTO` | contracts for preconditions; needs reflection native |

- `AUTO` takes the native path wherever the compiler has a measured row and its probe passes. That
  includes installed and packaged builds.
- `ON` fails the configure when the native path is out of reach, and names the reason.
- `OFF` takes the fallback.
- CMake's boolean spellings, in any case, count as `ON` (`YES`, `TRUE`, `Y`, `1`) and `OFF` (`NO`,
  `FALSE`, `N`, `0`), and `auto` as `AUTO`. The spellings are a table of rows
  (`ISOLINE_LANGUAGE_FEATURE_OPTION_SPELLINGS`). Any other value, such as `2`, is a configure error.

The options are defined from the feature rows below, one per feature. The help text that `cmake -LH`
shows is the feature's purpose and requirement, both taken from its first row. Adding a feature means adding its rows (and its probe); the option, the
order of the decisions and the status line follow. Features are decided in the order their rows first
appear, so a requirement's rows come first. A requirement that no earlier row defines is a configure
error.

The result is the cache entry `ISOLINE_<FEATURE>_PATH`, which is `native` or `fallback`.

## How the path is decided

Each feature has one row per compiler:

| Feature | Compiler | Minimum | Standard | Flags | Libraries | Requires | Probe | Purpose |
|---|---|---|---|---|---|---|---|---|
| reflection | GNU | 16.2 | 26 | `-freflection` | | | `cmake/probes/reflection.cpp` | for Isoline's table checks |
| annotations | GNU | 16.2 | 26 | `-freflection` | | reflection | `cmake/probes/annotations.cpp` | for table rows derived from enumerator annotations |
| contracts | GNU | 16.2 | 26 | `-freflection -fcontracts -fcontract-evaluation-semantic=enforce` | `stdc++exp` | reflection | `cmake/probes/contracts.cpp` | for preconditions in noexcept functions |

The probe always runs, except under `OFF`. A compiler without a row is probed with the feature's first
row's flags and libraries, so that a compiler which accepts them is told to measure itself. Only a
compiler's own row is ever applied to a target. The probe compiles and links what Isoline uses, in the
forms it uses it:

- **reflection:** enumerators in a `static constexpr` array iterated by `template for`; `identifier_of`;
  `members_of` over a namespace; `offset_of`; `is_noexcept` of the enclosing function.
- **annotations:** an enumerator annotated `[[=FixedText{"x"}]]`, read back through `constant_of`.
- **contracts:** `pre`, `post(r: …)`, `contract_assert` and `<contracts>`, compiled under the `enforce`
  semantic and linked.

The first rule that applies gives the outcome. The precedence is OFF, then no row, then the minimum,
then the probe, then the requirement:

| Rule | Under `AUTO` | Under `ON` |
|---|---|---|
| the option is `OFF` | fallback, "ISOLINE_<X>=OFF" | — |
| no row, and the probe passes | fallback, "the probe passes: measure <compiler> <version> and add its row" | configure error |
| no row, and the probe fails | fallback, "no measured row for <compiler> <version>" | configure error |
| the compiler is below its row's minimum | fallback, "<compiler> <version> is below <compiler> <minimum>" | configure error |
| the probe fails | fallback, "probe failed: <first diagnostic>" | configure error naming the probe |
| the requirement took the fallback | fallback, "needs reflection, which is fallback" | configure error |
| none of these | native | native |

A compiler takes the fallback without a row, even when its probe passes. Every flag, library and report
format is that compiler's own data, and it is measured before Isoline relies on it. Supporting a new
compiler means measuring it and adding its row.

`tests/cmake/language-features_test.cmake` (the `language-features-selftest` test, label `lint`) checks
each rule. It has one row per case, plus the precedence cases: GCC 14 is below its row before its probe
fails; Clang without a row also fails its probe; `AUTO` follows a requirement that fell back. It also runs
the selection the configure runs, over rows with a stub probe. Those cases cover reflection `OFF` with
contracts `ON`, a requirement falling back by its own probe, a requirement no row defines, a fourth
feature added as one row, and which probes run with which flags.

## What a native path changes

- **Contracts native:** `isoline_core` compiles as C++26 with the contracts row's flags, and links
  `stdc++exp` PRIVATE. A static library's PRIVATE link items reach consumers as `$<LINK_ONLY:…>`, so an
  installed package links the library without compiling with its flags. The target's compile feature
  stays `cxx_std_23`, so consumers and the tests compile as C++23. Under `AUTO`, a GCC 16.2 build
  already does this before any contract site exists. That is deliberate: contracts are on wherever
  supported (owner, Q1), and a C++26 `isoline_core` is what lets the header check fail on its own.
- **Reflection and annotations native:** they only add check translation units of their own.

## The installed headers stay C++23

`isoline_apply_language_features()` records every target it is given, on either path, in the global
property `ISOLINE_LANGUAGE_FEATURE_TARGETS`. Today that is `isoline_core`; another library is one call.
After all of them are defined, the top-level `CMakeLists.txt` adds one check over all of them,
`isoline_public_headers_cxx23`. Adding the check seals the registry: a target passed to
`isoline_apply_language_features()` afterwards would compile with the native features while its headers
go unchecked, so the configure fails and names the target. The fixture
`tests/cmake/header-check-sealed` (tests `language-features-header-check-sealed-late` and `-early`)
pins this. The check compiles every `.hpp` of their header sets on its own: one generated
translation unit per header, as C++23 without extensions, under Isoline's warning set. The target
itself turns a C++26 extension into an error (`-Werror=c++26-extensions`, and `c++2c-extensions` where
Clang knows it), with or without `ISOLINE_WERROR`. clang-tidy skips the one-line stubs, because each
header's code is tidied with its library.

The check is built with the tests (`ISOLINE_TESTING`). It fails when an installed header uses C++26
syntax, such as an annotation, `pre` or `contract_assert`, even where the library itself compiles as
C++26. It also fails when a header does not include what it uses.

## What reflection checks

Reflection only verifies. The tables, `src/isoline/meta/EnumTables.def` and `Opcodes.def` stay the
source on every compiler, and no shipped value is computed by reflection, so the object code is the
same on both paths. Where reflection is native, two targets exist that the fallback does not have
(`isoline_add_reflection_check()`, built with the tests):

- `isoline_meta_reflection_test` (label `unit`) tests the checks on fixtures of its own.
- `isoline_enum_tables_check` is compile-only: its `static_assert`s are the check, and the build fails
  when one does.

### The registry

`EnumTables.def` has one row per table that an enum indexes, sizes or keys, and one for each `Last` constant that none of those checks:

| Row | Means | Reflection adds |
|---|---|---|
| `ISOLINE_ENUM_TABLE(Table, Last, key)` | one row per enumerator, at its index | `Last` is the final enumerator; the enumerators are 0, 1, 2 and on |
| `ISOLINE_SIZED_TABLE(Table, Last)` | one entry per enumerator 0 through `Last`, no row struct | the same |
| `ISOLINE_PREFIX_TABLE(Table, Last, "reason")` | one entry per enumerator 0 through `Last`, which is not final | the enumerators are dense through `Last` |
| `ISOLINE_KEYED_TABLE(Table, "reason")` | rows that start with an enum but are keyed by it, not indexed | the row is registered |
| `ISOLINE_LAST_ENUMERATOR(Last, "reason")` | a `Last` constant no reachable table checks | `Last` is the final enumerator |
| `ISOLINE_SOURCE_LOCAL_TABLE("file", Table, "reason")` | a table in a source file, out of reflection's reach | nothing: the lint checks the row (below) |

Each `ENUM` and `SIZED` row restates its table's own C++23 check, which every compiler runs. C++23 can
check that a table has one row per enumerator 0 through `Last`, at its index, but not that `Last` is
the final enumerator: an enumerator appended after `Last`, with no row, builds on every C++23 compiler.
Reflection names it. With `Extra` appended to `TrapCode`, `isoline_core` still builds on
`gcc-fallback`, and `gcc-cxx26` fails:

```text
error: static assertion failed: EnumTables.def: runtime::TrapTable: TrapCode::Extra follows LastTrapCode (TrapCode::StepLimitExceeded), which must be TrapCode's final enumerator
```

A misplaced row is the one `meta::firstMisplacedRow()` finds, the C++23 search every table's check
uses. Reflection only names its row and enumerator ("row 0 names WriteError::PageSizeNotWritable,
where WriteError::TextTooLarge belongs").

### Discovery

The check compiles one generated umbrella header, which includes every `.hpp` of the header sets of
`isoline_core`, `isoline_text` and `isoline_cli` (an X-macro `.def` is left out), and every other
`.hpp` under their sources: the private headers, but not `testing/` or `meta/reflection/`. A table in a
new header is therefore reached without a row. Over those headers it asserts:

- every namespace-scope `std::array` whose row's first member is a scoped enum, anywhere under
  `isoline`, is a row of `EnumTables.def`. Discovery walks every namespace but a `testing` one, at any
  depth, and `isoline::meta`, which holds the checks;
- every `KEYED` row, and every `ENUM` row keyed by its row's first member, is such a table, so a stale
  row fails. `cli::FeatureChoices` and `text::KnownAnnotations` are indexed by a later member, which
  discovery does not look for;
- every namespace-scope `Last` constant (a variable named `Last…` of a scoped enum type) is the `Last`
  of a row. Where no reachable table names one, an `ISOLINE_LAST_ENUMERATOR` row checks only that it is
  its enum's final enumerator: appending an enumerator to `NameShape` without moving `LastNameShape`
  fails;
- every namespace that discovery walks, at any depth, is listed in `DiscoveredNamespaces`, or is
  anonymous or named `detail` (its parent's). A new namespace, such as `isoline::probe` or
  `isoline::runtime::probe`, fails until it has its row.

`DiscoveredNamespaces` is `isoline` and `isoline::api`, `binary`, `c`, `cli`, `config`, `interpreter`,
`runtime`, `text` and `validate`. The C API's private headers reach the check through its static library,
where the C API is built (`ISOLINE_C_API`), and are left out where it is not.

### What reflection cannot reach

A table in a source file, in its anonymous namespace or a function, is out of reflection's reach. Each
enum-first one is an `ISOLINE_SOURCE_LOCAL_TABLE("file", Table, "reason")` row, which keeps the table's
own C++23 check and says why it stays in its file. The lint test `enum-tables-current`
(`scripts/check-enum-tables.py`, on every compiler) scans every source file but the tests for table
declarations (`std::to_array<Row>` and `std::array<Row, N>`), and fails on an enum-first one that has no
row, on a row whose file declares no such table, and when this list differs from the rows:

<!-- source-local-tables:begin -->
- `c/detail/Convert.cpp`: `TypeRefusalRows`, indexed by TypeRefusal (c/detail/Convert.hpp); its rows hold the messages only the file's typeRefusalError() reads
- `binary/Encoder.cpp`: `SectionWriters`, keyed by SectionAnchor; its rows name ModuleEncoder's member functions
- `binary/NameSection.cpp`: `ShapeCodecs`, indexed by NameShape; its rows name the file's codec functions
- `interpreter/Numerics.cpp`: `NumericTable`, keyed by Opcode, a subset; its rows name the file's evaluators
- `interpreter/Simd.cpp`: `SimdTable`, keyed by Opcode, a subset; its rows hold the file's evaluators
- `runtime/Store.cpp`: `Usages`, indexed by Limit; local to Store::used(), whose accessors its rows name
- `text/Annotations.cpp`: `AnnotationReaders`, one reader per AnnotationKind, no row struct; it names the file's readers
- `text/Annotations.cpp`: `MultipleNameErrors`, one TextErrorCode per NameShape, no row struct; read by the file's name reader alone
- `text/InstructionReader.cpp`: `ConstantRows`, keyed by ImmediateKind, a subset; its rows name the file's literal parsers
- `validate/Validator.cpp`: `ConstantTable`, keyed by Opcode, a subset: the constant instructions, read by the validator alone
- `validate/Validator.cpp`: `LaneTable`, keyed by Opcode, a subset: the lane instructions, read by the validator alone
<!-- source-local-tables:end -->

`interpreter/Simd.cpp`'s `Evaluator` is a variant with one alternative per `SimdShape`, not a table;
`LastSimdShape` has an `ISOLINE_LAST_ENUMERATOR` row instead, as do the other `Last` constants no
reachable table checks.

The tables of `runtime/ValueCheck.cpp` and `cli/Verbs.cpp` are plain data, so they moved into the
private headers `runtime/ValueCheckTables.hpp` and `cli/VerbsTables.hpp`, where discovery reaches them.

### What stays a review question

- **`SIZED` and `PREFIX` tables have no row struct,** so discovery cannot find one: a sized table that
  is missing its row goes unnoticed. Its own C++23 size check still holds.
- **A `KEYED` table's relation is not checked.** Reflection checks that the table is registered and that
  its row still starts with a scoped enum, not that its rows hold the relation the reason names.

## The consumer floor

Consumers move to C++26 only once GCC, Clang, Apple Clang and MSVC all ship reflection and contracts in
a released version (owner, 2026-11-27). One row per compiler names the first such release:

| Compiler | First release with reflection and contracts |
|---|---|
| GNU | 16.2 |
| Clang | — |
| AppleClang | — |
| MSVC | — |

Until no row is empty, configure prints "consumer floor: pending" with the missing compilers, and the
cache entry `ISOLINE_CONSUMER_FLOOR_REACHED` is `OFF`. Once none is empty, it is `ON`, and the switches
can run: annotations become the tables' source, preconditions move into `pre`,
and the fallback is deleted.

## Presets

| Preset | Options | Use |
|---|---|---|
| `gcc-cxx26` | all three `ON` | the native lane: configure fails unless every feature goes native |
| `gcc-fallback` | all three `OFF` | the fallback on the same compiler, the local stand-in for a GCC 14 lane |

Each has a build and a test preset. `gcc-debug` keeps `AUTO`, so on GCC 16.2 it goes native.

## Measured facts (GCC 16.2.1, Clang 22.1.8, 2026-11-27)

- `-std=c++26 -freflection` provides everything the reflection probe uses. `#include <meta>` compiles
  without `-freflection`, and `__cpp_annotations` is undefined although annotations work. The probes
  therefore compile an expression, and never test a macro or a header.
- An annotation value must be structural. A `std::string_view` member is rejected; a character array
  works.
- Under `-std=c++23`, GCC rejects `[[=x]]` with `-Werror=c++26-extensions` and Clang rejects it as a
  syntax error. The header check relies on both.
- `-fcontracts` needs `-std=c++26`. Without `-fcontracts` on the link line, or without `-lstdc++exp`,
  the link fails with an undefined `handle_contract_violation`. The probe passes the flags to the
  compile only, as a target does, so it fails the same way when the library is missing.
- `isoline_enum_tables_check` compiles in 2.7 s and 547 MB on GCC 16.2.1, against 1.4 s and 364 MB to
  parse its umbrella header (70 headers) as C++23: reflection about doubles the cost of one translation
  unit, which only native lanes compile.
- Clang 22.1.8 accepts neither `-freflection` nor `-fcontracts`.
- CMake 4.3 knows no C++26 mode of MSVC 19.51, and `try_compile` refuses to generate a project that
  requires one. A probe whose standard is missing from `CMAKE_CXX_COMPILE_FEATURES` therefore fails
  without compiling, and the configure goes on.
