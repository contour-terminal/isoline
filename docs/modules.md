# C++ modules

Isoline is a header library first: every public name is declared in a header under `src/isoline`, and
the build, the tests and every internal TU include those headers. With `ISOLINE_MODULES` on, the build
also offers module `isoline`, whose interface unit (`src/isoline/isoline.cppm`) declares nothing of its
own: it re-exports, by `export using`, every public name the headers declare. The headers stay the one
source of truth, and `scripts/check-module-exports.py` keeps the unit in step with them.

## Using the module

Turn the module on when configuring Isoline, with CMake 4.3 or newer and the Ninja generator (a Visual
Studio generator builds it in-tree too):

```sh
cmake -B build -G Ninja -DISOLINE_MODULES=ON
```

Each module preset is an ordinary preset with the module on: `clang-modules` (Clang, Debug, ASan and
UBSan, clang-tidy), `clang-modules-release`, `gcc-modules`, `gcc-cxx26-modules` (GCC 16.2's C++26 reflection
and contracts, [docs/language-features.md](language-features.md)), `msvc-modules` (from a Visual Studio Developer
Command Prompt), `msvc-modules-visual-studio` (the same, under the Visual Studio generator) and, from a
Linux host, `msvc-wine-modules` (MSVC under wine, [docs/cross-compiling.md](cross-compiling.md)).

A toolchain that cannot build modules is refused at configure time, with the reason, rather than left to
fail later. The supported compilers are Clang (the GNU front end, targeting the GNU ABI), GCC and MSVC;
Apple Clang, clang-cl and Clang targeting the MSVC ABI are refused.

A target of the same build imports the module by linking its target, `isoline::core_module`, and being
scanned for module dependencies:

```cmake
add_executable(app main.cpp)
target_link_libraries(app PRIVATE isoline::core_module)
set_target_properties(app PROPERTIES CXX_SCAN_FOR_MODULES ON)
```

```cpp
#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>

import isoline;

auto const engine = isoline::api::Engine { isoline::api::EngineConfig {} };
```

`isoline::core_module` links `isoline::core`, the library itself, so a target that imports the module
links the same code as one that includes the headers. `CXX_SCAN_FOR_MODULES` is on by default in a
project whose policies are those of CMake 3.28 or newer (CMP0155); Isoline's own policies leave it off,
so that only its module consumers are scanned.

## Toolchains

The floors are the lowest versions a CI lane builds and tests, not the lowest that CMake documents
(`cmake/IsolineModuleSupport.cmake` holds them as rows, and refuses what is below):

| Tool | Floor | Notes |
|---|---|---|
| CMake | 4.3 | |
| Ninja | 1.11.1 | Ubuntu 24.04's; the other lanes run 1.13.2 |
| Clang (GNU front end and ABI) | 19.1.0 | see below |
| GCC | 15.2 | |
| MSVC | 19.44 (Visual Studio 17.14) | |

Apple Clang, clang-cl and Clang targeting the MSVC ABI are refused, each with its reason: CMake 4.3 has
no module dependency scanning for the first two, and the third is untested.

The generators are Ninja, Ninja Multi-Config and Visual Studio (in-tree only, see below). Of Ninja
Multi-Config the header package's install is measured, but no lane consumes its installed modules: that
row is supported as CMake documents it, and unmeasured.

CMake documents Clang 18.1.2 for `import std;`, but no Clang 18 builds Isoline's installed package:
core-cpp, which a packager installs first, deduces the template arguments of an alias template, which
Clang implements from 19. With libstdc++ (13 or 14, as on Ubuntu 24.04) Clang 18 has no `std::expected`
either: libstdc++'s `<expected>` requires `__cpp_concepts` 202002, which Clang defines from 19. The
floor is therefore Clang 19.1.0, LLVM's release archive, measured with Ubuntu 24.04's libstdc++ 14.

The module lanes of `.github/workflows/build.yml`, one matrix row per toolchain:

