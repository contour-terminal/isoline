// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/LoweringTable.hpp>
#include <isoline/fastinterpreter/testing/Lowering.hpp>

using isoline::binary::AddressType;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Limits;
using isoline::binary::Mutability;
using isoline::binary::Opcode;
using isoline::binary::OpcodeTable;
using isoline::binary::TableType;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::fastinterpreter::FusionRow;
using isoline::fastinterpreter::FusionTable;
using isoline::fastinterpreter::heapWord;
using isoline::fastinterpreter::IndexedAccess;
using isoline::fastinterpreter::indexedAccessWord;
using isoline::fastinterpreter::indexPairWord;
using isoline::fastinterpreter::loweringRow;
using isoline::fastinterpreter::LoweringRows;
using isoline::fastinterpreter::LoweringTable;
using isoline::fastinterpreter::OperationForm;
using isoline::fastinterpreter::OperationId;
using isoline::fastinterpreter::operationRow;
using isoline::fastinterpreter::testing::encodingOf;
using isoline::fastinterpreter::testing::functionOf;
using isoline::fastinterpreter::testing::listingOf;
using isoline::fastinterpreter::testing::memoryInstruction;
using isoline::fastinterpreter::testing::op;
using isoline::fastinterpreter::testing::prefixed;
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

/// A module whose function 0, of (i32) -> i32, has the body @p body, with a funcref table of one element for
/// the indirect calls.
[[nodiscard]] ModuleBuilder withTable(Bytes body)
{
    auto module = functionOf({ binary::I32 }, { binary::I32 }, {}, std::move(body));
    module.table(
        TableType { .element = binary::FuncRef,
                    .limits = { .min = 1, .max = std::nullopt, .addressType = binary::AddressType::I32 } });
    return module;
}

/// A module whose function 0, of ((ref null 0)) -> i32, has the body @p body: type 0 is () -> i32, the type
/// call_ref calls.
[[nodiscard]] ModuleBuilder withReference(Bytes body)
{
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = {}, .results = { binary::I32 } });
    module.type(
        FuncType { .params = { binary::concreteRef(0, binary::Nullable::Yes) }, .results = { binary::I32 } });
    module.function(1, {}, std::move(body));
    return module;
}

