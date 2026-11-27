// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

#include <isoline/runtime/Segment.hpp>

// The Spectre selects of the fast interpreter's bounds checks (docs/fast-interpreter.md, "Spectre"), each
// runtime::rangeMask() of what it checks, as the bulk operations' ranges are: what replaces an offset into a
// memory, or an index into a table, by zero where the explicit check fails, through
// a data-dependent select computed from the same bound as the check but not from its outcome, so that code
// running ahead of a mispredicted check reads at the start of the memory or table, never where the guest
// chose.

namespace isoline::fastinterpreter
{

/// The Spectre mask of an access of @p width bytes at offset @p effective into a memory of @p size bytes:
/// all ones where it fits, zero where it does not, computed without a branch (docs/fast-interpreter.md),
/// so that code running ahead of a mispredicted bounds check reads memory's first bytes, not past them.
///
/// It is runtime::rangeMask() of the access's bytes, the one mask every bounds check derives from: the sign
/// of the bytes left after the access, of the bytes left after its offset, and of the offset itself, any of
/// them negative meaning the access does not fit. An offset of 2^63 or more never fits (a memory is
/// allocated, so its size stays below 2^63), and without its own sign the bytes left would wrap to a positive
/// count for it. A memory smaller than the access fits nothing, its bytes left after the access negative
/// whatever the offset below 2^63. Either would otherwise let a guest choose where a speculative read lands.
///
/// What remains: a masked access reads @p width bytes at the memory's base, so for a memory smaller than
/// the access, code ahead of the check may read up to @p width bytes past its end: the tail every memory
/// keeps for its size (runtime::requiredTailBytesOf()), never another object's. The check itself traps.
/// @param effective The offset of the access's first byte: the address plus the static offset.
/// @param size The memory's size in bytes.
/// @param width The bytes accessed.
/// @return The mask.
[[nodiscard]] constexpr uint64_t accessMask(uint64_t effective, uint64_t size, uint64_t width) noexcept
{
    return runtime::rangeMask<uint64_t>(effective, width, size);
}

/// The Spectre mask of an index @p index into a table of @p size elements: all ones where the element exists,
/// zero where it does not, computed without a branch. It is runtime::rangeMask() of a one-element range,
/// derived from the bound alike: the sign of the elements left after the element, of the elements left after
/// the index, and of the index itself. An index of 2^63 or more never exists (a table's elements are
/// allocated, so its size stays below 2^63), and a table of no elements has none.
///
/// What remains: a masked index reads the table's first element, so for a table of no elements, code ahead
/// of the check may read one element at its base; the check itself traps.
/// @param index The element's index, as the guest gave it (a table32's read as 32 bits).
/// @param size The table's elements.
/// @return The mask.
[[nodiscard]] constexpr uint64_t elementMask(uint64_t index, uint64_t size) noexcept
{
    return runtime::rangeMask<uint64_t>(index, 1, size);
}

/// The index @p index into a table of @p size elements where the element exists, masked by elementMask(); or
/// nullopt where it does not, as the reference interpreter decides it (index >= size). What table.get,
/// table.set and call_indirect read their element at.
/// @param index The element's index.
/// @param size The table's elements.
/// @return The masked index, or nullopt.
[[nodiscard]] inline std::optional<uint64_t> elementChecked(uint64_t index, uint64_t size) noexcept
{
    if (index >= size) [[unlikely]]
        return std::nullopt;
    return index & elementMask(index, size);
}

} // namespace isoline::fastinterpreter
