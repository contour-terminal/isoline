# The spec test harness

`isoline_spectest` runs the imported suites: the core specification's (`tests/spec/files.txt`) and the
component model's three (`tests/component-files.txt`), each file beside the `.oracle` manifest
`scripts/import-spec-tests.py` wrote for it. `docs/conformance.md` lists what each suite skips and why;
this file names where the runner itself checks otherwise than a suite's own runner would.

## Declared deviations

- **Component failure wordings** (`ComponentWording.hpp`). The component suites assert Wasmtime's and
  wasm-tools' texts, which name the failure Isoline reports in other words, or a site Isoline does not tell
  apart (an out-of-bounds string and an out-of-bounds realloc result are one `OutOfBounds` trap, as
  `definitions.py`'s `trap_if` makes them). An assertion of a component suite therefore passes when its
  text is a substring of Isoline's message *or* of a wording a row gives that message. A row is bound to
  one stage (decode, link, instantiate or execute: a text error is a decode one), matches Isoline's message
  by a regular expression, and cites the Wasmtime 51.0.0 or wasm-tools 1.263.0 source of its text; no row
  lets a failure of another stage or kind pass, and the core suites use no row at all.
- **Core trap wordings** (`CoreTrapWording.hpp`). A core trap that reaches a component call is worded as
  Wasmtime words it (`wasmtime-environ`'s `impl Display for Trap`) before it is matched, one row per
  `TrapCode`.
