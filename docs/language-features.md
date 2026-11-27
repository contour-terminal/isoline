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

- **Contracts native:** `isoline_core` compiles as C++26 with the contracts row's flags, then the pinned
  semantic's flag as the last PRIVATE option `isoline_apply_language_features()` adds, and links
  `stdc++exp` PRIVATE. A static library's PRIVATE link items reach consumers as `$<LINK_ONLY:…>`, so an
  installed package links the library without compiling with its flags. The target's compile feature
  stays `cxx_std_23`, so consumers and the tests compile as C++23. That is deliberate: contracts are on
  wherever supported (owner, Q1), installed builds included, and a C++26 `isoline_core` is what lets the
  header check fail on its own.
- **Reflection and annotations native:** they only add check translation units of their own.

## Contracts

### The seam

Implementation files spell a precondition through two macros of `<isoline/meta/Check.hpp>`:

- `ISOLINE_PRECONDITION(cond)` for an entry check: among the function's first statements, reading only
  its parameters and `*this`. These are the checks that later move into `pre`.
- `ISOLINE_INVARIANT(cond)` for any other check.

`<isoline/meta/Check.hpp>` is a one-line forwarder that CMake generates into
`${PROJECT_BINARY_DIR}/private`, a PRIVATE include directory of every target
`isoline_apply_language_features()` sees and outside every `FILE_SET`, so it is never installed. It
includes `meta/contracts/Check.hpp` where contracts are native, and `meta/fallback/contracts/Check.hpp`
otherwise. No source tests for the feature. Both headers bring `Require()` too, for the sites a file
keeps.

- **Native:** `do { switch (0) default: { static_assert(std::meta::is_noexcept(std::meta::access_context::current().scope()), …); contract_assert(cond); } } while (false)`.
  The macros compile only in a `noexcept` function. A site in a function that may throw keeps
  `Require()`, and the build names it. The `switch (0) default:` is not decoration: without it GCC
  16.2.1 drops the check from some positions (see the measured facts below).
- **Fallback:** `Require(cond)`, exactly as before.

Contracts are confined to `noexcept` functions because a program has one violation handler, and an
embedder may install one that throws. Inside a `noexcept` function its exception terminates where the
check fails, as `Require()` aborts. Anywhere else the caller could catch it and continue past a violated
precondition. So can a `try` block, or a function-try-block, around the check inside the `noexcept`
function itself: the macros never go inside one, and the C boundary's catch-all handlers in particular
never hold one. The macros never appear in a header: an inline function compiled under two semantics keeps
whichever copy the linker picks.

`scripts/check-contract-spelling.py` (tests `contract-spelling` and `contract-spelling-selftest`, label
`lint`) refuses a raw `contract_assert` in a `.cpp` under `src/isoline`: one that bypasses the macros
also bypasses their `noexcept` proof. Its rules are rows of `RULES`.

### The semantic is `enforce`

The contract semantic is pinned, never an option. Each semantic is a row per compiler
(`ISOLINE_CONTRACT_SEMANTIC_ROWS`):

| Semantic | Verdict | Why |
|---|---|---|
| `enforce` | selected | reports the condition, then terminates |
| `quick_enforce` | not offered | terminates without a report, so no canary can tell which check fired |
| `observe` | refused | continues past a violated precondition |
| `ignore` | refused | does not evaluate the check |

Selecting any semantic but `enforce` is a configure error naming its reason. The flag of every row that
is not selected is refused, and so are the refused-flag rows (`ISOLINE_CONTRACT_REFUSED_FLAG_ROWS`):
`-fcontracts-definition-check=off`, which silences a definition's `pre`, and `-fno-contracts`.

The flag `isoline_apply_language_features()` adds is not last on the command line: linked INTERFACE
options and a later `target_compile_options()` follow it. Two layers refuse what would override it.

