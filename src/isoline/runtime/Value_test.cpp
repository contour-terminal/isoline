// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::binary::ValueKind;
using isoline::runtime::defaultValue;
using isoline::runtime::kindOf;

TEST_CASE("Value.kindOf")
{
    using namespace isoline::runtime;
    static_assert(kindOf(I32 { 1 }) == ValueKind::I32);
    CHECK(kindOf(I32 { 1 }) == ValueKind::I32);
    CHECK(kindOf(I64 { 2 }) == ValueKind::I64);
    CHECK(kindOf(F32 { 3 }) == ValueKind::F32);
    CHECK(kindOf(F64 { 4 }) == ValueKind::F64);
    CHECK(kindOf(V128 {}) == ValueKind::V128);
    CHECK(kindOf(Ref { .kind = RefKind::Func, .heap = {}, .address = 5 }) == ValueKind::Ref);
}

TEST_CASE("Value.defaultValueIsOfItsKind")
{
    for (auto const type: { isoline::binary::I32,
                            isoline::binary::I64,
                            isoline::binary::F32,
                            isoline::binary::F64,
                            isoline::binary::V128,
                            isoline::binary::FuncRef,
                            isoline::binary::ExternRef })
        CHECK(kindOf(defaultValue(type)) == type.kind);
}

TEST_CASE("Value.v128Bytes")
{
    using isoline::runtime::V128;
    using isoline::runtime::v128FromBytes;
    using isoline::runtime::v128ToBytes;
    // Byte 0 is the lowest byte of the low half, byte 15 the highest of the high half.
    constexpr auto Vector = V128 { .lanes = { 0x0706'0504'0302'0100, 0x0F0E'0D0C'0B0A'0908 } };
    constexpr auto Bytes = v128ToBytes(Vector);
    static_assert(Bytes[0] == std::byte { 0x00 } && Bytes[15] == std::byte { 0x0F });
    static_assert(v128FromBytes(Bytes) == Vector);
    CHECK(std::to_integer<int>(Bytes[7]) == 0x07);
    CHECK(std::to_integer<int>(Bytes[8]) == 0x08);
    CHECK(v128FromBytes(Bytes) == Vector);
    CHECK(v128ToBytes(V128 { .lanes = { ~0ULL, 0 } })[7] == std::byte { 0xFF });
}
