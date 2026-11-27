// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

// The Spectre selects of the fast interpreter's bounds checks (docs/fast-interpreter.md, "Spectre"): what
// replaces an offset into a memory, or an index into a table, by zero where the explicit check fails, through
// a data-dependent select computed from the same bound as the check but not from its outcome, so that code
// running ahead of a mispredicted check reads at the start of the memory or table, never where the guest
// chose.

namespace isoline::fastinterpreter
{

/// The Spectre mask of an access of @p width bytes at offset @p effective into a memory of @p size bytes:
/// all ones where it fits, zero where it does not, computed without a branch (docs/fast-interpreter.md),
/// so that code running ahead of a mispredicted bounds check reads memory's first bytes, not past them.
///
/// It is the sign of the bytes left after the access, of the offset itself, and of the memory's size less
/// the width, any of them negative meaning the access does not fit. An offset of 2^63 or more never fits (a
/// memory is allocated, so its size stays below 2^63), and without its own sign the bytes left would wrap to
/// a positive count for it. A memory smaller than the access (@p size below @p width) fits nothing, and
/// without the sign of its size less the width, which wraps, the bytes left would come out positive for
/// offsets just below 2^63. Either would let a guest choose where a speculative read lands.
///
/// What remains: a masked access reads @p width bytes at the memory's base, so for a memory smaller than
/// the access, code ahead of the check may read fewer than @p width bytes past its end; the check itself
/// traps.
/// @param effective The offset of the access's first byte: the address plus the static offset.
/// @param size The memory's size in bytes.
/// @param width The bytes accessed.
/// @return The mask.
[[nodiscard]] constexpr uint64_t accessMask(uint64_t effective, uint64_t size, uint64_t width) noexcept
{
    auto const left = static_cast<int64_t>(size - width - effective);
    auto const room = static_cast<int64_t>(size - width);
    return ~static_cast<uint64_t>((left | static_cast<int64_t>(effective) | room) >> 63U);
}

/// The Spectre mask of an index @p index into a table of @p size elements: all ones where the element exists,
/// zero where it does not, computed without a branch. It is accessMask() of a one-element access, derived
/// from the bound alike: the sign of the elements left after the element, of the index itself, and of the
/// size less one. An index of 2^63 or more never exists (a table's elements are allocated, so its size stays
/// below 2^63), and a table of no elements has none.
///
/// What remains: a masked index reads the table's first element, so for a table of no elements, code ahead
/// of the check may read one element at its base; the check itself traps.
/// @param index The element's index, as the guest gave it (a table32's read as 32 bits).
/// @param size The table's elements.
/// @return The mask.
[[nodiscard]] constexpr uint64_t elementMask(uint64_t index, uint64_t size) noexcept
{
    return accessMask(index, size, 1);
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
