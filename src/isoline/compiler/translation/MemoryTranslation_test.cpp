// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/FeatureSupport.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/translation/MemoryTranslation.hpp>
#include <isoline/compiler/translation/NumericRules.hpp>
#include <isoline/compiler/translation/testing/Translation.hpp>
#include <isoline/runtime/RawValue.hpp>
#include <isoline/runtime/Trap.hpp>

using isoline::binary::AddressType;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Opcode;
using isoline::binary::TableType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::drop;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::encodeU64;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::i64Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::compiler::RuntimeStub;
using isoline::compiler::intermediate::Function;
using isoline::compiler::intermediate::Operation;
using isoline::compiler::intermediate::Type;
using isoline::compiler::translation::StubCallTable;
using isoline::runtime::TrapCode;
using namespace isoline::compiler::translation::testing;

namespace
{

/// The bytes of @p opcode: its byte, or its prefix and its sub-opcode.
Bytes opcodeBytes(Opcode opcode)
{
    auto const& info = isoline::binary::OpcodeTable[std::to_underlying(opcode)];
    if (info.prefix == isoline::binary::NoPrefix)
        return { static_cast<uint8_t>(info.code) };
    return code({ { info.prefix }, encodeU32(info.code) });
}

/// A load or store @p opcode of @p memory at @p offset, its alignment 0.
Bytes access(Opcode opcode, uint32_t memory, uint64_t offset)
{
    // A memory other than 0 sets bit 6 of the alignment and follows it with its index.
    auto const flags = memory == 0 ? Bytes { 0x00 } : code({ { 0x40 }, encodeU32(memory) });
    return code({ opcodeBytes(opcode), flags, encodeU64(offset) });
}

/// The bytes of @p opcode with its index immediates @p indices.
Bytes withIndices(Opcode opcode, std::initializer_list<uint32_t> indices)
{
    auto bytes = opcodeBytes(opcode);
    for (auto const index: indices)
        bytes = code({ bytes, encodeU32(index) });
    return bytes;
}

/// ref.null func.
Bytes refNullFunc()
{
    return { 0xD0, 0x70 };
}

/// The memory type of @p pages pages of @p address addresses and a page of 2^@p pageSizeLog2 bytes.
MemoryType memoryOf(AddressType address, uint8_t pageSizeLog2 = isoline::binary::DefaultPageSizeLog2)
{
    return MemoryType { .limits = Limits { .min = 1, .max = std::nullopt, .addressType = address },
                        .pageSizeLog2 = pageSizeLog2 };
}

/// A table of funcref of @p address addresses.
TableType tableOf(AddressType address)
{
    return TableType { .element = FuncRef,
                       .limits = Limits { .min = 2, .max = std::nullopt, .addressType = address } };
}

/// The value @p function's instruction @p index defines.
isoline::compiler::intermediate::ValueId resultOf(Function const& function, uint32_t index)
{
    return function.operands(function.instruction(index).results)[0];
}

/// The operands of @p function's instruction @p index.
std::span<isoline::compiler::intermediate::ValueId const> operandsOf(Function const& function, uint32_t index)
{
    return function.operands(function.instruction(index).operands);
}

/// The immediates of @p function's instruction @p index.
std::span<uint64_t const> immediatesOf(Function const& function, uint32_t index)
{
    return function.immediates(function.instruction(index).immediates);
}

/// The operations of @p function's instructions from @p first on, in order.
std::vector<Operation> operationsFrom(Function const& function, uint32_t first)
{
    auto operations = std::vector<Operation> {};
    for (auto const index: std::views::iota(first, function.instructionCount()))
        operations.push_back(function.instruction(index).operation);
    return operations;
}

/// The one instruction of @p function that is @p operation.
uint32_t onlyOf(Function const& function, Operation operation)
{
    auto const found = instructionsOf(function, operation);
    REQUIRE(found.size() == 1);
    return found.front();
}

/// An immediate's value of @p value.
template <typename Enum>
uint64_t immediateOf(Enum value)
{
    return uint64_t { std::to_underlying(value) };
}

} // namespace

