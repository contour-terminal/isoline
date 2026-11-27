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
#include <isoline/component/types/TypeArena.hpp>

using isoline::api::F32;
using isoline::api::F64;
using isoline::api::I32;
using isoline::api::I64;
using isoline::api::Value;
using isoline::component::ComponentLimits;
using isoline::component::canonical::computeLayouts;
using isoline::component::canonical::equal;
using isoline::component::canonical::LayoutTable;
using isoline::component::canonical::LiftContext;
using isoline::component::canonical::liftFlat;
using isoline::component::canonical::LiveBytesBudget;
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
using isoline::component::canonical::testing::FakeGuestMemory;
using isoline::component::canonical::testing::FakeGuestMemoryOptions;
using isoline::component::canonical::testing::FakeHandleOps;
using isoline::component::canonical::testing::MemoryAccess;
using isoline::component::types::DefinedType;
using isoline::component::types::ResourceTypeId;
using isoline::component::types::ResourceTypeInfo;
using isoline::component::types::SharedName;
using isoline::component::types::TypeArena;
using isoline::component::types::TypeId;
using isoline::component::types::ValueKind;

namespace
{

/// Ample for every test that is not about the budget.
constexpr auto AmpleBudget = uint64_t { 1 } << 30U;

/// The bits of @p value.
[[nodiscard]] uint32_t bitsOf(float value)
{
    return std::bit_cast<uint32_t>(value);
}

/// The labels @p names.
[[nodiscard]] std::vector<SharedName> labelsOf(std::initializer_list<char const*> names)
{
    auto labels = std::vector<SharedName> {};
    for (auto const* name: names)
        labels.emplace_back(name);
    return labels;
}

/// @p values as bytes.
[[nodiscard]] std::vector<std::byte> bytesOf(std::initializer_list<unsigned> values)
{
    auto bytes = std::vector<std::byte> {};
    for (auto const value: values)
        bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

/// The little-endian bytes of @p value, @p width of them.
[[nodiscard]] std::vector<std::byte> littleEndian(uint64_t value, std::size_t width)
{
    auto bytes = std::vector<std::byte> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, width))
        bytes.push_back(static_cast<std::byte>((value >> (8U * index)) & 0xFFU));
    return bytes;
}