| Lane | Row | Compiler | Standard library | Build | Tests | `import std;` |
|---|---|---|---|---|---|---|
| `linux-clang-modules` | `clang-22-asan` | Clang 22 | libstdc++ 14 | Debug, ASan, clang-tidy | modules, lint, install | not expected |
| `linux-clang-modules` | `clang-22-libc++-install` | Clang 22 | libc++ 22 | Release | all | expected |
| `linux-clang-modules` | `clang-floor` | Clang 19.1.0, Ninja 1.11.1 | libstdc++ 14 | Release | all | not expected |
| `linux-gcc-modules` | `gcc-modules` | GCC 15.2 | libstdc++ 15 | Debug | all | expected |
| `linux-gcc-cxx26` | `gcc-cxx26-modules` | GCC 16.2, C++26 `isoline_core` | libstdc++ 16 | Debug | all | expected |
| `windows-msvc-modules` | `msvc-modules` | MSVC 19.44 | MSVC STL | Debug, Ninja and Visual Studio 2026 | modules, lint, install | expected (Ninja) |

Two rows run only the tests modules add (the labels `modules`, `lint` and `install`): the rest of the
suite runs on the same compiler in the ASan job and the Windows MSVC job.

Every Release and Debug row without a sanitizer runs the install tests (`ctest -L install`), which build
and install core-cpp once (`install-core-cpp`, their shared setup fixture) and then one consumer each. The
Windows lane also consumes the installed package under a real Visual Studio generator: a consumer that
includes the headers builds and runs, and the config refuses the module to one that imports it. The
`macos-clang` lane, which builds the headers with Apple Clang, runs `module-scanning-tripwire`, which
passes while CMake refuses to scan modules for Apple Clang.

## Consuming the installed module

With `ISOLINE_MODULES` on, `cmake --install` installs the module beside the library, in the same
component: `isoline::core_module` is in component `core`, as `isoline::core` is. It installs as source,
the interface unit under `lib/cmake/isoline/modules`, and never as a built module interface (BMI): a
BMI is only valid for the compiler and flags that built it, so CMake builds one in the consumer, from
the installed unit, with the consumer's own flags. None of Isoline's development flags (`-Werror`, the
sanitizers) is exported with it. The package names no path of the build that made it, so a prefix may
be moved or packaged after `cmake --install`.

### Installing it

The package's one dependency, [core-cpp](https://github.com/contour-terminal/core-cpp), is not installed
with it: install it first, into the same prefix, built by the same compiler and standard library as the
consumer (and as Isoline), since the three link into one program. From Isoline's source tree, with the
options its own install test uses:

```sh
cmake -S vendor/core-cpp -B build/core-cpp -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=<the consumer's compiler> \
      -DCORE_CPP_INSTALL=ON -DCORE_CPP_TESTING=OFF -DCORE_CPP_CATCH2_MAIN=OFF \
      -DCORE_CPP_BUILD_EXAMPLES=OFF -DCORE_CPP_FETCH_DEPS=OFF \
      -DCORE_CPP_WITH_TUI=OFF -DCORE_CPP_WITH_TUI_OUTPUT=OFF -DCORE_CPP_WITH_TUI_CORE=OFF \
      -DCORE_CPP_WITH_TLS=OFF -DCORE_CPP_WITH_SYSTEMD=OFF
cmake --build build/core-cpp
cmake --install build/core-cpp --prefix <prefix> --component core-cpp

cmake -S . -B build/isoline -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=<the consumer's compiler> -DISOLINE_MODULES=ON
cmake --build build/isoline
cmake --install build/isoline --prefix <prefix> --component isoline
```

A consumer then names the prefix, and nothing else:

```sh
cmake -S app -B app/build -G Ninja -DCMAKE_PREFIX_PATH=<prefix>
```

### Importing it

A consumer needs CMake 4.3 or newer and a Ninja generator, and imports the module as an in-tree target
does:

```cmake
cmake_minimum_required(VERSION 4.3)
project(app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

find_package(isoline 0.1 REQUIRED COMPONENTS core)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE isoline::core_module)
```

With a policy version of 3.28 or newer, as here, CMake scans `main.cpp` for its imports by itself.

CMake builds the module's BMI in a target of its own, with `CMAKE_CXX_FLAGS` and the module's exported
options, but not with the options of the target that imports it, nor of its directory. A flag that
changes the compilation environment must therefore be in `CMAKE_CXX_FLAGS`, or the importer and the BMI
disagree: MSVC refuses `import isoline;` from a TU compiled with `/utf-8` against a BMI compiled without
it (warning C5050, an error under `/WX`). Warning flags may stay on the target.

### Visual Studio generators

A Visual Studio generator can neither install module information nor build a BMI for an imported
module target (cmake-cxxmodules(7)). So:

- Isoline built with a Visual Studio generator installs the library and its headers, but not the
  module, and its configure says so.
