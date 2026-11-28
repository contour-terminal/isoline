// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <tuple>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/interpreter/Code.hpp>
#include <isoline/interpreter/SlotCodec.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>

using isoline::binary::AbstractHeapType;
using isoline::binary::HeapType;
using isoline::binary::Nullable;
using isoline::binary::ValueType;
using isoline::interpreter::loadValue;
using isoline::interpreter::nullSlots;
using isoline::interpreter::Slot;
using isoline::interpreter::SlotClass;
using isoline::interpreter::slotClassOf;
using isoline::interpreter::slotWidth;
using isoline::interpreter::storeValue;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::V128;
using isoline::runtime::Value;

namespace binary = isoline::binary;

namespace
{

/// The type of @p value: its kind, and a reference's own heap.
[[nodiscard]] ValueType typeOf(Value const& value)
{
    if (auto const* reference = std::get_if<Ref>(&value))
        return ValueType { .kind = binary::ValueKind::Ref,
                           .nullable = Nullable::Yes,
                           .heap = reference->heap };
    return ValueType { .kind = isoline::runtime::kindOf(value) };
}

/// The slots @p value is stored in, as many as its type is wide.
[[nodiscard]] std::vector<Slot> slotsOf(Value const& value)
{
    auto slots = std::vector<Slot>(slotWidth(slotClassOf(typeOf(value))), Slot { 0xCCCC'CCCC'CCCC'CCCC });
    storeValue(value, slots);
    return slots;
}

} // namespace

TEST_CASE("SlotCodec.slotWidths")
{
    CHECK(slotWidth(SlotClass::Scalar) == 1);
    CHECK(slotWidth(SlotClass::Vector) == 2);
    CHECK(slotWidth(SlotClass::Reference) == 2);
    CHECK(slotClassOf(binary::I32) == SlotClass::Scalar);
    CHECK(slotClassOf(binary::I64) == SlotClass::Scalar);
    CHECK(slotClassOf(binary::F32) == SlotClass::Scalar);
    CHECK(slotClassOf(binary::F64) == SlotClass::Scalar);
    CHECK(slotClassOf(binary::V128) == SlotClass::Vector);
    CHECK(slotClassOf(binary::FuncRef) == SlotClass::Reference);
    CHECK(slotClassOf(binary::concreteRef(3, Nullable::No)) == SlotClass::Reference);
}

TEST_CASE("SlotCodec.roundTripsEveryValueKind")
{
    auto const values = std::to_array<Value>({
        I32 { 0xFFFF'FFFF },
        I32 { 0 },
        I64 { std::numeric_limits<uint64_t>::max() },
        F32 { 0x7FA0'0001 },
        F64 { 0x8000'0000'0000'0000 },
        V128 { .lanes = { 1, 2 } },
        Ref { .kind = RefKind::Null,
              .heap = { .kind = AbstractHeapType::Concrete, .typeIndex = 0xDEAD'BEEF },
              .address = 0 },
        Ref { .kind = RefKind::Func, .heap = { .kind = AbstractHeapType::Func }, .address = 7 },
        Ref { .kind = RefKind::Func,
              .heap = { .kind = AbstractHeapType::Concrete, .typeIndex = 0xFFFF'FFFF },
              .address = 0xFFFF'FFFF },
        Ref { .kind = RefKind::Exn, .heap = { .kind = AbstractHeapType::Exn }, .address = 3 },
        Ref { .kind = RefKind::Null, .heap = { .kind = AbstractHeapType::NoExn }, .address = 0 },
        Ref { .kind = RefKind::Extern, .heap = { .kind = AbstractHeapType::Extern }, .address = 42 },
        Ref { .kind = RefKind::Null, .heap = { .kind = AbstractHeapType::NoExtern }, .address = 0 },
    });
    for (auto const& value: values)
    {
        auto const slots = slotsOf(value);
        CHECK(loadValue(typeOf(value), slots) == value);
    }
}

TEST_CASE("SlotCodec.layout")
{
    // The layout the operations read slots by: a vector's low lane first, a reference's tag word
    // (kind, heap kind, type index) then its address.
    CHECK(slotsOf(I64 { 0x0123'4567'89AB'CDEF }) == std::vector<Slot> { 0x0123'4567'89AB'CDEF });
    CHECK(slotsOf(F64 { 0x7FF8'0000'0000'0001 }) == std::vector<Slot> { 0x7FF8'0000'0000'0001 });
    CHECK(slotsOf(V128 { .lanes = { 1, 2 } }) == std::vector<Slot> { 1, 2 });
    auto const reference = Ref { .kind = RefKind::Func,
                                 .heap = { .kind = AbstractHeapType::Concrete, .typeIndex = 0xDEAD'BEEF },
                                 .address = 7 };
    auto const expectedTag = uint64_t { static_cast<uint8_t>(RefKind::Func) }
                             | (uint64_t { static_cast<uint8_t>(AbstractHeapType::Concrete) } << 8U)
                             | (uint64_t { 0xDEAD'BEEF } << 32U);
    CHECK(slotsOf(reference) == std::vector<Slot> { expectedTag, 7 });
}

TEST_CASE("SlotCodec.narrowValuesAreZeroExtended")
{
    CHECK(slotsOf(I32 { 0x8000'0000 })[0] == 0x0000'0000'8000'0000);
    CHECK(slotsOf(F32 { 0xFFC0'0000 })[0] == 0x0000'0000'FFC0'0000);
}

TEST_CASE("SlotCodec.nullSlotsMapConcreteHeapsThroughTheInstance")
{
    // A local's null keeps its own heap. A concrete heap names the module's type index, which the
    // instance's types map to the store's TypeId: two instances of one module may map it differently.
    auto const instanceTypes = std::to_array<uint32_t>({ 10, 11, 10 });
    auto const concrete = HeapType { .kind = AbstractHeapType::Concrete, .typeIndex = 2 };
    auto const mapped = loadValue(binary::concreteRef(10, Nullable::Yes), nullSlots(concrete, instanceTypes));
    CHECK(mapped
          == Value { Ref { .kind = RefKind::Null,
                           .heap = { .kind = AbstractHeapType::Concrete, .typeIndex = 10 },
                           .address = 0 } });

    // An abstract heap names no type index, so it maps to itself.
    auto const abstract = HeapType { .kind = AbstractHeapType::Extern };
    CHECK(loadValue(binary::ExternRef, nullSlots(abstract, instanceTypes))
          == Value { Ref { .kind = RefKind::Null, .heap = abstract, .address = 0 } });
}

// The contract tests below end the process at a failed Require, so they are hidden from the unit run:
// tests/CMakeLists.txt runs each in a process of its own and expects the Require's text
// (isoline::runtime::testing::endAtContractViolation()).

TEST_CASE("Contract.SlotCodec.storeIntoItsWidth", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto slots = std::array<Slot, 1> {};
    storeValue(V128 { .lanes = { 1, 2 } }, slots);
    FAIL("SURVIVED: a vector was stored into one slot");
}

TEST_CASE("Contract.SlotCodec.loadFromItsWidth", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto const slots = std::array<Slot, 1> {};
    std::ignore = loadValue(binary::FuncRef, slots);
    FAIL("SURVIVED: a reference was loaded from one slot");
}

TEST_CASE("Contract.SlotCodec.nullHeapIsTheInstances", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto const instanceTypes = std::to_array<uint32_t>({ 10 });
    std::ignore = nullSlots(HeapType { .kind = AbstractHeapType::Concrete, .typeIndex = 1 }, instanceTypes);
    FAIL("SURVIVED: a concrete heap outside the instance's types was mapped");
}
