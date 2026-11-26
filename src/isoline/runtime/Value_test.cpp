// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

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
