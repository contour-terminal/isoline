// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>

#include "SpecCheck.hpp"

// How the component suites word what Isoline words otherwise (D12): their assertions are Wasmtime's and
// wasm-tools' texts, which name the same failure as Isoline's message does but in other words, or with a
// site Isoline does not distinguish (Wasmtime words an out-of-bounds string and an out-of-bounds realloc
// result apart; Isoline traps both as canonical::TrapCode::OutOfBounds, as definitions.py's trap_if does).
// The runner checks an assertion of a component suite against Isoline's message and against every
// wording of it here (checkFailure()); no row makes a failure of another stage or kind pass.

namespace isoline::spectest
{

/// The component suites' wordings of Isoline's failures, by stage. Adding one is adding a row; each
/// names where its text comes from: Wasmtime 51.0.0 (crates/…) or wasm-tools 1.263.0 (crates/…).
inline constexpr auto ComponentWording = std::to_array<WordingRow>({
    // canonical::TrapCode::OutOfBounds: environ/src/trap_encoding.rs (StringOutOfBounds, ListOutOfBounds),
    // wasmtime/src/runtime/component/func/options.rs (realloc), func/typed.rs and func.rs (pointers).
    { .stage = Stage::Execute,
      .pattern = "^out of bounds of memory: ",
      .wording = "string content out-of-bounds" },
    { .stage = Stage::Execute,
      .pattern = "^out of bounds of memory: ",
      .wording = "list content out-of-bounds" },
    { .stage = Stage::Execute,
      .pattern = "^out of bounds of memory: ",
      .wording = "wasm trap: list content out-of-bounds" },
    { .stage = Stage::Execute,
      .pattern = "^out of bounds of memory: ",
      .wording = "realloc return: beyond end of memory" },
    { .stage = Stage::Execute,
      .pattern = "^out of bounds of memory: ",
      .wording = "pointer out of bounds of memory" },
    { .stage = Stage::Execute,
      .pattern = "^out of bounds of memory: ",
      .wording = "string pointer/length out of bounds of memory" },
    // canonical::TrapCode::UnalignedPointer: environ/src/trap_encoding.rs (UnalignedPointer),
    // component/func/options.rs (realloc), func/typed.rs, func.rs and values.rs (pointers).
    { .stage = Stage::Execute,
      .pattern = "^unaligned pointer [0-9]+ for an alignment of [0-9]+$",
      .wording = "wasm trap: unaligned pointer" },
    { .stage = Stage::Execute,
      .pattern = "^unaligned pointer [0-9]+ for an alignment of [0-9]+$",
      .wording = "realloc return: result not aligned" },
    { .stage = Stage::Execute,
      .pattern = "^unaligned pointer [0-9]+ for an alignment of [0-9]+$",
      .wording = "return pointer not aligned" },
    { .stage = Stage::Execute,
      .pattern = "^unaligned pointer [0-9]+ for an alignment of [0-9]+$",
      .wording = "list pointer is not aligned" },
    { .stage = Stage::Execute,
      .pattern = "^unaligned pointer [0-9]+ for an alignment of ([0-9]+)$",
      .wording = "string pointer not aligned to $1" },
    // canonical::TrapCode::InvalidDiscriminant: component/values.rs.
    { .stage = Stage::Execute,
      .pattern = "^invalid variant discriminant ([0-9]+) of ([0-9]+) cases$",
      .wording = "discriminant $1 out of range [0..$2)" },
    // canonical::TrapCode::InvalidEncoding: vm/component/libcalls.rs, and Rust's str::from_utf8 for a
    // string that ends inside a sequence.
    { .stage = Stage::Execute, .pattern = "^invalid utf-8$", .wording = "invalid utf8 encoding" },
    { .stage = Stage::Execute, .pattern = "^invalid utf-8$", .wording = "incomplete utf-8 byte sequence" },
    { .stage = Stage::Execute, .pattern = "^invalid utf-16$", .wording = "invalid utf16 encoding" },
    // canonical::TrapCode::InvalidChar: Rust's char::try_from(u32), "converted integer out of range for
    // `char`".
    { .stage = Stage::Execute, .pattern = "^invalid `char` bit pattern ", .wording = "integer out of range" },
    // canonical::TrapCode::UncaughtException: environ/src/trap_encoding.rs
    // (ExceptionPropagatedOutOfComponent),
    // in a call and in a start function alike.
    { .stage = Stage::Execute,
      .pattern = "^uncaught exception$",
      .wording = "uncaught exception propagated out of component" },
    { .stage = Stage::Instantiate,
      .pattern = "^uncaught exception$",
      .wording = "uncaught exception propagated out of component" },
    // canonical::TrapCode::OwnExpected: vm/component/resources.rs.
    { .stage = Stage::Execute,
      .pattern = "^handle index [0-9]+ is a borrow where an own is expected$",
      .wording = "cannot lift own resource from a borrow" },
    // A link failure: component/matching.rs, which names an expected function `function` and every
    // other sort by its own name, and component/linker.rs for a missing definition.
    { .stage = Stage::Link,
      .pattern = "expected func, found ([a-z]+)$",
      .wording = "expected function found $1" },
    { .stage = Stage::Link,
      .pattern = "expected ([a-z]+), found ([a-z]+)$",
      .wording = "expected $1 found $2" },
    { .stage = Stage::Link,
      .pattern = "resource types are not the same$",
      .wording = "mismatched resource types" },
    { .stage = Stage::Link,
      .pattern = "missing expected export `([^`]*)`$",
      .wording = "`$1`: a matching implementation was not found in the linker" },
    // A core module's link failure: component/matching.rs (module), its contexts then the mismatch.
    { .stage = Stage::Link,
      .pattern = "type mismatch in export `([^`]*)`",
      .wording = "module export `$1` has the wrong type" },
    { .stage = Stage::Link,
      .pattern = "type mismatch in import `([^`]*)`",
      .wording = "module import `$1` has the wrong type" },
    { .stage = Stage::Link,
      .pattern = "missing expected export `([^`]*)`$",
      .wording = "module export `$1` not defined" },
    { .stage = Stage::Link,
      .pattern = "missing expected import `([^`]*)`$",
      .wording = "module import `$1` not defined" },
    { .stage = Stage::Link,
      .pattern = "expected: ([^\n]*)\nfound: +([^\n]*)$",
      .wording = "expected type `$1`, found type `$2`" },
    // A text error: wasm-tools' crates/wast (lexer.rs, resolve/names.rs, component/*.rs).
    { .stage = Stage::Decode, .pattern = "^illegal escape", .wording = "invalid string escape" },
    { .stage = Stage::Decode,
      .pattern = "^duplicate ([a-z-]+) attribute",
      .wording = "duplicate `$1` attribute" },
    { .stage = Stage::Decode, .pattern = "^duplicate ([a-z ]+?) \\$", .wording = "duplicate $1 identifier" },
    { .stage = Stage::Decode,
      .pattern = "^unknown outer count ([0-9]+)",
      .wording = "outer count of `$1` is too large" },
    { .stage = Stage::Decode,
      .pattern = "^unknown outer component \\$([^ ]+)",
      .wording = "outer component `$1` not found" },
    { .stage = Stage::Decode,
      .pattern =
          "^unknown outer (?!component |count |module |type |core module |core type )[a-z ]+ \\$([^ ]+)",
      .wording = "outer item `$1` is not a module, type, or component" },
    { .stage = Stage::Decode,
      .pattern = "^unknown (?!outer )([a-z ]+) (\\$[^ ]+)",
      .wording = "unknown $1: failed to find name `$2`" },
    // A core module's decode failure: wasmparser's src/binary_reader.rs.
    { .stage = Stage::Decode,
      .pattern = "^unexpected content after last section",
      .wording = "section out of order" },
});

} // namespace isoline::spectest
