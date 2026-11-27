// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/types/TypeArena.hpp>

using isoline::component::ComponentLimits;
using isoline::component::binary::CoreValueKind;
using isoline::component::canonical::computeLayouts;
using isoline::component::canonical::CoreSignature;
using isoline::component::canonical::FlatShape;
using isoline::component::canonical::FlattenContext;
using isoline::component::canonical::flattenFunction;
using isoline::component::canonical::LayoutError;
using isoline::component::canonical::LayoutErrorCode;
using isoline::component::canonical::layoutMessage;
using isoline::component::canonical::LayoutTable;
using isoline::component::canonical::MaxFlagLabels;
using isoline::component::canonical::MaxFlatParams;
using isoline::component::canonical::MaxValueByteSize;
using isoline::component::canonical::PointerContent;
using isoline::component::types::DefinedType;
using isoline::component::types::FunctionTypeInfo;
using isoline::component::types::SharedName;
using isoline::component::types::TypeArena;
using isoline::component::types::TypeId;
using isoline::component::types::ValueKind;

namespace
{
constexpr auto I32 = CoreValueKind::I32;
constexpr auto I64 = CoreValueKind::I64;
constexpr auto F32 = CoreValueKind::F32;
constexpr auto F64 = CoreValueKind::F64;

/// The labels "l0" … "l<count - 1>".
[[nodiscard]] std::vector<SharedName> labelsOf(std::size_t count)
{
    auto labels = std::vector<SharedName> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        labels.emplace_back(std::format("l{}", index));
    return labels;
}

/// Builds types in one arena and lays them out.
class Types
{
  public:
    explicit Types(ComponentLimits const& limits = ComponentLimits {}): _arena { limits } {}

    [[nodiscard]] TypeId primitive(ValueKind kind) const { return _arena.primitive(kind); }

    [[nodiscard]] TypeId intern(DefinedType type)
    {
        auto const id = _arena.intern(std::move(type));
        REQUIRE(id.has_value());
        return *id;
    }

    /// A record (or a tuple, by @p kind) of @p fields.
    [[nodiscard]] TypeId record(std::vector<TypeId> const& fields, ValueKind kind = ValueKind::Record)
    {
        auto type = DefinedType { .kind = kind };
        for (auto const field: fields)
            type.children.emplace_back(field);
        if (kind == ValueKind::Record)
            type.labels = labelsOf(fields.size());
        return intern(std::move(type));
    }

    [[nodiscard]] TypeId tuple(std::vector<TypeId> const& elements)
    {
        return record(elements, ValueKind::Tuple);
    }

    /// A variant of cases with the payloads @p payloads.
    [[nodiscard]] TypeId variant(std::vector<std::optional<TypeId>> payloads)
    {
        auto const count = payloads.size();
        return intern(DefinedType {
            .kind = ValueKind::Variant, .children = std::move(payloads), .labels = labelsOf(count) });
    }

    [[nodiscard]] TypeId labelled(ValueKind kind, std::size_t labels)
    {
        return intern(DefinedType { .kind = kind, .labels = labelsOf(labels) });
    }

    [[nodiscard]] TypeId wrap(ValueKind kind, std::vector<std::optional<TypeId>> children)
    {
        return intern(DefinedType { .kind = kind, .children = std::move(children) });
    }

    [[nodiscard]] TypeId fixedList(TypeId element, uint32_t length)
    {
        return intern(
            DefinedType { .kind = ValueKind::FixedList, .children = { element }, .length = length });
    }

    /// The tuple of @p count elements of @p element.
    [[nodiscard]] TypeId repeated(TypeId element, std::size_t count)
    {
        return tuple(std::vector<TypeId>(count, element));
    }

    [[nodiscard]] TypeArena const& arena() const noexcept { return _arena; }

    /// Every type's layout, which must exist.
    [[nodiscard]] LayoutTable layouts() const
    {
        auto table = computeLayouts(_arena);
        REQUIRE(table.has_value());
        return std::move(*table);
    }