/// @p module with a memory of one page, of i32 addresses, added.
[[nodiscard]] ModuleBuilder withMemory(ModuleBuilder module)
{
    module.memory(Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 });
    return module;
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
    add(Opcode::Call,
        functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), op(0x10, 0) })),
        "move64 s1, s0\ncall s1, #0\nreturn s1\n");
    add(Opcode::CallIndirect,
        withTable(code({ localGet(0), i32Const(0), { 0x11, 0x00, 0x00 } })),
        "const64 s2, #0\nmove64 s1, s0\ncall_indirect s1, s2, #0, #0\nreturn s1\n");
    add(Opcode::CallRef,
        withReference(code({ localGet(0), { 0x14, 0x00 } })),
        "call_ref s2, s0, #0\nreturn s2\n");
    add(Opcode::ReturnCall,
        functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), op(0x12, 0) })),
        "move64 s1, s0\nreturn_call s1, #0\n");
    add(Opcode::ReturnCallIndirect,
        withTable(code({ localGet(0), i32Const(0), { 0x13, 0x00, 0x00 } })),
        "const64 s2, #0\nmove64 s1, s0\nreturn_call_indirect s1, s2, #0, #0\n");
    add(Opcode::ReturnCallRef,
        withReference(code({ localGet(0), { 0x15, 0x00 } })),
        "return_call_ref s2, s0, #0\n");
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
    add(Opcode::MemorySize,
        withMemory(functionOf({}, { binary::I32 }, {}, { 0x3F, 0x00 })),
        "memory.size s0, #0\nreturn s0\n");
    add(Opcode::MemoryGrow,
        withMemory(functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), { 0x40, 0x00 } }))),
        "memory.grow s1, s0, #0\nreturn s1\n");
    // The bulk operations over three params, their indices two to the word (indexPairWord()).
    auto const threeI32 = std::vector { binary::I32, binary::I32, binary::I32 };
    auto const threeOperands = code({ localGet(0), localGet(1), localGet(2) });
    auto withSegments = [](ModuleBuilder module) {
        module.memory(Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 });
        module.memory(Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 });
        module.table(
            TableType { .element = binary::FuncRef,
                        .limits = { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 } });
        module.table(
            TableType { .element = binary::FuncRef,
                        .limits = { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 } });
        module.dataCount(2);
        module.data({ 0x01, 0x01, 0x2A });
        module.data({ 0x01, 0x01, 0x2B });
        module.element({ 0x01, 0x00, 0x01, 0x00 });
        module.element({ 0x01, 0x00, 0x01, 0x00 });
        // Declares function 0 for ref.func.
        module.element({ 0x03, 0x00, 0x01, 0x00 });
        return module;
    };
    add(Opcode::MemoryFill,
        withSegments(functionOf(threeI32, {}, {}, code({ threeOperands, prefixed(11, { 1 }) }))),
        "memory.fill s0, s1, s2, #1\nreturn s3\n");
    add(Opcode::MemoryCopy,
        withSegments(functionOf(threeI32, {}, {}, code({ threeOperands, prefixed(10, { 0, 1 }) }))),
        std::format("memory.copy s0, s1, s2, #{}\nreturn s3\n", indexPairWord(0, 1)));
    add(Opcode::MemoryInit,
        withSegments(functionOf(threeI32, {}, {}, code({ threeOperands, prefixed(8, { 1, 0 }) }))),
        "memory.init s0, s1, s2, #1\nreturn s3\n");
    add(Opcode::DataDrop,
        withSegments(functionOf({}, {}, {}, prefixed(9, { 1 }))),
        "data.drop #1\nreturn s0\n");
    add(Opcode::TableFill,
        withSegments(functionOf({ binary::I32, binary::FuncRef, binary::I32 },
                                {},
                                {},
                                code({ threeOperands, prefixed(17, { 1 }) }))),
        "table.fill s0, s1, s3, #1\nreturn s4\n");
    add(Opcode::TableCopy,
        withSegments(functionOf(threeI32, {}, {}, code({ threeOperands, prefixed(14, { 1, 0 }) }))),
        "table.copy s0, s1, s2, #1\nreturn s3\n");
    add(Opcode::TableInit,
        withSegments(functionOf(threeI32, {}, {}, code({ threeOperands, prefixed(12, { 0, 1 }) }))),
        std::format("table.init s0, s1, s2, #{}\nreturn s3\n", indexPairWord(0, 1)));
    add(Opcode::ElemDrop,
        withSegments(functionOf({}, {}, {}, prefixed(13, { 1 }))),
        "elem.drop #1\nreturn s0\n");
    add(Opcode::TableGet,
        withSegments(
            functionOf({ binary::I32 }, { binary::FuncRef }, {}, code({ localGet(0), { 0x25, 0x01 } }))),
        "table.get s1, s0, #1\nreturn s1\n");
    add(Opcode::TableSet,
        withSegments(functionOf(
            { binary::I32, binary::FuncRef }, {}, {}, code({ localGet(0), localGet(1), { 0x26, 0x01 } }))),
        "table.set s0, s1, #1\nreturn s3\n");
    add(Opcode::TableSize,
        withSegments(functionOf({}, { binary::I32 }, {}, prefixed(16, { 1 }))),
        "table.size s0, #1\nreturn s0\n");
    add(Opcode::TableGrow,
        withSegments(functionOf({ binary::FuncRef, binary::I32 },
                                { binary::I32 },
                                {},
                                code({ localGet(0), localGet(1), prefixed(15, { 1 }) }))),
        "table.grow s3, s0, s2, #1\nreturn s3\n");
    // ref.null names its heap as the module does (heapWord()): extern, AbstractHeapType 1.
    add(Opcode::RefNull,
        functionOf({}, { binary::ExternRef }, {}, { 0xD0, 0x6F }),
        std::format("ref.null s0, #{}\nreturn s0\n",
                    heapWord(binary::HeapType { .kind = binary::AbstractHeapType::Extern })));
    add(Opcode::RefIsNull,
        functionOf({ binary::FuncRef }, { binary::I32 }, {}, code({ localGet(0), { 0xD1 } })),
        "ref.is_null s2, s0\nreturn s2\n");
    add(Opcode::RefFunc,
        withSegments(functionOf({}, { binary::FuncRef }, {}, { 0xD2, 0x00 })),
        "ref.func s0, #0\nreturn s0\n");
    add(Opcode::RefAsNonNull,
        functionOf({ binary::FuncRef }, { binary::FuncRef }, {}, code({ localGet(0), { 0xD4 } })),
        "ref.as_non_null s2, s0\nreturn s2\n");
    add(Opcode::I64Const, functionOf({}, { binary::I64 }, {}, { 0x42, 0x07 }), "const64 s0, #7\nreturn s0\n");
    add(Opcode::F32Const,
        functionOf({}, { binary::F32 }, {}, { 0x43, 0x00, 0x00, 0x80, 0x3F }),
        "const64 s0, #1065353216\nreturn s0\n");
    add(Opcode::F64Const,
        functionOf({}, { binary::F64 }, {}, { 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F }),
        "const64 s0, #4607182418800017408\nreturn s0\n");
    // v128.const's 16 bytes, two words of lanes; i8x16.shuffle's lane indices alike, its first operand moved
    // to the slots its result replaces.
    auto sixteen = Bytes {};
    for (auto const byte: std::views::iota(uint8_t { 0 }, uint8_t { 16 }))
        sixteen.push_back(byte);
    constexpr auto Low = uint64_t { 0x0706'0504'0302'0100 };
    constexpr auto High = uint64_t { 0x0F0E'0D0C'0B0A'0908 };
    add(Opcode::V128Const,
        functionOf({}, { binary::V128 }, {}, code({ encodingOf(Opcode::V128Const), sixteen })),
        std::format("const128 s0, #{}, #{}\nreturn s0\n", Low, High));
    add(Opcode::I8x16Shuffle,
        functionOf({ binary::V128, binary::V128 },
                   { binary::V128 },
                   {},
                   code({ localGet(0), localGet(1), encodingOf(Opcode::I8x16Shuffle), sixteen })),
        std::format("move128 s4, s0\ni8x16.shuffle s4, s2, #{}, #{}\nreturn s4\n", Low, High));
    return cases;
}

/// The slots a value of @p type takes: two for a v128 or a reference, one for any other.
[[nodiscard]] uint32_t slotsOf(ValueType const& type)
{
    return type.kind == ValueKind::V128 || type.kind == ValueKind::Ref ? 2U : 1U;
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
            if (row.form == OperationForm::Special || row.form == OperationForm::Load
                || row.form == OperationForm::Store)
                continue;
            auto const& info = binary::opcodeInfo(row.opcode);
            INFO(info.text);
            auto const& signature = binary::signatureInfo(info.signature);
            auto const params = typesOf(signature.paramKinds());
            auto const results = typesOf(signature.resultKinds());
            auto const unary = row.form == OperationForm::Unary;
            auto const body = unary ? code({ localGet(0), encodingOf(row.opcode) })
                                    : code({ localGet(0), localGet(1), encodingOf(row.opcode) });
            // A v128 operand takes two slots, a scalar one.
            auto const first = slotsOf(params[0]);
            auto const top = unary ? first : first + slotsOf(params[1]);
            auto const expected =
                unary
                    ? std::format("{} s{}, s0\nreturn s{}\n", operationRow(row.operation).text, top, top)
                    : std::format(
                          "{} s{}, s0, s{}\nreturn s{}\n", operationRow(row.operation).text, top, first, top);
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
    SECTION("every Load and Store row, to memory 0 of either address type and to another memory")
    {
        // memory 0 of i32 addresses takes the row's operation, of i64 addresses its memory64 twin; memory 1
        // the indexed operation naming memory 1 and the twin of memory 1's address type.
        for (auto const& row: LoweringRows)
        {
            if (row.form != OperationForm::Load && row.form != OperationForm::Store)
                continue;
            auto const& info = binary::opcodeInfo(row.opcode);
            INFO(info.text);
            auto const* access = binary::memoryAccessOf(row.opcode);
            REQUIRE(access != nullptr);
            auto const load = row.form == OperationForm::Load;
            // A lane access, at lane 1, takes the vector it changes or stores, which a lane load moves to the
            // slots above its address first.
            auto const lane = access->shape == binary::VectorShape::Lane;
            for (auto const addressType: { AddressType::I32, AddressType::I64 })
            {
                for (auto const memory: { 0U, 1U })
                {
                    auto const addressKind = addressType == AddressType::I64 ? binary::I64 : binary::I32;
                    auto params = std::vector { addressKind };
                    if (!load || lane)
                        params.push_back(binary::ValueType { .kind = access->value });
                    auto const results = load ? std::vector { binary::ValueType { .kind = access->value } }
                                              : std::vector<ValueType> {};
                    auto const operands =
                        params.size() == 1 ? localGet(0) : code({ localGet(0), localGet(1) });
                    auto instruction = memoryInstruction(row.opcode, 8, memory);
                    if (lane)
                        instruction.push_back(1);
                    auto module = functionOf(params, results, {}, code({ operands, instruction }));
                    if (memory != 0)
                        module.memory(
                            Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 });
                    module.memory(Limits { .min = 1, .max = std::nullopt, .addressType = addressType });
                    auto const twin = addressType == AddressType::I64 ? row.memory64 : row.operation;
                    auto const indexed = load ? OperationId::LoadIndexed : OperationId::StoreIndexed;
                    auto const operation = memory == 0 ? twin : indexed;
                    auto immediates = std::string { "#8" };
                    if (memory != 0)
                        immediates = std::format(
                            "#8, #{}",
                            indexedAccessWord(IndexedAccess { .memory = memory,
                                                              .access = twin,
                                                              .lane = static_cast<uint8_t>(lane ? 1 : 0) }));
                    else if (lane)
                        immediates = "#8, #1";
                    // The locals' slots: the address's, and a vector's two.
                    auto const locals = params.size() == 1 ? 1U : 1U + slotsOf(params[1]);
                    auto expected = std::string {};
                    if (load && lane)
                        expected = std::format("move128 s{}, s1\n{} s{}, s0, {}\nreturn s{}\n",
                                               locals + 1,
                                               operationRow(operation).text,
                                               locals,
                                               immediates,
                                               locals);
                    else if (load)
                        expected = std::format(
                            "{} s1, s0, {}\nreturn s1\n", operationRow(operation).text, immediates);
                    else
                        expected = std::format(
                            "{} s0, s1, {}\nreturn s{}\n", operationRow(operation).text, immediates, locals);
                    CHECK(listingOf(module) == expected);
                }
            }
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
    CHECK(loweringRow(Opcode::Throw).form == OperationForm::Special);
    CHECK(loweringRow(Opcode::Throw).operation == OperationId::Nop);
    CHECK(OpcodeTable.size() == LoweringTable.size());
}

TEST_CASE("LoweringRows.namingChecksDecide")
{
    // The name checks the static_asserts run, run here too (they are constexpr, for MSVC): each accepts the
    // rows of the tables and refuses a row naming another instruction's or another family's operation.
    namespace detail = isoline::fastinterpreter::detail;
    CHECK(detail::spells("i32.eq.br_if", { "i32.eq", ".", "br_if" }));
    CHECK_FALSE(detail::spells("i32.eq.br_if", { "i32.ne", ".", "br_if" }));
    CHECK_FALSE(detail::spells("i32.eq.br_if.loop", { "i32.eq", ".", "br_if" }));
    for (auto const& row: LoweringRows)
        if (row.form == OperationForm::Load || row.form == OperationForm::Store)
            CHECK(detail::memoryRowFits(row));
    auto swapped = loweringRow(Opcode::I32Load);
    std::swap(swapped.operation, swapped.memory64);
    CHECK_FALSE(detail::memoryRowFits(swapped));
    auto other = loweringRow(Opcode::I32Load);
    other.memory64 = OperationId::I64LoadMemory64;
    CHECK_FALSE(detail::memoryRowFits(other));
    auto indexed = loweringRow(Opcode::I32Load);
    indexed.memory64 = OperationId::LoadIndexed;
    CHECK_FALSE(detail::memoryRowFits(indexed));
    auto narrower = loweringRow(Opcode::I32Load);
    narrower.memory64 = OperationId::I32Load8S;
    CHECK_FALSE(detail::memoryRowFits(narrower));
    auto numeric = loweringRow(Opcode::I32Load);
    numeric.operation = OperationId::I32Add;
    CHECK_FALSE(detail::memoryRowFits(numeric));
}
