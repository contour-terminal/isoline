// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <isoline/binary/Opcode.hpp>
#include <isoline/text/Instructions.hpp>

using isoline::binary::ImmediateKind;
using isoline::binary::Opcode;
using isoline::binary::OpcodeTable;
using isoline::text::indexImmediateOf;
using isoline::text::opcodeNamed;
using isoline::text::textName;

TEST_CASE("Instructions.everyRowHasItsText")
{
    for (auto const& row: OpcodeTable)
    {
        INFO(row.text);
        if (row.opcode == Opcode::SelectTyped)
        {
            // Text spells typed select `select (result t)`: its table text names nothing.
            CHECK_FALSE(opcodeNamed(row.text).has_value());
            CHECK(textName(row.opcode) == "select");
            continue;
        }
        auto const named = opcodeNamed(row.text);
        REQUIRE(named.has_value());
        CHECK(*named == row.opcode);
        CHECK(textName(*named) == row.text);
    }
}

TEST_CASE("Instructions.unknownNamesNameNothing")
{
    CHECK(opcodeNamed("select") == Opcode::Select);
    CHECK_FALSE(opcodeNamed("").has_value());
    CHECK_FALSE(opcodeNamed("get_local").has_value()); // Between two rows.
    CHECK_FALSE(opcodeNamed("zzz").has_value());       // After every row.
    CHECK_FALSE(opcodeNamed("i32.add ").has_value());
}

TEST_CASE("Instructions.indexImmediates")
{
    // Only the kinds of immediates that are indices have a row; any other kind has none.
    auto const* const local = indexImmediateOf(ImmediateKind::LocalIndex);
    REQUIRE(local != nullptr);
    CHECK(local->kind == ImmediateKind::LocalIndex);
    CHECK(indexImmediateOf(ImmediateKind::None) == nullptr);
    CHECK(indexImmediateOf(ImmediateKind::LabelTable) == nullptr);
}