    /// The error laying the arena out reports; it must fail.
    [[nodiscard]] LayoutError refusal() const
    {
        auto const table = computeLayouts(_arena);
        REQUIRE_FALSE(table.has_value());
        return table.error();
    }

  private:
    TypeArena _arena;
};

/// The function type of @p params and @p result.
[[nodiscard]] FunctionTypeInfo functionOf(std::vector<TypeId> const& params, std::optional<TypeId> result)
{
    auto function = FunctionTypeInfo { .result = result };
    for (auto const param: params)
        function.params.emplace_back(SharedName { std::format("p{}", function.params.size()) }, param);
    return function;
}
} // namespace

TEST_CASE("Layout.matchesReferenceTable")
{
    // Rows of canonical-abi/run_tests.py at the pin (f78ac03c): the element sizes test_heap's lists
    // stride by, and definitions.py's own rules for the rest.
    auto types = Types {};
    auto const u8 = types.primitive(ValueKind::U8);
    auto const u16 = types.primitive(ValueKind::U16);
    auto const u32 = types.primitive(ValueKind::U32);
    auto const u64 = types.primitive(ValueKind::U64);
    auto const f32 = types.primitive(ValueKind::F32);
    auto const f64 = types.primitive(ValueKind::F64);
    auto const string = types.primitive(ValueKind::String);

    auto const recordU8U32 = types.record({ u8, u32 });
    auto const variantF32U64 = types.variant({ f32, u64 });
    auto const variantF32F64 = types.variant({ f32, f64 });
    auto const flags9 = types.labelled(ValueKind::Flags, 9);
    auto const flags17 = types.labelled(ValueKind::Flags, 17);
    auto const flags32 = types.labelled(ValueKind::Flags, 32);
    auto const flags2 = types.labelled(ValueKind::Flags, 2);
    auto const flags8 = types.labelled(ValueKind::Flags, 8);
    auto const flags16 = types.labelled(ValueKind::Flags, 16);
    auto const tupleBBHW = types.tuple({ u8, u8, u16, u32 });
    auto const tupleBHBW = types.tuple({ u8, u16, u8, u32 });
    auto const tupleHB = types.tuple({ u16, u8 });
    auto const tupleNested = types.tuple({ tupleHB, u8 });
    auto const listU8 = types.wrap(ValueKind::List, { u8 });
    auto const fixedU32x2 = types.fixedList(u32, 2);
    auto const mapU8U16 = types.wrap(ValueKind::Map, { u8, u16 });
    auto const optionU8 = types.wrap(ValueKind::Option, { u8 });
    auto const resultU8F32 = types.wrap(ValueKind::Result, { u8, f32 });
    auto const resultEmpty = types.wrap(ValueKind::Result, { std::nullopt, std::nullopt });
    auto const enum257 = types.labelled(ValueKind::Enum, 257);
    auto const enum256 = types.labelled(ValueKind::Enum, 256);
    auto const enum65536 = types.labelled(ValueKind::Enum, 65'536);
    auto const enum65537 = types.labelled(ValueKind::Enum, 65'537);
    auto const recordU8String = types.record({ u8, string });
    auto const layouts = types.layouts();

    struct Row
    {
        TypeId type;
        uint32_t size;
        uint32_t alignment;
        std::vector<CoreValueKind> flat;
    };
    auto const rows = std::vector<Row> {
        { .type = recordU8U32, .size = 8, .alignment = 4, .flat = { I32, I32 } },
        { .type = variantF32U64, .size = 16, .alignment = 8, .flat = { I32, I64 } },
        { .type = variantF32F64, .size = 16, .alignment = 8, .flat = { I32, I64 } },
        { .type = flags9, .size = 2, .alignment = 2, .flat = { I32 } },
        { .type = flags17, .size = 4, .alignment = 4, .flat = { I32 } },
        { .type = flags32, .size = 4, .alignment = 4, .flat = { I32 } },
        { .type = flags2, .size = 1, .alignment = 1, .flat = { I32 } },
        // elem_size_flags' boundaries: 8 labels still fit a u8, 16 still a u16.
        { .type = flags8, .size = 1, .alignment = 1, .flat = { I32 } },
        { .type = flags16, .size = 2, .alignment = 2, .flat = { I32 } },
        { .type = types.primitive(ValueKind::Bool), .size = 1, .alignment = 1, .flat = { I32 } },
        { .type = types.primitive(ValueKind::S64), .size = 8, .alignment = 8, .flat = { I64 } },
        { .type = types.primitive(ValueKind::Char), .size = 4, .alignment = 4, .flat = { I32 } },
        { .type = f64, .size = 8, .alignment = 8, .flat = { F64 } },
        { .type = f32, .size = 4, .alignment = 4, .flat = { F32 } },
        { .type = string, .size = 8, .alignment = 4, .flat = { I32, I32 } },
        { .type = tupleBBHW, .size = 8, .alignment = 4, .flat = { I32, I32, I32, I32 } },
        { .type = tupleBHBW, .size = 12, .alignment = 4, .flat = { I32, I32, I32, I32 } },
        { .type = tupleHB, .size = 4, .alignment = 2, .flat = { I32, I32 } },
        { .type = tupleNested, .size = 6, .alignment = 2, .flat = { I32, I32, I32 } },
        { .type = listU8, .size = 8, .alignment = 4, .flat = { I32, I32 } },
        { .type = fixedU32x2, .size = 8, .alignment = 4, .flat = { I32, I32 } },
        { .type = mapU8U16, .size = 8, .alignment = 4, .flat = { I32, I32 } },
        { .type = optionU8, .size = 2, .alignment = 1, .flat = { I32, I32 } },
        { .type = resultU8F32, .size = 8, .alignment = 4, .flat = { I32, I32 } },
        { .type = resultEmpty, .size = 1, .alignment = 1, .flat = { I32 } },
        { .type = enum256, .size = 1, .alignment = 1, .flat = { I32 } },
        { .type = enum257, .size = 2, .alignment = 2, .flat = { I32 } },
        // discriminant_type's u16/u32 boundary: 65'536 cases still fit a u16.
        { .type = enum65536, .size = 2, .alignment = 2, .flat = { I32 } },
        { .type = enum65537, .size = 4, .alignment = 4, .flat = { I32 } },
    };
    for (auto const& row: rows)
    {
        INFO(std::format("type {}", row.type.value));
        auto const& layout = layouts[row.type];
        CHECK(layout.size == row.size);
        CHECK(layout.alignment == row.alignment);
        CHECK(layout.flat == row.flat);
        CHECK(layout.flatShape == FlatShape::Fits);
    }

    // Offsets, discriminants and the 64-bit pointer sizes.
    CHECK(layouts[recordU8U32].fieldOffsets == std::vector<uint32_t> { 0, 4 });
    CHECK(layouts[tupleBHBW].fieldOffsets == std::vector<uint32_t> { 0, 2, 4, 8 });
    CHECK(layouts[tupleNested].fieldOffsets == std::vector<uint32_t> { 0, 4 });
    CHECK(layouts[variantF32U64].discriminantBytes == 1);
    CHECK(layouts[variantF32U64].payloadOffset == 8);
    CHECK(layouts[resultU8F32].payloadOffset == 4);
    CHECK(layouts[optionU8].payloadOffset == 1);
    CHECK(layouts[enum256].discriminantBytes == 1);
    CHECK(layouts[enum257].discriminantBytes == 2);
    CHECK(layouts[enum65536].discriminantBytes == 2);
    CHECK(layouts[enum65537].discriminantBytes == 4);
    CHECK(layouts[recordU8U32].discriminantBytes == 0);
    CHECK(layouts[string].size64 == 16);
    CHECK(layouts[string].alignment64 == 8);
    CHECK(layouts[listU8].size64 == 16);
    CHECK(layouts[recordU8U32].size64 == 8);
    CHECK(layouts[recordU8String].size == 12);
    CHECK(layouts[recordU8String].size64 == 24);
    CHECK(layouts[recordU8String].alignment64 == 8);
    CHECK(layouts[recordU8String].pointers == PointerContent::Held);
    CHECK(layouts[string].pointers == PointerContent::Held);
    CHECK(layouts[mapU8U16].pointers == PointerContent::Held);
    CHECK(layouts[recordU8U32].pointers == PointerContent::None);
    CHECK(layouts.all().size() == types.arena().valueTypeCount());
}

TEST_CASE("Layout.matchesReferenceFlattening")
{
    // test_flatten of canonical-abi/run_tests.py at the pin, with 32-bit addresses.
    auto types = Types {};
    auto const u8 = types.primitive(ValueKind::U8);
    auto const f32 = types.primitive(ValueKind::F32);
    auto const f64 = types.primitive(ValueKind::F64);
    auto const tupleF32 = types.tuple({ f32 });
    auto const tupleF32F32 = types.tuple({ f32, f32 });
    auto const tupleU8U8 = types.tuple({ u8, u8 });
    auto const layouts = types.layouts();
    auto const seventeen = std::vector<TypeId>(17, u8);

    struct Row
    {
        FunctionTypeInfo function;
        CoreSignature lifted;
        CoreSignature lowered;
    };
    auto const rows = std::vector<Row> {
        { .function = functionOf({ u8, f32, f64 }, std::nullopt),
          .lifted = { .params = { I32, F32, F64 }, .results = {} },
          .lowered = { .params = { I32, F32, F64 }, .results = {} } },
        { .function = functionOf({ u8, f32, f64 }, f32),
          .lifted = { .params = { I32, F32, F64 }, .results = { F32 } },
          .lowered = { .params = { I32, F32, F64 }, .results = { F32 } } },
        { .function = functionOf({ u8, f32, f64 }, u8),
          .lifted = { .params = { I32, F32, F64 }, .results = { I32 } },
          .lowered = { .params = { I32, F32, F64 }, .results = { I32 } } },
        { .function = functionOf({ u8, f32, f64 }, tupleF32),
          .lifted = { .params = { I32, F32, F64 }, .results = { F32 } },
          .lowered = { .params = { I32, F32, F64 }, .results = { F32 } } },
        { .function = functionOf({ u8, f32, f64 }, tupleF32F32),
          .lifted = { .params = { I32, F32, F64 }, .results = { I32 } },
          .lowered = { .params = { I32, F32, F64, I32 }, .results = {} } },
        { .function = functionOf(seventeen, std::nullopt),
          .lifted = { .params = { I32 }, .results = {} },
          .lowered = { .params = { I32 }, .results = {} } },
        { .function = functionOf(seventeen, tupleU8U8),
          .lifted = { .params = { I32 }, .results = { I32 } },
          .lowered = { .params = { I32, I32 }, .results = {} } },
    };
    for (auto const& row: rows)
    {
        CHECK(flattenFunction(layouts, row.function, FlattenContext::Lift) == row.lifted);
        CHECK(flattenFunction(layouts, row.function, FlattenContext::Lower) == row.lowered);
    }
}

TEST_CASE("Layout.flatStopsAtMaxPlusOne")
{
    auto types = Types {};
    auto const u32 = types.primitive(ValueKind::U32);
    auto const f32 = types.primitive(ValueKind::F32);
    auto const wide = types.repeated(u32, 40);
    auto const sixteen = types.repeated(u32, 16);
    auto const seventeen = types.repeated(u32, 17);
    auto const fifteenFloats = types.repeated(f32, 15);
    auto const variantFits = types.variant({ fifteenFloats, std::nullopt });
    auto const variantOverflows = types.variant({ sixteen, fifteenFloats });
    auto const fixedOverflows = types.fixedList(u32, 1'000'000);

    // very-nested.wast's $t0 … $t16 (wasm-tools 1.263.0 tests/cli/component-model/very-nested.wast:
    // 2522-2539): eight u32, doubled sixteen times, 524'288 leaves.
    auto nested = types.repeated(u32, 8);
    for ([[maybe_unused]] auto const level: std::views::iota(0, 16))
        nested = types.tuple({ nested, nested });
    auto const layouts = types.layouts();

    CHECK(layouts[wide].flat.size() == MaxFlatParams + 1);
    CHECK(layouts[wide].flatShape == FlatShape::Overflows);
    CHECK(layouts[wide].size == 160);
    CHECK(layouts[wide].alignment == 4);
    CHECK(layouts[sixteen].flat.size() == MaxFlatParams);
    CHECK(layouts[sixteen].flatShape == FlatShape::Fits);
    CHECK(layouts[seventeen].flat.size() == MaxFlatParams + 1);
    CHECK(layouts[seventeen].flatShape == FlatShape::Overflows);

    // A variant's discriminant counts: 1 + 15 fits, 1 + 16 does not; payloads join position-wise.
    CHECK(layouts[variantFits].flat.size() == MaxFlatParams);
    CHECK(layouts[variantFits].flatShape == FlatShape::Fits);
    CHECK(layouts[variantFits].flat[1] == F32);
    CHECK(layouts[variantOverflows].flat.size() == MaxFlatParams + 1);
    CHECK(layouts[variantOverflows].flatShape == FlatShape::Overflows);
    CHECK(layouts[variantOverflows].flat[1] == I32); // join(i32, f32)

    CHECK(layouts[fixedOverflows].flat.size() == MaxFlatParams + 1);
    CHECK(layouts[fixedOverflows].size == 4'000'000);

    CHECK(layouts[nested].flat.size() == MaxFlatParams + 1);
    CHECK(layouts[nested].flatShape == FlatShape::Overflows);
    CHECK(layouts[nested].size == 524'288 * 4);
    CHECK(layouts[nested].alignment == 4);
    CHECK(layouts[nested].fieldOffsets == std::vector<uint32_t> { 0, 524'288 * 2 });
    // Each type costs its own children and flat entries, not its leaves: 524'288 leaves at the top
    // would be 2^20 steps for $t16 alone.
    CHECK(layouts.steps() <= 64 * types.arena().valueTypeCount());
}

TEST_CASE("Layout.flattenFunctionSpills")
{
    auto types = Types {};
    auto const u32 = types.primitive(ValueKind::U32);
    auto const pair = types.tuple({ u32, u32 });
    auto const wide = types.repeated(u32, 17);
    auto const layouts = types.layouts();

    auto const seventeenParams = functionOf(std::vector<TypeId>(17, u32), std::nullopt);
    CHECK(flattenFunction(layouts, seventeenParams, FlattenContext::Lift)
          == CoreSignature { .params = { I32 }, .results = {} });
    auto const sixteenParams = functionOf(std::vector<TypeId>(16, u32), std::nullopt);
    CHECK(flattenFunction(layouts, sixteenParams, FlattenContext::Lift).params == std::vector(16, I32));
    // One parameter that flattens to 17 values spills as 17 parameters do.
    CHECK(flattenFunction(layouts, functionOf({ wide }, std::nullopt), FlattenContext::Lower).params
          == std::vector { I32 });

    auto const pairResult = functionOf({ u32 }, pair);
    CHECK(flattenFunction(layouts, pairResult, FlattenContext::Lift)
          == CoreSignature { .params = { I32 }, .results = { I32 } });
    CHECK(flattenFunction(layouts, pairResult, FlattenContext::Lower)
          == CoreSignature { .params = { I32, I32 }, .results = {} });
    auto const both = functionOf(std::vector<TypeId>(17, u32), pair);
    CHECK(flattenFunction(layouts, both, FlattenContext::Lower)
          == CoreSignature { .params = { I32, I32 }, .results = {} });
    CHECK(flattenFunction(layouts, functionOf({}, u32), FlattenContext::Lower)
          == CoreSignature { .params = {}, .results = { I32 } });
}

TEST_CASE("Layout.elementSize64Boundary")
{
    // test/validation/max-value-size.wast at the pin: elem_size(t, 'i64') < 2^28 for every type.
    auto const refusalOf = [](auto const& build) {
        auto types = Types {};
        auto const refused = build(types);
        auto const error = types.refusal();
        CHECK(error.code == LayoutErrorCode::ExceedsMaximumByteSize);
        CHECK(error.type == refused);
        return error;
    };
    auto const acceptedSize = [](auto const& build) {
        auto types = Types {};
        auto const accepted = build(types);
        return types.layouts()[accepted].size64;
    };

    // A record of 2^28 - 1 bytes is the largest; one byte more is refused, not wrapped.
    CHECK(acceptedSize([](Types& types) {
              auto const u8 = types.primitive(ValueKind::U8);
              return types.record({ types.fixedList(u8, MaxValueByteSize - 2), u8 });
          })
          == MaxValueByteSize - 1);
    auto const record = refusalOf([](Types& types) {
        auto const u8 = types.primitive(ValueKind::U8);
        return types.record({ types.fixedList(u8, MaxValueByteSize - 1), u8 });
    });
    CHECK(record.value == MaxValueByteSize);
    CHECK(layoutMessage(record.code) == "value type's maximum in-memory size exceeds maximum byte size");

    CHECK(acceptedSize([](Types& types) {
              return types.fixedList(types.primitive(ValueKind::U8), MaxValueByteSize - 1);
          })
          == MaxValueByteSize - 1);
    std::ignore = refusalOf(
        [](Types& types) { return types.fixedList(types.primitive(ValueKind::U8), MaxValueByteSize); });
    // 2^29 u64 is 2^32 bytes, which a 32-bit product wraps to 0.
    auto const wrapped =
        refusalOf([](Types& types) { return types.fixedList(types.primitive(ValueKind::U64), 1U << 29U); });
    CHECK(wrapped.value == uint64_t { 1 } << 32U);
    std::ignore = refusalOf([](Types& types) {
        return types.fixedList(types.fixedList(types.primitive(ValueKind::U8), MaxValueByteSize - 1), 2);
    });
    std::ignore = refusalOf([](Types& types) {
        auto const u8 = types.primitive(ValueKind::U8);
        return types.tuple({ types.fixedList(u8, MaxValueByteSize - 1), types.fixedList(u8, 1) });
    });
    std::ignore = refusalOf([](Types& types) {
        auto const u8 = types.primitive(ValueKind::U8);
        return types.variant({ types.fixedList(u8, MaxValueByteSize - 1), std::nullopt });
    });
    // Pointer width matters: 2^24 strings are 2^27 bytes with 32-bit pointers but 2^28 with 64.
    CHECK(acceptedSize([](Types& types) {
              return types.fixedList(types.primitive(ValueKind::String), (1U << 24U) - 1);
          })
          == MaxValueByteSize - 16);
    std::ignore = refusalOf(
        [](Types& types) { return types.fixedList(types.primitive(ValueKind::String), 1U << 24U); });
    // A list, map, stream or future of a maximal type is a pointer pair or a handle.
    CHECK(acceptedSize([](Types& types) {
              auto const big = types.fixedList(types.primitive(ValueKind::U8), MaxValueByteSize - 1);
              return types.wrap(ValueKind::Option,
                                { types.wrap(ValueKind::Map, { types.primitive(ValueKind::U8), big }) });
          })
          == 24);
    CHECK(acceptedSize([](Types& types) {
              auto const big = types.fixedList(types.primitive(ValueKind::U8), MaxValueByteSize - 1);
              return types.fixedList(types.wrap(ValueKind::Future, { big }), 1000);
          })
          == 4000);
}

TEST_CASE("Layout.flagsLabelCount")
{
    auto accepted = Types {};
    auto const most = accepted.labelled(ValueKind::Flags, MaxFlagLabels);
    CHECK(accepted.layouts()[most].flat == std::vector { I32 });

    for (auto const count: { std::size_t { 0 }, std::size_t { MaxFlagLabels + 1 }, std::size_t { 1'000 } })
    {
        auto types = Types {};
        auto const flags = types.labelled(ValueKind::Flags, count);
        auto const error = types.refusal();
        CHECK(error.code == LayoutErrorCode::FlagsLabelCount);
        CHECK(error.type == flags);
        CHECK(error.value == count);
    }
}

TEST_CASE("Layout.deepTypeIsIterative")
{
    // An option chain 100 deep at the default depth cap, and 200'000 deep under a raised one: a
    // recursive layout would take a frame per level, which 200'000 levels overflow under ASan.
    for (auto const depth: { uint32_t { 99 }, uint32_t { 200'000 } })
    {
        auto limits = ComponentLimits {};
        limits.maxTypeDepth = depth + 1;
        limits.maxTypeSize = depth + 2;
        auto types = Types { limits };
        auto chain = types.primitive(ValueKind::U32);
        for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, depth))
            chain = types.wrap(ValueKind::Option, { chain });
        auto const layouts = types.layouts();
        INFO(std::format("depth {}", depth));
        auto const& layout = layouts[chain];
        CHECK(layout.size == 4 + (4 * depth));
        CHECK(layout.alignment == 4);
        CHECK(layout.flat.size() == MaxFlatParams + 1);
        // Each option costs itself, one child and at most 17 flat entries: no frames, no rewalks.
        CHECK(layouts.steps() <= 20 * uint64_t { types.arena().valueTypeCount() });
    }
}

TEST_CASE("Layout.extendLaysOutOnlyNewTypes")
{
    auto types = Types {};
    auto table = LayoutTable { types.arena().id() };
    REQUIRE(table.extend(types.arena()).has_value());
    auto const primitives = table.steps();
    CHECK(table.all().size() == types.arena().valueTypeCount());
    auto const pair = types.tuple({ types.primitive(ValueKind::U32), types.primitive(ValueKind::U64) });
    REQUIRE(table.extend(types.arena()).has_value());
    CHECK(table.all().size() == types.arena().valueTypeCount());
    CHECK(table[pair].size == 16);
    // The tuple: itself, two children and two flat entries.
    CHECK(table.steps() - primitives == 5);
    REQUIRE(table.extend(types.arena()).has_value());
    CHECK(table.steps() - primitives == 5);
}

TEST_CASE("Layout.refusalStopsAtTheLimit")
{
    // A record refused at its second field reads no further field: refusing costs what was read up
    // to the limit, not the 100'000 fields after it, and no offset past it is ever narrowed to 32 bits.
    auto types = Types {};
    auto const u8 = types.primitive(ValueKind::U8);
    auto fields = std::vector<TypeId> { types.fixedList(u8, MaxValueByteSize - 1), u8 };
    fields.resize(100'002, u8);
    std::ignore = types.record(fields);
    auto table = LayoutTable { types.arena().id() };
    auto const extended = table.extend(types.arena());
    REQUIRE_FALSE(extended.has_value());
    CHECK(extended.error().value == MaxValueByteSize);
    CHECK(table.steps() < 1'000);
    // The table ends before the refused type, and refuses it again.
    CHECK(table.all().size() == types.arena().valueTypeCount() - 1);
    CHECK_FALSE(table.extend(types.arena()).has_value());
}