TEST_CASE("MemoryTranslation.loadChecksOffsetPlusSize")
{
    // i32.load offset=8 of memory 0: checkMemory of the index against the context's base and bound with
    // [memory 0, offset 8, size 4], then a load of the address it returns, its offset already in it.
    auto builder = oneFunction(FuncType { .params = { I32 }, .results = { I32 } },
                               {},
                               code({ localGet(0), access(Opcode::I32Load, 0, 8) }));
    builder.memory(memoryOf(AddressType::I32));
    auto const translated = translateOf(builder);
    auto const& function = *translated->function;
    auto const check = onlyOf(function, Operation::CheckMemory);
    auto const operands = operandsOf(function, check);
    REQUIRE(operands.size() == 3);
    CHECK(function.typeOf(operands[0]) == Type::I32);
    CHECK(std::ranges::equal(immediatesOf(function, check), std::array<uint64_t, 3> { 0, 8, 4 }));
    auto const load = onlyOf(function, Operation::Load);
    CHECK(load == check + 1);
    CHECK(operandsOf(function, load)[0] == resultOf(function, check));
    CHECK(std::ranges::equal(
        immediatesOf(function, load),
        std::array<uint64_t, 5> { 0,
                                  0,
                                  4,
                                  immediateOf(isoline::compiler::intermediate::Extension::Zero),
                                  immediateOf(Type::I32) }));
    // The base and bound are memory 0's context values: no memory_base or memory_bound.
    auto const text = translated->text();
    CHECK(text.contains("v2 = load_context [field=memory0_base, type=ptr]"));
    CHECK(text.contains("check_memory v0, v2, v3 [memory=0, offset=8, size=4]"));
    CHECK_FALSE(text.contains("memory_base"));
}

TEST_CASE("MemoryTranslation.indexKeepsTheAddressType")
{
    // A 32-bit memory's index reaches checkMemory as its I32, never widened: the guard reads its low 32
    // bits (GuardTable). A 64-bit memory's is an I64. A store's value follows the check.
    for (auto const address: { AddressType::I32, AddressType::I64 })
    {
        auto const index = address == AddressType::I32 ? I32 : I64;
        auto builder = oneFunction(FuncType { .params = { index, I64 }, .results = {} },
                                   {},
                                   code({ localGet(0), localGet(1), access(Opcode::I64Store16, 0, 2) }));
        builder.memory(memoryOf(address));
        auto const translated = translateOf(builder);
        auto const& function = *translated->function;
        auto const check = onlyOf(function, Operation::CheckMemory);
        CHECK(function.typeOf(operandsOf(function, check)[0])
              == (address == AddressType::I32 ? Type::I32 : Type::I64));
        CHECK(instructionsOf(function, Operation::I64ExtendI32U).empty());
        auto const store = onlyOf(function, Operation::Store);
        CHECK(operandsOf(function, store)[0] == resultOf(function, check));
        CHECK(std::ranges::equal(immediatesOf(function, store), std::array<uint64_t, 3> { 0, 0, 2 }));
    }
}

TEST_CASE("MemoryTranslation.memory64OffsetNearTwoToThe64Traps")
{
    // i64.load offset=2^64 - 4 of a 64-bit memory: offset + 8 passes 2^64 - 1, which no index fits, so the
    // access is a trap after its index is evaluated, with no checkMemory. Offset 2^64 - 9 still fits a 0
    // index, and is checked. Executed agreement with the reference is Task 16's.
    auto const translatedAt = [](uint64_t offset) {
        auto builder =
            oneFunction(FuncType { .params = { I64 }, .results = { I64 } },
                        {},
                        code({ localGet(0), i64Const(1), { 0x7C }, access(Opcode::I64Load, 0, offset) }));
        builder.memory(memoryOf(AddressType::I64));
        return translateOf(builder);
    };
    auto const max = std::numeric_limits<uint64_t>::max();
    auto const past = translatedAt(max - 3);
    auto const& function = *past->function;
    CHECK(instructionsOf(function, Operation::CheckMemory).empty());
    auto const add = onlyOf(function, Operation::I64Add);
    auto const trap = onlyOf(function, Operation::Trap);
    CHECK(trap > add);
    CHECK(immediatesOf(function, trap)[0] == immediateOf(TrapCode::OutOfBoundsMemoryAccess));
    auto const fits = translatedAt(max - 8);
    CHECK(instructionsOf(*fits->function, Operation::CheckMemory).size() == 1);
    CHECK(instructionsOf(*fits->function, Operation::Trap).empty());
}

TEST_CASE("MemoryTranslation.bulkOperationsAreStubCalls")
{
    // memory.fill: its operands zero-extended, the countdown stored, callStub(MemoryFill) of memory 0 and
    // the three operands, then the three context values loaded again.
    auto builder =
        oneFunction(FuncType { .params = {}, .results = {} },
                    {},
                    code({ i32Const(0), i32Const(7), i32Const(16), withIndices(Opcode::MemoryFill, { 0 }) }));
    builder.memory(memoryOf(AddressType::I32));
    auto const translated = translateOf(builder);
    auto const& function = *translated->function;
    auto const stub = onlyOf(function, Operation::CallStub);
    CHECK(immediatesOf(function, stub)[0] == immediateOf(RuntimeStub::MemoryFill));
    auto const operands = operandsOf(function, stub);
    REQUIRE(operands.size() == 4);
    CHECK(function.typeOf(operands[0]) == Type::I32);
    for (auto const operand: operands.subspan(1))
        CHECK(function.typeOf(operand) == Type::I64);
    CHECK(instructionsOf(function, Operation::I64ExtendI32U).size() == 3);
    CHECK(function.instruction(stub - 1).operation == Operation::StoreContext);
    auto const after = operationsFrom(function, stub + 1);
    REQUIRE(after.size() >= 3);
    CHECK(std::ranges::equal(
        after | std::views::take(3),
        std::array { Operation::LoadContext, Operation::LoadContext, Operation::LoadContext }));
}

