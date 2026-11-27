// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Types.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/canonical/GuestMemory.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/canonical/LiftLower.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/canonical/testing/GuestDoubles.hpp>
#include <isoline/component/canonical/testing/Sides.hpp>
#include <isoline/component/types/TypeArena.hpp>

using isoline::api::F32;
using isoline::api::F64;
using isoline::api::I32;
using isoline::api::I64;
using isoline::api::Value;
using isoline::component::canonical::equal;
using isoline::component::canonical::LiftContext;
using isoline::component::canonical::liftFlat;
using isoline::component::canonical::load;
using isoline::component::canonical::lowerFlat;
using isoline::component::canonical::MaxFlatParams;
using isoline::component::canonical::MaxFlatResults;
using isoline::component::canonical::outOfBounds;
using isoline::component::canonical::Ownership;
using isoline::component::canonical::ResourceTypeIndex;
using isoline::component::canonical::ResourceValue;
using isoline::component::canonical::store;
using isoline::component::canonical::StringEncoding;
using isoline::component::canonical::Trap;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::trapOf;
using isoline::component::canonical::ValueBuilder;
using isoline::component::canonical::Values;
using isoline::component::canonical::ValueView;
using isoline::component::canonical::testing::bytesOf;
using isoline::component::canonical::testing::equalValues;
using isoline::component::canonical::testing::FakeGuestMemoryOptions;
using isoline::component::canonical::testing::lifted;
using isoline::component::canonical::testing::littleEndian;
using isoline::component::canonical::testing::lowered;
using isoline::component::canonical::testing::MemoryAccess;
using isoline::component::canonical::testing::rootsOf;
using isoline::component::canonical::testing::Side;
using isoline::component::canonical::testing::trapIn;
using isoline::component::canonical::testing::ZeroGuestMemory;
using isoline::component::types::DefinedType;
using isoline::component::types::TypeId;
using isoline::component::types::ValueKind;

namespace
{
/// The bits of @p value.
[[nodiscard]] uint32_t bitsOf(float value)
{
    return std::bit_cast<uint32_t>(value);
}
} // namespace

