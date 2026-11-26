// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/LoweringTable.hpp>
#include <isoline/fastinterpreter/testing/Lowering.hpp>

using isoline::binary::GlobalType;
using isoline::binary::Mutability;
using isoline::binary::Opcode;
using isoline::binary::OpcodeTable;
using isoline::binary::ValueKind;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::fastinterpreter::FusionRow;
using isoline::fastinterpreter::FusionTable;
using isoline::fastinterpreter::loweringRow;
using isoline::fastinterpreter::LoweringRows;
using isoline::fastinterpreter::LoweringTable;
using isoline::fastinterpreter::OperationForm;
using isoline::fastinterpreter::OperationId;
using isoline::fastinterpreter::operationRow;
using isoline::fastinterpreter::testing::encodingOf;
using isoline::fastinterpreter::testing::functionOf;
using isoline::fastinterpreter::testing::listingOf;
using isoline::fastinterpreter::testing::op;
using isoline::fastinterpreter::testing::typesOf;

namespace binary = isoline::binary;

namespace
{

/// A module and its listing that exercise one Special row of LoweringRows.
struct SpecialRowCase
{
    Opcode opcode;        ///< The row's instruction.
    ModuleBuilder module; ///< A function using it.
    std::string listing;  ///< The function's listing.
};

/// A block, loop or if of @p blockType around @p body.
[[nodiscard]] Bytes blockOf(uint8_t opcode, uint8_t blockType, Bytes const& body)
{
    return code({ { opcode, blockType }, body, { 0x0B } });
}

/// One case per Special row: the listings name the row's operation where it emits one.
[[nodiscard]] std::vector<SpecialRowCase> specialRowCases()
{
    auto withGlobal = functionOf({}, {}, {}, code({ op(0x23, 0), op(0x24, 0) }));
    withGlobal.global(GlobalType { .type = binary::I64, .mutability = Mutability::Var }, { 0x42, 0x00 });
    auto cases = std::vector<SpecialRowCase> {};
    auto add = [&cases](Opcode opcode, ModuleBuilder module, std::string listing) {
        cases.push_back({ .opcode = opcode, .module = std::move(module), .listing = std::move(listing) });
    };
    add(Opcode::Unreachable, functionOf({}, {}, {}, op(0x00)), "unreachable\n");
    add(Opcode::Nop, functionOf({}, {}, {}, op(0x01)), "return s0\n");
    add(Opcode::Block, functionOf({}, {}, {}, blockOf(0x02, 0x40, {})), "return s0\n");
    add(Opcode::Loop, functionOf({}, {}, {}, blockOf(0x03, 0x40, {})), "return s0\n");
    add(Opcode::If,
        functionOf({ binary::I32 }, {}, {}, code({ localGet(0), blockOf(0x04, 0x40, {}) })),
        "if s0 → @3\nreturn s1\n");
    add(Opcode::Else,
        functionOf({ binary::I32 }, {}, {}, code({ localGet(0), op(0x04), { 0x40 }, op(0x05), op(0x0B) })),
        "if s0 → @5\njump → @5\nreturn s1\n");
    add(Opcode::Br, functionOf({}, {}, {}, blockOf(0x02, 0x40, op(0x0C, 0))), "jump → @2\nreturn s0\n");
    add(Opcode::BrIf,
        functionOf({ binary::I32 }, {}, {}, blockOf(0x02, 0x40, code({ localGet(0), op(0x0D, 0) }))),
        "br_if s0 → @3\nreturn s1\n");
    add(Opcode::BrTable,
        functionOf(
            { binary::I32 }, {}, {}, blockOf(0x02, 0x40, code({ localGet(0), { 0x0E, 0x01, 0x00, 0x01 } }))),
        "br_table s0, s1, #0, #1 → @8 s1, @8 s1\nreturn s1\n");
    add(Opcode::BrOnNull,
        functionOf({ binary::FuncRef },
                   {},
                   {},
                   blockOf(0x02, 0x40, code({ localGet(0), { 0xD5, 0x00 }, op(0x1A) }))),
        "br_on_null s0 → @3\nreturn s2\n");
    add(Opcode::BrOnNonNull,
        functionOf(
            { binary::FuncRef },
            {},
            {},
            code({ blockOf(0x02, 0x70, code({ localGet(0), { 0xD6, 0x00 }, localGet(0) })), op(0x1A) })),
        "move128 s2, s0\nbr_on_non_null s2 → @7\nmove128 s2, s0\nreturn s2\n");
    add(Opcode::End,
        functionOf({ binary::I32 }, { binary::I32 }, {}, localGet(0)),
        "move64 s1, s0\nreturn s1\n");
    add(Opcode::Return,
        functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), op(0x0F) })),
        "move64 s1, s0\nreturn s1\n");
    add(Opcode::Drop, functionOf({ binary::I32 }, {}, {}, code({ localGet(0), op(0x1A) })), "return s1\n");
    add(Opcode::Select,
        functionOf(
            { binary::I32 }, { binary::I32 }, {}, code({ localGet(0), localGet(0), localGet(0), op(0x1B) })),
        "select64 s1, s0, s0, s0\nreturn s1\n");
    add(Opcode::SelectTyped,
        functionOf({ binary::I32 },
                   { binary::I32 },
                   {},
                   code({ localGet(0), localGet(0), localGet(0), { 0x1C, 0x01, 0x7F } })),
        "select64 s1, s0, s0, s0\nreturn s1\n");
    add(Opcode::LocalGet,
        functionOf({ binary::I32 }, { binary::I32 }, {}, localGet(0)),
        "move64 s1, s0\nreturn s1\n");
    add(Opcode::LocalSet,
        functionOf({ binary::I32, binary::I32 }, {}, {}, code({ localGet(1), op(0x21, 0) })),
        "move64 s0, s1\nreturn s2\n");
    add(Opcode::LocalTee,
        functionOf({ binary::I32, binary::I32 }, {}, {}, code({ localGet(1), op(0x22, 0), op(0x1A) })),
        "move64 s0, s1\nreturn s2\n");
    add(Opcode::GlobalGet, withGlobal, "global.get s0, #0\nglobal.set s0, #0\nreturn s0\n");
    add(Opcode::GlobalSet, withGlobal, "global.get s0, #0\nglobal.set s0, #0\nreturn s0\n");
    add(Opcode::I32Const, functionOf({}, { binary::I32 }, {}, i32Const(7)), "const64 s0, #7\nreturn s0\n");
    add(Opcode::I64Const, functionOf({}, { binary::I64 }, {}, { 0x42, 0x07 }), "const64 s0, #7\nreturn s0\n");
    add(Opcode::F32Const,
        functionOf({}, { binary::F32 }, {}, { 0x43, 0x00, 0x00, 0x80, 0x3F }),
        "const64 s0, #1065353216\nreturn s0\n");
    add(Opcode::F64Const,
        functionOf({}, { binary::F64 }, {}, { 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F }),
        "const64 s0, #4607182418800017408\nreturn s0\n");
    return cases;
}

