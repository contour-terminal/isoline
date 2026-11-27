// SPDX-License-Identifier: Apache-2.0
#pragma once

// The edge rows of the guards' lowering contract (intermediate/Guards.hpp): inputs at the edges of every
// guard, with the outcome the contract's functions give. Guards_test checks the functions against them;
// P5 Task 13 executes every GuardTable row's lowering on every row here, checking both the trap and the
// pointer its select leaves, so that a lowering that loses a rule fails on the row that names it.

#include <array>
#include <cstdint>
#include <limits>

#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>

namespace isoline::compiler::intermediate::testing
{

/// What a guard does with one input.
enum class EdgeOutcome : uint8_t
{
    Traps, ///< It traps, and its select leaves 0.
    Fits,  ///< It passes, and its pointer addresses the row's `fits`.
};

namespace detail
{
    inline constexpr auto All = std::numeric_limits<uint64_t>::max();
    inline constexpr auto Half = uint64_t { 1 } << 63U;
    inline constexpr auto Page = uint64_t { 65'536 };
} // namespace detail

/// One input of checkMemory.
struct MemoryGuardEdge
{
    Type indexType;      ///< The index's type: the memory's address type.
    uint64_t index;      ///< The index register's bits, high half included.
    uint64_t offset;     ///< The static offset.
    uint64_t size;       ///< The access's width.
    uint64_t bound;      ///< The memory's bytes.
    EdgeOutcome outcome; ///< Whether it fits.
    uint64_t fits;       ///< The effective offset when it fits; 0 otherwise.
};

/// checkMemory's edges, the ones P4's fast interpreter once got wrong among them: an end that wraps past
/// 2^64 (whose carry a `lea; cmp; cmova` lowering without a select on the carry path misses: the row
/// {I64, All - 3, 0, 8}), an offset at or past 2^63, a memory smaller than the access, and an I32 index
/// whose register carries high bits.
inline constexpr auto MemoryGuardEdges = std::to_array<MemoryGuardEdge>({
    { Type::I32, 0, 0, 4, detail::Page, EdgeOutcome::Fits, 0 },
    { Type::I32, detail::Page - 4, 0, 4, detail::Page, EdgeOutcome::Fits, detail::Page - 4 },
    { Type::I32, detail::Page - 3, 0, 4, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I32, 0, detail::Page - 4, 4, detail::Page, EdgeOutcome::Fits, detail::Page - 4 },
    { Type::I32, 1, detail::Page - 4, 4, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I32, detail::Page, 0, 1, detail::Page, EdgeOutcome::Traps, 0 },
    // An I32 index is its low 32 bits: high bits in the register neither help nor harm.
    { Type::I32, 0xFFFF'FFFF'0000'0004, 0, 4, detail::Page, EdgeOutcome::Fits, 4 },
    { Type::I32, 0xFFFF'FFFF'FFFF'FFFE, 0, 4, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I32, 0xFFFF'FFFF, 0xFFFF'FFFF, 8, detail::Page, EdgeOutcome::Traps, 0 },
    // A 64-bit memory: ends that wrap past 2^64, offsets at and past 2^63.
    { Type::I64, detail::All, 0, 1, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I64, detail::All - 3, 0, 4, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I64, detail::All - 3, 0, 8, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I64, 4, detail::All - 3, 4, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I64, detail::Half, 0, 1, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I64, detail::Half, detail::Half, 1, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I64, detail::Half + detail::Page, 0, 4, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I64, 0, detail::Half, 1, detail::Page, EdgeOutcome::Traps, 0 },
    { Type::I64, detail::Page - 8, 0, 8, detail::Page, EdgeOutcome::Fits, detail::Page - 8 },
    { Type::I64, detail::Page - 8, 4, 8, detail::Page, EdgeOutcome::Traps, 0 },
    // A memory smaller than the access traps for every index, the smallest included.
    { Type::I32, 0, 0, 8, 4, EdgeOutcome::Traps, 0 },
    { Type::I64, 0, 0, 8, 4, EdgeOutcome::Traps, 0 },
    { Type::I64, 0, 0, 1, 0, EdgeOutcome::Traps, 0 },
    { Type::I64, 0, 0, 8, 8, EdgeOutcome::Fits, 0 },
    { Type::I64, 1, 0, 8, 8, EdgeOutcome::Traps, 0 },
});

/// One input of checkTable.
struct TableGuardEdge
{
    Type indexType;      ///< The index's type: the table's address type.
    uint64_t index;      ///< The index register's bits, high half included.
    uint64_t size;       ///< The table's size.
    EdgeOutcome outcome; ///< Whether it fits.
    uint64_t fits;       ///< The element when it fits; 0 otherwise.
};

/// checkTable's edges: the size itself, an empty table, a table32 index with high bits, a table64 index
/// past 2^32.
inline constexpr auto TableGuardEdges = std::to_array<TableGuardEdge>({
    { Type::I32, 0, 1, EdgeOutcome::Fits, 0 },
    { Type::I32, 9, 10, EdgeOutcome::Fits, 9 },
    { Type::I32, 10, 10, EdgeOutcome::Traps, 0 },
    { Type::I32, 0, 0, EdgeOutcome::Traps, 0 },
    { Type::I32, 0xFFFF'FFFF'0000'0003, 10, EdgeOutcome::Fits, 3 },
    { Type::I32, 0x0000'0001'FFFF'FFFF, 10, EdgeOutcome::Traps, 0 },
    { Type::I64, 0x0000'0001'0000'0003, 10, EdgeOutcome::Traps, 0 },
    { Type::I64, detail::All, detail::All, EdgeOutcome::Traps, 0 },
    { Type::I64, detail::All - 1, detail::All, EdgeOutcome::Fits, detail::All - 1 },
});

/// One input of checkDispatch.
struct DispatchGuardEdge
{
    uint64_t reference;  ///< The non-null Ref's word: its address in bits 32 to 63.
    uint64_t rows;       ///< The store's dispatch rows.
    EdgeOutcome outcome; ///< Whether it fits.
    uint64_t fits;       ///< The function address when it fits; 0 otherwise.
};

/// The word of a Ref of @p address whose low bits (kind and heap) are @p low.
[[nodiscard]] constexpr uint64_t refWordOf(uint64_t address, uint64_t low) noexcept
{
    return (address << RefWordAddressShift) | low;
}

/// checkDispatch's edges: the last row of a chunk, the rows themselves, an address past 2^31, no rows; the
/// word's low bits never move the row.
inline constexpr auto DispatchGuardEdges = std::to_array<DispatchGuardEdge>({
    { refWordOf(0, 1), 1, EdgeOutcome::Fits, 0 },
    { refWordOf(4'095, 0xFFFF), 4'096, EdgeOutcome::Fits, 4'095 },
    { refWordOf(4'096, 0), 4'096, EdgeOutcome::Traps, 0 },
    { refWordOf(0xFFFF'FFFF, 1), 4'096, EdgeOutcome::Traps, 0 },
    { refWordOf(7, 1), 0, EdgeOutcome::Traps, 0 },
});

} // namespace isoline::compiler::intermediate::testing
