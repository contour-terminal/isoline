// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ComponentValues.hpp"
#include "Script.hpp"
#include "SpecCheck.hpp"
#include "WastReader.hpp"
#include <isoline/component/Limits.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/text/TextLimits.hpp>

using isoline::component::ComponentLimits;
using isoline::component::canonical::LiveBytesBudget;
using isoline::component::canonical::Values;
using isoline::component::types::DefinedType;
using isoline::component::types::SharedName;
using isoline::component::types::TypeArena;
using isoline::component::types::TypeId;
using isoline::component::types::ValueKind;
using isoline::spectest::ComponentLiteral;
using isoline::spectest::componentValueForm;
using isoline::spectest::ComponentValueForms;
using isoline::spectest::componentValuesOf;
using isoline::spectest::Failure;
using isoline::spectest::Stage;

namespace
{

/// The component arguments of `(invoke "f" <values>)`, which must parse.
std::vector<ComponentLiteral> literalsOf(std::string_view values)
{
    auto const commands = isoline::spectest::parseWast("(invoke \"f\"\n" + std::string { values } + ")",
                                                       isoline::text::TextLimits {});
    if (!commands)
        FAIL(commands.error().line << ": " << commands.error().message);
    REQUIRE(commands->size() == 1);
    return commands->front().componentArgs;
}

/// A type of @p arena, interned (REQUIREd).
TypeId intern(TypeArena& arena, DefinedType type)
{
    auto const id = arena.intern(std::move(type));
    REQUIRE(id.has_value());
    return *id;
}

TypeId compound(TypeArena& arena,
                ValueKind kind,
                std::vector<std::optional<TypeId>> children,
                std::vector<SharedName> labels = {},
                uint32_t length = 0)
{
    return intern(
        arena,
        DefinedType {
            .kind = kind, .children = std::move(children), .labels = std::move(labels), .length = length });
}

/// @p literals converted against @p types in @p arena.
std::expected<Values, Failure> convert(TypeArena const& arena,
                                       std::vector<ComponentLiteral> const& literals,
                                       std::vector<TypeId> const& types)
{
    return componentValuesOf(literals, arena, types, std::make_shared<LiveBytesBudget>(uint64_t { 1 } << 20));
}

} // namespace

TEST_CASE("ComponentValues.everyForm")
{
    auto arena = TypeArena { ComponentLimits {} };
    auto const u8 = arena.primitive(ValueKind::U8);
    auto const string = arena.primitive(ValueKind::String);
    struct Row
    {
        std::string_view text; ///< The literal, as canonical::toText() prints it back.
        TypeId type;
    };
    auto const rows = std::vector<Row> {
        { .text = "(bool.const true)", .type = arena.primitive(ValueKind::Bool) },
        { .text = "(s8.const -128)", .type = arena.primitive(ValueKind::S8) },
        { .text = "(u8.const 255)", .type = u8 },
        { .text = "(s16.const -2)", .type = arena.primitive(ValueKind::S16) },
        { .text = "(u16.const 65535)", .type = arena.primitive(ValueKind::U16) },
        { .text = "(s32.const -7)", .type = arena.primitive(ValueKind::S32) },
        { .text = "(u32.const 4294967295)", .type = arena.primitive(ValueKind::U32) },
        { .text = "(s64.const -9223372036854775808)", .type = arena.primitive(ValueKind::S64) },
        { .text = "(u64.const 18446744073709551615)", .type = arena.primitive(ValueKind::U64) },
        { .text = "(f32.const 1.5)", .type = arena.primitive(ValueKind::F32) },
        { .text = "(f64.const -inf)", .type = arena.primitive(ValueKind::F64) },
        { .text = "(char.const \"☃\")", .type = arena.primitive(ValueKind::Char) },
        { .text = "(str.const \"hi\")", .type = string },
        { .text = "(list.const (u8.const 1) (u8.const 2))",
          .type = compound(arena, ValueKind::List, { u8 }) },
        { .text = R"((record.const (field "a" u8.const 1) (field "b" str.const "x")))",
          .type = compound(arena, ValueKind::Record, { u8, string }, { "a", "b" }) },
        { .text = "(tuple.const (u8.const 1) (str.const \"x\"))",
          .type = compound(arena, ValueKind::Tuple, { u8, string }) },
        { .text = "(variant.const \"b\" (u8.const 2))",
          .type = compound(arena, ValueKind::Variant, { std::nullopt, u8 }, { "a", "b" }) },
        { .text = "(enum.const \"b\")", .type = compound(arena, ValueKind::Enum, {}, { "a", "b" }) },
        { .text = "(option.none)", .type = compound(arena, ValueKind::Option, { u8 }) },
        { .text = "(option.some (u8.const 3))", .type = compound(arena, ValueKind::Option, { u8 }) },
        { .text = "(result.ok)", .type = compound(arena, ValueKind::Result, { std::nullopt, string }) },
        { .text = "(result.err (str.const \"no\"))",
          .type = compound(arena, ValueKind::Result, { std::nullopt, string }) },
        { .text = R"((flags.const "a" "c"))",
          .type = compound(arena, ValueKind::Flags, {}, { "a", "b", "c" }) },
        { .text = R"((map.const (entry (u8.const 1) (str.const "a")) (entry (u8.const 2) (str.const "b"))))",
          .type = compound(arena, ValueKind::Map, { u8, string }) },
    };
    auto keywords = std::set<std::string_view> {};
    for (auto const& row: rows)
    {
        INFO(row.text);
        auto const literals = literalsOf(row.text);
        REQUIRE(literals.size() == 1);
        keywords.insert(componentValueForm(literals.front().nodes.front()).keyword);
        auto const values = convert(arena, literals, { row.type });
        if (!values)
            FAIL(values.error().message);
        CHECK(isoline::component::canonical::toText(arena, values->root(0)) == row.text);
    }
    CHECK(keywords.size() == ComponentValueForms.size()); // every row's form at least once

    // A map of two entries is a map value of two key-value pairs.
    auto const map = convert(arena,
                             literalsOf("(map.const (entry (u8.const 1) (str.const \"a\")) "
                                        "(entry (u8.const 2) (str.const \"b\")))"),
                             { rows.back().type });
    REQUIRE(map.has_value());
    auto const root = map->root(0);
    CHECK(root.kind() == ValueKind::Map);
    REQUIRE(root.childCount() == 4);
    CHECK(root.child(0).scalar<uint8_t>() == 1);
    CHECK(root.child(1).string().utf8() == "a");
    CHECK(root.child(2).scalar<uint8_t>() == 2);
    CHECK(root.child(3).string().utf8() == "b");

    // A list.const is a fixed-length list of its length, and nested values convert at any depth.
    auto const fixed = compound(arena, ValueKind::FixedList, { u8 }, {}, 2);
    CHECK(convert(arena, literalsOf("(list.const (u8.const 1) (u8.const 2))"), { fixed }).has_value());
    auto const nested = compound(arena, ValueKind::List, { compound(arena, ValueKind::Option, { fixed }) });
    auto const deep =
        convert(arena,
                literalsOf("(list.const (option.some (list.const (u8.const 1) (u8.const 2))) (option.none))"),
                { nested });
    REQUIRE(deep.has_value());
    CHECK(isoline::component::canonical::toText(arena, deep->root(0))
          == "(list.const (option.some (list.const (u8.const 1) (u8.const 2))) (option.none))");
}