/// i32.const 5 or i64.const 5, by @p kind.
[[nodiscard]] Bytes constantOf(ValueKind kind)
{
    return kind == ValueKind::I32 ? i32Const(5) : Bytes { 0x42, 0x05 };
}

} // namespace

TEST_CASE("LoweringRows.everyRowIsExercised")
{
    SECTION("every Unary, Binary and BinaryImmediate row, over params of its operand types")
    {
        // The listing names the row's operation, and a folding row's twin when its right operand is a
        // constant.
        for (auto const& row: LoweringRows)
        {
            if (row.form == OperationForm::Special)
                continue;
            auto const& info = binary::opcodeInfo(row.opcode);
            INFO(info.text);
            auto const& signature = binary::signatureInfo(info.signature);
            auto const params = typesOf(signature.paramKinds());
            auto const results = typesOf(signature.resultKinds());
            auto const unary = row.form == OperationForm::Unary;
            auto const body = unary ? code({ localGet(0), encodingOf(row.opcode) })
                                    : code({ localGet(0), localGet(1), encodingOf(row.opcode) });
            auto const expected =
                unary ? std::format("{} s1, s0\nreturn s1\n", operationRow(row.operation).text)
                      : std::format("{} s2, s0, s1\nreturn s2\n", operationRow(row.operation).text);
            CHECK(listingOf(functionOf(params, results, {}, body)) == expected);
            CHECK(loweringRow(row.opcode) == row);
            if (row.form != OperationForm::BinaryImmediate)
                continue;
            CHECK(listingOf(
                      functionOf(params,
                                 results,
                                 {},
                                 code({ localGet(0), constantOf(params[1].kind), encodingOf(row.opcode) })))
                  == std::format("{} s2, s0, #5\nreturn s2\n", operationRow(row.immediate).text));
        }
    }
    SECTION("every Special row, by a case of its own")
    {
        auto const cases = specialRowCases();
        for (auto const& row: LoweringRows)
        {
            if (row.form != OperationForm::Special)
                continue;
            auto const& info = binary::opcodeInfo(row.opcode);
            INFO(info.text);
            auto const found = std::ranges::find(cases, row.opcode, &SpecialRowCase::opcode);
            REQUIRE(found != cases.end());
            auto const listing = listingOf(found->module);
            CHECK(listing == found->listing);
            // The operation's text, then its first operand or the line's end.
            auto const text = operationRow(row.operation).text;
            if (row.operation != OperationId::Nop)
                CHECK((listing.contains(std::format("{} ", text))
                       || listing.contains(std::format("{}\n", text))));
            CHECK(loweringRow(row.opcode) == row);
        }
    }
    SECTION("every FusionTable row, to a block and to a loop, of slots and of a constant")
    {
        for (auto const& row: FusionTable)
        {
            auto const& info = binary::opcodeInfo(row.compare);
            INFO(info.text << " with " << binary::opcodeInfo(row.branch).text);
            auto const params = typesOf(binary::signatureInfo(info.signature).paramKinds());
            auto const unary = params.size() == 1;
            auto const operands = unary ? localGet(0) : code({ localGet(0), localGet(1) });
            auto const folded = unary ? localGet(0) : code({ localGet(0), constantOf(params[1].kind) });
            auto const slots = unary ? std::string { "s0" } : std::string { "s0, s1" };
            auto const end = params.size();
            auto const listingWith = [&](OperationId operation, std::string const& reads, uint32_t target) {
                return std::format(
                    "{} {} → @{}\nreturn s{}\n", operationRow(operation).text, reads, target, end);
            };
            auto const lowered = [&](uint8_t block, Bytes const& compared) {
                auto const branch =
                    row.branch == Opcode::BrIf
                        ? blockOf(block, 0x40, code({ compared, encodingOf(row.compare), op(0x0D, 0) }))
                        : code({ compared, encodingOf(row.compare), { 0x04, 0x40, 0x0B } });
                return listingOf(functionOf(params, {}, {}, branch));
            };
            auto const words = operationRow(row.fused).words;
            CHECK(lowered(0x02, operands) == listingWith(row.fused, slots, words));
            auto const immediateReads = unary ? std::string { "s0" } : std::string { "s0, #5" };
            CHECK(lowered(0x02, folded)
                  == listingWith(row.immediate, immediateReads, operationRow(row.immediate).words));
            if (row.branch != Opcode::BrIf)
            {
                CHECK(row.fusedLoop == row.fused);
                continue;
            }
            CHECK(lowered(0x03, operands) == listingWith(row.fusedLoop, slots, 0));
            CHECK(lowered(0x03, folded) == listingWith(row.immediateLoop, immediateReads, 0));
        }
    }
    // An opcode without a row is Special, which a later part of lowering handles.
    CHECK(loweringRow(Opcode::Call).form == OperationForm::Special);
    CHECK(OpcodeTable.size() == LoweringTable.size());
}
