// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

#include <isoline/runtime/Trap.hpp>

namespace isoline::runtime
{

/// Whether @p count items from @p offset fit in @p size, without overflowing.
/// @param offset The first item's index.
/// @param count How many items.
/// @param size The size of what they must fit in.
/// @return Whether offset + count <= size.
[[nodiscard]] constexpr bool fits(uint64_t offset, uint64_t count, uint64_t size) noexcept
{
    return offset <= size && count <= size - offset;
}

/// Copies part of a segment into a table or memory, as table.init and memory.init do, and as
/// instantiation does with each whole active segment: both ranges are checked before anything is
/// copied, so an out-of-bounds copy changes nothing.
/// @param segment The segment's items: element references or data bytes; empty once dropped.
/// @param source The first item of @p segment to copy.
/// @param count How many items to copy.
/// @param target The table's elements or the memory's bytes; of the item type @p segment names.
/// @param destination Where in @p target the first item goes.
/// @param outOfBounds The trap either range out of bounds ends with.
/// @return Nothing, or the trap @p outOfBounds.
template <typename T>
[[nodiscard]] std::expected<void, Trap> copySegment(std::span<T const> segment,
                                                    uint64_t source,
                                                    uint64_t count,
                                                    std::type_identity_t<std::span<T>> target,
                                                    uint64_t destination,
                                                    TrapCode outOfBounds)
{
    if (!fits(source, count, segment.size()) || !fits(destination, count, target.size()))
        return trap(outOfBounds);
    std::ranges::copy(segment.subspan(static_cast<std::size_t>(source), static_cast<std::size_t>(count)),
                      target.begin() + static_cast<std::ptrdiff_t>(destination));
    return {};
}

} // namespace isoline::runtime