- A consumer configured with a Visual Studio generator may include the installed headers, but a target
  of it that links `isoline::core_module` fails its configure:

  ```text
  [isoline] installed C++ modules need a Ninja generator (CMake cannot build BMIs for imported targets under Visual Studio)
  ```

  Use a Ninja generator for it (in the Visual Studio IDE, a CMake preset with `"generator": "Ninja"`).
- Within Isoline's own build, a Visual Studio generator builds and imports the module as usual.

## `import std;`

The interface unit never imports `std`, so whether to `import std;` is the consumer's choice. Isoline
works either way: an importer may include the standard headers it spells, or replace them all with
`import std;`.

```cpp
import std;
import isoline;
```

CMake 4.3 supports `import std;` as an experimental feature, behind a gate that each CMake release names
with its own UUID. A consumer turns it on before `project()`, and then on its target:

```cmake
cmake_minimum_required(VERSION 4.3)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b") # CMake 4.3's
project(app LANGUAGES CXX)
# ... as above, then:
set_target_properties(app PROPERTIES CXX_MODULE_STD ON)
```

It needs a toolchain that ships a std module, which `CMAKE_CXX_COMPILER_IMPORT_STD` lists by standard
(`23` for C++23): GCC 15 and newer with its libstdc++, Clang with a libstdc++ or libc++ that ships one
(not the libstdc++ 13 or 14 of Ubuntu 24.04), and MSVC with a Ninja generator. The installed-consumer
test `install-import-std-consumer` skips, naming the reason, where the toolchain lacks one, and fails
instead where the lane says it has one (`ISOLINE_EXPECT_IMPORT_STD`). CMake 4.3 lists `23` for a Clang
whose libstdc++ has no std module all the same (it takes the bare name Clang's `-print-file-name` prints
for a missing `libstdc++.modules.json` as the file), and then fails to generate; the consumer treats a
`CMAKE_CXX_STDLIB_MODULES_JSON` that is no file as no std module.

Setting the gate makes CMake print a `CMake Warning (dev)` that the feature is experimental. The test
tolerates that one warning, and only in that mode (E3 below).

CMake builds the standard library's own std module sources in the consumer, in a target of its own that
takes `CMAKE_CXX_FLAGS`. Clang warns there that `'std' is a reserved name for a module`, on libstdc++'s
`bits/std.cc` or libc++'s `std.cppm` (E4 below). Keep `-Werror` out of `CMAKE_CXX_FLAGS` when you
`import std;` with Clang: in `CMAKE_CXX_FLAGS` it reaches that target, and the warning in the
toolchain's own file breaks the build. Put warning flags on your own targets instead.

## `import isoline;` exports no standard names

The interface unit's global module fragment includes Isoline's headers, and with them the standard
headers they use, but a module exports only what its purview names, and the purview names Isoline's
names alone. An importer therefore includes (or `import std;`s) every standard header whose names it
spells itself: `std::array`, `std::span`, `std::expected` and the rest are not visible through
`import isoline;`, even where an Isoline signature uses them.

The types an Isoline signature names are reachable all the same: an importer may call a function that
returns `std::expected<Instance, InstantiateError>` and use the result without spelling the type, but
naming `std::expected` needs `<expected>`.

## GCC needs `<memory>` before the import

GCC instantiates `isoline::api::makeHostFunction()` in the importer, and that instantiation reaches
`std::shared_ptr`'s use of `typeid`. GCC refuses it unless `<typeinfo>` is visible in the importer:
`<memory>`, `<typeinfo>` or `import std;` before `import isoline;` satisfies it. Measured on GCC 16, an
importer with only `<expected>` and `<span>` fails with

```text
shared_ptr_base.h:710: must '#include <typeinfo>' before using 'typeid'
```

Clang and MSVC do not need it. Include `<memory>` in every importer that calls `makeHostFunction()`, so
that the TU builds with all three.

## Including and importing together

Every declaration of the interface unit's global module fragment stays attached to the global module,
so the entities a header declares and those the module exports are the same ones, under the ordinary
one-definition rule. One program may hold TUs that include Isoline's headers and TUs that import the
module, and pass values of Isoline's types between them; `FeatureTable`, an inline variable, has one
address in all of them.

One TU may also do both. The supported order is every Isoline `#include` first, then `import isoline;`.
Both orders conform to the standard, but the other one is measured rather than promised:

