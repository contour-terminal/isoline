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
row matches `error: redefinition of '`, whatever entity follows; MSVC's matches `C(2011|4348)`, the two
codes its versions report. A compiler that starts accepting the order turns it red, which is the
reminder to update this table.
