// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>

#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/meta/EnumRows.hpp>

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

/// How the bytes a vector access reads or writes map onto the lanes of its v128.
enum class VectorShape : uint8_t
{
    Whole,      ///< The bytes are the value: a scalar, widened per Extension, or all 16 of a v128.
    Extend8x8,  ///< 8 bytes, each widened per Extension into one of eight 16-bit lanes.
    Extend16x4, ///< 8 bytes as four 16-bit values, each widened into one of four 32-bit lanes.
    Extend32x2, ///< 8 bytes as two 32-bit values, each widened into one of two 64-bit lanes.
    Splat,      ///< width bytes, copied into every lane of that width.
    Zero,       ///< width bytes into the lowest lane of that width; every other lane is zero.
    Lane,       ///< width bytes to or from the lane the lane index names; the others pass through.
};

/// The last VectorShape enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastVectorShape = VectorShape::Lane;

/// One load or store: its direction, value type, width, extension and vector shape.
///
/// The width is the one fact both the validator and the interpreter need: the validator's largest
/// valid alignment exponent is its log2, and the interpreter reads or writes that many bytes.
struct MemoryAccess
{
    Opcode opcode;           ///< The instruction.
    MemoryAccessKind access; ///< Load or store.
    ValueKind value;         ///< The type of the value loaded or stored.
    uint8_t width;           ///< The bytes accessed: 1, 2, 4, 8 or 16.
    Extension extension;     ///< A load's widening, of the value or of each lane; Zero for a store.
    VectorShape shape;       ///< How a v128's lanes take the bytes; Whole for every scalar access.

    /// The largest alignment exponent the access may declare: log2 of its width.
    /// @return The exponent of its natural alignment.
    [[nodiscard]] constexpr uint32_t naturalAlignment() const noexcept
    {
        return static_cast<uint32_t>(std::countr_zero(width));
    }

    /// The bytes of memory one lane takes: a widening load's narrow lane, else the whole width.
    /// @return 1, 2 or 4 for the three Extend shapes; width for every other access.
    [[nodiscard]] constexpr uint32_t memoryLaneWidth() const noexcept;
};

namespace detail
{
    /// The bytes of memory one lane of each VectorShape takes, by shape: the narrow lane of a
    /// widening load, or 0 where a lane is as wide as the access.
    inline constexpr auto MemoryLaneWidthByShape = std::to_array<uint8_t>({ 0, 1, 2, 4, 0, 0, 0 });

    static_assert(meta::sizedByEnum<LastVectorShape>(MemoryLaneWidthByShape.size()),
                  "MemoryLaneWidthByShape holds one entry per VectorShape");
} // namespace detail

constexpr uint32_t MemoryAccess::memoryLaneWidth() const noexcept
{
    auto const narrow = detail::MemoryLaneWidthByShape[static_cast<std::size_t>(shape)];
    return narrow != 0 ? narrow : width;
}

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
                              .extension = extension,
                              .shape = VectorShape::Whole };
    }

    [[nodiscard]] consteval MemoryAccess storeRow(Opcode opcode, ValueKind value, uint8_t width) noexcept
    {
        return MemoryAccess { .opcode = opcode,
                              .access = MemoryAccessKind::Store,
                              .value = value,
                              .width = width,
                              .extension = Extension::Zero,
                              .shape = VectorShape::Whole };
    }

    [[nodiscard]] consteval MemoryAccess vectorLoadRow(Opcode opcode,
                                                       uint8_t width,
                                                       VectorShape shape,
                                                       Extension extension = Extension::Zero) noexcept
    {
        return MemoryAccess { .opcode = opcode,
                              .access = MemoryAccessKind::Load,
                              .value = ValueKind::V128,
                              .width = width,
                              .extension = extension,
                              .shape = shape };
    }

    [[nodiscard]] consteval MemoryAccess vectorStoreRow(Opcode opcode,
                                                        uint8_t width,
                                                        VectorShape shape) noexcept
    {
        return MemoryAccess { .opcode = opcode,
                              .access = MemoryAccessKind::Store,
                              .value = ValueKind::V128,
                              .width = width,
                              .extension = Extension::Zero,
                              .shape = shape };
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
    detail::vectorLoadRow(Opcode::V128Load, 16, VectorShape::Whole),
    detail::vectorLoadRow(Opcode::V128Load8x8S, 8, VectorShape::Extend8x8, Extension::Sign),
    detail::vectorLoadRow(Opcode::V128Load8x8U, 8, VectorShape::Extend8x8),
    detail::vectorLoadRow(Opcode::V128Load16x4S, 8, VectorShape::Extend16x4, Extension::Sign),
    detail::vectorLoadRow(Opcode::V128Load16x4U, 8, VectorShape::Extend16x4),
    detail::vectorLoadRow(Opcode::V128Load32x2S, 8, VectorShape::Extend32x2, Extension::Sign),
    detail::vectorLoadRow(Opcode::V128Load32x2U, 8, VectorShape::Extend32x2),
    detail::vectorLoadRow(Opcode::V128Load8Splat, 1, VectorShape::Splat),
    detail::vectorLoadRow(Opcode::V128Load16Splat, 2, VectorShape::Splat),
    detail::vectorLoadRow(Opcode::V128Load32Splat, 4, VectorShape::Splat),
    detail::vectorLoadRow(Opcode::V128Load64Splat, 8, VectorShape::Splat),
    detail::vectorLoadRow(Opcode::V128Load32Zero, 4, VectorShape::Zero),
    detail::vectorLoadRow(Opcode::V128Load64Zero, 8, VectorShape::Zero),
    detail::vectorLoadRow(Opcode::V128Load8Lane, 1, VectorShape::Lane),
    detail::vectorLoadRow(Opcode::V128Load16Lane, 2, VectorShape::Lane),
    detail::vectorLoadRow(Opcode::V128Load32Lane, 4, VectorShape::Lane),
    detail::vectorLoadRow(Opcode::V128Load64Lane, 8, VectorShape::Lane),
    detail::vectorStoreRow(Opcode::V128Store, 16, VectorShape::Whole),
    detail::vectorStoreRow(Opcode::V128Store8Lane, 1, VectorShape::Lane),
    detail::vectorStoreRow(Opcode::V128Store16Lane, 2, VectorShape::Lane),
    detail::vectorStoreRow(Opcode::V128Store32Lane, 4, VectorShape::Lane),
    detail::vectorStoreRow(Opcode::V128Store64Lane, 8, VectorShape::Lane),
});

static_assert(
    [] {
        for (auto const& row: MemoryAccessTable)
            if (!std::has_single_bit(row.width) || row.width > 16
                || (row.access == MemoryAccessKind::Store && row.extension != Extension::Zero)
                || ((row.shape == VectorShape::Whole) != (row.value != ValueKind::V128 || row.width == 16)))
                return false;
        return true;
    }(),
    "MemoryAccessTable widths are 1, 2, 4, 8 or 16 bytes, only loads extend, and a vector access "
    "has a shape unless it moves all 16 bytes");

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