TEST_CASE("MemoryTranslation.everyStubRowCallsItsStub")
{
    // Each row of StubCallTable, on a 32-bit and a 64-bit memory and table: callStub of its stub with its
    // indices, then its operands; a grow's result narrowed for 32 bits.
    auto const bodyOf = [](Opcode opcode, Bytes const& index, Bytes const& count) {
        switch (opcode)
        {
            case Opcode::TableGet: return code({ index, withIndices(opcode, { 0 }), drop() });
            case Opcode::TableSet: return code({ index, refNullFunc(), withIndices(opcode, { 0 }) });
            case Opcode::MemoryGrow: return code({ index, withIndices(opcode, { 0 }), drop() });
            case Opcode::MemoryInit:
                return code({ index, i32Const(0), i32Const(0), withIndices(opcode, { 0, 0 }) });
            case Opcode::DataDrop:
            case Opcode::ElemDrop: return withIndices(opcode, { 0 });
            case Opcode::MemoryCopy: return code({ index, index, count, withIndices(opcode, { 0, 0 }) });
            case Opcode::MemoryFill: return code({ index, i32Const(0), count, withIndices(opcode, { 0 }) });
            case Opcode::TableInit:
                return code({ index, i32Const(0), i32Const(0), withIndices(opcode, { 0, 0 }) });
            case Opcode::TableCopy: return code({ index, index, count, withIndices(opcode, { 0, 0 }) });
            case Opcode::TableGrow: return code({ refNullFunc(), index, withIndices(opcode, { 0 }), drop() });
            case Opcode::TableFill: return code({ index, refNullFunc(), count, withIndices(opcode, { 0 }) });
            default: FAIL("no body for a row"); return Bytes {};
        }
    };
    for (auto const& row: StubCallTable)
        for (auto const address: { AddressType::I32, AddressType::I64 })
        {
            INFO(isoline::binary::OpcodeTable[std::to_underlying(row.opcode)].text
                 << ", 64-bit: " << (address == AddressType::I64));
            auto const index = address == AddressType::I32 ? i32Const(0) : i64Const(0);
            auto builder =
                oneFunction(FuncType { .params = {}, .results = {} }, {}, bodyOf(row.opcode, index, index));
            // table.get and table.set are stub calls on an imported table only.
            if (row.tables == isoline::compiler::translation::StubTables::Imported)
                builder.importTable("m", "t", tableOf(address));
            else
                builder.table(tableOf(address));
            builder.memory(memoryOf(address));
            builder.dataCount(1);
            builder.element({ 0x01, 0x00, 0x00 });
            builder.data({ 0x01, 0x01, 0xAA });
            auto const translated = translateOf(builder);
            auto const& function = *translated->function;
            auto const stub = onlyOf(function, Operation::CallStub);
            CHECK(immediatesOf(function, stub)[0] == immediateOf(row.stub));
            auto const indices = isoline::compiler::translation::indexCountOf(row.indices);
            auto const operands = operandsOf(function, stub);
            REQUIRE(operands.size() == indices + row.operands);
            for (auto const operand: operands.first(indices))
                CHECK(function.typeOf(operand) == Type::I32);
            for (auto const operand: operands.subspan(indices))
                CHECK((function.typeOf(operand) == Type::I64 || function.typeOf(operand) == Type::Ref));
            auto const isGrow = row.result == isoline::compiler::translation::StubResult::MemoryAddress
                                || row.result == isoline::compiler::translation::StubResult::TableAddress;
            auto const wraps = instructionsOf(function, Operation::I32WrapI64);
            CHECK(wraps.size() == (isGrow && address == AddressType::I32 ? 1U : 0U));
            for (auto const wrap: wraps)
                CHECK(operandsOf(function, wrap)[0] == resultOf(function, stub));
        }
}

