# C++26 language features

Isoline is a C++23 library. Where a compiler supports C++26 reflection (P2996), annotations (P3394) and
contracts (P2900), Isoline uses them. Reflection adds compile-time checks of its hand-maintained tables,
and contracts carry preconditions. Every other compiler builds the same library through a C++23
fallback. The installed headers parse as C++23 on every compiler once their declaration contracts
(`ISOLINE_PRE`, `ISOLINE_POST`) expand to nothing, as they do on the fallback path. Consumers of a fallback
package compile as C++23; consumers of a native-contracts package compile as C++26.

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

- **Contracts native:** `isoline_core`, `isoline_text`, `isoline_component` and `isoline_component_text`, the
  libraries whose checks are spelled through the seam, compile as C++26 with the contracts row's flags, then
  the pinned semantic's flag as the last PRIVATE option `isoline_apply_language_features()` adds, and links
  `stdc++exp` PRIVATE. A static library's PRIVATE link items reach consumers as `$<LINK_ONLY:…>`. Its
  headers' declarations then carry contracts (`ISOLINE_PRE`, `ISOLINE_POST`; "Declaration contracts"
  below), so its interface asks every consumer, the tests and an installed package's included, for the
  contract-consumer row's standard: `cxx_std_26`, which enables contracts on GCC 16.2 without a flag. No
  flag and no semantic is asked. On the fallback path the compile feature stays `cxx_std_23`, and consumers
  compile as C++23. The C API's two libraries, `isoline_c_shared` and `isoline_c_static`, compile with the
  same flags, but their headers are C: a library whose header set holds headers, none of them a `.hpp`, asks
  its consumers for no C++ standard (their interface keeps `c_std_11`), which `tests/c/CMakeLists.txt` checks
  at configure through the transitive link interface. The link still asks what it must: a static
  `isoline_c_static` passes `$<LINK_ONLY:stdc++exp>` on, as `isoline_core` does, while the shared library
  holds it inside.
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

The AST lint `enum-tables-current` (`scripts/check-enum-tables.py`, below) enforces the try rule from the
same dump of every source file: it refuses `ISOLINE_PRECONDITION` and `ISOLINE_INVARIANT` invoked inside a
try block or a function-try-block, a constructor's included, and inside a handler of a try that is itself
inside one. A lambda (or a local class's member function) is a function of its own: a macro in a
`noexcept` lambda defined inside a try block is allowed, because the lambda's own `noexcept` boundary
ends the process before any handler of the enclosing function is reached, and the native macro proves
the lambda `noexcept`. Two native canaries pin both halves: the case `invariant-in-noexcept-lambda-in-try`
fails its check inside such a lambda and must end the process, its throwing-handler twin included, and
the test `contract-lambda-may-throw-refused` builds `tests/canary/LambdaMayThrow.cpp`, the same lambda
without `noexcept`, and passes only on the macro's `static_assert`. A try block inside the lambda is
refused like any other. The lint finds a macro
by the name at its invocation, so a macro invoked through another macro is not seen.

`scripts/check-contract-spelling.py` (tests `contract-spelling` and `contract-spelling-selftest`, label
`lint`) checks the spelling. Its rules are rows of `RULES`, and its fixtures are the cases of
`scripts/testdata/contract-spelling/`:

| Files | Refused | Why |
|---|---|---|
| `src/isoline/**/*.cpp` | a raw `contract_assert` | it bypasses the macros, and their `noexcept` proof |
| `src/isoline/**/*.{hpp,def}`, installed or private, except the two seam headers | `ISOLINE_PRECONDITION`, `ISOLINE_INVARIANT`, `contract_assert`, and a `pre`/`post` specifier after a declarator and any qualifiers, ref-qualifiers, `override`, `final` or trailing return type (`) pre(`, `) const & post(`, `) -> T pre(`, `) override pre(`; not a call such as `loop.post(`, nor `ISOLINE_PRE(`) | a contract in a header is `ISOLINE_PRE` or `ISOLINE_POST` on a declaration, whose expansion is fixed per package |
| `src/**/*.{cpp,def,h}`, `tests/**/*.{cpp,hpp,h,def}` but the header's fixtures, and the seam's headers (`meta/reflection/`, `meta/contracts/`, `meta/fallback/`) | `ISOLINE_PRE`, `ISOLINE_POST` (rule `declaration-contract-unproven`) | the noexcept proof does not read them |
| `src/isoline/**/*.hpp` | `ISOLINE_PRE`/`ISOLINE_POST` on a declaration that does not end with `;` after them (a body, member initializers, `= default`), or that is `inline`, `constexpr`, `consteval` or a template's, or that sits inside a class template's braces at any depth (rule `declaration-contract-inline`) | its consumers would compile the check under their own semantic |
| installed headers (every `FILE_SET HEADERS` of `src/isoline/**/CMakeLists.txt`) | `#include <isoline/meta/…>` other than `EnumRows.hpp` | an installed header includes only `meta/EnumRows.hpp` |
| `src/isoline/**/*.cpp`, tests and `testing/` directories left out | a `Require(` or `Guarantee(` beyond the file's throwing-site rows, fewer than they count, or a row whose file is gone or no longer defines its function (the name before a parameter list a definition's tail follows, not a call) | use `ISOLINE_PRECONDITION`/`ISOLINE_INVARIANT` in a `noexcept` function, or add a row |

