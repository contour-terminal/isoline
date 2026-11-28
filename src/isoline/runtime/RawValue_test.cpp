// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>

#include <isoline/binary/Types.hpp>
#include <isoline/interpreter/Code.hpp>
#include <isoline/interpreter/SlotCodec.hpp>
#include <isoline/runtime/RawValue.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>

using isoline::binary::AbstractHeapType;
using isoline::binary::ValueType;
using isoline::interpreter::slotClassOf;
using isoline::interpreter::slotWidth;
using isoline::interpreter::storeValue;
using isoline::runtime::decodeValue;
using isoline::runtime::encodeValue;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::RawValue;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::RefLayoutOfRef;
using isoline::runtime::V128;
using isoline::runtime::Value;

namespace
{

/// A value, its type, and its two words written out.
struct Sample
{
    Value value;                   ///< The value.
    ValueType type;                ///< Its type.
    std::array<uint64_t, 2> words; ///< What it encodes to.
};

/// A reference of @p kind to @p address in the abstract heap @p heap.
constexpr Ref refOf(RefKind kind, AbstractHeapType heap, uint32_t address)
{
    return Ref { .kind = kind, .heap = { .kind = heap, .typeIndex = 0 }, .address = address };
}

/// One sample per kind, the words written as literals: RefKind Null 0, Func 1, Extern 2, Exn 3 in bits 0
/// to 7; the heap's kind, Func 0, Extern 1, Exn 2, Concrete 7, in bits 8 to 15; its type index in bits 32
/// to 63.
constexpr auto Samples = std::to_array<Sample>({
    { .value = I32 { .bits = 0xFFFF'FFFF },
      .type = isoline::binary::I32,
      .words = { 0x0000'0000'FFFF'FFFF, 0 } },
    { .value = I64 { .bits = 0x8000'0000'0000'0000 },
      .type = isoline::binary::I64,
      .words = { 0x8000'0000'0000'0000, 0 } },
    { .value = F32 { .bits = 0x7FA0'0001 }, .type = isoline::binary::F32, .words = { 0x7FA0'0001, 0 } },
    { .value = F64 { .bits = 0x7FF4'0000'0000'0001 },
      .type = isoline::binary::F64,
      .words = { 0x7FF4'0000'0000'0001, 0 } },
    { .value = V128 { .lanes = { 0x0706'0504'0302'0100, 0x0F0E'0D0C'0B0A'0908 } },
      .type = isoline::binary::V128,
      .words = { 0x0706'0504'0302'0100, 0x0F0E'0D0C'0B0A'0908 } },
    { .value = refOf(RefKind::Null, AbstractHeapType::Func, 0),
      .type = isoline::binary::FuncRef,
      .words = { 0x0000, 0 } },
    { .value = refOf(RefKind::Null, AbstractHeapType::Extern, 0),
      .type = isoline::binary::ExternRef,
      .words = { 0x0100, 0 } },
    { .value = refOf(RefKind::Func, AbstractHeapType::Func, 7),
      .type = isoline::binary::FuncRef,
      .words = { 0x0001, 7 } },
    { .value = refOf(RefKind::Extern, AbstractHeapType::Extern, 9),
      .type = isoline::binary::ExternRef,
      .words = { 0x0102, 9 } },
    { .value = refOf(RefKind::Exn, AbstractHeapType::Exn, 5),
      .type = isoline::binary::ExnRef,
      .words = { 0x0203, 5 } },
    { .value = Ref { .kind = RefKind::Null,
                     .heap = { .kind = AbstractHeapType::Concrete, .typeIndex = 0x1234'5678 },
                     .address = 0 },
      .type = isoline::binary::FuncRef,
      .words = { 0x1234'5678'0000'0700, 0 } },
});

} // namespace

TEST_CASE("RawValue.roundTripsEveryKind")
{
    for (auto const& sample: Samples)
    {
        CAPTURE(sample.words[0], sample.words[1]);
        CHECK(decodeValue(encodeValue(sample.value), sample.type) == sample.value);
    }
}

TEST_CASE("RawValue.wordsMatchTheTable")
{
    // Each sample's words, written out, are what encodeValue() gives and what the interpreter's
    // slots hold: a scalar its first word only, a vector and a reference both.
    for (auto const& sample: Samples)
    {
        CAPTURE(sample.words[0], sample.words[1]);
        CHECK(encodeValue(sample.value) == RawValue { .words = sample.words });
        auto const width = slotWidth(slotClassOf(sample.type));
        auto slots = std::array<isoline::interpreter::Slot, 2> {};
        storeValue(sample.value, std::span { slots }.first(width));
        CHECK(std::span<uint64_t const> { slots }.first(width).front() == sample.words[0]);
        if (width == 2)
            CHECK(slots[1] == sample.words[1]);
        else
            CHECK(sample.words[1] == 0);
    }
}

TEST_CASE("RawValue.refEncodingIsCanonical")
{
    // Two references equal by operator==, built in storage that held different bytes first: whatever
    // their padding holds, their cells are equal.
    alignas(Ref) auto first = std::array<std::byte, sizeof(Ref)> {};
    alignas(Ref) auto second = std::array<std::byte, sizeof(Ref)> {};
    std::memset(first.data(), 0xA5, first.size());
    std::memset(second.data(), 0x5A, second.size());
    // Field by field, so that the padding keeps the bytes the storage held.
    auto const build = [](std::span<std::byte, sizeof(Ref)> storage) {
        auto* const reference = ::new (storage.data()) Ref;
        reference->kind = RefKind::Func;
        reference->heap.kind = AbstractHeapType::Func;
        reference->heap.typeIndex = 0;
        reference->address = 7;
        return reference;
    };
    auto const* const refA = build(first);
    auto const* const refB = build(second);
    REQUIRE(first != second); // their padding differs
    REQUIRE(*refA == *refB);
    CHECK(encodeValue(Value { *refA }) == encodeValue(Value { *refB }));
    CHECK(encodeValue(Value { *refA }) == RawValue { .words = { 0x0001, 7 } });
}

TEST_CASE("RawValue.refLayoutLocatesEveryField")
{
    // Each field read from a Ref's bytes at its RefLayoutOfRef offset is the field.
    auto const reference = Ref { .kind = RefKind::Extern,
                                 .heap = { .kind = AbstractHeapType::Concrete, .typeIndex = 0x0102'0304 },
                                 .address = 0x0A0B'0C0D };
    auto bytes = std::array<std::byte, sizeof(Ref)> {};
    std::memcpy(bytes.data(), &reference, sizeof(Ref));
    auto const read = [&bytes]<typename T>(std::size_t offset, T /*type*/) {
        auto field = T {};
        std::memcpy(&field, bytes.data() + offset, sizeof(T));
        return field;
    };
    CHECK(RefLayoutOfRef.size == sizeof(Ref));
    CHECK(read(RefLayoutOfRef.kindOffset, RefKind {}) == RefKind::Extern);
    CHECK(read(RefLayoutOfRef.heapKindOffset, AbstractHeapType {}) == AbstractHeapType::Concrete);
    CHECK(read(RefLayoutOfRef.heapTypeIndexOffset, uint32_t {}) == 0x0102'0304);
    CHECK(read(RefLayoutOfRef.addressOffset, uint32_t {}) == 0x0A0B'0C0D);
}

TEST_CASE("RawValue.globalCellsHoldTheirValue")
{
    // A global's cell is its value's words; value() decodes them as its type, assign() re-encodes.
    auto global = isoline::runtime::GlobalInstance {
        .type = { .type = isoline::binary::ExternRef, .mutability = isoline::binary::Mutability::Var },
        .cell = encodeValue(refOf(RefKind::Null, AbstractHeapType::Extern, 0)),
    };
    CHECK(global.value() == Value { refOf(RefKind::Null, AbstractHeapType::Extern, 0) });
    global.assign(refOf(RefKind::Extern, AbstractHeapType::Extern, 9));
    CHECK(global.cell == RawValue { .words = { 0x0102, 9 } });
    CHECK(global.value() == Value { refOf(RefKind::Extern, AbstractHeapType::Extern, 9) });
}

// The contract checks: each ends the process on purpose, so the unit run skips them and CTest runs
// each alone (tests/CMakeLists.txt).

TEST_CASE("Contract.RawValue.globalAssignsItsOwnKind", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto global = isoline::runtime::GlobalInstance {
        .type = { .type = isoline::binary::I32, .mutability = isoline::binary::Mutability::Var },
        .cell = {},
    };
    global.assign(I64 { .bits = 1 });
    FAIL("SURVIVED: an i64 assigned to an i32 global");
}