TEST_CASE("MemoryTranslation.callIndirectCheckOrder")
{
    // call_indirect, in the reference's order: the element (UndefinedElement), a null element
    // (UninitializedElement, naming the index), the dispatch row (UndefinedElement, naming the index), the
    // type (IndirectCallTypeMismatch), then the call of the row, all before the callee's entry.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I64 }, .results = { I32 } });
    builder.type(FuncType { .params = { I64, I32 }, .results = { I32 } });
    builder.function(1, {}, code({ localGet(0), localGet(1), { 0x11, 0x00, 0x00 } }));
    builder.table(tableOf(AddressType::I32));
    auto const translated = translateOf(builder);
    auto const& function = *translated->function;
    auto const first = onlyOf(function, Operation::CheckTable);
    auto const expected = std::to_array({ Operation::CheckTable,
                                          Operation::LoadTableElement,
                                          Operation::RefIsNull,
                                          Operation::TrapIfSubject,
                                          Operation::CheckDispatch,
                                          Operation::LoadDispatchType,
                                          Operation::TypeId,
                                          Operation::I32Ne,
                                          Operation::TrapIf,
                                          Operation::StoreContext,
                                          Operation::CallIndirect });
    auto const operations = operationsFrom(function, first);
    REQUIRE(operations.size() >= expected.size());
    CHECK(std::ranges::equal(operations | std::views::take(expected.size()), expected));
    auto const index = operandsOf(function, first)[0];
    CHECK(function.typeOf(index) == Type::I32);
    CHECK(immediatesOf(function, first)[1] == immediateOf(TrapCode::UndefinedElement));
    auto const nullTrap = first + 3;
    CHECK(operandsOf(function, nullTrap)[1] == index);
    CHECK(immediatesOf(function, nullTrap)[0] == immediateOf(TrapCode::UninitializedElement));
    auto const dispatch = first + 4;
    CHECK(operandsOf(function, dispatch)[0] == resultOf(function, first + 1));
    CHECK(operandsOf(function, dispatch)[1] == index);
    CHECK(immediatesOf(function, dispatch)[0] == immediateOf(TrapCode::UndefinedElement));
    CHECK(immediatesOf(function, first + 6)[0] == 0);
    CHECK(immediatesOf(function, first + 8)[0] == immediateOf(TrapCode::IndirectCallTypeMismatch));
    auto const call = first + 10;
    auto const arguments = operandsOf(function, call);
    REQUIRE(arguments.size() == 2);
    CHECK(arguments[0] == resultOf(function, dispatch));
    CHECK(function.typeOf(arguments[1]) == Type::I64);
    // The context values are reloaded after the call.
    CHECK(function.instruction(call + 1).operation == Operation::LoadContext);
}

TEST_CASE("MemoryTranslation.tableElementsByField")
{
    // A defined table's table.set: checkTable (OutOfBoundsTableAccess), then storeTableElement, whose
    // immediates name the table and RefLayout's four field offsets; no 16-byte access anywhere. table.get
    // reads the same fields.
    auto builder = oneFunction(FuncType { .params = { I32 }, .results = {} },
                               {},
                               code({ localGet(0),
                                      localGet(0),
                                      withIndices(Opcode::TableGet, { 0 }),
                                      withIndices(Opcode::TableSet, { 0 }) }));
    builder.table(tableOf(AddressType::I32));
    auto const translated = translateOf(builder);
    auto const& function = *translated->function;
    CHECK(instructionsOf(function, Operation::CallStub).empty());
    auto const& layout = isoline::runtime::RefLayoutOfRef;
    auto const fields = std::array<uint64_t, 5> {
        0, layout.kindOffset, layout.heapKindOffset, layout.heapTypeIndexOffset, layout.addressOffset
    };
    auto const store = onlyOf(function, Operation::StoreTableElement);
    CHECK(std::ranges::equal(immediatesOf(function, store), fields));
    CHECK(std::ranges::equal(immediatesOf(function, onlyOf(function, Operation::LoadTableElement)), fields));
    for (auto const check: instructionsOf(function, Operation::CheckTable))
        CHECK(immediatesOf(function, check)[1] == immediateOf(TrapCode::OutOfBoundsTableAccess));
    CHECK(operandsOf(function, store)[0]
          == resultOf(function, instructionsOf(function, Operation::CheckTable).back()));
    CHECK(instructionsOf(function, Operation::Load).empty());
    CHECK(instructionsOf(function, Operation::Store).empty());
    CHECK_FALSE(translated->text().contains("size=16"));
}

