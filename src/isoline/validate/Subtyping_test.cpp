// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/validate/Subtyping.hpp>

using isoline::binary::AbstractHeapType;
using isoline::binary::concreteRef;
using isoline::binary::ExnRef;
using isoline::binary::ExternRef;
using isoline::binary::F32;
using isoline::binary::FuncRef;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Nullable;
using isoline::binary::NullExnRef;
using isoline::binary::V128;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;
using isoline::validate::isStoreSubtype;
using isoline::validate::isSubtype;
using isoline::validate::topOf;

namespace
{

/// A non-null reference to the abstract heap @p kind.
ValueType nonNull(AbstractHeapType kind)
{
    return ValueType { .kind = ValueKind::Ref, .nullable = Nullable::No, .heap = { .kind = kind } };
}

} // namespace

TEST_CASE("Subtyping.rules")
{
    // Types 0 and 1 are equivalent, type 2 is not.
    auto const canonical = std::vector<uint32_t> { 0, 0, 2 };
    auto const ref = [](uint32_t index) {
        return concreteRef(index, Nullable::No);
    };
    auto const refNull = [](uint32_t index) {
        return concreteRef(index, Nullable::Yes);
    };

    CHECK(isSubtype(ref(0), FuncRef, canonical)); // (ref $t) <: funcref
    CHECK(isSubtype(ref(0), nonNull(AbstractHeapType::Func), canonical));
    CHECK(isSubtype(ref(0), refNull(0), canonical));       // non-null <: nullable
    CHECK_FALSE(isSubtype(refNull(0), ref(0), canonical)); // (ref null $t) !<: (ref $t)
    CHECK(isSubtype(ref(0), ref(1), canonical));           // equal ids
    CHECK(isSubtype(ref(1), ref(0), canonical));
    CHECK_FALSE(isSubtype(ref(0), ref(2), canonical));      // different ids
    CHECK_FALSE(isSubtype(FuncRef, refNull(0), canonical)); // funcref !<: (ref null $t)
    CHECK_FALSE(isSubtype(FuncRef, nonNull(AbstractHeapType::Func), canonical));
    CHECK(isSubtype(NullExnRef, ExnRef, canonical)); // noexn <: exn
    CHECK_FALSE(isSubtype(ExnRef, NullExnRef, canonical));
    CHECK(isSubtype(nonNull(AbstractHeapType::NoFunc), ref(2), canonical)); // the bottom
    // Hierarchies do not mix.
    CHECK_FALSE(isSubtype(ExternRef, FuncRef, canonical));
    CHECK_FALSE(isSubtype(ref(0), ExternRef, canonical));
    CHECK_FALSE(isSubtype(NullExnRef, FuncRef, canonical));
    // Numbers and vectors match only themselves.
    CHECK(isSubtype(I32, I32, canonical));
    CHECK(isSubtype(V128, V128, canonical));
    CHECK_FALSE(isSubtype(I32, I64, canonical));
    CHECK_FALSE(isSubtype(F32, FuncRef, canonical));
    CHECK_FALSE(isSubtype(FuncRef, I32, canonical));
}

TEST_CASE("Subtyping.storeFormComparesTypeIds")
{
    CHECK(isStoreSubtype(concreteRef(7, Nullable::No), concreteRef(7, Nullable::Yes)));
    CHECK_FALSE(isStoreSubtype(concreteRef(7, Nullable::No), concreteRef(8, Nullable::No)));
    CHECK(isStoreSubtype(concreteRef(7, Nullable::No), FuncRef));
    CHECK_FALSE(isStoreSubtype(FuncRef, concreteRef(7, Nullable::Yes)));
}

TEST_CASE("Subtyping.tops")
{
    CHECK(topOf(AbstractHeapType::Concrete) == AbstractHeapType::Func);
    CHECK(topOf(AbstractHeapType::NoFunc) == AbstractHeapType::Func);
    CHECK(topOf(AbstractHeapType::NoExtern) == AbstractHeapType::Extern);
    CHECK(topOf(AbstractHeapType::NoExn) == AbstractHeapType::Exn);
    CHECK(topOf(AbstractHeapType::Extern) == AbstractHeapType::Extern);
}

TEST_CASE("Subtyping.botIsBelowEveryHierarchy")
{
    // (ref bot), what validation pops from the polymorphic stack, matches every reference type, but
    // no number or vector, and nothing but bot is below it.
    auto const canonical = std::vector<uint32_t> { 0 };
    auto const bot = nonNull(AbstractHeapType::Bot);
    CHECK(isSubtype(bot, concreteRef(0, Nullable::No), canonical));
    CHECK(isSubtype(bot, nonNull(AbstractHeapType::NoFunc), canonical));
    CHECK(isSubtype(bot, ExternRef, canonical));
    CHECK(isSubtype(bot, NullExnRef, canonical));
    CHECK(isSubtype(bot, bot, canonical));
    CHECK(isStoreSubtype(bot, concreteRef(7, Nullable::No)));
    CHECK_FALSE(isSubtype(bot, I32, canonical));
    CHECK_FALSE(isSubtype(bot, V128, canonical));
    CHECK_FALSE(isSubtype(nonNull(AbstractHeapType::NoFunc), bot, canonical));
    CHECK_FALSE(isSubtype(concreteRef(0, Nullable::No), bot, canonical));
}
