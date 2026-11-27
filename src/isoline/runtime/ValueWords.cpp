// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/ValueWords.hpp>

#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>

namespace isoline::runtime
{

namespace
{
    using binary::AbstractHeapType;

    /// The tag word of @p reference: its kind, its heap's kind and its heap's type index.
    [[nodiscard]] constexpr uint64_t tagOf(Ref const& reference) noexcept
    {
        return uint64_t { static_cast<uint8_t>(reference.kind) }
               | (uint64_t { static_cast<uint8_t>(reference.heap.kind) } << 8U)
               | (uint64_t { reference.heap.typeIndex } << 32U);
    }

    /// The reference whose tag word is @p tag and whose address is @p address.
    [[nodiscard]] constexpr Ref referenceOf(uint64_t tag, uint64_t address) noexcept
    {
        return Ref { .kind = static_cast<RefKind>(tag & 0xFFU),
                     .heap = { .kind = static_cast<AbstractHeapType>((tag >> 8U) & 0xFFU),
                               .typeIndex = static_cast<uint32_t>(tag >> 32U) },
                     .address = static_cast<uint32_t>(address) };
    }
} // namespace

void encodeWords(Value const& value, std::span<uint64_t, 2> words) noexcept
{
    std::visit(
        [words]<typename T>(T const& alternative) {
            if constexpr (std::is_same_v<T, V128>)
            {
                words[0] = alternative.lanes[0];
                words[1] = alternative.lanes[1];
            }
            else if constexpr (std::is_same_v<T, Ref>)
            {
                words[0] = tagOf(alternative);
                words[1] = alternative.address;
            }
            else
            {
                words[0] = uint64_t { alternative.bits };
                words[1] = 0;
            }
        },
        value);
}

Value decodeWords(binary::ValueType const& type, std::span<uint64_t const, 2> words) noexcept
{
    switch (type.kind)
    {
        case binary::ValueKind::I32: return I32 { static_cast<uint32_t>(words[0]) };
        case binary::ValueKind::I64: return I64 { words[0] };
        case binary::ValueKind::F32: return F32 { static_cast<uint32_t>(words[0]) };
        case binary::ValueKind::F64: return F64 { words[0] };
        case binary::ValueKind::V128: return V128 { .lanes = { words[0], words[1] } };
        case binary::ValueKind::Ref: break;
    }
    return referenceOf(words[0], words[1]);
}

} // namespace isoline::runtime