**The configure scan** gives early feedback: `isoline_refuse_contract_overrides()`, deferred to the end
of the top-level directory, so that it runs after the whole project has configured, Isoline as a
subproject included. For every target compiled with native contracts, with the compiler and rows its
own directory sees (a parent need not be a C++ project), it reads:

- `CMAKE_CXX_FLAGS` and `CMAKE_CXX_FLAGS_<CONFIG>`, as the target's directory sees them, so an
  intermediate directory's flags count;
- the target's `COMPILE_OPTIONS`, which hold its directories' options from its creation on, and its
  `COMPILE_FLAGS`, and both of its sources;
- the `INTERFACE_COMPILE_OPTIONS` of every target in its link closure (`LINK_LIBRARIES`, then
  `INTERFACE_LINK_LIBRARIES` and `INTERFACE_LINK_LIBRARIES_DIRECT`, generator expressions read to any
  depth), a parent project's `core::base` included.

Generator expressions are matched as text, so a refused flag counts under any condition. A refused flag
fails the configure, naming where it was found and why it is refused, and so does a target whose compiler
no contract row names:

```text
[isoline] INTERFACE_COMPILE_OPTIONS of core::base (fixture_core_base), which isoline_core links:
-fcontract-evaluation-semantic=ignore is refused, since it does not evaluate the check. Isoline's
contracts are checked under the enforce semantic only (docs/language-features.md).
```

**The build-time check** is authoritative. A native-contracts build exports the compile commands of
Isoline's targets, and every such target depends on `isoline_contract_flags_check`, which runs
`cmake/IsolineContractFlags.cmake` over `compile_commands.json` before anything is compiled. It refuses
every translation unit under `src/` compiled with a refused flag, as the generator wrote the command:
flags built by generator expressions, `add_definitions()`, the contents of `@response` files, and GCC
specs files included. A specs file (`-specs=` or `--specs=`, on the command line or in a response file)
can add any flag to the compiler proper's command line, and measured on GCC 16.2.1 one that adds
`-fcontract-evaluation-semantic=ignore` silently replaces `enforce`, so the check reads it as the driver
does: a bare name is found through `<compiler> -print-file-name=<name>`, then in the entry's directory,
and the files it names by `%include` are read too (a missing `%include_noerr` excepted, as the driver
skips it). Specs files are not refused outright: distributions' default flags use them (Fedora's
`redhat-hardened-cc1`, for one). A response or specs file it cannot read is an error, and every one it read
is a dependency of the check, so editing one reruns it. The check is linear in the size of `compile_commands.json` and runs in constant
stack: it holds escapes, and then the braces inside strings, in placeholders (control characters, which
valid JSON never holds unescaped), cuts the document at the `},{` that remain, and parses each entry
alone; no regular expression it uses recurses per character. 5,000 entries take it about 0.15 s, and a
1 MB command line about 0.05 s. `contract-flags-selftest` (label `lint`) holds both under 2 s, and pins
braces in strings, the `arguments` form (50,000 arguments), paths through `..`, a 100 KB string, 40,000
escapes, a trailing escaped backslash, a flag the shell unquotes, a raw control character, and a
refused flag or a response file's `@` spelled through `\uXXXX` escapes, which the JSON decodes: the
pre-filter that skips entries with neither a refused flag nor an `@` parses every entry holding `\u`.
Eight cases pin the specs files: an absolute one, `--specs=`, one relative to the entry's directory, a
bare name the compiler finds, one a specs file includes, an optional include that is missing, a missing
one, and one a response file names. It holds 33 cases.