| Compiler | include, then import | import, then include |
|---|---|---|
| Clang 19.1.0 | works | fails: type alias template redefinition with different types |
| Clang 22 | works | works |
| GCC 15.2 | works | fails: redefinition of `std::__byte_operand<bool>` |
| GCC 16 | works | fails: redefinition of `std::__is_constant_evaluated` |
| MSVC 19.44 | works | fails: C2011, type redefinition of `isoline::Feature` |
| MSVC 19.51 | works | fails: C4348 (redefinition of a default template argument) under `/WX` |

Each failure is the header redefining what the module's global module fragment already holds; the first
entity a compiler names differs between versions.

The test `module-import-then-include` (label `modules`) builds an import-then-include TU and holds the
outcome to its compiler's row of `ISOLINE_MODULE_IMPORT_THEN_INCLUDE` (`tests/modules/CMakeLists.txt`),
the row of its compiler with the highest lowest version not above its own: a row expects success, or a
failure whose English diagnostics match the row's regular expression. GCC's
row matches `error: redefinition of '`, whatever entity follows; MSVC's matches
`(error|warning) C(2011|4348):`, the two codes its versions report. A compiler that starts accepting the
order turns it red, which is the reminder to update this table.

## Adding a module

Module `isoline` covers `isoline_core`. Each later public library gets a module of its own, by the
recipe below. `tests/modules/recipe` follows it with a fixture library, `isoline_recipe_fixture`, whose
module `isoline.recipe_fixture` re-exports `isoline`, and the tests prove each step.

1. **The library** declares its public headers in a `FILE_SET HEADERS` whose `BASE_DIRS` is `src`, the
   headers under `src/isoline/<library>/`, so that they install under `include/isoline/<library>/` and
   are included as `<isoline/<library>/....hpp>` (the fixture's, from its own directory:
   `isoline/recipe_fixture/RecipeFixture.hpp`). The set is the module's surface, and the module covers
   nothing else: every `.hpp` beside one of the set must be in the set too, which the lint checks. The
   library names its consumers' standard in a PUBLIC `target_compile_features(... cxx_std_23)`, which the
   lint parses the headers in. Its sources stay header-based (`CXX_SCAN_FOR_MODULES OFF`), as
   `isoline_core`'s do.
2. **The row**, one `isoline_add_module()` call in the library's `CMakeLists.txt`:

   ```cmake
   isoline_add_module(
       NAME isoline.recipe_fixture       # what consumers import
       LIBRARY isoline_recipe_fixture    # its HEADER_SET is the surface
       SOURCE isoline.recipe_fixture.cppm
       NAMESPACES isoline::recipe_fixture
       IMPORTS isoline)
   ```

   - `NAMESPACES` are public: the lint exports every name declared directly in them.
     `INTERNAL_NAMESPACES` are not, nor is any namespace named `detail` or `testing`. A namespace beneath a
     public one that is neither public nor internal fails the lint.
   - `IMPORTS` names the modules the unit re-exports with `export import`, and links their module targets
     PUBLIC, so that an importer of the new module names their types too. A library built on
     `isoline_core` imports `isoline`: its signatures name `isoline::api` types, and an importer must be
     able to spell them.
   - `FOREIGN_NAMES` lists names of another library that the unit re-exports, qualified
     (`core::async::Task`). Any other name outside the module's namespaces fails the lint.
3. **The interface unit** is generated, never written by hand: `cmake --build <build> --target
   isoline_write_module_exports` runs the lint's `--write` for every row, and the result is committed. A new
   row's unit must exist before its first configure: start it as the single line
   `export module <name>;`, configure, then generate it.
4. **The lint**, the test `module-exports-<name>` (label `lint`), checks the committed unit against the
   headers. Every public name must be exported and none stale, the `export import` lines must equal
   `IMPORTS`, no macro may be left defined, no conditional directive may read a compiler's macro
   (`__GNUC__`, `__clang__`, `_MSC_VER`, …: a header reads the same on every compiler), nor a macro's
   definition name one, no declaration may be of internal linkage, no public header may be missing from
   the set, and the unit must precompile, the modules it imports first. A header changed without `--write` fails it, naming the name.
   `--list [--kinds <kind>...]` prints the public names it reads, each with the declaration kinds that
   declare it; where reflection is native, the test
   `meta-module-surface.reflection` compares them with GCC's reading of the same headers
   ([docs/language-features.md](language-features.md), "The reflected module surface").