TEST_CASE("ComponentValues.typeMismatchIsHarness")
{
    auto arena = TypeArena { ComponentLimits {} };
    auto const u8 = arena.primitive(ValueKind::U8);
    auto const u32 = arena.primitive(ValueKind::U32);
    struct Row
    {
        std::string_view text;
        TypeId type;
        std::string_view message;
    };
    for (auto const& row: std::vector<Row> {
             { .text = "(u64.const 1)",
               .type = u32,
               .message = "line 2: (u64.const …) is no value of type u32" },
             { .text = "(list.const (u8.const 1))",
               .type = compound(arena, ValueKind::FixedList, { u8 }, {}, 2),
               .message = "line 2: list of 2 elements given 1" },
             { .text = "(tuple.const (u8.const 1))",
               .type = compound(arena, ValueKind::Tuple, { u8, u8 }),
               .message = "line 2: tuple of 2 elements given 1" },
             { .text = "(record.const (field \"b\" u8.const 1))",
               .type = compound(arena, ValueKind::Record, { u8 }, { "a" }),
               .message = "line 2: record fields are not the type's" },
             { .text = "(variant.const \"c\")",
               .type = compound(arena, ValueKind::Variant, { std::nullopt }, { "a" }),
               .message = "line 2: variant has no case \"c\"" },
             { .text = "(variant.const \"a\")",
               .type = compound(arena, ValueKind::Variant, { u8 }, { "a" }),
               .message = "line 2: (variant.const …) gives no payload where its case takes one" },
             { .text = "(result.ok (u8.const 1))",
               .type = compound(arena, ValueKind::Result, { std::nullopt, std::nullopt }),
               .message = "line 2: (result.ok …) gives a payload where its case takes none" },
             { .text = "(enum.const \"z\")",
               .type = compound(arena, ValueKind::Enum, {}, { "a" }),
               .message = "line 2: enum has no case \"z\"" },
             { .text = "(flags.const \"z\")",
               .type = compound(arena, ValueKind::Flags, {}, { "a" }),
               .message = "line 2: flags have no flag \"z\"" },
             { .text = R"((char.const "\u{d7ff}"))",
               .type = u32,
               .message = "line 2: (char.const …) is no value" },
             { .text = "(list.const (u8.const 1) (u32.const 2))",
               .type = compound(arena, ValueKind::List, { u8 }),
               .message = "line 2: (u32.const …) is no value of type u8" },
         })
    {
        INFO(row.text);
        auto const values = convert(arena, literalsOf(row.text), { row.type });
        REQUIRE_FALSE(values.has_value());
        CHECK(values.error().stage == Stage::Harness);
        INFO(values.error().message);
        CHECK(values.error().message.starts_with(row.message));
    }

    // As many values as roots, else the harness is wrong.
    auto const count = convert(arena, literalsOf("(u32.const 1) (u32.const 2)"), { u32 });
    REQUIRE_FALSE(count.has_value());
    CHECK(count.error().stage == Stage::Harness);
    CHECK(count.error().message == "2 values given where 1 are taken");
}
