# C++ Coding Guidelines

These rules are shared across the Contour Terminal projects and were written for Contour, so their
worked examples (`TerminalSession`, `Renderer`, `WrapPending`, …) name types in Contour's code base
(<https://github.com/contour-terminal/contour>). The rules apply to Isoline unchanged.

- Prefer C++23: `constexpr`, `std::ranges`, `std::format`, `std::expected`, structured bindings
- C-style loops are forbidden; use range-based for loops exclusively
- Use `std::views::iota` and other views for generating and transforming ranges
- Use `std::span` for passing arrays and contiguous sequences
- Use `auto` type deduction to improve readability
- Use `const` correctness throughout (refs, pointers, member functions)
- Mark return values `[[nodiscard]]` where ignoring the result would be a bug
- Document new public functions, classes, structs, and their members using Doxygen style:
  ```cpp
  /// Short description of the function (be concise).
  /// @param name Description.
  /// @return Description.
  ```
- **Header files use the `.hpp` extension.** `.h` is reserved for what is genuinely a C header —
  system and third-party headers. Every header carries `#pragma once`, never an include guard.
- Naming conventions and static-analysis rules live in **`./.clang-tidy`**, the single config for
  the whole tree: it sits at the repository root so every first-party directory resolves to it by
  clang-tidy's normal parent-directory search. There are deliberately no module-local overrides —
  one style, one config. It is the authoritative source and wins over any prose here.
- **Types are `CamelCase`, everything else `camelBack`**, with a leading underscore on non-public
  data members. The exceptions are names the standard library binds to *by spelling* — member
  typedefs (`value_type`, `iterator`, `difference_type`, …), nested `iterator` classes, container
  methods (`begin`, `push_back`, …) and `std::formatter`/`std::hash` specializations. Each such
  name carries a `// NOLINT(readability-identifier-naming)` at its declaration.
- C++ code formatting rules are defined in `.clang-format`; run `clang-format` after changes.
- Use smart pointers for ownership; do not use raw owning pointers
- Do not introduce new third-party dependencies without strong justification
- Do not suppress clang-tidy warnings with `NOLINT` comments; fix the underlying issue

## Design Patterns & Principles

Always aim for a clean software architecture. The following principles are load-bearing and
should be adhered to unless there is a very strong, explicitly justified reason not to.

### Error handling: `std::expected<T, E>`
Prefer `std::expected<T, E>` for fallible API surface. Give each subsystem its own error enum,
introduced *as the need arises* — do not invent a taxonomy up
front. Chain monadically with `and_then`, `or_else`, `transform`, `transform_error` rather
than nested `if`s. Reserve exceptions for programmer errors (precondition violation, contract
misuse), not for expected, recoverable failures.

### `enum class` over `bool`
**A `bool` in an API is an anonymous enum whose two values are named after their representation
instead of their meaning.** A `bool` parameter, return type, or data member is a finding unless one
of the exceptions below applies; the replacement is a purpose-named `enum class`. The tree already
has dozens of these and they are the model to copy: `WrapPending`, `HighlightSearchMatches`,
`JumpOver`, `StatusLineStyling`, `PixelReporting`, `ClusterWidthPolicy`, `GlyphWidthPolicy`.

The parameter case carries all three costs at once:

- **The call site loses the meaning.** `Line::toUtf8Trimmed(true, false)` tells a reader nothing,
  and no amount of careful naming *inside* the function repairs the code that calls it.
- **The compiler stops helping.** `bool` accepts pointers, integers and characters through implicit
  conversion, so an overload taking `bool` can quietly swallow an argument meant for another one,
  and two adjacent `bool` parameters can be exchanged without a diagnostic. An `enum class`
  converts from nothing.
- **A third case rewrites every signature.** When yes/no becomes yes/no/inherit, a `bool` forces a
  signature change and an edit at every call site; an `enum class` gains an enumerator and `switch`
  exhaustiveness names the places that must now handle it — the same argument data-driven design
  makes.

`TerminalSession`'s guarded-role API is the case to learn from: `applyPendingFontChange`,
`applyPendingPaste`, `executePendingBufferCapture` and `executeShowHostWritableStatusLine` all take
`(bool allow, bool remember)`, as does `executeRole(GuardedRole, bool, bool)`. The two arguments
are adjacent, identically typed and silently exchangeable, and together they gate a *permission*
decision — swap them at one call site and you grant what should have been remembered. Also
`Terminal::programUDK(bool clearAll, bool locked, …)`, `tabVisualStateFor(bool active,
bool hovered, bool windowActive)`, and `Terminal::refreshRenderBuffer(bool locked = false)`, which
manages to be both a defaulted `bool` parameter and a `bool` return.

**The shape of the fix.**

- **Name the enum after the decision, not after the type.** Domain words beat `Yes`/`No`
  (`ClusterWidthPolicy::FirstCodepoint`); reserve `Yes`/`No` for a type whose own name already
  reads as the question, as `JumpOver::Yes` and `HighlightSearchMatches::No` do.
- **Give it an explicit underlying type** — `: uint8_t`, as nearly every enum in `src/` already
  does. Order the enumerators so the off/absent/default case is zero. We are inconsistent here
  (`WrapPending` is `{Yes, No}`, `HighlightSearchMatches` is `{No, Yes}`); new enums use zero for
  the negative case so a zero-initialized value still means what the `false` meant.
- **Flags that genuinely combine are a bitmask, not a pile of enums.** Where several booleans are
  truly orthogonal and every combination is legal, the answer is one bitmask type — this is why
  `cppcoreguidelines-use-enum-class` is off for `vtbackend::Modifier`, `LockKey` and
  `MatchModes::Flag`, whose values are protocol-defined bit positions.
- **A strong typedef is the other acceptable shape** where a value must stay boolean in behaviour
  but distinct in type — `using Handled = boxed::boxed<bool, HandledTag>;` on `InputHandler`. Reach
  for it when the type name supplies the meaning and the two states have no better names.

**Per position.**

- **Parameters.** A defaulted `bool` is the worst form — it shows neither the name nor the value at
  the call site, so prefer two named functions. Two adjacent `bool` parameters are the next worst,
  being silently exchangeable; fix those signatures first.
- **Returns.** A `bool` return is right when the function name *asks the question*: `empty()`,
  `contains()`, `isLineWrapped()`, `hasLineAt()`, the comparison operators. It is a finding when it
  reports success or failure — `captureScreen()`, `loadConfig()`, `tryAddKey()`, `openUrl()`,
  `runDetached()` are all really `std::expected<void, E>`, which carries the *reason* instead of
  discarding it — or when it selects between two named outcomes, which is an `enum class`.
- **Members.** The same test as parameters, plus one more: two or more `bool` members in a type are
  usually a state machine hiding in flags. Where some combinations cannot legally occur, the states
  are one `enum class`, not a set of independent switches.
- **A surviving `bool` reads as a predicate** — `_isVisible`, not `_visible` — so the use site
  still reads as a question.

**When you cannot.** Each of these must be documented at the declaration, with the reason:

- **The parameter is the property** — a setter that exists only to assign a `bool` member which
  itself passed the test above. The narrow carve-out, not a general licence: if the member should
  have been an `enum class`, so should the setter.
- **A signature you do not own** — a Qt virtual or slot, a standard concept (`std::predicate`,
  comparators), a C callback typedef.
- **Serialization and wire boundaries** — `Config` fields parsed from YAML, protocol flags. Convert
  at the boundary and keep the `enum class` inside it. `Config`'s public `bool` fields are the
  documented exception; do not open a sweep of the config schema on their account.
- **Generic code with no domain meaning** — a `bool` template argument threaded to `if constexpr`,
  as in `Parser.hpp`'s `TraceStateChanges`.
- **QML-facing `Q_INVOKABLE`/`Q_PROPERTY`** — an enum reaches QML only once registered with the
  meta-object system (`Q_ENUM`), and the QML side must then name the enumerators. Worth doing, but
  find the QML callers before promising it.

**Enforcement.** Mostly a review question — *at the call site, can you tell what `true` means
without opening the header?* — because the two checks that would help are off:
`bugprone-easily-swappable-parameters` (adjacent parameters of convertible type) and
`readability-implicit-bool-conversion` (the conversions that let a `bool` overload swallow a
pointer). A clean build therefore says nothing about signatures; enabling either on one module is a
reasonable first move, and what it reports is the finding, not noise. Where a `bool` is
deliberately kept, `bugprone-argument-comment` is enabled via `bugprone-*`, and its
`CommentBoolLiterals` option (off by default) would make `/*asciiHint=*/true` a *checked* comment
rather than a hopeful one — a mitigation, not a substitute for the type. The practice barely exists
today: 27 such comments in `src/`, all but one of them in tests, against ~165 bare literal
arguments in non-test code. Tellingly, `/*allow=*/` and `/*remember=*/` are among the 27 — the
guarded-role call sites already felt the need to annotate what the type should have said.

### Dependency injection
**This is a load-bearing principle, not a nice-to-have.** Anything that
touches I/O, time, randomness, the filesystem, the network, or any other
ambient/global resource is reached through an interface — never through a
concrete type, a singleton, or a free function with hidden state.

### Configuration at construction time
**A constructed object is a usable object.** Everything a class needs to do its job —
collaborators, policy, tuning knobs, limits — is supplied to its constructor and is fixed
thereafter. No `init()`/`setup()` second phase, no default constructor followed by a run of
setters, no static knob poked from elsewhere at startup.

This is the **Complete Constructor** pattern, realized through **constructor injection** and
**immutability**; in C++ it is **RAII** generalized from resources to configuration. What it
forbids is **two-phase initialization** and the **temporal coupling** it creates — a hidden call
order the caller must know, and a not-yet-configured state every method must tolerate.

**Configuration is not state.** This governs how an object is *set up*, not what it does
afterwards. A setter that mutates the domain state the object exists to manage is fine:
`CellProxy::setForegroundColor()` is the cell's data; `InputGenerator::setCursorKeysMode()` is VT
state driven by DECCKM at runtime. `BoxDrawingRenderer::setBrailleStyle()` — a *static*
rendering-policy knob written once from `Config.cpp` — is the thing to avoid. Ask: *would two
differently-configured instances be two different objects, or one object in two states?*
Different objects → constructor.

- Omit the default constructor when there is nothing sensible to default to.
- Configuration members are private and have no setter. Prefer this encapsulated immutability
  over `const` members: a `const` member deletes copy- and move-assignment, quietly breaking
  types held in containers or reassigned. `const`/reference members are permitted
  (`cppcoreguidelines-avoid-const-or-ref-data-members` is off) — use them for value types that
  genuinely never need assignment.
- A long constructor is a fact about the *data*, not a reason to add setters: group related
  parameters into a config struct (which data-driven design wants anyway). A builder is for
  genuinely optional, order-independent parameters only.
- Never wire with a global. A `static` setter is post-construction configuration plus unbounded
  scope, no thread-safety, and state leaking between tests.
- Fallible setup belongs in a static factory returning `std::expected<T, E>` — not in a
  constructor that leaves the object half-built.

**When you cannot.** Each of these must be documented at the declaration, with the reason:

- **Live reconfiguration is the feature** — `Renderer::setFonts()` on config reload. Note the
  price `Renderer` pays for it: `_reconfigMutex`, staged-vs-published state,
  `applyPendingReconfig()`. Pay it only where a user-visible requirement demands it.
- **Externally-driven geometry** — `Renderer::setPageSize()`/`applyResize()`; the window manager
  decides, not us.
- **Framework-mandated** — QML default-constructs types and assigns `Q_PROPERTY`s.
- **Documented rebinding seams** — `TerminalDisplay::setSession()` exists so a session can move
  between displays; the seam is the design.
- **Cyclic wiring** — when A and B must know each other, one `attach`-style call after
  construction is acceptable; a *sequence* of them is not.

**Enforcement.** The mechanical half is automated: `cppcoreguidelines-pro-type-member-init` and
`cppcoreguidelines-prefer-member-initializer` are enabled, so every member is initialized, in the
member-initializer list. The design half is a review question — *how many calls must a caller
make before this object is usable?* The answer must be zero. This is also why it pays off for
testing: a fully-constructed object is built with test doubles in one expression, with no setup
ritual and no half-configured state to reason about.

### Data-driven design
**Behaviour is described by data; code interprets that data.** This is
equally load-bearing and goes well beyond "no magic numbers". The aim is
that adding a flag, a protocol verb, a storage backend, or an error code
is a matter of *adding a row to a table*, not editing logic scattered
across the codebase.

As with DI, **adhere to this unless there is a very strong, explicitly
justified reason not to.** When in doubt, ask: "if a sixth case showed up
tomorrow, how many places would I edit?" If the answer is more than one,
the design is not data-driven enough yet.

### Testability of every code area
**Every code area must be testable, and new code lands with tests.** Every `*_test.cpp` under
`src/isoline/` is compiled into the Catch2 binary `isoline_test` (CTest label `unit`, one test per
test area, the part of a case's name before its first dot: a new area is a row of
`isolineTestAreas` in `src/isoline/CMakeLists.txt`, which `isoline_test.areas` holds every case to),
except those of `text/` and `cli/`, which link their own targets and build `isoline_text_test` and
`isoline_cli_test` (label `unit` too); the
official WebAssembly 3.0 spec suite and the suites of the proposals beyond it run beside it (label
`spec`), every file of the pinned suite imported or excluded with a reason (`docs/conformance.md`).
`isoline_spec_runner` reads each imported `.wast` script itself, with Isoline's text format, against
the oracle manifest the importer writes beside it; its harness (`tests/spec/`) is tested by
`isoline_spectest_test` and the self-test scripts under `tests/spec-*`. A new skip is a GC one or none.
If something is hard to test, that is a design smell: inject the dependency and extract the
decision, don't skip the test. Aim always to increase coverage.

## Zero-warning policy

**The codebase is warning-free, and a warning is a build break.** Dev and CI builds compile
with `-Werror` (`/WX` for MSVC), enabled via `ISOLINE_WERROR: ON` in `CMakePresets.json` and
inherited by every preset through `isoline-common` (see `cmake/IsolinePedantic.cmake`).

- Fix the cause of a warning — never silence it. No `NOLINT`, no `#pragma` mutes, and no
  widening of `-Wno-error=…` without an explicit, justified reason.
- clang-tidy runs as part of the pedantic build; treat its diagnostics the same way — fix,
  don't suppress.

## Repository layout

Isoline is a WebAssembly runtime written in C++23 only — **no Rust in its source or build**. Prebuilt
tools made in Rust may serve as test inputs only (pinned by version and SHA-256 in
`scripts/tools.lock`, beside the CMake and Ninja the CI module lanes install); a default build and an
offline `ctest` need none of them.

- `src/isoline/` — the library, `isoline::core` (target `isoline_core`). Namespace `isoline`, with
  **one sub-namespace per directory** beneath it (`src/isoline/binary/` is `isoline::binary`, and so on).
  Unit tests sit next to the code they test as `*_test.cpp`.
- `tests/` — suites that are not unit tests, such as the imported WebAssembly spec suite.
- `cmake/` — `IsolineOptions.cmake` (every `ISOLINE_*` switch), `IsolineDependencies.cmake`
  (core-cpp and Catch2) and `IsolinePedantic.cmake` (warnings, sanitizers, coverage, clang-tidy, applied
  per target by `isoline_apply_target_policy()` so that dependencies are never built with `-Werror`).
- `scripts/` — the tooling around the build (tool pins, spec-test import, coverage ratchet).
- `vendor/core-cpp/` — [core-cpp](https://github.com/contour-terminal/core-cpp), the shared C++23
  foundation of the Contour Terminal projects, as a verbatim copy of tag v0.21.0 (the tag Contour
  vendors; the two move in lockstep). When a parent project already defines `core::base`, Isoline
  uses the parent's core-cpp instead.

**Never edit `vendor/core-cpp`.** It is checked byte for byte against its `MANIFEST`:

```sh
cmake -DMODE=check -DDEST=vendor/core-cpp -P vendor/core-cpp/cmake/CoreCppVendor.cmake
```

A defect there is fixed in core-cpp, released, and re-vendored with `MODE=sync` (see
`vendor/core-cpp/cmake/CoreCppVendor.cmake`).

# Building

- Linux debug (default): `cmake --preset clang-asan && cmake --build --preset clang-asan` — Clang,
  Debug, AddressSanitizer + UndefinedBehaviorSanitizer, clang-tidy in the build.
- Release: `clang-release`. Coverage: `clang-coverage` (`-fprofile-instr-generate -fcoverage-mapping`).
- GCC: `gcc-debug`, `gcc-release`. macOS: `macos-clang` (Apple Clang). Windows: `msvc-debug`, from a Developer
  Command Prompt.
- Cross from Linux, tests under an emulator: `clangcl-cross-debug` (Windows x64, wine) and
  `clang-aarch64-cross` (arm64, qemu-user); see [docs/cross-compiling.md](docs/cross-compiling.md).
- Catch2 v3 comes from `find_package(Catch2 3)`; configure with `-DISOLINE_FETCH_DEPS=ON` to fetch it
  when it is not installed (the macOS and Windows presets do).
- Every preset builds into `out/<preset>/`.

# Testing

- Run the suite: `ctest --preset clang-asan`.
- Run a single test case: `out/clang-asan/src/isoline/isoline_test "TestName"`.
- Never `co_await` inside a Catch2 assertion: await into a local, then assert on it.

# Workflow

- Format C++ with `clang-format` (version 22, as CI uses) after every change.
- Commits: a Conventional Commits subject; the message ends with a `Signed-off-by:` line.
- Changes land with tests, and the build stays clean under `-Werror` and clang-tidy.