5. **The install row**, one row of `ISOLINE_INSTALL_LIBRARIES` (`cmake/IsolineInstallRows.cmake`): the
   library, its export name (a consumer's `isoline::<name>` and `COMPONENTS <name>`), its export set
   `isoline<Name>Targets`, its install component, and whether a plain `cmake --install` leaves it out. The
   module target installs beside the library, in the row's export set and install component, so the
   module row names neither. What the component needs loaded first is derived from what its library and
   module target link (the row's `IMPORTS` among them) and installed beside its targets file, so
   `isolineConfig.cmake`, which names no component, needs no edit. The fixture's row is
   `isoline_recipe_fixture`, `recipe_fixture`, `isolineRecipeFixtureTargets`, `isoline-test-fixture`
   (left out of a plain install): `install-import-fixture-consumer` installs that component, asks
   `find_package(isoline COMPONENTS recipe_fixture)` for it alone, and runs `import isoline.recipe_fixture;`.
6. **The importers.** An in-tree target imports the module through `isoline_add_module_consumer(<target>
   <module>)`, which also lifts the clang-tidy checks that misfire on an importer of a module that
   re-exports another (E2's `import-reexporting`), read from the row. The installed-consumer test gets the
   module as a row of `ISOLINE_CONSUMER_USES` (`tests/consumer/ConsumerModes.cmake`): the module, the
   package component that holds it and its target, and a mode that uses it.

The modules planned, one row and one install row per public library, with no other edit to the module
machinery:

| Module | Namespace | Library | `IMPORTS` | Note |
|---|---|---|---|---|
| `isoline.text` | `isoline::text` | the text format (`isoline::text`, installed already) | `isoline` | its library is installed as component `text`; the module row is still to come |
| `isoline.cache` | `isoline::cache` | the ahead-of-time compilation cache (P8) | `isoline` | |
| `isoline.component` | `isoline::component` | the component model (P11, P12) | `isoline` | |
| `isoline.wasi` | `isoline::wasi` | WASI 0.3 (P14) | to be decided, likely `isoline.component` | its binding headers are generated from WIT, so `--write` regenerates the purview after each regeneration of them |

The C API (P17) stays C: `wasm.h` and `isoline.h` are for other languages' bindings, and get no module.

## The documented exceptions

Isoline builds warning-free under `-Werror` and `/W4 /WX`, its module targets and the installed
consumer included. Four exceptions are declared where they apply, each with its measured reason, and
each has a tripwire test that fails once its cause is gone:

- **E1 (Clang 22 and newer):** `-Wno-error=unused-command-line-argument` on the module targets, as a
  `$<BUILD_INTERFACE:...>` option that is never exported. CMake 4.3 builds an importer's BMI with
  `clang++ --precompile ... -c`, and Clang reports the `-c` as unused. The warning still prints, so
  the cause stays visible; the installed-consumer tests of a mode that imports tolerate exactly that
  diagnostic, by its text, however often it appears. Tripwire: `cmake-precompile-tripwire`. Clang 19.1.0
  accepts the `-c` silently, so E1 starts at the lowest version measured to warn, 22; the versions between
  are unmeasured, and one that warns fails its own `-Werror` build.
- **E2:** clang-tidy's checks that misfire on module sources are lifted on exactly the targets of their
  kind (`ISOLINE_MODULE_TIDY_FALSE_POSITIVES` in `cmake/IsolineModules.cmake`), and every other check
  still runs there. Tripwire: `clang-tidy-module-tripwire`.
- **E3:** CMake's dev warning that `import std;` is experimental, tolerated in the configure of the
  installed consumer's `import-std` mode and nowhere else. That test requires the warning: once CMake
  no longer prints it, it fails, saying that the gate table and E3 can go.
- **E4 (Clang):** Clang's `'std' is a reserved name for a module` (`-Wreserved-module-identifier`) on
  the standard library's own std module sources, which CMake builds in the consumer for `import std;`:
  libstdc++'s `bits/std.cc` and `bits/std.compat.cc`, libc++'s `std.cppm` and `std.compat.cppm`. The
  files are the toolchain's, not Isoline's or the consumer's. The installed consumer's `import-std` mode
  tolerates exactly that diagnostic, on the files of the standard library in use, in that mode only;
  built by Clang, it requires it, so the test fails once Clang no longer prints it.