Comments are never code, and literals are blanked for every pattern but an `#include`. On success the lint
prints how many `Require()`s the installed headers keep (30, the component layer's among them). They are
inline header code, where a contract would be evaluated under the consumer's semantic, so they stay `Require()`.

### Declaration contracts

The owner's ruling of 2026-11-27 brings declaration contracts forward from Task 16: where contracts are
native, a precondition or postcondition of a `noexcept` member function may sit on its first declaration,
in a header, installed ones included. It is spelled through two macros of `<isoline/Contracts.hpp>`:

- `ISOLINE_PRE(cond)` expands to `pre(cond)`.
- `ISOLINE_POST(result, cond)` expands to `post(result: cond)`.

Both expand to nothing on the fallback path.

**One expansion per installed package.** D3 refused header macros because a consumer's translation unit
would decide their expansion, and first declarations would differ between units (ill-formed, no
diagnostic required). `isoline/Contracts.hpp` is therefore generated once per build, into
`${PROJECT_BINARY_DIR}/include`, and installed with `isoline_core`'s headers. It is a copy of
`src/isoline/meta/contracts/Contracts.hpp.in` where contracts are native, and of
`src/isoline/meta/fallback/contracts/Contracts.hpp.in` otherwise. It records the mode as
`ISOLINE_DECLARATION_CONTRACTS`, `1` or `0`. Nothing a consumer defines or compiles with changes the
expansion, and no installed header tests a compiler's macro (the export lint's `compilerConditional` rule
holds).

- **A native package refuses a consumer it cannot be read by.** The native header starts with
  `#if !defined(__cpp_contracts)` and an `#error`. That is a feature-test macro, not a compiler's: it asks
  whether this unit can parse the declarations at all, and no expansion depends on it. `isolineConfig.cmake`'s refusal of another compiler is the CMake
  twin (see "The consumer side of a native package").
- **A fallback package asks nothing.** Its expansion is empty in every unit, so a consumer compiled with
  contracts reads the same declarations as one without. The refusal is one-directional for that reason, as
  the CMake twin is.

The contracts-consumer tests (label `unit`) compile `tests/contracts-consumer/Consumer.cpp` alone, at a row's
standard and flags, never linked to `isoline::core`. On a native build, a unit compiled as C++23 or with
`-fno-contracts` must stop at the `#error`, and one compiled as the package asks (C++26) must compile. On a
fallback build, a C++23 unit and, on GCC, a C++26 unit with `-fcontracts` must compile. Each also checks `ISOLINE_DECLARATION_CONTRACTS` against the path CMake selected.

**The rules of the body macros hold, unweakened:**

- **Only on `noexcept` member functions of no class template, proven by reflection.** The reflection
  check `isoline_declaration_contracts_check` (`meta/reflection/DeclarationContractsCheck.cpp`,
  compile-only, where contracts are native; `isoline_add_declaration_contracts_proof()`) reads every
  header of the libraries, installed, private and the test helpers' (`testing/`) alike. It leaves out only
  its own seam's (`ISOLINE_DECLARATION_CONTRACTS_PROOF_EXCLUDES`: `meta/reflection/`, `meta/contracts/`,
  `meta/fallback/contracts/`). The fixture project `tests/cmake/contracts-proof-testing` (test
  `language-features-contracts-proof-reads-testing`) declares a throwing `ISOLINE_PRE` in a `testing/`
  header, and its proof must fail to build naming it. It reads them through the *proving expansion* of `<isoline/Contracts.hpp>`
  (`src/isoline/meta/reflection/ContractsProof.hpp.in`, written to `${PROJECT_BINARY_DIR}/contracts-proof`
  and put first on the check's include path). There each macro carries a first `pre` whose condition
  instantiates `isoline::meta::detail::declarationContractOnNoexcept<std::meta::is_noexcept(std::meta::access_context::current().scope())>()`,
  whose `static_assert` names every declaration contract on a function that may throw. A member function's
  contract sees the function as its scope. A free function's sees its namespace, which is no function, so
  it is refused as well (measured). The compile-fail tests `compile-fail-declaration-contract-may-throw` and
  `compile-fail-declaration-contract-on-free-function` pin both through the same expansion. The same `pre`
  refuses a member of a class template's specialization
  (`std::meta::has_template_arguments(std::meta::parent_of(scope))`), whose contract its consumers would
  compile. GCC 16.2 evaluates a dependent contract only when the member function is instantiated, not
  when the class is (measured: an explicit instantiation of the class does not reach it), so the lint's
  `declaration-contract-inline` rule refuses every declaration contract inside a class template's braces
  by its text too. The installed expansion stays `pre(cond)` alone. The test `contracts-expansion` (label
  `lint`) preprocesses `tests/contracts-consumer/Expansion.cpp` through both headers, the proof emptied,
  and fails unless both expand `ISOLINE_PRE`, `ISOLINE_POST` and `ISOLINE_DECLARATION_CONTRACTS` alike. Its proof would need `-freflection` in every consumer, which changes
  how libstdc++'s headers preprocess and breaks an `import std;` consumer of module isoline (measured).
  The check never meets the installed expansion: nothing links it. The lint's
  `declaration-contract-unproven` rule refuses the macros in every file the check does not read: every
  source file, the seam's headers, and everything under `tests/` but the header's own fixtures.
- **The body check stays.** A declaration contract is an addition, not a replacement, until Task 16. The
  functions keep their `ISOLINE_PRECONDITION`, which the fallback path checks. A postcondition is new and
  has no body check, so on the fallback path it checks nothing.
- **Never on host misuse reachable from C.** That stays an error value (the cross-plan notes): the C API
  checks it first. The lint's rule `contract-in-c-api` refuses every contract spelling (`ISOLINE_PRECONDITION`,
  `ISOLINE_INVARIANT`, `ISOLINE_PRE`, `ISOLINE_POST`) under `src/isoline/c/`.
- **`enforce` only, and never the consumer's semantic.** GCC 16.2 evaluates a non-inline function's
  contract in the callee (measured: a caller compiled with `-fcontract-evaluation-semantic=ignore` still
  terminates). A declaration contract therefore sits only on a function that an implementation file defines,
  which Isoline compiles under the pinned semantic. The lint's `declaration-contract-inline` rule refuses one
  on a header's definition, or on an `inline`, `constexpr`, `consteval` or template declaration. That is
  also why the consumer row carries no semantic.