TEST_CASE("MemoryTranslation.customPageSizeOfOneByte")
{
    // memory.size is the bound in pages: shifted by 16 for the default page, not at all for a page of one
    // byte; a 32-bit memory's is narrowed to an I32, a 64-bit one's is the I64 itself.
    struct Row
    {
        AddressType address;
        uint8_t pageSizeLog2;
        std::size_t shifts;
        std::size_t wraps;
    };
    for (auto const& row: std::to_array<Row>({ { AddressType::I32, 16, 1, 1 },
                                               { AddressType::I32, 0, 0, 1 },
                                               { AddressType::I64, 16, 1, 0 },
                                               { AddressType::I64, 0, 0, 0 } }))
    {
        auto const result = row.address == AddressType::I32 ? I32 : I64;
        auto builder = oneFunction(
            FuncType { .params = {}, .results = { result } }, {}, withIndices(Opcode::MemorySize, { 0 }));
        builder.memory(memoryOf(row.address, row.pageSizeLog2));
        auto const translated = translateOf(builder);
        auto const& function = *translated->function;
        INFO(translated->text());
        auto const shifts = instructionsOf(function, Operation::I64ShrU);
        CHECK(shifts.size() == row.shifts);
        CHECK(instructionsOf(function, Operation::I32WrapI64).size() == row.wraps);
        if (row.shifts != 0)
            CHECK(translated->text().contains("i64.const [value=16]"));
        CHECK(instructionsOf(function, Operation::MemoryBound).empty());
    }
}

TEST_CASE("MemoryTranslation.tableSizeNarrowsForThirtyTwoBits")
{
    for (auto const address: { AddressType::I32, AddressType::I64 })
    {
        auto const result = address == AddressType::I32 ? I32 : I64;
        auto builder = oneFunction(
            FuncType { .params = {}, .results = { result } }, {}, withIndices(Opcode::TableSize, { 0 }));
        builder.table(tableOf(address));
        auto const translated = translateOf(builder);
        auto const& function = *translated->function;
        auto const size = onlyOf(function, Operation::TableSize);
        auto const wraps = instructionsOf(function, Operation::I32WrapI64);
        CHECK(wraps.size() == (address == AddressType::I32 ? 1U : 0U));
        for (auto const wrap: wraps)
            CHECK(operandsOf(function, wrap)[0] == resultOf(function, size));
    }
}

TEST_CASE("MemoryTranslation.otherMemoriesReloadPerBlockAndAfterCalls")
{
    // Memory 1's base and bound: loaded once for two accesses in a block, again after a call, again in an
    // if's arm, and again after its end, where the arm's values do not dominate. Memory 0 never loads them.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = {} });
    builder.type(FuncType { .params = {}, .results = {} });
    builder.function(0,
                     {},
                     code({ localGet(0),
                            access(Opcode::I32Load, 1, 0),
                            drop(),
                            localGet(0),
                            access(Opcode::I32Load8U, 1, 0),
                            drop(),
                            localGet(0),
                            access(Opcode::I32Load, 0, 0),
                            drop(),
                            isoline::binary::testing::call(1),
                            localGet(0),
                            access(Opcode::I32Load, 1, 0),
                            drop(),
                            localGet(0),
                            { 0x04, 0x40 },
                            localGet(0),
                            access(Opcode::I32Load, 1, 4),
                            drop(),
                            { 0x0B },
                            localGet(0),
                            access(Opcode::I32Load, 1, 8),
                            drop() }));
    builder.function(1, {}, {});
    builder.memory(memoryOf(AddressType::I32));
    builder.memory(memoryOf(AddressType::I32));
    auto const translated = translateOf(builder);
    auto const& function = *translated->function;
    CHECK(instructionsOf(function, Operation::CheckMemory).size() == 6);
    CHECK(instructionsOf(function, Operation::MemoryBase).size() == 4);
    CHECK(instructionsOf(function, Operation::MemoryBound).size() == 4);
    for (auto const base: instructionsOf(function, Operation::MemoryBase))
        CHECK(immediatesOf(function, base)[0] == 1);
}