The check reads the command line, not the compiler: a translation unit cannot prove its own semantic.
Measured on GCC 16.2.1, a contract that fails during constant evaluation is a hard error under `enforce`
and `quick_enforce` even where substitution failure or tentative constant initialization would otherwise
absorb it (a requires-expression, a default template argument, a non-`constexpr` `const` initializer),
a warning under `observe`, and unchecked under `ignore`; `__builtin_constant_p` folds it under every
semantic, and a predicate's side effects are discarded in constant evaluation. No probe therefore
compiles under `enforce` and fails under `observe` without `-Werror`. A native-contracts configure with a generator that writes no compile commands (anything but
Ninja and the Makefile generators) is refused. The test `contract-flags-current` (label `lint`) runs the
same check on the build tree. What reaches the compiler past the generated command line, a compiler
launcher, an override of `CMAKE_CXX_COMPILE_OBJECT`, a `-wrapper` program (which runs the compiler proper
and may change its arguments) or a `-fplugin=` plugin (which runs inside it), is the embedder's own
toolchain, and outside what a build can audit.

The fixture `tests/cmake/contract-override` is a parent project with its own core-cpp. The tests
`contract-override-<case>` (label `lint`, on native lanes) configure it once per override, then run the
build-time check: an INTERFACE option of its `core::base`, one of a library `core::base` links under a
generator expression, under nested ones, or as `INTERFACE_LINK_LIBRARIES_DIRECT`,
`CMAKE_CXX_FLAGS_DEBUG`, a `target_compile_options()` on `isoline_core` after Isoline has configured, its
`COMPILE_FLAGS`, an option of one of its sources, an intermediate directory's `CMAKE_CXX_FLAGS`, a parent
that is a C project, `add_definitions()`, a flag a generator expression builds, a response file that is
missing, a specs file that adds the semantic, and one response file edited after a check passed. Each must
fail naming its source, the last five at the build-time check. `contract-override-none` overrides nothing and must
pass both.

### Two report channels