/// The trap @p result failed with.
template <typename T>
[[nodiscard]] Trap trapIn(std::expected<T, Trap> const& result)
{
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// Types of one arena, a memory, a handle table and a budget: one side of a call.
class Side
{
  public:
    explicit Side(FakeGuestMemoryOptions memory = {}, uint64_t budget = AmpleBudget):
        _arena { ComponentLimits {} },
        _memory { memory },
        _budget { std::make_shared<LiveBytesBudget>(budget) }
    {
    }

    [[nodiscard]] TypeId primitive(ValueKind kind) const { return _arena.primitive(kind); }

    [[nodiscard]] TypeId intern(DefinedType type)
    {
        auto const id = _arena.intern(std::move(type));
        REQUIRE(id.has_value());
        return *id;
    }

    [[nodiscard]] TypeId compound(ValueKind kind, std::vector<std::optional<TypeId>> children)
    {
        return intern(DefinedType { .kind = kind, .children = std::move(children) });
    }

    [[nodiscard]] TypeId record(std::initializer_list<char const*> labels,
                                std::vector<std::optional<TypeId>> fields)
    {
        return intern(DefinedType {
            .kind = ValueKind::Record, .children = std::move(fields), .labels = labelsOf(labels) });
    }

    [[nodiscard]] TypeId variant(std::initializer_list<char const*> labels,
                                 std::vector<std::optional<TypeId>> payloads)
    {
        return intern(DefinedType {
            .kind = ValueKind::Variant, .children = std::move(payloads), .labels = labelsOf(labels) });
    }

    [[nodiscard]] TypeId labelled(ValueKind kind, std::initializer_list<char const*> labels)
    {
        return intern(DefinedType { .kind = kind, .labels = labelsOf(labels) });
    }

    [[nodiscard]] TypeId handle(ValueKind kind)
    {
        if (!_resource)
        {
            auto const resource = _arena.newResource(ResourceTypeInfo {});
            REQUIRE(resource.has_value());
            _resource = *resource;
        }
        return intern(DefinedType { .kind = kind, .resource = _resource });
    }

    [[nodiscard]] ResourceTypeId resource() const
    {
        REQUIRE(_resource.has_value());
        return *_resource;
    }

    /// A context lifting and lowering on this side, its layouts computed for every type so far.
    [[nodiscard]] LiftContext context()
    {
        auto layouts = computeLayouts(_arena);
        REQUIRE(layouts.has_value());
        // Every context keeps its table: a later one, laid out after more types, leaves it valid.
        auto const& table = *_layouts.emplace_back(std::make_unique<LayoutTable>(std::move(*layouts)));
        return LiftContext { .arena = _arena,
                             .layouts = table.all(),
                             .memory = &_memory,
                             .encoding = StringEncoding::Utf8,
                             .budget = _budget,
                             .handles = _handles };
    }

    /// The values @p steps build for @p roots; they must build.
    [[nodiscard]] Values build(std::vector<TypeId> const& roots,
                               std::function<void(ValueBuilder&)> const& steps)
    {
        auto builder = ValueBuilder { _arena, roots, _budget };
        steps(builder);
        auto values = builder.finish();
        if (!values)
            FAIL(values.error().message());
        REQUIRE(values.has_value());
        return std::move(*values);
    }

    [[nodiscard]] FakeGuestMemory& memory() noexcept { return _memory; }
    [[nodiscard]] FakeHandleOps& handles() noexcept { return _handles; }
    [[nodiscard]] LiveBytesBudget const& budget() const noexcept { return *_budget; }

  private:
    TypeArena _arena;
    FakeGuestMemory _memory;
    FakeHandleOps _handles {};
    std::shared_ptr<LiveBytesBudget> _budget;
    std::vector<std::unique_ptr<LayoutTable>> _layouts {};
    std::optional<ResourceTypeId> _resource {};
};

/// Every root of @p values.
[[nodiscard]] std::vector<ValueView> rootsOf(Values const& values)
{
    auto roots = std::vector<ValueView> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, values.rootCount()))
        roots.push_back(values.root(index));
    return roots;
}

/// The values @p flat lifts to as @p types; they must lift.
[[nodiscard]] Values lifted(LiftContext& context,
                            std::vector<TypeId> const& types,
                            std::vector<Value> const& flat,
                            uint32_t maxFlat = MaxFlatParams)
{
    auto values = liftFlat(context, types, flat, maxFlat);
    if (!values)
        FAIL(values.error().message());
    REQUIRE(values.has_value());
    return std::move(*values);
}

/// The core values @p values lower to as @p types; they must lower.
[[nodiscard]] std::vector<Value> lowered(LiftContext& context,
                                         std::vector<TypeId> const& types,
                                         Values const& values,
                                         uint32_t maxFlat = MaxFlatParams,
                                         std::optional<uint32_t> resultPointer = {})
{
    auto flat = lowerFlat(context, types, rootsOf(values), maxFlat, resultPointer);
    if (!flat)
        FAIL(flat.error().message());
    REQUIRE(flat.has_value());
    return std::move(*flat);
}

/// Whether @p lhs and @p rhs hold equal roots.
[[nodiscard]] bool equalValues(Values const& lhs, Values const& rhs)
{
    if (lhs.rootCount() != rhs.rootCount())
        return false;
    return std::ranges::all_of(std::views::iota(std::size_t { 0 }, lhs.rootCount()),
                               [&](std::size_t index) { return equal(lhs.root(index), rhs.root(index)); });
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