TEST_CASE("MemoryTranslation.everyCompiledOpcodeTranslates")
{
    // Every instruction of every feature compiledFeatures() holds translates: none reaches a refusal it
    // should not, or the end of the translator's routes. A row of a compiled feature added without a
    // sample here fails, as does one whose route is missing.
    auto samples = std::map<Opcode, Bytes> {
        { Opcode::Unreachable, { 0x00 } },
        { Opcode::Nop, { 0x01 } },
        { Opcode::Block, { 0x02, 0x40, 0x0B } },
        { Opcode::Loop, { 0x03, 0x40, 0x0B } },
        { Opcode::If, code({ i32Const(0), { 0x04, 0x40, 0x05, 0x0B } }) },
        { Opcode::Else, code({ i32Const(0), { 0x04, 0x40, 0x05, 0x0B } }) },
        { Opcode::End, {} },
        { Opcode::Br, { 0x02, 0x40, 0x0C, 0x00, 0x0B } },
        { Opcode::BrIf, code({ { 0x02, 0x40 }, i32Const(0), { 0x0D, 0x00, 0x0B } }) },
        { Opcode::BrTable, code({ { 0x02, 0x40 }, i32Const(0), { 0x0E, 0x01, 0x00, 0x00, 0x0B } }) },
        { Opcode::Return, { 0x0F } },
        { Opcode::Call, code({ i32Const(0), isoline::binary::testing::call(1), drop() }) },
        { Opcode::CallIndirect, code({ i32Const(0), i32Const(0), { 0x11, 0x01, 0x00 }, drop() }) },
        { Opcode::Drop, code({ i32Const(0), drop() }) },
        { Opcode::Select, code({ i32Const(1), i32Const(2), i32Const(0), { 0x1B }, drop() }) },
        { Opcode::SelectTyped,
          code({ i32Const(1), i32Const(2), i32Const(0), { 0x1C, 0x01, 0x7F }, drop() }) },
        { Opcode::LocalGet, code({ localGet(0), drop() }) },
        { Opcode::LocalSet, code({ i32Const(0), isoline::binary::testing::localSet(0) }) },
        { Opcode::LocalTee, code({ i32Const(0), { 0x22, 0x00 }, drop() }) },
        { Opcode::GlobalGet, code({ { 0x23, 0x00 }, drop() }) },
        { Opcode::GlobalSet, code({ i32Const(0), { 0x24, 0x00 } }) },
        { Opcode::TableGet, code({ i32Const(0), withIndices(Opcode::TableGet, { 0 }), drop() }) },
        { Opcode::TableSet, code({ i32Const(0), refNullFunc(), withIndices(Opcode::TableSet, { 0 }) }) },
        { Opcode::MemorySize, code({ withIndices(Opcode::MemorySize, { 0 }), drop() }) },
        { Opcode::MemoryGrow, code({ i32Const(1), withIndices(Opcode::MemoryGrow, { 0 }), drop() }) },
        { Opcode::I32Const, code({ i32Const(0), drop() }) },
        { Opcode::I64Const, code({ i64Const(0), drop() }) },
        { Opcode::F32Const, code({ { 0x43, 0, 0, 0, 0 }, drop() }) },
        { Opcode::F64Const, code({ { 0x44, 0, 0, 0, 0, 0, 0, 0, 0 }, drop() }) },
        { Opcode::RefNull, code({ refNullFunc(), drop() }) },
        { Opcode::RefIsNull, code({ refNullFunc(), { 0xD1 }, drop() }) },
        { Opcode::RefFunc, code({ { 0xD2, 0x01 }, drop() }) },
        { Opcode::MemoryInit,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::MemoryInit, { 0, 0 }) }) },
        { Opcode::DataDrop, withIndices(Opcode::DataDrop, { 0 }) },
        { Opcode::MemoryCopy,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::MemoryCopy, { 0, 0 }) }) },
        { Opcode::MemoryFill,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::MemoryFill, { 0 }) }) },
        { Opcode::TableInit,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::TableInit, { 0, 0 }) }) },
        { Opcode::ElemDrop, withIndices(Opcode::ElemDrop, { 0 }) },
        { Opcode::TableCopy,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::TableCopy, { 0, 0 }) }) },
        { Opcode::TableGrow,
          code({ refNullFunc(), i32Const(1), withIndices(Opcode::TableGrow, { 0 }), drop() }) },
        { Opcode::TableSize, code({ withIndices(Opcode::TableSize, { 0 }), drop() }) },
        { Opcode::TableFill,
          code({ i32Const(0), refNullFunc(), i32Const(1), withIndices(Opcode::TableFill, { 0 }) }) },
    };
    // Every scalar load and store, from its row: an address, the value a store stores, the access.
    for (auto const& row: isoline::compiler::translation::MemoryAccessTable)
    {
        auto const value = [&row] {
            switch (row.type)
            {
                case Type::I32: return i32Const(0);
                case Type::I64: return i64Const(0);
                case Type::F32: return Bytes { 0x43, 0, 0, 0, 0 };
                default: return Bytes { 0x44, 0, 0, 0, 0, 0, 0, 0, 0 };
            }
        }();
        samples[row.opcode] = row.kind == isoline::compiler::translation::AccessKind::Load
                                  ? code({ i32Const(0), access(row.opcode, 0, 0), drop() })
                                  : code({ i32Const(0), value, access(row.opcode, 0, 0) });
    }
    // Every numeric instruction, from its operation's operand types.
    auto const constantOf = [](isoline::compiler::intermediate::OperandType type) {
        using isoline::compiler::intermediate::OperandType;
        switch (type)
        {
            case OperandType::I32: return i32Const(0);
            case OperandType::I64: return i64Const(0);
            case OperandType::F32: return Bytes { 0x43, 0, 0, 0, 0 };
            default: return Bytes { 0x44, 0, 0, 0, 0, 0, 0, 0, 0 };
        }
    };
    auto translatedCount = 0;
    for (auto const& info: isoline::binary::OpcodeTable)
    {
        if (!isoline::compiler::compiledFeatures().includes(info.required))
            continue;
        INFO(info.text);
        auto body = Bytes {};
        if (auto const found = samples.find(info.opcode); found != samples.end())
            body = found->second;
        else
        {
            auto const operation = isoline::compiler::translation::numericOperationOf(info.opcode);
            REQUIRE(operation.has_value());
            auto const& row = isoline::compiler::intermediate::operationRow(*operation);
            for (auto const operand:
                 isoline::compiler::intermediate::operandSignatureRow(row.operands).fixed())
                body = code({ body, constantOf(operand) });
            body = code({ body, opcodeBytes(info.opcode), drop() });
        }
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = {}, .results = {} });
        builder.type(FuncType { .params = { I32 }, .results = { I32 } });
        builder.global(
            isoline::binary::GlobalType { .type = I32, .mutability = isoline::binary::Mutability::Var },
            i32Const(0));
        builder.function(0, { I32 }, body);
        builder.function(1, {}, localGet(0));
        builder.table(tableOf(AddressType::I32));
        builder.memory(memoryOf(AddressType::I32));
        builder.dataCount(1);
        builder.element({ 0x01, 0x00, 0x01, 0x01 });
        builder.data({ 0x01, 0x01, 0xAA });
        auto const translated = translateOf(builder);
        CHECK(translated->function.has_value());
        ++translatedCount;
    }
    // Every row of a compiled feature: 172 of the MVP, 5 + 8 + 9 + 7 of four proposals.
    CHECK(translatedCount == 201);
}

