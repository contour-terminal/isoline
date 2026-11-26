// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <isoline/binary/Types.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::fastinterpreter
{

/// How many slots a value takes, by the kind of its type.
enum class SlotClass : uint8_t
{
    Scalar,    ///< i32, i64, f32, f64: one slot, a 32-bit value zero-extended.
    Vector,    ///< v128: two slots, lane 0 (the low 64 bits) first.
    Reference, ///< A reference: two slots, its tag word (kind, heap) and then its address.
};

/// The slots a value of each SlotClass takes, indexed by the class.
inline constexpr auto SlotWidths = std::to_array<uint32_t>({ 1, 2, 2 });

/// The number of slots a value of @p slotClass takes.
/// @param slotClass The class.
/// @return 1 for Scalar, 2 for Vector and Reference.
[[nodiscard]] constexpr uint32_t slotWidth(SlotClass slotClass) noexcept
{
    return SlotWidths[static_cast<std::size_t>(slotClass)];
}

/// The SlotClass of the values of @p type.
/// @param type A value type.
/// @return Scalar for a number type, Vector for v128, Reference for any reference type.
[[nodiscard]] constexpr SlotClass slotClassOf(binary::ValueType const& type) noexcept
{
    switch (type.kind)
    {
        case binary::ValueKind::I32:
        case binary::ValueKind::I64:
        case binary::ValueKind::F32:
        case binary::ValueKind::F64: return SlotClass::Scalar;
        case binary::ValueKind::V128: return SlotClass::Vector;
        case binary::ValueKind::Ref: break;
    }
    return SlotClass::Reference;
}

/// Stores @p value into @p slots, an exact bijection: loadValue() gives it back bit for bit, a typed
/// null's heap included.
///
/// A 32-bit value is zero-extended, a 64-bit one stored as its bits, a vector as lane 0 then lane 1,
/// and a reference as `kind | heap.kind << 8 | heap.typeIndex << 32`, then its address. Slots are
/// written as whole integers, never through their bytes, so the layout is the same on every host.
///
/// Precondition (checked): @p slots is exactly as wide as @p value's SlotClass.
/// @param value The value.
/// @param slots The slots it goes into.
void storeValue(runtime::Value const& value, std::span<Slot> slots) noexcept;

/// The value of @p type that storeValue() stored into @p slots.
///
/// Precondition (checked): @p slots is exactly as wide as @p type's SlotClass.
/// @param type The value's type; a reference's heap comes from the slots, not from it.
/// @param slots The slots.
/// @return The value.
[[nodiscard]] runtime::Value loadValue(binary::ValueType const& type, std::span<Slot const> slots) noexcept;

/// The slots of the null reference of @p moduleHeap, as the module names the heap, in an instance whose
/// type section maps to @p instanceTypes: how a reference local starts, and what ref.null makes.
///
/// A concrete heap names a type index of its module, which each instance maps to its store's TypeId;
/// lowered code is shared by every instance of a module in every store, so it keeps the module's index
/// and the null is made through the running instance. An abstract heap maps to itself.
///
/// Precondition (checked): a concrete heap's type index is an index of @p instanceTypes.
/// @param moduleHeap The null's heap, as its module names it.
/// @param instanceTypes The running instance's TypeIds, by the module's type index.
/// @return The null reference's two slots.
[[nodiscard]] std::array<Slot, 2> nullSlots(binary::HeapType moduleHeap,
                                            std::span<uint32_t const> instanceTypes) noexcept;

} // namespace isoline::fastinterpreter