**Mixed packages do not link.** Each header declares
`isoline::meta::detail::declaration_contracts_<mode>::declarationContractsMode()` in an inline namespace
named after `ISOLINE_DECLARATION_CONTRACTS`, and beside it an inline variable that references it (a
`[[gnu::used]]` constant address where the compiler has the attribute, so a consumer's `-Wglobal-constructors`
has nothing to report; an initializer that calls it elsewhere, MSVC), so every
translation unit that sees a declaration contract, through any Isoline header that includes
`<isoline/Contracts.hpp>`, references its mode's symbol, whether or not it uses anything else of the header
(`src/isoline/Contracts.cpp` defines the library's). A consumer compiled against a fallback package's headers
and linked with a native library, or two such objects in one program, fails to link. A shared library linked
without `-z defs` (`-Wl,--no-undefined`) leaves the symbol unresolved until it is loaded, and then fails at
load time instead. The test `contracts-mode-mismatch` (native lanes) builds
`tests/contracts-consumer/Mismatch.cpp`, which reads nothing of the header, against the fallback copy, and
passes only on the linker's undefined `declaration_contracts_0` (GNU ld's "undefined reference", lld's and
mold's "undefined symbol"). The namespace is under `isoline::meta`, which discovery skips and module isoline
keeps internal.

**The conversions,** each with a canary case (below): the four `noexcept` accessors behind installed
declarations that Task 6 named, `Engine::Engine` (`ISOLINE_PRE(config.features.checkPrerequisites().has_value())`),
`StoreLink::owner()`, `Function::type()` and `Tag::type()`. Thirteen canary cases reach them, every one
that reaches `owner()` included. A postcondition bounds the decoder's offsets:
`ByteReader::offset()`'s `ISOLINE_POST(absolute, absolute >= _baseOffset)` says the offset never wraps.
The plan's other candidates are not on this branch or cannot be contracts. P5's `encodedSize` family and
`CodeWords` are Task 10's. The LEB128 writers allocate and are inline. `FixedText`'s sizes are
`constexpr`, and checked at compile time already. `writtenAnchor()` is a free function, and keeps its body
checks.

**The export lint reads the fallback copy.** The lint's Clang has no contracts, so it would stop at the
native header's `#error`. On a native build, the module row's manifest names the fallback copy, written
to `${PROJECT_BINARY_DIR}/modules/substitutes`, as the header's substitute. The lint maps one onto the
other through a Clang VFS overlay (`-ivfsoverlay`), and its text rules read the substitute too. Neither
declares a name; they differ in their macros alone. GCC's reflected surface reads the native header, and
`meta-module-surface.reflection` compares the two readings.
The header leaves its three macros defined on purpose. Module isoline's row lists them as
`DEFINED_MACROS` ([docs/modules.md](modules.md)), because an importer needs none of them.

### Which checks are contracts

A check of an implementation file is a contract when it can be one: in a `noexcept` function, with no
try block around it, its condition only reading. An entry check is `ISOLINE_PRECONDITION`, any other
`ISOLINE_INVARIANT`. Every other check keeps `Require()`, and its function is a throwing-site row of
`scripts/contract-spelling-rows.txt`, `file | function | count | reason`, the reason one of three: it
allocates (`std::bad_alloc` reaches the P17 boundary), it calls a function that may throw, or it is a
throwing interface (not declared `noexcept`, a lambda or a virtual override among them, though what it
calls may not throw). The rows were made by spelling every check as a contract and building the native
lane, whose `static_assert` names each site in a function that may throw.

Fourteen checks, in thirteen functions, are contracts. The entry checks: `Engine::Engine` and `ThreadPreparation`'s constructor
(`api/Engine.cpp`), `StoreLink::owner()`, `Function::type()` and `Tag::type()` (`api/Extern.cpp`),
`Store::define()` (`api/Store.cpp`), `sectionWriterOf()`'s two, that its anchor is neither
`BeforeFirst` nor past `LastSectionAnchor` (`binary/Encoder.cpp`),
`Store::releaseTrailing()` and `InvocationScope`'s constructor (`runtime/Store.cpp`), and
`Natural::bitWidth()` (`text/FloatLiteral.cpp`). The others: `Store::call()`'s refusal naming a mismatch
(`api/Store.cpp`), `shuffle()`'s lane (`interpreter/Simd.cpp`) and `addressOf()`'s value
(`runtime/Instantiate.cpp`). 84 checks in 68 functions keep `Require()`, as do the 15 of the installed
headers. The `noexcept` accessors behind installed declarations, `Engine::Engine`, `StoreLink::owner()`,
`Function::type()` and `Tag::type()`, also carry their entry check on the declaration ("Declaration
contracts", above).

A condition only reads: no check interns, allocates or advances anything, so that evaluating it once,
again, or not at all changes nothing but whether the program ends. The compiler does not hold this. A
contract's predicate sees `*this` and the locals as `const`, but not what a pointer or a reference member
reaches: `contract_assert(_store->internType(type))` compiles on GCC 16.2.1 (measured), and so does a call
of any free function given such a member. The AST lint `enum-tables-current` holds it instead: inside
the expansion of `ISOLINE_PRECONDITION` or `ISOLINE_INVARIANT` it refuses `++` and `--`, an assignment or
compound assignment, a call of a non-const member function (through `.` or `->`, operators included),
and a call of a function with a reference or pointer parameter to non-const. Two writes are not seen,
and stay review questions, asked of every check of the sweep (P19 Task 6):

- a callee that writes through a view it takes by value (a `std::span<T>`);
- a lambda defined before the check whose `operator()` is `const` (the default) but writes through a
  by-reference capture: `auto const bump = [&count] { return ++count > 0; };` and then
  `ISOLINE_INVARIANT(bump())`. The condition calls a `const` member, and the write sits in the lambda's
  body, outside the macro's expansion. GCC 16.2.1 compiles the same in a `contract_assert` (measured).
  The lint's self-test pins this gap (`test_namedLambdaWritingThroughACaptureIsAKnownGap`).

The one condition that interned a type now interns it into a local first (`Instantiation::internTypes()`).

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

Where contracts are native, a contract site that fails, a declaration's `pre` or `post` included, reports through the compiler's violation handler
(GCC's default prints to standard error), then terminates. Every `Require()` still reports through
core-cpp's `core::setFailHandler()`. An embedder's fail handler therefore sees no contract site on
GCC 16.2 and later. Both channels end the process. `docs/embedding.md` says the same.

### The canary

`tests/canary/RequireCanaryCases.def` names each case's spelling, measured by running it natively. It is
the check that fires first there: `require`, `contract`, or `pre` or `post` for a declaration contract.
Report rows (`ISOLINE_CONTRACT_REPORT_ROWS`) give, per compiler, path and spelling, what the test expects
around the check's condition:

| Compiler | Path | Spelling | Report |
|---|---|---|---|
| any | fallback | any | `Precondition failed. <cond>` |
| GNU | native | require | `Precondition failed. <cond>` |
| GNU | native | contract | `contract violation in function <signature> at <file>:<line>: <cond>`, then `[assertion_kind: assert, semantic: enforce, mode: predicate_false, terminating: yes]` |
| GNU | native | pre, post | the same, with `assertion_kind: pre` or `post` |

A `pre` case is a death test on both paths. Natively the declaration's contract reports. On the fallback
path the declaration contract is a no-op, and the body check it keeps reports `Precondition failed.`, so
the case's check regex matches both spellings of the condition (`_?config[.]features…`). A `post` case
(`byte-reader-offset-wraps`) has no body check. Unchecked rows (`ISOLINE_CONTRACT_UNCHECKED_ROWS`:
`*|fallback|post`) name where a spelling compiles to no check. There the case passes only on the canary's
"the check let the call through" line, and a report of its check fails it: on `gcc-fallback` the
postcondition must be a no-op.

Where contracts are native, `isoline_require_canary_throwing_handler` runs each `contract`, `pre` and `post` case again,
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
property `ISOLINE_LANGUAGE_FEATURE_TARGETS`. Today those are `isoline_core`, `isoline_text` and the C API's two
libraries (whose C headers the check, which reads `.hpp` files, leaves to `tests/c/`); another library is one
call.
After all of them are defined, the top-level `CMakeLists.txt` adds one check over all of them,
`isoline_public_headers_cxx23`, and, where contracts are native, a second one, `isoline_public_headers_cxx26`.
Adding the check seals the registry: a target passed to
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
C++26. It also fails when a header does not include what it uses. Where contracts are native, the
recorded libraries' interface asks every consumer for C++26 (see "Declaration contracts"), but not
`isoline_public_headers_cxx23`, which sets `ISOLINE_HEADERS_AS_CXX23` (the interface's compile feature is
`$<BUILD_INTERFACE:$<$<NOT:$<BOOL:$<TARGET_PROPERTY:ISOLINE_HEADERS_AS_CXX23>>>:cxx_std_26>>`, and
`cxx_std_26` installed). It still compiles as C++23 there, reading `<isoline/Contracts.hpp>` as its fallback
copy, first on its include path, so every native lane also proves that the installed headers are C++23 once
the two macros expand to nothing. Each of its stubs refuses `__cpp_contracts` with an `#error`, so a
C++26 requirement reaching it fails it on GCC (`__cplusplus` cannot say it: MSVC's C++23 mode reports a
later value). `isoline_public_headers_cxx26` compiles the same stubs as the consumers
do, through the installed expansion.

## What reflection checks

Reflection only verifies. The tables, `src/isoline/meta/EnumTables.def` and `Opcodes.def` stay the
source on every compiler, and no shipped value is computed by reflection, so the object code is the
same on both paths. Where reflection is native, two targets exist that the fallback does not have
(`isoline_add_reflection_check()`, built with the tests):

- `isoline_meta_reflection_test` (label `unit`) tests the checks on fixtures of its own.
- `isoline_enum_tables_check` and `isoline_component_enum_tables_check` are compile-only: their
  `static_assert`s are the check, and the build fails when one does. They are one source,
  `meta/reflection/EnumTablesCheck.cpp`, built once per scope: a row of `src/isoline/CMakeLists.txt`'s
  `isolineEnumTablesChecks` names the target, its scope header (the discovery root, its namespaces and its
  registry: `CoreEnumTablesScope.hpp` against `meta/EnumTables.def`, `ComponentEnumTablesScope.hpp` against
  `component/EnumTables.def`), the libraries whose headers it reads and what it leaves out. A layer is a row.
- `isoline_meta_surface_reflection` prints module isoline's reflected surface, which the lint test
  `meta-module-surface.reflection` compares with the export lint's ("The reflected module surface").
- `isoline_c_api_check`, where the C API is built, is compile-only too: the C API's enum mirrors and struct
  members ("The C API's mirrors and layouts").

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

A table keyed by a name or a spelling, not by an enum, has no row: its check is that no two rows share
the key a lookup reads, which `meta::firstRepeatedKey()` states in C++23 beside the table, and which
reflection could only restate. Exactly these are checked:

- by the key the reader or the command line looks up: the CLI's `Verbs`, `VerbOptions` (long and short
  spellings, `'\0'` meaning none), `FeatureChoices` (by value) and `BinaryFormats` (by preamble); the text
  format's `ValueTypeKeywords`, `HeapTypeKeywords`, `AddressTypeKeywords`, `SectionWords`, `LaneShapes`,
  `CatchKeywords`, `ExternRows` and `TopLevelForms`, and in source files `SideWords` (`text/Annotations.cpp`),
  `SimpleEscapes` (by escape character, `text/Lexer.cpp`), `FieldRows` and `FieldSpaces`
  (`text/ModuleReader.cpp`);
- by the value the writer spells by the first row that holds it: `ValueTypeKeywords` (type),
  `HeapTypeKeywords` (heap type), `CatchKeywords` (catch kind) and `ExternRows` (extern kind).
  `SectionWords` repeats an anchor on purpose (`first` and `last`), so its check is that the first repeat is
  the row after one per `SectionAnchor` but `BeforeFirst`: every anchor's own word comes before the aliases.

The CLI's `Refusals`, indexed by `Refusal`, is an `ENUM` row.

### Discovery

Each check compiles one generated umbrella header, which includes every `.hpp` of the header sets of its
scope's libraries (`isoline_core`, `isoline_text` and `isoline_cli` for the core one, `isoline_component` and
`isoline_component_text` for the component model's; an X-macro `.def` is left out), and every other `.hpp`
under their sources: the private headers, but not `testing/`, `meta/reflection/`, or, for the core one,
`component/`. A table in a new header is therefore reached without a row. Over those headers it asserts,
from its scope's root down (`isoline`, or `isoline::component`):

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
- `component/api/Linker.cpp`: `EntityWords`, indexed by EntityWord; its rows hold the words only the file's linker messages read
- `component/binary/Decoder.cpp`: `CoreReadErrors`, keyed by the core ByteReader's errors; its rows hold the component decoder's error for each, read by the file's reader alone
- `component/binary/Encoder.cpp`: `AbstractHeapCodes`, keyed by AbstractHeapType, a subset: the heaps a decoded core type names; its rows hold the codes only the file's core type writer reads
- `component/binary/Encoder.cpp`: `CoreNumberTypes`, keyed by the core number and vector types, a subset of ValueType; its rows hold the codes only the file's core type writer reads
- `component/canonical/Layout.cpp`: `LayoutRuleTable`, indexed by ValueKind; its rows hold the layout rules only the file's LayoutBuilder reads
- `component/canonical/LiftLower.cpp`: `CoreKindTable`, indexed by CoreValueKind; its rows hold what only the file's flat lifting and lowering read
- `component/canonical/LiftLower.cpp`: `JoinTable`, keyed by a pair of distinct CoreValueKinds; its rows hold the joins only the file's flat lifting and lowering read
- `component/canonical/Strings.cpp`: `SourceForms`, keyed by StringEncoding with the SimpleEncoding it admits; its rows name the forms only the file's transcoder walks
- `component/canonical/Values.cpp`: `BuildStepTable`, indexed by BuildStep; its rows hold what only the file's ValueBuilder reads
- `component/runtime/Calls.cpp`: `CoreValueTypes`, indexed by CoreValueKind, no row struct; read by the file's loweredCoreType() alone
- `component/validate/Names.cpp`: `SemanticVersionErrorTexts`, indexed by SemanticVersionErrorKind; its rows hold the semver crate's messages only the file's name errors read
- `component/validate/Validator.cpp`: `SortLimitTable`, indexed by Sort; its rows hold the limits only the file's validator reads
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

The tables of `runtime/ValueCheck.cpp` and `cli/Verbs.cpp` are plain data, so they moved into headers
of their own, `runtime/ValueCheckTables.hpp` and `cli/VerbsTables.hpp`, where discovery reaches them. The
first is installed with `isoline_core`'s headers, C++23 like each of them, as `binary/OpcodeNames.hpp` and
`meta/FixedText.hpp` are: module isoline's surface is its header set, and `isoline::meta` is one of its
internal namespaces. Nothing under `meta/contracts/`, `meta/fallback/` or `meta/reflection/` is installed.

### The reflected module surface

Module isoline's surface is what its interface unit exports: the public names of `isoline_core`'s header
set, as the export lint (`scripts/check-module-exports.py`, [docs/modules.md](modules.md)) reads them through
Clang's AST. Where reflection is native, a second reading checks the first: `isoline_meta_surface_reflection`
(`meta/reflection/ModuleSurface.cpp`) includes exactly the header set, through an umbrella of its own
(`isoline_add_reflection_check(... PROGRAM ... HEADER_SETS_ONLY)`), and prints `std::meta::members_of` over
each of the row's `NAMESPACES`, the internal ones and any `detail` or `testing` namespace left out, named as
the lint names what it lists (`meta/reflection/ModuleSurface.hpp`; `isoline_meta_reflection_test` checks the
naming on a fixture). The row's namespaces reach it as a generated row file,
`ModuleSurfaceNamespaces.def`, so a namespace made public is compared without an edit here
(`isoline_add_module_surface_reflection()`, `cmake/IsolineModuleSurface.cmake`).

The lint test `meta-module-surface.reflection` (`scripts/compare-module-surface.py`) runs the lint's
`--list` and the program, and fails on every whole name only one of them sees, and on every header only one
of them reads (the program's umbrella against the lint's header set). The two readings are different
compilers' (Clang's and GCC's), so such a name is a declaration only one compiler compiles: a public function
behind `#if defined(__GNUC__) && !defined(__clang__)` is named, as is one behind `#if defined(__clang__)`.
Names are compared, not declarations: an overload that only one compiler sees, of a name both see, is no
difference. `compare-module-surface-selftest` pins the comparison on the lint's fixture module and stand-in
programs, on every lane with the lint.

The test registers where reflection is native, the compiler is not Clang (Clang's reading compared with
itself cannot find a declaration only one compiler compiles), and Python 3 and the export lint's Clang exist
(`isoline_module_surface_decision()`, pinned by `module-surface-selftest`). Elsewhere the configure prints
why it is off. `ISOLINE_EXPECT_MODULE_SURFACE=ON` makes that a configure error instead, so the test cannot
vanish from a lane that expects it: CI's `linux-gcc-cxx26` rows set it, pass the export lint's Clang as the
pinned `llvm-lint` row of `scripts/tools.lock` (`ISOLINE_MODULE_LINT_CLANG`), and check with `ctest -N` that
the test is registered.

`members_of` does not report every kind the lint lists. The row `ISOLINE_MODULE_SURFACE_UNREFLECTED_KINDS`
holds the kinds it was measured not to report: `UsingDecl`, a using-declaration (of a type or a function),
is no member of the namespace (GCC 16.2.1). A name the lint lists only by such a kind is left out of the
comparison, and one the program reflects all the same fails the test, so a compiler that starts reporting
the kind makes the row stale visibly. An unscoped enumeration's enumerators are not members either; the
program reads them through their enumeration, as the lint does.

### The C API's mirrors and layouts

The C layer converts each C enumeration through a table of rows (`src/isoline/c/detail/EnumRows.hpp`,
`ErrorRows.hpp`), and `src/isoline/c/abi/layout.def` describes each struct a caller allocates. C++23 counts a
table's rows against a C++ enumeration's Last, but a C enumeration has no Last, and `offsetof` checks the
fields a row names, not the members it leaves out. `isoline_c_api_check` (`meta/reflection/CApiCheck.cpp`)
reads the C headers as C++ and adds both:

- **Mirrors.** Each `ISOLINE_C_MIRROR(Rows, column, without, aliases, Enums...)` row is one side of a table:
  the C column and the C enumerations whose constants it holds (Isoline's beside wasm.h's, as the value kinds
  are), or the C++ column and its enumeration. The column is a member, or a projection of a row, as the type
  refusals' details are: `isoline_invalid_type_detail_enum` mirrors `EnumErrorRows<api::InvalidTypeReason>`
  through `detailOf()`, the enumerator plus one. `mirrorProblem()` (`meta/reflection/CApiReflection.hpp`)
  runs `enumerators_of` over each: every constant is in exactly one row; every row holds one of the
  constants; two constants of one value fail unless `aliases` lists one of them (`ISOLINE_ERROR_LAST`, the
  only one), which is then in its constant's row; and `without` lists the constants that are in no row on
  purpose, each with its reason (`ISOLINE_HEAP_NOT_REFERENCE`, `ISOLINE_VALKIND_INVALID`,
  `AbstractHeapType::Bot`). A listed constant that gains a row, or is no constant of the side, fails as
  well. A constant added to `val.h` without a row fails the build naming it, as
  `compile-fail-c-mirror-constant-without-row` pins.
- **Every C enumeration is accounted for.** Each mirror row also declares its side, and `cEnumsProblem()`
  walks the global namespace for the enumerations of the C API's prefixes (`wasm_`, `isoline_`): each is
  one a row checks, or a row of `UnmirroredCEnums` with its reason. Two are: `isoline_nullability_enum` and
  `isoline_maximum_kind_enum` have two constants each, which the conversions read through one condition
  rather than a table (`detail/Convert.cpp`), so there are no rows to mirror; `Types_test.cpp` and the limits
  cases pin both. A new C enumeration with neither fails the build naming it.
- **Layouts.** `layoutProblem()` reads every `ISOLINE_LAYOUT_STRUCT`, `ISOLINE_LAYOUT_FIELD` and type row of
  `layout.def`: the field rows of a struct name each of its members, in declaration order, one row each; the
  type rows (`ISOLINE_LAYOUT_FIELD_TYPE`, and `ISOLINE_LAYOUT_UNION_MEMBER_TYPE` for each member of an
  unnamed union, wasm_val_t's `of`) name each member's type as `type_of` sees it; and a row of a struct
  without a struct row fails as well. `compile-fail-c-layout-member-without-row` pins it.

A C++23 build keeps the checks it had, and one more: the row counts of the C++ side, and `Layout_test.cpp`'s
offsets, sizes and, through `std::is_same_v`, each type row. What reflection adds is completeness: a C
constant, a C enumeration or a member without its row.

A mirror checks which constants the rows hold, not which pairs they hold: two rows that swap their C
constants still hold each constant once. Where a pairing is pinned, it is spelled out rather than read from
the rows: the heaps' by the `Expected` table of `Types_test.cpp` (`Types.referenceKindIsItsHierarchys`: each
heap, its kind and its C heap), the error details' by `ErrorDetails_test.cpp`. The extern kinds', the
mutabilities' and the value kinds' tests read the rows they check, so a swap there is a review question
(below).

### What stays a review question

- **A swapped pairing in `ExternKindRows`, `MutabilityRows` or `ValueKindRows`** passes both the mirror and
  their round-trip tests, which read the rows; wasm.h's numbers for them are upstream's, and a change to a
  row is reviewed against it. The heaps and the error details are pinned (above).

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
- **A using-declaration only GCC sees is seen by neither reading of the module surface:** Clang skips it,
  and `members_of` reports no using-declaration. What closes the hole is the export lint's rule
  `compilerConditional`, which refuses every conditional directive of the header set that reads a compiler's
  macro (`__GNUC__`, `__clang__`, `_MSC_VER` and their version macros, `COMPILER_MACROS`), and every macro the
  header set defines whose body names one (`#define ISO_GCC (defined(__GNUC__) && !defined(__clang__))`, read
  by a later `#if ISO_GCC`). What stays open is a condition on a macro defined outside the header set from a
  compiler's (in a dependency's header, such as core-cpp's, or on the command line), which the rule cannot
  tell from any other macro. The other direction, a declaration Clang sees and another compiler does not, is the module
  lanes': the interface unit exports it by a using-declaration, which fails to compile on GCC
  (`linux-gcc-modules`, `gcc-cxx26-modules`) and MSVC (`windows-msvc-modules`).

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
- **The C API's component asks nothing of the toolchain.** A consumer that asks `find_package(isoline)` for
  components with a C interface alone (`c`, whose header set holds no `.hpp`) links the shared library,
  which holds `libstdc++exp` inside it: a C program configures without C++, and any compiler links it
  (`install-c-only-consumer`, a project of C alone).
- **A consumer without CMake adds `-lstdc++exp`** to its link line, after `-lisoline_core`, and compiles
  with the same GCC release. `pkg-config --static isoline-c` names it in `Libs.private` already.
- **An embedder may define `handle_contract_violation`** to report a failed check its own way
  (`docs/embedding.md`, "Failed checks"). It must not throw: every contract Isoline compiles sits in a
  `noexcept` function.

- **A consumer compiles as C++26.** The headers' declarations carry contracts ("Declaration contracts"),
  so `isoline::core` exports the contract-consumer row's standard as its interface
  (`INTERFACE_COMPILE_FEATURES` `cxx_std_26`). GCC 16.2 enables contracts there without a flag. A consumer
  without CMake passes `-std=c++26` itself. A unit compiled without contracts stops at
  `isoline/Contracts.hpp`'s `#error`.

None of the native flags reach a consumer: they are build options (`isoline_apply_build_options()`),
wrapped in `$<BUILD_INTERFACE:…>` and recorded, so `install-forbidden-options` and check-install's leak
check prove that no exported file holds `-freflection`, `-fcontracts` or the semantic. A package built on
the fallback asks nothing of its consumer beyond C++23.

## Annotations, rehearsed

Annotations let an enumerator carry its own table row:

```cpp
enum class RehearsalCode : uint8_t
{
    BadToken[[= FixedText { "bad token" }]],
    Unterminated[[= FixedText { "unterminated string" }]],
};
```

That is the source every enum table moves to once every compiler has annotations (P19 Task 15). Until
then the hand tables stay the source, and the derivation is rehearsed on fixtures only:
`isoline_meta_annotations_test` (label `unit`, `meta/reflection/AnnotatedRows_test.cpp`) exists where
annotations are native, added by `isoline_add_reflection_check(… FEATURE annotations)`; a fallback build
does not have it and compiles the hand tables unchanged.

`meta/reflection/AnnotatedRows.hpp` holds the derivation. An annotation is found by a key, a
reflection: a type's (`^^Weight`, `^^int`) finds the annotations of exactly that type, a class
template's (`^^FixedText`, `^^TextRow`) those of any of its specializations. That is what a real row needs:
Task 15's `TrapText { FixedText<N>, … }` is a class template over the text's length, so every enumerator's
annotation has a type of its own.

- `annotatedRows<Enum, Row, Annotation = ^^FixedText, Project = AnnotationAsRow {}>`: each enumerator's
  one annotation the key finds, projected onto the hand table's row type `Row` by
  `Project(enumerator, annotation)`, in declaration order. The default projection gives a `FixedText`'s
  text and any other annotation as it is.
- `annotatedTexts<Enum>`: `annotatedRows<Enum, std::string_view>`, each enumerator's one `FixedText<N>`
  (any `N`) as text.
- `annotationProblem<Enum, Annotation = ^^FixedText>()`: why some enumerator lacks exactly one annotation
  the key finds, naming it and its count (`Doubled::Twice has 2 FixedText annotations, where each
  enumerator needs exactly one`); empty when none does. Both derivations refuse to compile with that
  message (`tests/compile-fail/`).
- `derivedTableProblem<Enum, Annotation, Project>(table)`: why a hand table (a `std::array`) differs from
  the derived rows: the row counts when they differ, else the first differing row by index and
  enumerator, and for a row struct its first differing column with both values where they can be spelled
  (a text, an enumerator, an unsigned number). A table derived from annotations states its check as
  `static_assert(derivedTableProblem<…>(Table).empty(), derivedTableProblem<…>(Table))`, so the build names
  the row and column.

The rehearsed shape of a real row, which Task 15 follows:

```cpp
template <std::size_t N>
struct TextRow
{
    FixedText<N> text;
    Detail detail;
};

enum class RehearsalTrap : uint8_t
{
    OutOfBounds[[= TextRow { FixedText { "out of bounds memory access" }, Detail::Fatal }]],
    Unreachable[[= TextRow { FixedText { "unreachable" }, Detail::None }]],
};

constexpr auto ToTrapRow = [](RehearsalTrap code, auto const& row) {
    return TrapRow { .code = code, .text = row.text.view(), .detail = row.detail };
};

static_assert(derivedTableProblem<RehearsalTrap, ^^TextRow, ToTrapRow>(TrapRows).empty(),
              derivedTableProblem<RehearsalTrap, ^^TextRow, ToTrapRow>(TrapRows));
// A wrong Detail: "row 1 (RehearsalTrap::Unreachable), column detail: the table has Detail::Fatal,
// its annotation Detail::None"
```

clang-format 22 breaks a long line inside a splice (`[:column:` and `]` on two lines), which no longer
parses; a splice is therefore bound to a short local before a long call.

An annotation's value, like a class-type template argument, must be **structural**: a literal type
whose every base and non-static data member is public, not `mutable`, and itself structural.
`std::string_view` is not (its members are private): GCC 16.2.1 rejects `[[=Text { "a" }]]` with
`annotation does not have structural type` when `Text` holds a `std::string_view`, and refuses `Text` as
a template parameter type with `is not a valid type for a template non-type parameter because it is not
structural` (measured). `isoline::meta::FixedText<N>` (`meta/FixedText.hpp`, C++23, private) is the
structural text: it holds the literal's characters by value in `char data[N]`, its constructor from a
literal is implicit so that `Tag<"x">` converts (and, in constant evaluation, refuses an array without a
terminating NUL, `tests/compile-fail/FixedTextUnterminated.cpp`), and `view()` returns the text without
its terminating NUL. It is plain C++23, tested on every compiler (`FixedText_test.cpp`), since a template
argument needs no annotations.

`isoline_add_reflection_check()`'s `FEATURE` names a row of `ISOLINE_LANGUAGE_FEATURE_ROWS`, looked up
in the rows (a stale cache entry for a misspelt name does not pass); any other name fails the configure
(`language-features-reflection-check-feature`).

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
| `gcc-cxx26-modules` | `gcc-cxx26`'s, `ISOLINE_MODULES` and `ISOLINE_EXPECT_IMPORT_STD` `ON` | module isoline over a C++26, contracts-native `isoline_core` |

Each has a build and a test preset. `gcc-debug` keeps `AUTO`, so on GCC 16.2 it goes native.

## CI

`.github/workflows/build.yml` builds both paths on every push to master, every pull request and every
`workflow_dispatch`:

| Job | Compiler | Preset | Path |
|---|---|---|---|
| `linux-gcc-cxx26` | GCC 16.2.0 (the `gcc:16.2.0` image, pinned by its index digest) | `gcc-cxx26`, `gcc-cxx26-modules` | native, required |
| `linux-gcc` | GCC 14 (Ubuntu 24.04) | `gcc-debug`, `gcc-release` | fallback: "GNU 14.2 is below GNU 16.2" |
| the Clang, Apple Clang, MSVC and GCC 15 modules jobs | their own | their own | fallback |

`linux-gcc-cxx26` is one row of a matrix in the shape of `linux-gcc-modules`: the preset, the compiler and
the release it must report (`compilerVersion`), where CMake and Ninja come from (`pinned`: the release
archives of `scripts/tools.lock`), and the path its features must take (`languageFeatures`). Adding a native
lane means adding a row. The job refuses to go on unless:

- every tool reports its row's version (`scripts/fetch-tools.py --check-version`), the compiler the row's
  release and no snapshot, so a changed image cannot swap it;
- configure succeeds with `-DISOLINE_EXPECT_LANGUAGE_FEATURES=native`: every feature of the rows took the
  native path, which holds the lane even should its preset change. `linux-gcc` passes `fallback`.

`ISOLINE_EXPECT_LANGUAGE_FEATURES` (empty, `native` or `fallback`) compares the path of every feature the
rows define, so a feature added as a row is held to it too; `language-features-selftest` pins its cases.
The build then runs the compile-commands check as a dependency, and the test suite runs the native
canaries, the compile-fail tests, the install tests and everything else. The image is pinned by digest: a
digest that no longer names an image fails the pull, so the job never runs on an image nobody measured.

**Required** means the job has no `continue-on-error`, and a pull request merges only when every job,
this one included, is green. The repository has no branch protection to enforce it: the rule is the
merge-when-green rule every Isoline pull request follows.

### Updating an image pin

A container job names its image by tag and digest, `gcc:16.2.0@sha256:…`, and the digest decides: the tag
is only read by people. The digest is the tag's *index* digest, the multi-architecture manifest list, not
one platform's manifest: the index is what the tag names on every architecture, and the runner pulls its
own platform's image through it. To move a pin to a new image:

```sh
skopeo inspect --raw docker://docker.io/library/gcc:16.2.0 | sha256sum
```

hashes the raw index, which is its digest, and `skopeo inspect` without `--raw` reports the same value as
`Digest`. The per-platform digests the index lists under `manifests`, and the image ID of a pulled image,
are the wrong ones: they name one architecture's image. Replace the tag and the digest on the job's `container:` line in
`.github/workflows/build.yml`, and the `compilerVersion` of its matrix row to the release the image ships.
The same applies to `linux-gcc-modules`' `gcc:15.2.0` pin. A digest whose last character is changed fails
the pull, which is how to check that a pin is in force.

## Measured facts (GCC 16.2.1, Clang 22.1.8, 2026-11-27)

- `-std=c++26 -freflection` provides everything the reflection probe uses. `#include <meta>` compiles
  without `-freflection`, and `__cpp_annotations` is undefined although annotations work. The probes
  therefore compile an expression, and never test a macro or a header.
- An annotation value must be structural. A `std::string_view` member is rejected ("annotation does
  not have structural type"); a character array works (`FixedText`, see "Annotations, rehearsed").
- Under `-std=c++23`, GCC rejects `[[=x]]` with `-Werror=c++26-extensions` and Clang rejects it as a
  syntax error. The header check relies on both.
- `-fcontracts` needs `-std=c++26`. Without `-fcontracts` on the link line, or without `-lstdc++exp`,
  the link fails with an undefined `handle_contract_violation`. The probe passes the flags to the
  compile only, as a target does, so it fails the same way when the library is missing.
- `isoline_enum_tables_check` compiled in 2.7 s and 547 MB on GCC 16.2.1, against 1.4 s and 364 MB to
  parse its umbrella header (70 headers) as C++23: reflection about doubles the cost of one translation
  unit, which only native lanes compile. Reflecting over the component model's headers in the same unit,
  from `isoline` down, ran past 11 GB (refused at an 11 GB address-space cap after 31 s, and killed on CI's
  runners): split into one unit per scope, the core one peaks at 763 MB and the component model's at
  737 MB (GCC 16.2.1, `/usr/bin/time -v`, 2026-11-28).
- Clang 22.1.8 accepts neither `-freflection` nor `-fcontracts`.
- `std::meta::members_of` over a namespace reports its classes, enumerations, variables, functions (each
  overload), aliases, typedefs, concepts, the templates of each, and its nested namespaces. It does not
  report a using-declaration (of a type or a function), a hidden friend, an explicit specialization or an
  unscoped enumeration's enumerators.
- GCC 16.2.1 does not evaluate a `contract_assert` that is the first statement of a `for`, `while` or
  `do` body, or the whole body of a braceless one (a range-based `for` and a later statement are fine): `do { contract_assert(false); } while (0)` returns, and so does a loop whose body starts
  with one. Wrapped in `if (true) { … } else …`, it is dropped there too. Behind
  `switch (0) default: { … }` it is evaluated in every position measured: a function's first statement,
  the first statement of a `for`, `while`, `do` and range-based `for` body, and a braceless `if` or
  `else`. The native macros are written that way, and the canary's position cases fail otherwise.
- The last `-fcontract-evaluation-semantic=` on the command line wins.
- `-std=c++26` defines `__cpp_contracts` (202502) without `-fcontracts`, and `-fno-contracts` undefines it.
  `__cpp_impl_reflection` needs `-freflection`.
- A `pre`/`post` on a non-inline function is evaluated in the callee, under the semantic it was compiled
  with: a caller compiled with `-fcontract-evaluation-semantic=ignore` still terminates.
- In a member function's `pre`, `std::meta::access_context::current().scope()` is the function, the class
  complete there (a later private member may be named). In a free function's it is the enclosing namespace,
  and `std::meta::current_function()` throws ("current scope does not represent a function").
- A module whose global module fragment was compiled with `-freflection`, imported beside `import std;`
  built without it, fails to compile ("conflicting type" in `__mbstate_t.h`): `__cpp_impl_reflection`
  changes what libstdc++'s headers declare. `-std=c++26` and `-std=c++26 -fcontracts` preprocess
  libstdc++ identically, and define the same macros.
- A failed `pre` and `post` report `assertion_kind: pre` and `post`. Their violation's comment is the
  condition's text alone.
- A violation handler that throws, in a `noexcept` function, terminates there. In a function that may
  throw, the caller can catch the exception and continue.
- CMake 4.3 knows no C++26 mode of MSVC 19.51, and `try_compile` refuses to generate a project that
  requires one. A probe whose standard is missing from `CMAKE_CXX_COMPILE_FEATURES` therefore fails
  without compiling, and the configure goes on.
