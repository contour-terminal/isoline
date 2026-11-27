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
- Clang 22.1.8 accepts neither `-freflection` nor `-fcontracts`.
- CMake 4.3 knows no C++26 mode of MSVC 19.51, and `try_compile` refuses to generate a project that
  requires one. A probe whose standard is missing from `CMAKE_CXX_COMPILE_FEATURES` therefore fails
  without compiling, and the configure goes on.