TEST_CASE("LiftLower.scalarsFlat")
{
    auto side = Side {};
    auto const types = std::vector<TypeId> {
        side.primitive(ValueKind::U8),  side.primitive(ValueKind::S8),  side.primitive(ValueKind::Bool),
        side.primitive(ValueKind::F32), side.primitive(ValueKind::F64), side.primitive(ValueKind::S16),
        side.primitive(ValueKind::U32), side.primitive(ValueKind::S64), side.primitive(ValueKind::Char),
        side.primitive(ValueKind::U64), side.primitive(ValueKind::U16), side.primitive(ValueKind::S32),
    };
    auto context = side.context();
    auto const values = lifted(context,
                               types,
                               { I32 { 0x1ff },
                                 I32 { 0x80 },
                                 I32 { 2 },
                                 F32 { 0x7fa0'0001 },
                                 F64 { 0xfff0'0000'0000'0001 },
                                 I32 { 0xffff'8000 },
                                 I32 { 0xffff'ffff },
                                 I64 { ~uint64_t { 0 } },
                                 I32 { 0x1f600 },
                                 I64 { uint64_t { 1 } << 63U },
                                 I32 { 0x0001'0002 },
                                 I32 { 0x8000'0000 } });
    // Integers wrap to their width (lift_flat_unsigned/signed), a bool is any nonzero value, and a NaN is
    // canonicalized.
    CHECK(values.root(0).scalar<uint8_t>() == 0xff);
    CHECK(values.root(1).scalar<int8_t>() == -128);
    CHECK(values.root(2).scalar<bool>());
    CHECK(values.root(3).scalarBits() == 0x7fc0'0000);
    CHECK(values.root(4).scalarBits() == 0x7ff8'0000'0000'0000);
    CHECK(values.root(5).scalar<int16_t>() == -32768);
    CHECK(values.root(6).scalar<uint32_t>() == 0xffff'ffff);
    CHECK(values.root(7).scalar<int64_t>() == -1);
    CHECK(values.root(8).scalar<char32_t>() == U'\U0001F600');
    CHECK(values.root(9).scalar<uint64_t>() == uint64_t { 1 } << 63U);
    CHECK(values.root(10).scalar<uint16_t>() == 2);
    CHECK(values.root(11).scalar<int32_t>() == -2147483648);

    // Lowering sign-extends a signed integer into its core value, and writes a bool as 1.
    CHECK(lowered(context, types, values)
          == std::vector<Value> { I32 { 0xff },
                                  I32 { 0xffff'ff80 },
                                  I32 { 1 },
                                  F32 { 0x7fc0'0000 },
                                  F64 { 0x7ff8'0000'0000'0000 },
                                  I32 { 0xffff'8000 },
                                  I32 { 0xffff'ffff },
                                  I64 { ~uint64_t { 0 } },
                                  I32 { 0x1f600 },
                                  I64 { uint64_t { 1 } << 63U },
                                  I32 { 2 },
                                  I32 { 0x8000'0000 } });

    // A bool is any nonzero core value, its low byte or not (convert_int_to_bool); in a variant's joined
    // i64 slot it is the wrapped i32's (wrap_i64_to_i32).
    auto const boolType = std::vector<TypeId> { side.primitive(ValueKind::Bool) };
    for (auto const truthy: { 0x100U, 0xffff'ff00U, 1U })
        CHECK(lifted(context, boolType, { I32 { truthy } }).root(0).scalar<bool>());
    CHECK_FALSE(lifted(context, boolType, { I32 { 0 } }).root(0).scalar<bool>());
    auto const boolOrWide = side.variant({ "a", "b" }, { boolType.front(), side.primitive(ValueKind::U64) });
    auto boolContext = side.context();
    CHECK(
        lifted(boolContext, { boolOrWide }, { I32 { 0 }, I64 { 0x100 } }).root(0).payload()->scalar<bool>());
    CHECK_FALSE(lifted(boolContext, { boolOrWide }, { I32 { 0 }, I64 { uint64_t { 1 } << 32U } })
                    .root(0)
                    .payload()
                    ->scalar<bool>());

    // A char past 0x10FFFF or a surrogate traps; the scalar values around them lift.
    auto const charType = std::vector<TypeId> { side.primitive(ValueKind::Char) };
    auto const invalid = GENERATE(0xd800U, 0xdbffU, 0xdc00U, 0xdfffU, 0x11'0000U, 0xffff'ffffU);
    CAPTURE(invalid);
    auto const trap =
        trapIn(liftFlat(context, charType, std::vector<Value> { I32 { invalid } }, MaxFlatParams));
    CHECK(trap.code == TrapCode::InvalidChar);
    CHECK(trap.message().contains(std::format("{:#x}", invalid)));
    for (auto const valid: { 0U, 0xd7ffU, 0xe000U, 0x10'ffffU })
        CHECK(lifted(context, charType, { I32 { valid } }).root(0).scalar<char32_t>() == valid);
    // Lifted values are charged to the budget, and released with them.
    CHECK(side.budget().live() == values.hostBytes());
}

TEST_CASE("LiftLower.scalarsInMemory")
{
    auto side = Side {};
    auto const types = std::vector<TypeId> {
        side.primitive(ValueKind::Bool), side.primitive(ValueKind::S8),  side.primitive(ValueKind::S16),
        side.primitive(ValueKind::F32),  side.primitive(ValueKind::F64), side.primitive(ValueKind::Char),
    };
    auto context = side.context();
    // tuple<bool, s8, s16, f32, f64, char> at 64: offsets 0, 1, 2, 4, 8, 16; 20 bytes, aligned to 8.
    side.memory().put(64, bytesOf({ 7, 0x80, 0x00, 0x80, 0x01, 0x00, 0xc0, 0xff }));
    side.memory().put(72, littleEndian(0x7ff0'0000'0000'0001, 8));
    side.memory().put(80, littleEndian(0xdfff, 4));
    CHECK(trapIn(load(context, types, 64)).code == TrapCode::InvalidChar);
    side.memory().put(80, littleEndian(0x10'ffff, 4));
    auto const values = load(context, types, 64);
    REQUIRE(values.has_value());
    CHECK(values->root(0).scalar<bool>());
    CHECK(values->root(1).scalar<int8_t>() == -128);
    CHECK(values->root(2).scalar<int16_t>() == -32768);
    CHECK(values->root(3).scalarBits() == 0x7fc0'0000); // 0xffc00001 is a NaN: canonicalized
    CHECK(values->root(4).scalarBits() == 0x7ff8'0000'0000'0000);
    CHECK(values->root(5).scalar<char32_t>() == 0x10'ffff);

    // A NaN the host built with a payload of its own is stored canonical too.
    auto const f32 = side.primitive(ValueKind::F32);
    auto const hostNan = side.build({ f32 }, [](ValueBuilder& builder) { builder.scalar(0xffa0'0001); });
    REQUIRE(store(context, f32, hostNan.root(0), 160).has_value());
    CHECK(std::ranges::equal(side.memory().bytes().subspan(160, 4), littleEndian(0x7fc0'0000, 4)));
    // Stored back, a bool is 1, a NaN canonical, an integer its width's bytes.
    auto const addresses = std::array { 128U, 129U, 130U, 132U, 136U, 144U };
    for (auto const index: std::views::iota(std::size_t { 0 }, types.size()))
        REQUIRE(store(context, types[index], values->root(index), addresses[index]).has_value());
    auto const bytes = side.memory().bytes();
    CHECK(
        std::ranges::equal(bytes.subspan(128, 8), bytesOf({ 1, 0x80, 0x00, 0x80, 0x00, 0x00, 0xc0, 0x7f })));
    CHECK(std::ranges::equal(bytes.subspan(136, 8), littleEndian(0x7ff8'0000'0000'0000, 8)));
    CHECK(std::ranges::equal(bytes.subspan(144, 4), littleEndian(0x10'ffff, 4)));
}

TEST_CASE("LiftLower.variantJoinBitcast")
{
    auto side = Side {};
    auto const f32 = side.primitive(ValueKind::F32);
    auto const u64 = side.primitive(ValueKind::U64);
    auto const u32 = side.primitive(ValueKind::U32);
    auto const u8 = side.primitive(ValueKind::U8);
    auto const wide = side.variant({ "a", "b" }, { f32, u64 });                      // [i32, i64]
    auto const word = side.variant({ "a", "b" }, { f32, u32 });                      // [i32, i32]
    auto const pair = side.compound(ValueKind::Tuple, { u32, f32 });                 // [i32, f32]
    auto const padded = side.variant({ "x", "y", "z" }, { u8, pair, std::nullopt }); // [i32, i32, f32]
    auto const nested = side.compound(ValueKind::Option, { wide });                  // [i32, i32, i64]
    auto const single = side.variant({ "x" }, { f32 });                              // [i32, f32]
    auto const widened = side.variant({ "a", "b" }, { single, u64 });                // [i32, i64, f32]
    auto context = side.context();
    auto const roundTrips = [&](TypeId type, std::vector<Value> const& flat) {
        auto values = lifted(context, { type }, flat);
        CHECK(lowered(context, { type }, values) == flat);
        return values;
    };

    // variant{a(f32), b(u64)}: a's f32 travels in the joined i64, bit-cast both ways.
    auto const a = roundTrips(wide, { I32 { 0 }, I64 { bitsOf(1.5F) } });
    CHECK(a.root(0).caseIndex() == 0);
    CHECK(a.root(0).payload()->scalar<float>() == 1.5F);
    auto const b = roundTrips(wide, { I32 { 1 }, I64 { 0x1234'5678'9abc'def0 } });
    CHECK(b.root(0).payload()->scalar<uint64_t>() == 0x1234'5678'9abc'def0);
    // The joined i64 wraps to the f32's 32 bits, and a NaN found there is canonicalized.
    auto const nan = lifted(context, { wide }, { I32 { 0 }, I64 { 0xdead'beef'7fa0'0001 } });
    CHECK(nan.root(0).payload()->scalarBits() == 0x7fc0'0000);
    // variant{a(f32), b(u32)}: an f32 in a joined i32.
    auto const inWord = roundTrips(word, { I32 { 0 }, I32 { bitsOf(-2.25F) } });
    CHECK(inWord.root(0).payload()->scalar<float>() == -2.25F);
    // A shorter case is padded with zeros of the joined types; a case without payload is all zeros.
    auto const x = roundTrips(padded, { I32 { 0 }, I32 { 7 }, F32 { 0 } });
    CHECK(x.root(0).payload()->scalar<uint8_t>() == 7);
    auto const y = roundTrips(padded, { I32 { 1 }, I32 { 3 }, F32 { bitsOf(2.5F) } });
    CHECK(y.root(0).payload()->child(1).scalar<float>() == 2.5F);
    auto const z = roundTrips(padded, { I32 { 2 }, I32 { 0 }, F32 { 0 } });
    CHECK_FALSE(z.root(0).payload().has_value());
    // A variant in an option's payload: its discriminant and its joined slots follow the option's.
    auto const some = roundTrips(nested, { I32 { 1 }, I32 { 0 }, I64 { bitsOf(0.5F) } });
    CHECK(some.root(0).payload()->payload()->scalar<float>() == 0.5F);
    auto const none = roundTrips(nested, { I32 { 0 }, I32 { 0 }, I64 { 0 } });
    CHECK_FALSE(none.root(0).payload().has_value());
    // The outermost variant's joins decide the slots: the inner discriminant travels in an i64 there.
    auto const inner = roundTrips(widened, { I32 { 0 }, I64 { 0 }, F32 { bitsOf(4.0F) } });
    CHECK(inner.root(0).payload()->payload()->scalar<float>() == 4.0F);
    auto const outer = roundTrips(widened, { I32 { 1 }, I64 { 77 }, F32 { 0 } });
    CHECK(outer.root(0).payload()->scalar<uint64_t>() == 77);
    // Padding of a lifted case is skipped, whatever it holds.
    auto const skipped = lifted(context, { padded, u8 }, { I32 { 2 }, I32 { 99 }, F32 { 99 }, I32 { 5 } });
    CHECK(skipped.root(1).scalar<uint8_t>() == 5);
}

TEST_CASE("LiftLower.badDiscriminant")
{
    auto side = Side {};
    auto const u8 = side.primitive(ValueKind::U8);
    auto const enumeration = side.labelled(ValueKind::Enum, { "a", "b" });
    auto const option = side.compound(ValueKind::Option, { u8 });
    auto const result = side.compound(ValueKind::Result, { u8, std::nullopt });
    auto const variant = side.variant({ "a", "b", "c" }, { u8, std::nullopt, u8 });
    auto context = side.context();

    auto const trap =
        trapIn(liftFlat(context, std::vector { enumeration }, std::vector<Value> { I32 { 2 } }, 16));
    CHECK(trap == trapOf(TrapCode::InvalidDiscriminant, 2, 2));
    CHECK(trap.message().starts_with("invalid variant discriminant"));
    CHECK(trapIn(liftFlat(context, std::vector { option }, std::vector<Value> { I32 { 2 }, I32 { 0 } }, 16))
          == trapOf(TrapCode::InvalidDiscriminant, 2, 2));
    CHECK(trapIn(liftFlat(
              context, std::vector { result }, std::vector<Value> { I32 { 0xffff'ffff }, I32 { 0 } }, 16))
          == trapOf(TrapCode::InvalidDiscriminant, 0xffff'ffffU, 2));
    CHECK(trapIn(liftFlat(context, std::vector { variant }, std::vector<Value> { I32 { 3 }, I32 { 0 } }, 16))
          == trapOf(TrapCode::InvalidDiscriminant, 3, 3));
    // In memory, the discriminant is its byte; the last case lifts.
    side.memory().put(0, bytesOf({ 2, 9, 3, 0 }));
    CHECK(trapIn(load(context, std::vector { option }, 0)) == trapOf(TrapCode::InvalidDiscriminant, 2, 2));
    CHECK(trapIn(load(context, std::vector { enumeration }, 0))
          == trapOf(TrapCode::InvalidDiscriminant, 2, 2));
    CHECK(trapIn(load(context, std::vector { variant }, 2)) == trapOf(TrapCode::InvalidDiscriminant, 3, 3));
    auto const last = load(context, std::vector { variant }, 0);
    REQUIRE(last.has_value());
    CHECK(last->root(0).caseIndex() == 2);
    CHECK(last->root(0).payload()->scalar<uint8_t>() == 9);
}

TEST_CASE("LiftLower.recordRoundTripMemory")
{
    auto side = Side {};
    auto const u8 = side.primitive(ValueKind::U8);
    auto const u64 = side.primitive(ValueKind::U64);
    auto const u16 = side.primitive(ValueKind::U16);
    auto const option = side.compound(ValueKind::Option, { u16 });
    auto const record = side.record({ "a", "b", "c" }, { u8, u64, option });
    auto context = side.context();
    auto const values = side.build({ record }, [](ValueBuilder& builder) {
        builder.begin(3).scalar(7).scalar(0x0102'0304'0506'0708).begin(1).scalar(0xbeef).end().end();
    });

    // a at 0, b at 8, c's discriminant at 16 and its u16 at 18: 24 bytes, aligned to 8.
    REQUIRE(store(context, record, values.root(0), 8).has_value());
    auto const bytes = side.memory().bytes();
    CHECK(bytes[8] == std::byte { 7 });
    CHECK(std::ranges::equal(bytes.subspan(16, 8), littleEndian(0x0102'0304'0506'0708, 8)));
    CHECK(std::ranges::equal(bytes.subspan(24, 4), bytesOf({ 1, 0, 0xef, 0xbe })));
    auto const loaded = load(context, std::vector { record }, 8);
    REQUIRE(loaded.has_value());
    CHECK(equalValues(*loaded, values));

    // A pointer not aligned to the record's 8 bytes traps, and so does one whose record passes the end.
    CHECK(trapIn(load(context, std::vector { record }, 4)) == trapOf(TrapCode::UnalignedPointer, 4, 8));
    CHECK(trapIn(store(context, record, values.root(0), 4)) == trapOf(TrapCode::UnalignedPointer, 4, 8));
    auto const end = static_cast<uint32_t>(side.memory().size());
    CHECK(trapIn(load(context, std::vector { record }, end - 16)) == outOfBounds(end - 16, 24, end));
    CHECK(trapIn(store(context, record, values.root(0), end - 16)) == outOfBounds(end - 16, 24, end));
    // Nothing was written by the refused stores.
    CHECK(side.memory().writes().size() == 4);
}

TEST_CASE("LiftLower.lowerAcrossArenas")
{
    // Values built in arena A, lowered against arena B's equal type: the same core values and bytes as
    // values built in B, although the TypeIds differ.
    auto a = Side {};
    auto b = Side {};
    std::ignore = b.compound(ValueKind::Tuple, { b.primitive(ValueKind::U64), b.primitive(ValueKind::F64) });
    auto const recordOf = [](Side& side) {
        auto const option = side.compound(ValueKind::Option, { side.primitive(ValueKind::F32) });
        auto const choice = side.labelled(ValueKind::Enum, { "x", "y", "z" });
        auto const flags = side.labelled(ValueKind::Flags, { "p", "q" });
        return side.record({ "a", "b", "c", "d" }, { side.primitive(ValueKind::U8), option, choice, flags });
    };
    auto const inA = recordOf(a);
    auto const inB = recordOf(b);
    REQUIRE(inA != inB);
    auto const steps = [](ValueBuilder& builder) {
        builder.begin(4).scalar(200).begin(1).scalar(bitsOf(3.5F)).end().scalar(2).scalar(0b10).end();
    };
    auto const fromA = a.build({ inA }, steps);
    auto const fromB = b.build({ inB }, steps);
    auto context = b.context();
    auto const flat = lowered(context, { inB }, fromB);
    CHECK(flat
          == std::vector<Value> { I32 { 200 }, I32 { 1 }, F32 { bitsOf(3.5F) }, I32 { 2 }, I32 { 0b10 } });
    CHECK(lowered(context, { inB }, fromA) == flat);
    REQUIRE(store(context, inB, fromB.root(0), 0).has_value());
    REQUIRE(store(context, inB, fromA.root(0), 64).has_value());
    auto const bytes = b.memory().bytes();
    CHECK(std::ranges::equal(bytes.subspan(0, 16), bytes.subspan(64, 16)));
    CHECK(equalValues(lifted(context, { inB }, flat), fromA));
}

TEST_CASE("LiftLower.flagsIgnoreUnknownBits")
{
    auto side = Side {};
    auto const two = side.labelled(ValueKind::Flags, { "a", "b" });
    auto const ten = side.labelled(ValueKind::Flags, { "a", "b", "c", "d", "e", "f", "g", "h", "i", "j" });
    auto const all =
        side.labelled(ValueKind::Flags, { "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "10",
                                          "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21",
                                          "22", "23", "24", "25", "26", "27", "28", "29", "30", "31" });
    auto context = side.context();
    CHECK(lifted(context, { two }, { I32 { 0b111 } }).root(0).flagBits() == 0b11);
    CHECK(lifted(context, { all }, { I32 { 0xffff'ffff } }).root(0).flagBits() == 0xffff'ffff);
    side.memory().put(0, bytesOf({ 0xff, 0xff, 0xff, 0xff }));
    auto const ten16 = load(context, std::vector { ten }, 0);
    REQUIRE(ten16.has_value());
    CHECK(ten16->root(0).flagBits() == 0x3ff);
    // Stored, the flags take their width: two bytes for ten labels.
    REQUIRE(store(context, ten, ten16->root(0), 8).has_value());
    CHECK(side.memory().writes().back() == MemoryAccess { .address = 8, .length = 2 });
    CHECK(std::ranges::equal(side.memory().bytes().subspan(8, 2), bytesOf({ 0xff, 0x03 })));
}

TEST_CASE("LiftLower.manyResultsUseReturnPointer")
{
    auto side = Side {};
    auto const u32 = side.primitive(ValueKind::U32);
    auto const pair = side.compound(ValueKind::Tuple, { u32, u32 });
    auto context = side.context();
    side.memory().put(16, littleEndian(0x0000'0002'0000'0001, 8));

    // tuple<u32, u32> flattens to two core values, more than MaxFlatResults: the one core value is the
    // tuple's address.
    auto const values = lifted(context, { pair }, { I32 { 16 } }, MaxFlatResults);
    CHECK(values.root(0).child(0).scalar<uint32_t>() == 1);
    CHECK(values.root(0).child(1).scalar<uint32_t>() == 2);
    CHECK(side.memory().reads()
          == std::vector { MemoryAccess { .address = 16, .length = 4 },
                           MemoryAccess { .address = 20, .length = 4 } });
    CHECK(trapIn(liftFlat(context, std::vector { pair }, std::vector<Value> { I32 { 18 } }, MaxFlatResults))
          == trapOf(TrapCode::UnalignedPointer, 18, 4));

    // Lowered with a result pointer, they are written there and no core value is returned.
    CHECK(lowered(context, { pair }, values, MaxFlatResults, 32).empty());
    CHECK(std::ranges::equal(side.memory().bytes().subspan(32, 8), littleEndian(0x0000'0002'0000'0001, 8)));
    CHECK(trapIn(lowerFlat(context, std::vector { pair }, rootsOf(values), MaxFlatResults, 34))
          == trapOf(TrapCode::UnalignedPointer, 34, 4));
    auto const end = static_cast<uint32_t>(side.memory().size());
    CHECK(trapIn(lowerFlat(context, std::vector { pair }, rootsOf(values), MaxFlatResults, end - 4))
          == outOfBounds(end - 4, 8, end));
    // Without one, realloc allocates the tuple and its address is the one core value.
    auto const allocated = lowered(context, { pair }, values, MaxFlatResults);
    REQUIRE(side.memory().reallocations().size() == 1);
    auto const& reallocation = side.memory().reallocations().front();
    CHECK((reallocation.old == 0 && reallocation.oldSize == 0));
    CHECK((reallocation.alignment == 4 && reallocation.newSize == 8));
    CHECK(allocated == std::vector<Value> { I32 { reallocation.result } });
    CHECK(std::ranges::equal(side.memory().bytes().subspan(reallocation.result, 8),
                             littleEndian(0x0000'0002'0000'0001, 8)));
    // A tuple no 32-bit memory holds traps before realloc runs, and before any value is read: 8193 copies
    // of a 512 KiB tuple, each type below the size limit.
    auto wide = DefinedType { .kind = ValueKind::Tuple };
    wide.children.assign(std::size_t { 1 } << 16U, side.primitive(ValueKind::U64));
    auto const huge = side.intern(std::move(wide));
    auto hugeContext = side.context();
    auto const hugeTypes = std::vector<TypeId>((std::size_t { 1 } << 13U) + 1, huge);
    auto const anyValues = std::vector<ValueView>(hugeTypes.size(), values.root(0));
    CHECK(trapIn(lowerFlat(hugeContext, hugeTypes, anyValues, MaxFlatParams, std::nullopt)).code
          == TrapCode::OutOfBounds);
    CHECK(trapIn(lowerFlat(hugeContext, hugeTypes, anyValues, MaxFlatParams, std::nullopt))
          == trapOf(TrapCode::OutOfBounds, "a tuple of 4295491584 bytes, more than any 32-bit memory holds"));
    CHECK(side.memory().reallocations().size() == 1);
    // Seventeen parameters spill past MaxFlatParams the same way; sixteen stay flat.
    auto const sixteen = std::vector<TypeId>(16, u32);
    auto const seventeen = std::vector<TypeId>(17, u32);
    auto const flat16 = std::vector<Value>(16, I32 { 9 });
    CHECK(lowered(context, sixteen, lifted(context, sixteen, flat16)) == flat16);
    side.memory().put(256, std::vector<std::byte>(std::size_t { 17 } * 4, std::byte { 1 }));
    CHECK(lifted(context, seventeen, { I32 { 256 } }).root(16).scalar<uint32_t>() == 0x0101'0101);
}

TEST_CASE("LiftLower.reachesMemoryOnlyByCopies")
{
    // A memory that a realloc grows, by moving it: a view of the old memory kept past reallocate()
    // would read freed storage (ASan).
    auto side = Side { FakeGuestMemoryOptions { .size = 64, .heapStart = 64 } };
    auto const u8 = side.primitive(ValueKind::U8);
    auto const u64 = side.primitive(ValueKind::U64);
    auto const u16 = side.primitive(ValueKind::U16);
    auto const option = side.compound(ValueKind::Option, { u16 });
    auto const record = side.record({ "a", "b", "c" }, { u8, u64, option });
    auto context = side.context();
    side.memory().put(8, bytesOf({ 7, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa }));
    side.memory().put(16, littleEndian(0x0102'0304'0506'0708, 8));
    side.memory().put(24, bytesOf({ 1, 0xaa, 0xef, 0xbe }));

    // One read per scalar, each once, none of padding: a value range is copied once, never re-read.
    auto const values = load(context, std::vector { record }, 8);
    REQUIRE(values.has_value());
    CHECK(side.memory().reads()
          == std::vector { MemoryAccess { .address = 8, .length = 1 },
                           MemoryAccess { .address = 16, .length = 8 },
                           MemoryAccess { .address = 24, .length = 1 },
                           MemoryAccess { .address = 26, .length = 2 } });

    // Seventeen copies spill into a block realloc allocates past the memory's end: it grows (moves),
    // and every write lands in the grown memory.
    auto const types = std::vector<TypeId>(17, record);
    auto const roots = std::vector<ValueView>(17, values->root(0));
    auto const flat = lowerFlat(context, types, roots, MaxFlatParams, std::nullopt);
    REQUIRE(flat.has_value());
    REQUIRE(side.memory().reallocations().size() == 1);
    CHECK(side.memory().reallocations().front().newSize == 17 * 24);
    CHECK(side.memory().size() >= 64 + (17 * 24));
    REQUIRE(flat->size() == 1);
    auto const address = side.memory().reallocations().front().result;
    CHECK(*flat == std::vector<Value> { I32 { address } });
    CHECK(side.memory().reads().size() == 4);
    auto const reloaded = load(context, types, address);
    REQUIRE(reloaded.has_value());
    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 17 }))
        CHECK(equal(reloaded->root(index), values->root(0)));
}

TEST_CASE("LiftLower.handles")
{
    auto side = Side {};
    auto const own = side.handle(ValueKind::Own);
    auto const borrow = side.handle(ValueKind::Borrow);
    auto context = side.context();
    auto const resource = ResourceValue { .type = ResourceTypeIndex { side.resource().value }, .rep = 42 };
    auto const index = side.handles().put(resource);

    // lift_borrow leaves the handle; lift_own removes it, so a second lift is an unknown handle.
    auto const lent = lifted(context, { borrow }, { I32 { index } });
    CHECK(lent.root(0).resource().ownership == Ownership::Borrow);
    CHECK(lent.root(0).resource().rep == 42);
    auto const owned = lifted(context, { own }, { I32 { index } });
    CHECK(owned.root(0).resource() == resource);
    CHECK_FALSE(side.handles().at(index).has_value());
    auto const unknown =
        trapIn(liftFlat(context, std::vector { own }, std::vector<Value> { I32 { index } }, 16));
    CHECK(unknown.message() == "unknown handle index 1");
    side.memory().put(0, littleEndian(5, 4));
    CHECK(trapIn(load(context, std::vector { own }, 0)) == trapOf(TrapCode::UnknownHandle, 5));

    // lower_own adds a handle; its index is the core value, and its slot holds the resource.
    auto const ownIndex = lowered(context, { own }, owned);
    REQUIRE(ownIndex.size() == 1);
    auto const added = std::get<I32>(ownIndex.front()).bits;
    CHECK(side.handles().at(added) == resource);
    REQUIRE(store(context, borrow, lent.root(0), 8).has_value());
    auto const borrowed = side.handles().at(added + 1);
    REQUIRE(borrowed.has_value());
    CHECK(borrowed->ownership == Ownership::Borrow);
    CHECK(std::ranges::equal(side.memory().bytes().subspan(8, 4), littleEndian(added + 1, 4)));
}

TEST_CASE("LiftLower.fixedList")
{
    auto side = Side {};
    auto const u16 = side.primitive(ValueKind::U16);
    auto const list =
        side.intern(DefinedType { .kind = ValueKind::FixedList, .children = { u16 }, .length = 3 });
    auto context = side.context();
    auto const values = lifted(context, { list }, { I32 { 1 }, I32 { 0x1'0002 }, I32 { 3 } });
    CHECK(values.root(0).childCount() == 3);
    CHECK(values.root(0).child(1).scalar<uint16_t>() == 2);
    CHECK(lowered(context, { list }, values) == std::vector<Value> { I32 { 1 }, I32 { 2 }, I32 { 3 } });
    REQUIRE(store(context, list, values.root(0), 2).has_value());
    CHECK(std::ranges::equal(side.memory().bytes().subspan(2, 6), bytesOf({ 1, 0, 2, 0, 3, 0 })));
    auto const loaded = load(context, std::vector { list }, 2);
    REQUIRE(loaded.has_value());
    CHECK(equalValues(*loaded, values));
}

TEST_CASE("LiftLower.liftIsChargedToTheBudget")
{
    // A budget too small for the values: the lift traps LiveValueBytes, with the cap's text, and leaves
    // nothing charged.
    auto side = Side { FakeGuestMemoryOptions {}, 16 };
    auto const u32 = side.primitive(ValueKind::U32);
    auto const triple = side.compound(ValueKind::Tuple, { u32, u32, u32 });
    auto context = side.context();
    auto const trap = trapIn(liftFlat(context, std::vector { triple }, std::vector<Value>(3, I32 { 1 }), 16));
    CHECK(trap == trapOf(TrapCode::LiveValueBytes, 16));
    CHECK(trap.message() == "live value bytes exceed the limit of 16");
    CHECK(side.budget().live() == 0);
}

TEST_CASE("LiftLower.bulkCopiesPoll")
{
    // tuple<u64 × 8200>: 65'600 bytes, one BulkPollBytes granule and a part. Loading it polls once,
    // storing it polls once; an interrupt at the first poll ends the load there.
    constexpr auto Count = std::size_t { 8200 };
    static_assert(Count * 8 > isoline::api::BulkPollBytes && Count * 8 < 2 * isoline::api::BulkPollBytes);
    auto const interruptAt = GENERATE(std::optional<uint32_t> {}, std::optional<uint32_t> { 1 });
    auto side = Side { FakeGuestMemoryOptions { .size = 2 * Count * 8, .interruptAtPoll = interruptAt } };
    auto const u64 = side.primitive(ValueKind::U64);
    auto type = DefinedType { .kind = ValueKind::Tuple };
    type.children.assign(Count, u64);
    auto const tuple = side.intern(std::move(type));
    auto context = side.context();
    auto const values = load(context, std::vector { tuple }, 0);
    if (interruptAt)
    {
        CHECK(trapIn(values) == trapOf(TrapCode::Interrupted));
        CHECK(side.memory().polls() == 1);
        CHECK(side.memory().reads().size() == isoline::api::BulkPollBytes / 8);
        CHECK(side.budget().live() == 0);
        return;
    }
    REQUIRE(values.has_value());
    CHECK(side.memory().polls() == 1);
    REQUIRE(store(context, tuple, values->root(0), Count * 8).has_value());
    CHECK(side.memory().polls() == 2);
}

TEST_CASE("Lists.roundTrip")
{
    // list<record{a: u8, b: string}>, flat and in memory: realloc'd blocks in order, read back equal.
    auto side = Side {};
    auto const u8 = side.primitive(ValueKind::U8);
    auto const string = side.primitive(ValueKind::String);
    auto const record = side.record({ "a", "b" }, { u8, string });
    auto const list = side.compound(ValueKind::List, { record });
    auto const values = side.build({ list }, [](ValueBuilder& builder) {
        builder.begin(2).begin(2).scalar(1).string("hi").end().begin(2).scalar(2).string("").end().end();
    });
    auto context = side.context();
    auto const flat = lowered(context, { list }, values);
    REQUIRE(flat.size() == 2);
    // The list's block first, then each element's string, as store_list_into_range allocates them.
    auto const& reallocations = side.memory().reallocations();
    REQUIRE(reallocations.size() == 3);
    CHECK(reallocations[0].newSize == 24);
    CHECK(reallocations[0].alignment == 4);
    CHECK(reallocations[1].newSize == 2);
    CHECK(reallocations[2].newSize == 0);
    CHECK(flat == std::vector<Value> { I32 { reallocations[0].result }, I32 { 2 } });
    CHECK(equalValues(lifted(context, { list }, flat), values));

    // In memory: the pointer pair at its address, the elements in the block.
    REQUIRE(store(context, list, values.root(0), 8).has_value());
    auto const reloaded = load(context, std::vector { list }, 8);
    REQUIRE(reloaded.has_value());
    CHECK(equalValues(*reloaded, values));

    // An empty list still calls realloc, with (0, 0, alignment, 0) (test/values/realloc.wast).
    auto const empty = side.build({ list }, [](ValueBuilder& builder) { builder.begin(0).end(); });
    auto const before = side.memory().reallocations().size();
    auto const none = lowered(context, { list }, empty);
    REQUIRE(side.memory().reallocations().size() == before + 1);
    CHECK(side.memory().reallocations().back()
          == isoline::component::canonical::testing::Reallocation {
              .old = 0,
              .oldSize = 0,
              .alignment = 4,
              .newSize = 0,
              .result = side.memory().reallocations().back().result });
    CHECK(equalValues(lifted(context, { list }, none), empty));
}

TEST_CASE("Lists.inVariantFlat")
{
    // A list in a flat variant: its pointer pair travels in the variant's joined slots, which the other
    // case's payload (an i64) widens.
    auto side = Side {};
    auto const u64 = side.primitive(ValueKind::U64);
    auto const u16 = side.primitive(ValueKind::U16);
    auto const list = side.compound(ValueKind::List, { u16 });
    auto const variant = side.variant({ "l", "n" }, { list, u64 });
    auto const values = side.build({ variant }, [](ValueBuilder& builder) {
        builder.begin(0).begin(3).scalar(7).scalar(8).scalar(9).end().end();
    });
    auto context = side.context();
    auto const flat = lowered(context, { variant }, values);
    REQUIRE(flat.size() == 3);
    CHECK(std::holds_alternative<I64>(flat[1]));
    CHECK(std::get<I32>(flat[2]).bits == 3);
    CHECK(equalValues(lifted(context, { variant }, flat), values));
}

TEST_CASE("Lists.scalarsInOneCopy")
{
    // A list of scalars is one read when lifted and one write when lowered (up to BlockWriter::BufferBytes),
    // not one per element; each element still lifts by its rule: a bool is any nonzero byte, a NaN is
    // canonicalized, a char traps on a surrogate.
    auto side = Side {};
    auto const u32 = side.primitive(ValueKind::U32);
    auto const list = side.compound(ValueKind::List, { u32 });
    auto words = std::vector<std::byte> {};
    for (auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 1000 }))
    {
        auto const bytes = littleEndian(uint64_t { index } * 3, 4);
        words.insert(words.end(), bytes.begin(), bytes.end());
    }
    side.memory().put(0, words);
    auto context = side.context();
    auto const values = lifted(context, { list }, { I32 { 0 }, I32 { 1000 } });
    CHECK(side.memory().reads() == std::vector { MemoryAccess { .address = 0, .length = 4000 } });
    CHECK(values.root(0).childCount() == 1000);
    CHECK(values.root(0).child(999).scalar<uint32_t>() == 2997);
    auto const flat = lowered(context, { list }, values);
    REQUIRE(side.memory().writes().size() == 1);
    CHECK(side.memory().writes().front().length == 4000);
    CHECK(std::ranges::equal(side.memory().bytes().subspan(std::get<I32>(flat[0]).bits, 4000), words));

    auto const boolList = side.compound(ValueKind::List, { side.primitive(ValueKind::Bool) });
    auto const f32List = side.compound(ValueKind::List, { side.primitive(ValueKind::F32) });
    auto const s16List = side.compound(ValueKind::List, { side.primitive(ValueKind::S16) });
    auto const charList = side.compound(ValueKind::List, { side.primitive(ValueKind::Char) });
    auto scalars = side.context();
    side.memory().put(0, bytesOf({ 0, 1, 2, 0xff }));
    auto const bools = lifted(scalars, { boolList }, { I32 { 0 }, I32 { 4 } });
    CHECK(bools.root(0).child(2).scalar<bool>());
    CHECK(bools.root(0).child(2).scalarBits() == 1);
    CHECK_FALSE(bools.root(0).child(0).scalar<bool>());
    side.memory().put(0, littleEndian(0x7fc0'0001'7f80'0001, 8));
    auto const floats = lifted(scalars, { f32List }, { I32 { 0 }, I32 { 2 } });
    CHECK(floats.root(0).child(0).scalarBits() == 0x7fc0'0000);
    CHECK(floats.root(0).child(1).scalarBits() == 0x7fc0'0000);
    side.memory().put(0, littleEndian(0x8000'ffff, 4));
    auto const shorts = lifted(scalars, { s16List }, { I32 { 0 }, I32 { 2 } });
    CHECK(shorts.root(0).child(0).scalar<int16_t>() == -1);
    CHECK(shorts.root(0).child(1).scalar<int16_t>() == -32768);
    auto const loweredShorts = lowered(scalars, { s16List }, shorts);
    CHECK(std::ranges::equal(side.memory().bytes().subspan(std::get<I32>(loweredShorts[0]).bits, 4),
                             littleEndian(0x8000'ffff, 4)));
    side.memory().put(0, littleEndian(0x0000'd800'0000'0041, 8));
    CHECK(trapIn(liftFlat(scalars, std::vector { charList }, std::vector<Value> { I32 { 0 }, I32 { 2 } }, 16))
          == trapOf(TrapCode::InvalidChar, "0xd800"));
    CHECK(side.budget().live()
          == bools.hostBytes() + floats.hostBytes() + shorts.hostBytes() + values.hostBytes());
}

TEST_CASE("Lists.lyingLengthTrapsBeforeAllocation")
{
    // Lengths that lie, in a budget of 4 KiB: refused before anything is read or charged.
    auto side = Side { FakeGuestMemoryOptions { .size = std::size_t { 64 } * 1024 }, 4096 };
    auto const string = side.primitive(ValueKind::String);
    auto const u8 = side.primitive(ValueKind::U8);
    auto const strings = side.compound(ValueKind::List, { string });
    auto const bytes = side.compound(ValueKind::List, { u8 });
    auto context = side.context();

    // list<string> of 2^28 - 1: 8 bytes each, past MaxListByteLength (in 64-bit, no wrap).
    auto const tooLong =
        trapIn(liftFlat(context,
                        std::vector { strings },
                        std::vector<Value> { I32 { 0 }, I32 { (uint32_t { 1 } << 28U) - 1 } },
                        16));
    CHECK(tooLong.code == TrapCode::OutOfBounds);
    // 2^29 elements of 8 bytes is 2^32 bytes: 0 in 32-bit arithmetic, refused all the same.
    CHECK(trapIn(liftFlat(context,
                          std::vector { strings },
                          std::vector<Value> { I32 { 0 }, I32 { uint32_t { 1 } << 29U } },
                          16))
              .code
          == TrapCode::OutOfBounds);
    // list<u8> of 2^28 - 1: within MaxListByteLength, past the memory's end.
    CHECK(trapIn(liftFlat(context,
                          std::vector { bytes },
                          std::vector<Value> { I32 { 0 }, I32 { (uint32_t { 1 } << 28U) - 1 } },
                          16))
          == trapOf(TrapCode::OutOfBounds,
                    std::format("{} bytes at 0 pass its end at 65536", (1U << 28U) - 1)));
    // Misaligned: a list<string> is 4-aligned.
    CHECK(trapIn(liftFlat(context, std::vector { strings }, std::vector<Value> { I32 { 2 }, I32 { 1 } }, 16))
          == trapOf(TrapCode::UnalignedPointer, 2, 4));
    CHECK(side.memory().reads().empty());

    // In a gibibyte of memory, 2^28 - 1 bytes pass the bounds: the budget refuses them before the host
    // allocates or reads them.
    auto memory = ZeroGuestMemory { uint64_t { 1 } << 30U, 0 };
    auto big = side.contextOver(memory);
    CHECK(trapIn(liftFlat(big,
                          std::vector { bytes },
                          std::vector<Value> { I32 { 0 }, I32 { (uint32_t { 1 } << 28U) - 1 } },
                          16))
          == trapOf(TrapCode::LiveValueBytes, 4096));
    CHECK(trapIn(liftFlat(big,
                          std::vector { strings },
                          std::vector<Value> { I32 { 0 }, I32 { (uint32_t { 1 } << 25U) - 1 } },
                          16))
          == trapOf(TrapCode::LiveValueBytes, 4096));
    // 2^28 bytes lie in it too, but pass MaxListByteLength: refused for their length, not the budget.
    CHECK(trapIn(liftFlat(big,
                          std::vector { bytes },
                          std::vector<Value> { I32 { 0 }, I32 { uint32_t { 1 } << 28U } },
                          16))
              .code
          == TrapCode::OutOfBounds);
    CHECK(memory.readBytes() == 0);
    CHECK(side.budget().live() == 0);
}

TEST_CASE("Lists.hostListPastAnyMemoryTraps")
{
    // A host list of 33 values of a 2^27-byte variant, each its payload-less case: a few nodes, but 2^32
    // bytes and more in memory, which no `realloc` can return. Refused before realloc is called.
    auto side = Side {};
    auto const huge = side.intern(DefinedType { .kind = ValueKind::FixedList,
                                                .children = { side.primitive(ValueKind::U8) },
                                                .length = uint32_t { 1 } << 27U });
    auto const variant = side.variant({ "none", "huge" }, { std::nullopt, huge });
    auto const list = side.compound(ValueKind::List, { variant });
    auto const values = side.build({ list }, [](ValueBuilder& builder) {
        builder.begin(33);
        for ([[maybe_unused]] auto const element: std::views::iota(0, 33))
            builder.begin(0).end();
        builder.end();
    });
    auto context = side.context();
    CHECK(trapIn(lowerFlat(context, std::vector { list }, rootsOf(values), 16, std::nullopt)).code
          == TrapCode::OutOfBounds);
    CHECK(side.memory().reallocations().empty());
}

TEST_CASE("Lists.nestedAmplificationIsCharged")
{
    // list<list<u8>>: 1000 inner lists, each the same 200 MiB of a 256 MiB memory. Each fits the budget
    // alone; together they would take 200 GB. The store-wide budget refuses the second, and the lift
    // traps LiveValueBytes instead of exhausting the host. Run scaled down, and at full size.
    struct Scale
    {
        uint32_t inner;
        uint64_t budget;
    };
    auto const scale = GENERATE(values<Scale>({
        { 200 * 1024, uint64_t { 256 } * 1024 },
        { 200U << 20U, uint64_t { 256 } << 20U },
    }));
    constexpr auto Count = uint32_t { 1000 };
    auto memory = ZeroGuestMemory { uint64_t { 256 } << 20U, std::size_t { 8 } * Count };
    auto side = Side { FakeGuestMemoryOptions {}, scale.budget };
    auto const inner = side.compound(ValueKind::List, { side.primitive(ValueKind::U8) });
    auto const outer = side.compound(ValueKind::List, { inner });
    for (auto const index: std::views::iota(uint32_t { 0 }, Count))
    {
        memory.put(8 * index, littleEndian(uint64_t { 8 } * Count, 4));
        memory.put((8 * index) + 4, littleEndian(scale.inner, 4));
    }
    auto context = side.contextOver(memory);
    auto const trap =
        trapIn(liftFlat(context, std::vector { outer }, std::vector<Value> { I32 { 0 }, I32 { Count } }, 16));
    CHECK(trap == trapOf(TrapCode::LiveValueBytes, scale.budget));
    // Two pointer pairs and one inner list were read: the second inner list was refused before its read.
    CHECK(memory.readBytes() == 16 + scale.inner);
    CHECK(side.budget().live() == 0);
}

TEST_CASE("Lists.mapIsListOfTuples")
{
    // map<string, u32> {"a": 1} is stored as list<tuple<string, u32>> [("a", 1)] is: the same bytes and
    // the same `realloc` calls; and it lifts back as the map.
    auto mapSide = Side {};
    auto const mapType = mapSide.compound(
        ValueKind::Map, { mapSide.primitive(ValueKind::String), mapSide.primitive(ValueKind::U32) });
    auto const map = mapSide.build(
        { mapType }, [](ValueBuilder& builder) { builder.begin(1).string("a").scalar(1).end(); });
    auto listSide = Side {};
    auto const tuple = listSide.compound(
        ValueKind::Tuple, { listSide.primitive(ValueKind::String), listSide.primitive(ValueKind::U32) });
    auto const listType = listSide.compound(ValueKind::List, { tuple });
    auto const list = listSide.build({ listType }, [](ValueBuilder& builder) {
        builder.begin(1).begin(2).string("a").scalar(1).end().end();
    });
    auto mapContext = mapSide.context();
    auto listContext = listSide.context();
    REQUIRE(store(mapContext, mapType, map.root(0), 0).has_value());
    REQUIRE(store(listContext, listType, list.root(0), 0).has_value());
    CHECK(mapSide.memory().reallocations() == listSide.memory().reallocations());
    CHECK(std::ranges::equal(mapSide.memory().bytes(), listSide.memory().bytes()));
    auto const back = load(mapContext, std::vector { mapType }, 0);
    REQUIRE(back.has_value());
    CHECK(equalValues(*back, map));
    CHECK(lowered(mapContext, { mapType }, map).size() == 2);
}

TEST_CASE("LiftLower.pollsAcrossCalls")
{
    // One BulkPolls per canonical call: two loads of 40 KiB each poll once together, where a counter per
    // load would never reach a granule.
    constexpr auto Bytes = uint32_t { 40 * 1024 };
    auto side = Side { FakeGuestMemoryOptions { .size = std::size_t { 2 } * Bytes } };
    auto const list = side.compound(ValueKind::List, { side.primitive(ValueKind::U8) });
    side.memory().put(0, littleEndian(16, 4));
    side.memory().put(4, littleEndian(Bytes, 4));
    auto context = side.context();
    REQUIRE(load(context, std::vector { list }, 0).has_value());
    CHECK(side.memory().polls() == 0);
    REQUIRE(load(context, std::vector { list }, 0).has_value());
    CHECK(side.memory().polls() == 1);
    // A string counts towards the same granule.
    auto const string = side.primitive(ValueKind::String);
    auto fresh = side.context();
    REQUIRE(load(fresh, std::vector { list }, 0).has_value());
    REQUIRE(load(fresh, std::vector { string }, 0).has_value());
    CHECK(side.memory().polls() == 2);
}

TEST_CASE("LiftLower.neverAborts")
{
    // Whatever core values a guest passes, a lift of a string, list, map, stream, future or error
    // context returns values or a trap: no input reaches an abort. Streams, futures and error contexts
    // have no handle before P12 (validation refuses their types), so any index is unknown.
    auto side = Side { FakeGuestMemoryOptions { .size = 4096 } };
    auto const string = side.primitive(ValueKind::String);
    auto const u8 = side.primitive(ValueKind::U8);
    auto const types = std::vector<TypeId> {
        string,
        side.compound(ValueKind::List, { u8 }),
        side.compound(ValueKind::List, { string }),
        side.compound(ValueKind::List, { side.primitive(ValueKind::Char) }),
        side.compound(ValueKind::Map, { string, u8 }),
        side.primitive(ValueKind::ErrorContext),
        side.compound(ValueKind::Stream, { u8 }),
        side.compound(ValueKind::Future, {}),
    };
    auto const words =
        std::vector<uint32_t> { 0,    1,           2,           3,           4095,        4096,
                                4097, 0x7fff'ffff, 0x8000'0000, 0x8000'0001, 0xffff'fff0, 0xffff'ffff };
    // Memory that reads as pointer pairs into itself, at every offset.
    for (auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 1024 }))
        side.memory().put(4 * index, littleEndian(words[index % words.size()], 4));
    auto const encoding = GENERATE(StringEncoding::Utf8, StringEncoding::Utf16, StringEncoding::Latin1Utf16);
    auto context = side.context(encoding);
    auto outcomes = std::size_t { 0 };
    for (auto const type: types)
    {
        auto const& layout = context.layouts[type.value];
        for (auto const first: words)
            for (auto const second: words)
            {
                auto flat = std::vector<Value> { I32 { first } };
                if (layout.flat.size() == 2)
                    flat.emplace_back(I32 { second });
                auto const values = liftFlat(context, std::vector { type }, flat, 16);
                outcomes += values.has_value() ? 1 : 0;
            }
    }
    CHECK(outcomes > 0);
    for (auto const index: std::views::iota(std::size_t { 5 }, types.size()))
        CHECK(trapIn(liftFlat(context, std::vector { types[index] }, std::vector<Value> { I32 { 5 } }, 16))
              == trapOf(TrapCode::UnknownHandle, 5));
}

TEST_CASE("Lists.reallocMisbehaviour")
{
    // test/values/realloc.wast: a list's block is checked like a string's, even an empty one.
    struct Case
    {
        uint32_t forced;
        uint32_t count;
        TrapCode code;
    };
    auto const misbehaving = GENERATE(values<Case>({
        { 0xffff'ffff, 2, TrapCode::UnalignedPointer },
        { 0xffff'fffc, 2, TrapCode::OutOfBounds },
        { 0xffff'fffc, 0, TrapCode::OutOfBounds },
        { 4094, 1, TrapCode::UnalignedPointer },
        { 4096, 1, TrapCode::OutOfBounds },
    }));
    auto side = Side { FakeGuestMemoryOptions { .forcedResults = { misbehaving.forced } } };
    auto const list = side.compound(ValueKind::List, { side.primitive(ValueKind::U32) });
    auto const values = side.build({ list }, [&](ValueBuilder& builder) {
        builder.begin(misbehaving.count);
        for ([[maybe_unused]] auto const element: std::views::iota(uint32_t { 0 }, misbehaving.count))
            builder.scalar(1);
        builder.end();
    });
    auto context = side.context();
    CHECK(trapIn(lowerFlat(context, std::vector { list }, rootsOf(values), 16, std::nullopt)).code
          == misbehaving.code);
    CHECK(side.memory().writes().empty());
}

TEST_CASE("Lists.pairWrittenAfterElements")
{
    // definitions.py's store_list writes a list's pointer pair after its elements. A `realloc` that hands
    // out the pair's own bytes as an element's string block sees that order: the pair wins.
    auto side = Side { FakeGuestMemoryOptions { .forcedResults = { 256, 64 } } };
    auto const strings = side.compound(ValueKind::List, { side.primitive(ValueKind::String) });
    auto const record = side.record({ "l" }, { strings });
    auto const values = side.build(
        { record }, [](ValueBuilder& builder) { builder.begin(1).begin(1).string("abcdefgh").end().end(); });
    auto context = side.context();
    REQUIRE(store(context, record, values.root(0), 64).has_value());
    CHECK(std::ranges::equal(side.memory().bytes().subspan(64, 8), bytesOf({ 0, 1, 0, 0, 1, 0, 0, 0 })));
    CHECK(std::ranges::equal(side.memory().bytes().subspan(256, 8), bytesOf({ 64, 0, 0, 0, 8, 0, 0, 0 })));
}

TEST_CASE("LiftLower.everyFailurePointTraps")
{
    // A record of every kind that reaches memory, spilled through a realloc'd tuple: lowered, then lifted
    // back. A trap at any poll, a failing read or write, a misbehaving `realloc`, or a full handle table
    // ends the call in that trap, at every point it can happen.
    auto build = [](Side& side) {
        auto const u8 = side.primitive(ValueKind::U8);
        auto const u16 = side.primitive(ValueKind::U16);
        auto const string = side.primitive(ValueKind::String);
        auto const fields = std::vector<std::optional<TypeId>> {
            u8,
            string,
            side.compound(ValueKind::List, { u16 }),
            side.compound(ValueKind::List, { string }),
            side.compound(ValueKind::Map, { string, u8 }),
            side.variant({ "x", "y" },
                         { side.compound(ValueKind::List, { u8 }), side.primitive(ValueKind::U32) }),
            side.compound(ValueKind::Option, { string }),
            side.handle(ValueKind::Own),
            side.labelled(ValueKind::Enum, { "p", "q" }),
            side.labelled(ValueKind::Flags, { "f", "g" }),
            side.compound(ValueKind::List, { side.primitive(ValueKind::Char) }),
        };
        return side.record({ "a", "s", "l", "ls", "m", "v", "o", "h", "e", "f", "c" }, fields);
    };
    static constexpr auto Shorts = uint32_t { 100'000 };
    auto const steps = [](ValueBuilder& builder, ResourceValue resource) {
        builder.begin(11).scalar(7).string("hello");
        builder.begin(Shorts);
        for (auto const index: std::views::iota(uint32_t { 0 }, Shorts))
            builder.scalar(index & 0xFFFFU);
        builder.end();
        builder.begin(2).string("a").string("bc").end();
        builder.begin(1).string("k").scalar(3).end();
        builder.begin(0).begin(3).scalar(1).scalar(2).scalar(3).end().end();
        builder.begin(1).string("some").end();
        builder.resource(resource).scalar(1).scalar(0b11);
        builder.begin(2).scalar('x').scalar(0x1f600).end();
        builder.end();
    };

    // The clean run: how many polls, reads, writes and `realloc` calls each half makes.
    auto clean = Side {};
    auto const record = build(clean);
    auto const resource = ResourceValue { .type = ResourceTypeIndex { clean.resource().value }, .rep = 9 };
    auto const values = clean.build({ record }, [&](ValueBuilder& builder) { steps(builder, resource); });
    auto lowering = clean.context();
    auto const spilled =
        lowerFlat(lowering, std::vector { record }, rootsOf(values), MaxFlatParams, std::nullopt);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->size() == 1);
    auto const lowerPolls = clean.memory().polls();
    auto const lowerWrites = static_cast<uint32_t>(clean.memory().writes().size());
    auto const reallocations = static_cast<uint32_t>(clean.memory().reallocations().size());
    auto const image = std::vector<std::byte>(clean.memory().bytes().begin(), clean.memory().bytes().end());
    auto lifting = clean.context();
    auto const lifted = liftFlat(lifting, std::vector { record }, *spilled, MaxFlatParams);
    REQUIRE(lifted.has_value());
    CHECK(equalValues(*lifted, values));
    auto const liftPolls = clean.memory().polls() - lowerPolls;
    auto const liftReads = static_cast<uint32_t>(clean.memory().reads().size());
    REQUIRE(lowerPolls >= 2);
    REQUIRE(liftPolls >= 2);

    auto const lowersTrap = [&](FakeGuestMemoryOptions options) {
        auto side = Side { std::move(options) };
        auto const type = build(side);
        auto const own = side.build({ type }, [&](ValueBuilder& builder) { steps(builder, resource); });
        auto context = side.context();
        return trapIn(lowerFlat(context, std::vector { type }, rootsOf(own), MaxFlatParams, std::nullopt));
    };
    for (auto const poll: std::views::iota(uint32_t { 1 }, lowerPolls + 1))
        CHECK(lowersTrap(FakeGuestMemoryOptions { .interruptAtPoll = poll })
              == trapOf(TrapCode::Interrupted));
    for (auto const write: std::views::iota(uint32_t { 1 }, lowerWrites + 1))
        CHECK(lowersTrap(FakeGuestMemoryOptions { .failAtWrite = write }).code == TrapCode::OutOfBounds);
    for (auto const call: std::views::iota(uint32_t { 1 }, reallocations + 1))
        CHECK(lowersTrap(FakeGuestMemoryOptions { .misbehaveAtReallocation = call }).code
              == TrapCode::OutOfBounds);
    {
        auto full = Side {};
        auto const type = build(full);
        auto const own = full.build({ type }, [&](ValueBuilder& builder) { steps(builder, resource); });
        full.handles().refuseLowering(trapOf(TrapCode::Handles, 1));
        auto context = full.context();
        CHECK(trapIn(lowerFlat(context, std::vector { type }, rootsOf(own), MaxFlatParams, std::nullopt))
              == trapOf(TrapCode::Handles, 1));
    }

    auto const liftsTrap = [&](FakeGuestMemoryOptions options) {
        options.size = image.size();
        auto side = Side { std::move(options) };
        auto const type = build(side);
        side.memory().put(0, image);
        std::ignore = side.handles().put(resource);
        auto context = side.context();
        return trapIn(liftFlat(context, std::vector { type }, *spilled, MaxFlatParams));
    };
    for (auto const poll: std::views::iota(uint32_t { 1 }, liftPolls + 1))
        CHECK(liftsTrap(FakeGuestMemoryOptions { .interruptAtPoll = poll }) == trapOf(TrapCode::Interrupted));
    for (auto const read: std::views::iota(uint32_t { 1 }, liftReads + 1))
        CHECK(liftsTrap(FakeGuestMemoryOptions { .failAtRead = read }).code == TrapCode::OutOfBounds);

    // A stream, future or error context read from memory: a failing read ends it before the handle.
    auto side = Side { FakeGuestMemoryOptions { .failAtRead = 1 } };
    auto const errorContext = side.primitive(ValueKind::ErrorContext);
    auto context = side.context();
    CHECK(trapIn(load(context, std::vector { errorContext }, 0)).code == TrapCode::OutOfBounds);
}

TEST_CASE("LiftLower.budgetRefusesTheRoots")
{
    // A budget that cannot hold the roots' index refuses the lift at its first step.
    auto side = Side { FakeGuestMemoryOptions {}, 0 };
    auto const u8 = side.primitive(ValueKind::U8);
    auto context = side.context();
    CHECK(trapIn(liftFlat(context, std::vector { u8 }, std::vector<Value> { I32 { 1 } }, 16))
          == trapOf(TrapCode::LiveValueBytes, 0));
}
