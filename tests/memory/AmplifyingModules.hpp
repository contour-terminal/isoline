// SPDX-License-Identifier: Apache-2.0
#pragma once

// Modules shaped to amplify one entity the decoder or the store copies: each is small in bytes and
// large in what a cost per item that grows with another item's size would hold. The memory-bound
// suites measure them, the binary ones as they are and the text ones as their text.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace isoline::testing
{

/// A module whose import section is one compact group (0x7E) of @p items global imports, all from
/// one module named by @p nameBytes bytes, each item named by the empty name: one input byte each.
[[nodiscard]] std::vector<std::byte> compactImportGroup(std::size_t nameBytes, uint32_t items);

/// A module of @p tags tags, all of one type of @p params i32 params: two input bytes per tag.
[[nodiscard]] std::vector<std::byte> wideTags(std::size_t params, uint32_t tags);

/// A module of @p count functions of type [] -> [] with empty bodies.
[[nodiscard]] std::vector<std::byte> manyFunctions(uint32_t count);

/// A module of one function that pushes and drops @p count v128 constants.
[[nodiscard]] std::vector<std::byte> manyV128Literals(uint32_t count);

/// A module of one function whose try_table has @p count catch clauses, each of tag 0 to label 0.
[[nodiscard]] std::vector<std::byte> manyCatchClauses(uint32_t count);

/// A module of @p depth types, each taking a nullable reference to the one before it.
[[nodiscard]] std::vector<std::byte> deepTypeChain(uint32_t depth);

/// A module of @p count function imports, all of one type of @p params i32 params, each from the
/// module "" named "": four input bytes per import.
[[nodiscard]] std::vector<std::byte> manyFunctionImports(std::size_t params, uint32_t count);

/// A module of a function $f of type [] -> [i32 x 1000], and a function [] -> [] whose body is
/// @p calls calls of $f: two input bytes per call, each pushing a thousand operands.
[[nodiscard]] std::vector<std::byte> multiValueCalls(uint32_t calls);

/// A module whose name section names @p count functions and gives @p count functions a local map, every
/// name and map empty: an index and one byte per entry.
[[nodiscard]] std::vector<std::byte> manyNames(uint32_t count);

/// A module whose name section names @p count functions, and one local of each of @p count functions,
/// at indices spread up to 2^32 - 1, each by a name of @p nameBytes bytes.
[[nodiscard]] std::vector<std::byte> sparseLongNames(uint32_t count, std::size_t nameBytes);

/// A module whose name section has one function name subsection of @p bytes bytes that claims as many
/// entries as it has bytes, every byte after the count 0xFF: the first index is a LEB128 too large for a u32.
[[nodiscard]] std::vector<std::byte> lyingNameCount(uint32_t bytes);

/// A module of @p functions functions, each of one run of @p locals i32 locals and an empty body: two
/// bytes declare a run the text declares local by local.
[[nodiscard]] std::vector<std::byte> wideLocalRuns(uint32_t functions, uint32_t locals);

/// A module of @p functions functions of one type of @p params i32 params, whose name section names the
/// first parameter of each: the text writes every parameter inline, so that one can carry the name.
[[nodiscard]] std::vector<std::byte> namedParameterFunctions(std::size_t params, uint32_t functions);

/// A module of one function whose body nests @p depth empty blocks: three bytes per level, each a line
/// of capped indentation in the text.
[[nodiscard]] std::vector<std::byte> nestedBlocks(uint32_t depth);

/// compactImportGroup(@p nameBytes, @p items), and a function [i32] -> [] whose if a branch hint section,
/// before the code section, marks likely: what the writer re-synthesizes as an annotation beside the
/// group's repeated module name.
[[nodiscard]] std::vector<std::byte> hintedImportGroup(std::size_t nameBytes, uint32_t items);

} // namespace isoline::testing
