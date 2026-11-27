// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <vector>

#include <isoline/binary/testing/ModuleBuilder.hpp>

namespace isoline::binary::testing
{

/// Bytes of a function body, assembled from pieces.
using Bytes = std::vector<uint8_t>;

/// The concatenation of @p pieces.
/// @param pieces The pieces, in order.
/// @return Their bytes, one after another.
[[nodiscard]] inline Bytes code(std::initializer_list<Bytes> pieces)
{
    // Copied, not inserted: GCC 14's release build reports inserting a piece as a null dereference.
    auto out = Bytes {};
    for (auto const& piece: pieces)
        std::ranges::copy(piece, std::back_inserter(out));
    return out;
}

/// i32.const @p value.
[[nodiscard]] inline Bytes i32Const(int32_t value)
{
    return code({ { 0x41 }, encodeS64(value) });
}

/// i64.const @p value.
[[nodiscard]] inline Bytes i64Const(int64_t value)
{
    return code({ { 0x42 }, encodeS64(value) });
}

/// local.get @p index.
[[nodiscard]] inline Bytes localGet(uint32_t index)
{
    return code({ { 0x20 }, encodeU32(index) });
}

/// local.set @p index.
[[nodiscard]] inline Bytes localSet(uint32_t index)
{
    return code({ { 0x21 }, encodeU32(index) });
}

/// call @p index.
[[nodiscard]] inline Bytes call(uint32_t index)
{
    return code({ { 0x10 }, encodeU32(index) });
}

/// A load or store of @p opcode byte with alignment exponent @p align and offset @p offset.
[[nodiscard]] inline Bytes memoryAccess(uint8_t opcode, uint32_t align, uint32_t offset)
{
    return code({ { opcode }, encodeU32(align), encodeU32(offset) });
}

/// A block of no parameters and no results around @p body.
[[nodiscard]] inline Bytes block(Bytes const& body)
{
    return code({ { 0x02, 0x40 }, body, { 0x0B } });
}

/// A loop of no parameters and no results around @p body.
[[nodiscard]] inline Bytes loop(Bytes const& body)
{
    return code({ { 0x03, 0x40 }, body, { 0x0B } });
}

/// br @p label.
[[nodiscard]] inline Bytes br(uint32_t label)
{
    return code({ { 0x0C }, encodeU32(label) });
}

/// br_if @p label.
[[nodiscard]] inline Bytes brIf(uint32_t label)
{
    return code({ { 0x0D }, encodeU32(label) });
}

/// drop.
[[nodiscard]] inline Bytes drop()
{
    return { 0x1A };
}

/// unreachable.
[[nodiscard]] inline Bytes unreachable()
{
    return { 0x00 };
}

/// memory.fill of memory 0.
[[nodiscard]] inline Bytes memoryFill()
{
    return { 0xFC, 0x0B, 0x00 };
}

/// memory.grow of memory 0.
[[nodiscard]] inline Bytes memoryGrow()
{
    return { 0x40, 0x00 };
}

/// table.grow @p table.
[[nodiscard]] inline Bytes tableGrow(uint32_t table)
{
    return code({ { 0xFC, 0x0F }, encodeU32(table) });
}

} // namespace isoline::binary::testing
