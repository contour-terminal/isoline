// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <isoline/binary/Types.hpp>

namespace isoline::binary
{

/// The canonical id of every type of a type section: the index of the first type structurally equal
/// to it, references to concrete types compared by the canonical ids of the types they name.
///
/// Without GC a type may only name types before it (no recursion groups), so equivalence is
/// structural over an acyclic graph, and the ids are found bottom-up in one pass: each type's
/// references are rewritten to ids already known, and the rewritten type is looked up among those
/// seen. The lookup is an ordered map, so the cost is O(n log n) comparisons of types whatever
/// the guest declares, never a hash table's adversarial worst case; nothing recurses.
///
/// Precondition (checked): every concrete type index in type i is below i, as validation checks.
/// @param types The type section.
/// @return One id per type, in type order; ids[i] <= i.
[[nodiscard]] std::vector<uint32_t> canonicalTypeIds(std::span<FuncType const> types);

/// The canonical ids of the types of several type sections in one space, as canonicalTypeIds() finds
/// them for one: the sections are read as one section of their types in order, each concrete type
/// index of a section shifted by the number of types before that section. Types of two modules are
/// thus equivalent exactly when their ids are equal, which is how core import matching compares
/// types across modules without GC (validate::TypeMatcher). It costs what canonicalTypeIds() costs for
/// the sections' types together, O(n log n) comparisons and memory linear in them, and copies no section.
///
/// Precondition (checked): every concrete type index in type i of a section is below i.
/// @param sections The type sections, in the order their ids are counted.
/// @return One id per type, the sections' types in order; an id is the position of the first type,
///         counted across the sections, that is equivalent to it.
[[nodiscard]] std::vector<uint32_t> jointCanonicalTypeIds(
    std::span<std::span<FuncType const> const> sections);

} // namespace isoline::binary
