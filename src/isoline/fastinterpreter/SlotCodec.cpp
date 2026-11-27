// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/SlotCodec.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <variant>

#include <isoline/runtime/ValueWords.hpp>

namespace isoline::fastinterpreter
{

void storeValue(runtime::Value const& value, std::span<Slot> slots) noexcept
{
    auto const width = slotWidth(slotClassOf(binary::ValueType { .kind = runtime::kindOf(value) }));
    Require(slots.size() == width);
    auto words = std::array<uint64_t, 2> {};
    runtime::encodeWords(value, words);
    std::ranges::copy(std::span { words }.first(width), slots.begin());
}

runtime::Value loadValue(binary::ValueType const& type, std::span<Slot const> slots) noexcept
{
    auto const width = slotWidth(slotClassOf(type));
    Require(slots.size() == width);
    // A scalar's one slot is its first word; the second word of a scalar is 0.
    auto words = std::array<uint64_t, 2> {};
    std::ranges::copy(slots, words.begin());
    return runtime::decodeWords(type, words);
}

runtime::Ref loadReference(std::span<Slot const, 2> slots) noexcept
{
    return std::get<runtime::Ref>(
        runtime::decodeWords(binary::ValueType { .kind = binary::ValueKind::Ref }, slots));
}

void storeReference(runtime::Ref const& reference, std::span<Slot, 2> slots) noexcept
{
    runtime::encodeWords(reference, slots);
}

std::array<Slot, 2> nullSlots(binary::HeapType moduleHeap, std::span<uint32_t const> instanceTypes) noexcept
{
    auto const heap =
        binary::mapTypeIndex(binary::ValueType { .kind = binary::ValueKind::Ref, .heap = moduleHeap },
                             [instanceTypes](uint32_t typeIndex) {
                                 Require(typeIndex < instanceTypes.size());
                                 return instanceTypes[typeIndex];
                             })
            .heap;
    auto slots = std::array<Slot, 2> {};
    runtime::encodeWords(runtime::Ref { .kind = runtime::RefKind::Null, .heap = heap, .address = 0 }, slots);
    return slots;
}

} // namespace isoline::fastinterpreter
