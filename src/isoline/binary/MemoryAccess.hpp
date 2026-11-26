// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>

#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>

namespace isoline::binary
{

/// Whether a memory access reads memory onto the stack or writes a value to it.
enum class MemoryAccessKind : uint8_t
{
    Load,  ///< [address] -> [value].
    Store, ///< [address value] -> [].
};

/// How a load narrower than its value type widens the bytes it reads.
enum class Extension : uint8_t
{
    Zero, ///< Zero-extends; also every access as wide as its type, and every store.
    Sign, ///< Sign-extends.
};

/// One load or store: its direction, value type, width and extension.
///
/// The width is the one fact both the validator and the interpreter need: the validator's largest
/// valid alignment exponent is its log2, and the interpreter reads or writes that many bytes.
struct MemoryAccess
{
    Opcode opcode;           ///< The instruction.
    MemoryAccessKind access; ///< Load or store.
    ValueKind value;         ///< The type of the value loaded or stored.
    uint8_t width;           ///< The bytes accessed: 1, 2, 4 or 8.
    Extension extension;     ///< A load's widening; Zero for a store.

    /// The largest alignment exponent the access may declare: log2 of its width.
    /// @return The exponent of its natural alignment.
    [[nodiscard]] constexpr uint32_t naturalAlignment() const noexcept
    {
        return static_cast<uint32_t>(std::countr_zero(width));
    }
};

namespace detail
{
    [[nodiscard]] consteval MemoryAccess loadRow(Opcode opcode,
                                                 ValueKind value,
                                                 uint8_t width,
                                                 Extension extension = Extension::Zero) noexcept
    {
        return MemoryAccess { .opcode = opcode,
                              .access = MemoryAccessKind::Load,
                              .value = value,
                              .width = width,
                              .extension = extension };
    }

    [[nodiscard]] consteval MemoryAccess storeRow(Opcode opcode, ValueKind value, uint8_t width) noexcept
    {
        return MemoryAccess { .opcode = opcode,
                              .access = MemoryAccessKind::Store,
                              .value = value,
                              .width = width,
                              .extension = Extension::Zero };
    }
} // namespace detail

/// Every load and store, one row each.
inline constexpr auto MemoryAccessTable = std::to_array<MemoryAccess>({
    detail::loadRow(Opcode::I32Load, ValueKind::I32, 4),
    detail::loadRow(Opcode::I64Load, ValueKind::I64, 8),
    detail::loadRow(Opcode::F32Load, ValueKind::F32, 4),
    detail::loadRow(Opcode::F64Load, ValueKind::F64, 8),
    detail::loadRow(Opcode::I32Load8S, ValueKind::I32, 1, Extension::Sign),
    detail::loadRow(Opcode::I32Load8U, ValueKind::I32, 1),
    detail::loadRow(Opcode::I32Load16S, ValueKind::I32, 2, Extension::Sign),
    detail::loadRow(Opcode::I32Load16U, ValueKind::I32, 2),
    detail::loadRow(Opcode::I64Load8S, ValueKind::I64, 1, Extension::Sign),
    detail::loadRow(Opcode::I64Load8U, ValueKind::I64, 1),
    detail::loadRow(Opcode::I64Load16S, ValueKind::I64, 2, Extension::Sign),
    detail::loadRow(Opcode::I64Load16U, ValueKind::I64, 2),
    detail::loadRow(Opcode::I64Load32S, ValueKind::I64, 4, Extension::Sign),
    detail::loadRow(Opcode::I64Load32U, ValueKind::I64, 4),
    detail::storeRow(Opcode::I32Store, ValueKind::I32, 4),
    detail::storeRow(Opcode::I64Store, ValueKind::I64, 8),
    detail::storeRow(Opcode::F32Store, ValueKind::F32, 4),
    detail::storeRow(Opcode::F64Store, ValueKind::F64, 8),
    detail::storeRow(Opcode::I32Store8, ValueKind::I32, 1),
    detail::storeRow(Opcode::I32Store16, ValueKind::I32, 2),
    detail::storeRow(Opcode::I64Store8, ValueKind::I64, 1),
    detail::storeRow(Opcode::I64Store16, ValueKind::I64, 2),
    detail::storeRow(Opcode::I64Store32, ValueKind::I64, 4),
});

static_assert(
    [] {
        for (auto const& row: MemoryAccessTable)
            if (!std::has_single_bit(row.width) || row.width > 8
                || (row.access == MemoryAccessKind::Store && row.extension != Extension::Zero))
                return false;
        return true;
    }(),
    "MemoryAccessTable widths are 1, 2, 4 or 8 bytes, and only loads extend");

namespace detail
{
    /// One more than the MemoryAccessTable row of each opcode, by opcode; 0 for an opcode that
    /// accesses no memory.
    inline constexpr auto MemoryAccessRowByOpcode = [] {
        auto rows = std::array<uint8_t, OpcodeCount> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, MemoryAccessTable.size()))
            rows.at(static_cast<std::size_t>(MemoryAccessTable[index].opcode)) =
                static_cast<uint8_t>(index + 1);
        return rows;
    }();
} // namespace detail

/// The load or store @p opcode is, in constant time.
/// @param opcode The instruction.
/// @return Its row of MemoryAccessTable, or nullptr when it is no load or store.
[[nodiscard]] constexpr MemoryAccess const* memoryAccessOf(Opcode opcode) noexcept
{
    auto const row = detail::MemoryAccessRowByOpcode[static_cast<std::size_t>(opcode)];
    return row == 0 ? nullptr : &MemoryAccessTable[row - 1U];
}

} // namespace isoline::binary
