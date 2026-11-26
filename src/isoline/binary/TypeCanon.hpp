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

} // namespace isoline::binary