Where contracts are native, a contract site that fails reports through the compiler's violation handler
(GCC's default prints to standard error), then terminates. Every `Require()` still reports through
core-cpp's `core::setFailHandler()`. An embedder's fail handler therefore sees no contract site on
GCC 16.2 and later. Both channels end the process. `docs/embedding.md` says the same.

### The canary

`tests/canary/RequireCanaryCases.def` names each case's spelling, `contract` or `require`, measured by
running it natively. Report rows (`ISOLINE_CONTRACT_REPORT_ROWS`) give, per compiler, path and spelling,
what the test expects around the check's condition:

| Compiler | Path | Spelling | Report |
|---|---|---|---|
| any | fallback | any | `Precondition failed. <cond>` |
| GNU | native | require | `Precondition failed. <cond>` |
| GNU | native | contract | `contract violation in function <signature> at <file>:<line>: <cond>`, then `[assertion_kind: assert, semantic: enforce, mode: predicate_false, terminating: yes]` |

Where contracts are native, `isoline_require_canary_throwing_handler` runs each `contract` case again,
with a violation handler that prints `throwing-handler: <cond>` and throws (`tests/canary/ThrowingHandler.cpp`).
The canary's `main()` catches what reaches it and reports that the check let the call through, which
fails the test. It passes only if no exception leaves the failed check: every contract site in a
`noexcept` function, and none inside a `try` block or function-try-block that catches it there.

The last seven cases pin the positions a check must be evaluated in (`tests/canary/CheckPositions.cpp`):
`ISOLINE_PRECONDITION` as a function's first statement and as a braceless `if`'s body, and
`ISOLINE_INVARIANT` as a braceless `else`'s body and as the first statement of a `for`, `while`, `do`
and range-based `for` body.

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

### Opcode names

Every compiler checks `Opcodes.def`'s names, in `binary/Opcode.cpp`: an instruction's enumerator is its
text in PascalCase, each part between a `.` or a `_` capitalized and the separators dropped
(`i64.extend_i32_s` is `I64ExtendI32S`), which `binary::namedByRule()` decides. The one exception is a
row of `OpcodeNameExceptions`, with its reason: `SelectTyped`, whose text `select_t` names it after the
binary grammar's `select t*`, as typed select shares `select`'s text. A name that follows neither, an
exception row for a name the rule already holds for, and a row no instruction uses each fail the build.
The checks (`firstMisnamedOpcode()`, `firstUnusedException()`) live beside the rule, over spans, and are
tested on fixtures of their own. A C++23 `static_assert` cannot compute its message, so the failure names
the offender by index alone, in the comparison the compiler prints (`the comparison reduces to
'(96 == 471)'`): an opcode's index is its row of `Opcodes.def`, from zero.

`OpcodeNames` and `OpcodeTexts` (`binary/OpcodeNames.hpp`, generated from `Opcodes.def`) are `SIZED`
rows indexed by `Opcode`. An enumerator added to the enum by hand, after `Opcodes.def`'s `#include`,
builds on every C++23 compiler; reflection names it, from `OpcodeTable`'s row and from these. That each
enumerator's identifier is its `OpcodeNames` entry cannot differ while both come from `Opcodes.def`; the
reflection switch checks it once the enum is no longer generated.

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

A table in a source file (at namespace scope, in an anonymous namespace, in a function or a class), or
in a header's function or class, is out of reflection's reach. Each enum-first one is an
`ISOLINE_SOURCE_LOCAL_TABLE("file", Table, "reason")` row, which keeps the table's own C++23 check and
says why it stays where it is. The lint test `enum-tables-current` (`scripts/check-enum-tables.py`)
reads Clang's JSON AST of every first-party source file of the build tree, from its
`compile_commands.json`, and finds every variable whose type is `std::array<Row, N>` after sugar:
`std::to_array` with or without its template argument, class template argument deduction, east or west
`const`, a macro or a helper that builds it. A table is enum-first when Row's first field, read from
the class's declaration, is a scoped enum. The lint fails on an enum-first table that has no row, on a
row whose file declares no such variable, and when this list differs from the rows. The dump holds
Isoline's namespaces alone, so the lint also fails on every `std::array` or `std::to_array` a source
file spells outside `namespace isoline` (in `main()`, a global anonymous namespace or another
namespace), read from the code with comments, literals and preprocessor directives left out. A
function defined by its qualified name under isoline (`int isoline::b::f() {…}`) counts as inside. The
pass sees spellings, so a macro invoked outside the namespace, an alias such as `using Arr =
std::array<…>` used outside it, and an unqualified `array` (`using namespace std`) escape it; a brace it
cannot attribute to isoline, such as a constructor's member initializers, fails closed. It runs on Clang
trees (the other compilers build the same sources, linted on CI's Clang lanes), in parallel, and caches
each translation unit's result in the build tree, keyed by the content of the file, the headers, the
Clang binary and the script rather than their time stamps. `check-enum-tables-selftest` lints a fixture of every
declaration form with the tree's Clang.

<!-- source-local-tables:begin -->
- `c/detail/ErrorRows.hpp`: `Rows`, EnumErrorRows<Enum>'s member, one per error enumeration the C API reports; rowsAreComplete() checks each
- `c/detail/Convert.cpp`: `TypeRefusalRows`, indexed by TypeRefusal (c/detail/Convert.hpp); its rows hold the messages only the file's typeRefusalError() reads
- `binary/Encoder.cpp`: `SectionWriters`, keyed by SectionAnchor; its rows name ModuleEncoder's member functions
- `binary/NameSection.cpp`: `ShapeCodecs`, indexed by NameShape; its rows name the file's codec functions
- `interpreter/Numerics.cpp`: `NumericTable`, keyed by Opcode, a subset; its rows name the file's evaluators
- `interpreter/Numerics.cpp`: `NumericByOpcode`, NumericTable indexed by Opcode for lookup, built from it by a lambda; other opcodes' rows are empty
- `interpreter/Numerics.cpp`: `table`, the lambda's working copy that becomes NumericByOpcode
- `interpreter/Simd.cpp`: `SimdTable`, keyed by Opcode, a subset; its rows hold the file's evaluators
- `runtime/Store.cpp`: `claims`, keyed by Limit, a subset in Limit order; local to Store::checkInstanceDemand(), its rows hold the demand computed per call
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

- **A library table in any `testing` namespace, or any `testing/` directory, is never checked:**
  discovery skips them as test data, and the lint skips them as test helpers.
- **An `ISOLINE_LAST_ENUMERATOR` row checks only its own enum:** that `Last` is that enum's final
  enumerator. It cannot tell which table, if any, the enum indexes. An `ENUM` row's `Last` must have the
  type of its key member, which `firstMisplacedRow()`'s signature enforces, so the two cannot name
  different enums.
- **`SIZED` and `PREFIX` tables have no row struct,** so discovery cannot find one: a sized table that
  is missing its row goes unnoticed. Its own C++23 size check still holds.
- **A `KEYED` table's relation is not checked.** Reflection checks that the table is registered and that
  its row still starts with a scoped enum, not that its rows hold the relation the reason names.

## The consumer side of a native package

A package whose contracts are native asks more of its consumer than a fallback one, because
`isoline_core` links `libstdc++exp`, the GCC library that holds the contract violation handler:

- **A consumer compiles with GCC 16.2 or later, at the release that built the package.** `libstdc++exp`
  is a static archive whose ABI GCC does not keep across releases, so a package built by GCC 16.2 is
  linked by a GCC 16.2 toolchain, and a GCC 16.3 one is a rebuild.
- **A libc++ consumer cannot link it,** nor an MSVC STL one: neither standard library has
  `libstdc++exp` or GCC's violation handler. Clang over libstdc++ is no exception, as the package records
  GCC alone.
- **CMake consumers get it from the package.** The installed `isolineConfig.cmake` records the contracts
  path and the compiler (`isoline_CONTRACTS_PATH`, `isoline_CXX_COMPILER_ID`,
  `isoline_CXX_COMPILER_VERSION`), and `find_package(isoline)` fails at the consumer's configure, naming
  both compilers, where the consumer's is another or another major.minor release, or C++ is not enabled
  (`cmake/isolineConsumerToolchain.cmake`; `install-consumer-toolchain`, label `install`, pins its
  cases). A static `isoline::core` carries `$<LINK_ONLY:stdc++exp>`, so the link needs nothing more.
- **A consumer without CMake adds `-lstdc++exp`** to its link line, after `-lisoline_core`, and compiles
  with the same GCC release.
- **An embedder may define `handle_contract_violation`** to report a failed check its own way
  (`docs/embedding.md`, "Failed checks"). It must not throw: every contract Isoline compiles sits in a
  `noexcept` function.

None of the native flags reach a consumer: they are build options (`isoline_apply_build_options()`),
wrapped in `$<BUILD_INTERFACE:…>` and recorded, so `install-forbidden-options` and check-install's leak
check prove that no exported file holds `-freflection`, `-fcontracts` or the semantic. A package built on
the fallback asks nothing of its consumer beyond C++23.

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
- GCC 16.2.1 does not evaluate a `contract_assert` that is the first statement of a `for`, `while` or
  `do` body, or the whole body of a braceless one (a range-based `for` and a later statement are fine): `do { contract_assert(false); } while (0)` returns, and so does a loop whose body starts
  with one. Wrapped in `if (true) { … } else …`, it is dropped there too. Behind
  `switch (0) default: { … }` it is evaluated in every position measured: a function's first statement,
  the first statement of a `for`, `while`, `do` and range-based `for` body, and a braceless `if` or
  `else`. The native macros are written that way, and the canary's position cases fail otherwise.
- The last `-fcontract-evaluation-semantic=` on the command line wins.
- A violation handler that throws, in a `noexcept` function, terminates there. In a function that may
  throw, the caller can catch the exception and continue.
- CMake 4.3 knows no C++26 mode of MSVC 19.51, and `try_compile` refuses to generate a project that
  requires one. A probe whose standard is missing from `CMAKE_CXX_COMPILE_FEATURES` therefore fails
  without compiling, and the configure goes on.