TEST_CASE("MemoryTranslation.wideIndirectCallsRefuseAtThePoolCap", "[.hostile]")
{
    // 200 call_indirects of a type of 1'000 parameters: each reserves its operands before it emits, so a pool
    // cap of 100'000 refuses at the call that would pass it, not after the body; the default caps take it.
    // Run capped (tests/CMakeLists.txt).
    constexpr auto Parameters = std::size_t { 1'000 };
    constexpr auto Calls = 200;
    auto body = Bytes {};
    body.reserve(((Parameters * 2) + 5) * Calls);
    for ([[maybe_unused]] auto const call: std::views::iota(0, Calls))
    {
        for ([[maybe_unused]] auto const argument: std::views::iota(std::size_t { 0 }, Parameters))
            body.insert(body.end(), { 0x20, 0x00 });
        body.insert(body.end(), { 0x41, 0x00, 0x11, 0x01, 0x00 });
    }
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = {} });
    builder.type(FuncType { .params = std::vector(Parameters, I32), .results = {} });
    builder.function(0, {}, body);
    builder.table(tableOf(AddressType::I32));
    auto limits = isoline::compiler::CompileLimits {};
    limits.maxPoolEntries = 100'000;
    auto const refused = translateOf(builder, Options { .limits = limits });
    auto const error = refusalOf(*refused);
    CHECK(error.code == isoline::compiler::CompileErrorCode::PoolTooLarge);
    CHECK(error.limit == 100'000);
    CHECK(error.requested - error.limit <= Parameters + 1'100);
    auto const taken = translateOf(builder);
    CHECK(instructionsOf(*taken->function, Operation::CallIndirect).size() == Calls);

    // One such call and an unreachable, after which nothing reserves: the call's own reservation refuses it.
    auto single = Bytes {};
    for ([[maybe_unused]] auto const argument: std::views::iota(std::size_t { 0 }, Parameters))
        single.insert(single.end(), { 0x20, 0x00 });
    single.insert(single.end(), { 0x41, 0x00, 0x11, 0x01, 0x00, 0x00 });
    auto lone = ModuleBuilder {};
    lone.type(FuncType { .params = { I32 }, .results = {} });
    lone.type(FuncType { .params = std::vector(Parameters, I32), .results = {} });
    lone.function(0, {}, single);
    lone.table(tableOf(AddressType::I32));
    limits.maxPoolEntries = 1'500;
    CHECK(refusalOf(*translateOf(lone, Options { .limits = limits })).code
          == isoline::compiler::CompileErrorCode::PoolTooLarge);
}

TEST_CASE("MemoryTranslation.lookupsMissOutsideTheirTables")
{
    using isoline::compiler::translation::indexCountOf;
    using isoline::compiler::translation::memoryAccessOf;
    using isoline::compiler::translation::stubCallOf;
    CHECK_FALSE(memoryAccessOf(Opcode::Nop).has_value());
    CHECK_FALSE(memoryAccessOf(Opcode::MemorySize).has_value());
    CHECK_FALSE(stubCallOf(Opcode::Nop).has_value());
    CHECK_FALSE(stubCallOf(Opcode::TableSize).has_value());
    CHECK_FALSE(stubCallOf(Opcode::V128Load).has_value());
    REQUIRE(stubCallOf(Opcode::TableFill).has_value());
    CHECK(stubCallOf(Opcode::TableFill)->stub == RuntimeStub::TableFill);
    CHECK(indexCountOf(isoline::compiler::translation::StubIndices::None) == 0);
}

TEST_CASE("MemoryTranslation.importedEntitiesComeFirst")
{
    // An imported global, memory and table, beside a tag: each index space starts with its imports, which
    // global.get, a load, memory.size and table.get (a stub call, the table being imported) reach.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = { I32 } });
    builder.type(FuncType { .params = {}, .results = {} });
    builder.importGlobal("m", "g", isoline::binary::GlobalType { .type = I32 });
    builder.importMemory("m", "memory", memoryOf(AddressType::I64, 0));
    builder.importTable("m", "table", tableOf(AddressType::I32));
    builder.importTag("m", "tag", 1);
    builder.function(0,
                     {},
                     code({ { 0x23, 0x00, 0x1A },
                            i64Const(0),
                            access(Opcode::I32Load, 0, 0),
                            drop(),
                            withIndices(Opcode::MemorySize, { 0 }),
                            drop(),
                            localGet(0),
                            withIndices(Opcode::TableGet, { 0 }),
                            drop(),
                            localGet(0) }));
    auto const translated = translateOf(builder);
    auto const& function = *translated->function;
    CHECK(function.typeOf(operandsOf(function, onlyOf(function, Operation::CheckMemory))[0]) == Type::I64);
    CHECK(instructionsOf(function, Operation::I64ShrU).empty());
    CHECK(immediatesOf(function, onlyOf(function, Operation::CallStub))[0]
          == immediateOf(RuntimeStub::TableGet));
}

TEST_CASE("MemoryTranslation.tablesOfDeferredTypesAreRefused")
{
    // A table of references to a concrete type, met by each instruction that names one, in either of its
    // table positions: refused, as its elements need more than one word.
    auto const typed =
        isoline::binary::ValueType { .kind = isoline::binary::ValueKind::Ref,
                                     .nullable = isoline::binary::Nullable::Yes,
                                     .heap = { .kind = isoline::binary::AbstractHeapType::Concrete,
                                               .typeIndex = 0 } };
    auto const bodies = std::to_array<Bytes>({
        code({ i32Const(0), withIndices(Opcode::TableGet, { 1 }), drop() }),
        code({ withIndices(Opcode::TableSize, { 1 }), drop() }),
        code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::TableInit, { 1, 1 }) }),
        code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::TableCopy, { 0, 1 }) }),
        code({ i32Const(0), { 0x11, 0x00, 0x01 } }),
    });
    for (auto const& body: bodies)
    {
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = {}, .results = {} });
        builder.function(0, {}, body);
        builder.table(tableOf(AddressType::I32));
        builder.table(TableType { .element = typed, .limits = Limits { .min = 1, .max = std::nullopt } });
        builder.element({ 0x01, 0x00, 0x00 });
        builder.element({ 0x05, 0x63, 0x00, 0x00 }); // passive, of (ref null 0), empty
        auto const translated = translateOf(builder);
        CHECK(refusalOf(*translated).code == isoline::compiler::CompileErrorCode::UnsupportedFeature);
    }
}

