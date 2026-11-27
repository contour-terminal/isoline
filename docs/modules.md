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
instead where the lane says it has one (`ISOLINE_EXPECT_IMPORT_STD`).

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
| Clang 22 | works | works |
| GCC 15.2 | works | fails: redefinition of `std::__byte_operand<bool>` |
| GCC 16 | works | fails: redefinition of `std::__is_constant_evaluated` |
| MSVC 19.44 | works | fails: C2011, type redefinition of `isoline::Feature` |
| MSVC 19.51 | works | fails: C4348 (redefinition of a default template argument) under `/WX` |

Each failure is the header redefining what the module's global module fragment already holds; the first
entity a compiler names differs between versions.

The test `module-import-then-include` (label `modules`) builds an import-then-include TU and holds the
outcome to its compiler's row of `ISOLINE_MODULE_IMPORT_THEN_INCLUDE` (`tests/modules/CMakeLists.txt`):
a row expects success, or a failure whose English diagnostics match the row's regular expression. GCC's
row matches `error: redefinition of '`, whatever entity follows; MSVC's matches
`(error|warning) C(2011|4348):`, the two codes its versions report. A compiler that starts accepting the
order turns it red, which is the reminder to update this table.

## The documented exceptions

Isoline builds warning-free under `-Werror` and `/W4 /WX`, its module targets and the installed
consumer included. Four exceptions are declared where they apply, each with its measured reason, and
each has a tripwire test that fails once its cause is gone:

- **E1 (Clang):** `-Wno-error=unused-command-line-argument` on the module targets, as a
  `$<BUILD_INTERFACE:...>` option that is never exported. CMake 4.3 builds an importer's BMI with
  `clang++ --precompile ... -c`, and Clang reports the `-c` as unused. The warning still prints, so
  the cause stays visible; the installed-consumer tests of a mode that imports tolerate exactly that
  diagnostic, by its text, however often it appears. Tripwire: `cmake-precompile-tripwire`.
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
