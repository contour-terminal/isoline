// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/SlotCodec.hpp>

#include <core/Assert.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>

namespace isoline::fastinterpreter
{

namespace
{
    using binary::AbstractHeapType;
    using runtime::Ref;
    using runtime::RefKind;

    /// The tag word of @p reference: its kind, its heap's kind and its heap's type index.
    [[nodiscard]] constexpr Slot tagOf(Ref const& reference) noexcept
    {
        return Slot { static_cast<uint8_t>(reference.kind) }
               | (Slot { static_cast<uint8_t>(reference.heap.kind) } << 8U)
               | (Slot { reference.heap.typeIndex } << 32U);
    }

    /// The reference whose tag word is @p tag and whose address is @p address.
    [[nodiscard]] constexpr Ref referenceOf(Slot tag, Slot address) noexcept
    {
        return Ref { .kind = static_cast<RefKind>(tag & 0xFFU),
                     .heap = { .kind = static_cast<AbstractHeapType>((tag >> 8U) & 0xFFU),
                               .typeIndex = static_cast<uint32_t>(tag >> 32U) },
                     .address = static_cast<uint32_t>(address) };
    }
} // namespace

void storeValue(runtime::Value const& value, std::span<Slot> slots) noexcept
{
    auto const width = slotWidth(slotClassOf(binary::ValueType { .kind = runtime::kindOf(value) }));
    Require(slots.size() == width);
    std::visit(
        [slots]<typename T>(T const& alternative) {
            if constexpr (std::is_same_v<T, runtime::V128>)
            {
                slots[0] = alternative.lanes[0];
                slots[1] = alternative.lanes[1];
            }
            else if constexpr (std::is_same_v<T, Ref>)
            {
                slots[0] = tagOf(alternative);
                slots[1] = alternative.address;
            }
            else
                slots[0] = Slot { alternative.bits };
        },
        value);
}

runtime::Value loadValue(binary::ValueType const& type, std::span<Slot const> slots) noexcept
{
    auto const width = slotWidth(slotClassOf(type));
    Require(slots.size() == width);
    switch (type.kind)
    {
        case binary::ValueKind::I32: return runtime::I32 { static_cast<uint32_t>(slots[0]) };
        case binary::ValueKind::I64: return runtime::I64 { slots[0] };
        case binary::ValueKind::F32: return runtime::F32 { static_cast<uint32_t>(slots[0]) };
        case binary::ValueKind::F64: return runtime::F64 { slots[0] };
        case binary::ValueKind::V128: return runtime::V128 { .lanes = { slots[0], slots[1] } };
        case binary::ValueKind::Ref: break;
    }
    return referenceOf(slots[0], slots[1]);
}

std::array<Slot, 2> nullSlots(binary::HeapType moduleHeap, std::span<uint32_t const> instanceTypes) noexcept
{
    auto heap = moduleHeap;
    if (heap.kind == AbstractHeapType::Concrete)
    {
        Require(heap.typeIndex < instanceTypes.size());
        heap.typeIndex = instanceTypes[heap.typeIndex];
    }
    return { tagOf(Ref { .kind = RefKind::Null, .heap = heap, .address = 0 }), 0 };
}

} // namespace isoline::fastinterpreter