TEST_CASE("MemoryTranslation.trapSitesStoreTheCountdown")
{
    // In a loop the countdown is the header's parameter, not the stored one: a load's checkMemory, a
    // table.get's checkTable and a call_indirect's checkTable each follow a store of it.
    auto const firstIn = [](Bytes const& access) {
        auto builder = oneFunction(FuncType { .params = { I32 }, .results = {} },
                                   {},
                                   code({ { 0x03, 0x40 }, access, localGet(0), { 0x0D, 0x00, 0x0B } }));
        builder.type(FuncType { .params = {}, .results = {} });
        builder.memory(memoryOf(AddressType::I32));
        builder.table(tableOf(AddressType::I32));
        return translateOf(builder);
    };
    struct Row
    {
        Bytes access;
        Operation check;
    };
    for (auto const& row: std::to_array<Row>({
             { code({ localGet(0), access(Opcode::I32Load, 0, 0), drop() }), Operation::CheckMemory },
             { code({ localGet(0), withIndices(Opcode::TableGet, { 0 }), drop() }), Operation::CheckTable },
             { code({ localGet(0), { 0x11, 0x01, 0x00 } }), Operation::CheckTable },
         }))
    {
        auto const translated = firstIn(row.access);
        auto const& function = *translated->function;
        auto const check = onlyOf(function, row.check);
        CHECK(function.instruction(check - 1).operation == Operation::StoreContext);
    }
}
