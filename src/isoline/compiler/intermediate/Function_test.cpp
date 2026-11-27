// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/compiler/FeatureSupport.hpp>
#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/intermediate/Printer.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>

using isoline::compiler::intermediate::BlockId;
using isoline::compiler::intermediate::Builder;
using isoline::compiler::intermediate::Effect;
using isoline::compiler::intermediate::effectOf;
using isoline::compiler::intermediate::Function;
using isoline::compiler::intermediate::Hotness;
using isoline::compiler::intermediate::ImmediateField;
using isoline::compiler::intermediate::immediateSchemaRow;
using isoline::compiler::intermediate::ImmediateSchemaTable;
using isoline::compiler::intermediate::isTerminator;
using isoline::compiler::intermediate::nameOf;
using isoline::compiler::intermediate::operandSignatureRow;
using isoline::compiler::intermediate::OperandTail;
using isoline::compiler::intermediate::OperandType;
using isoline::compiler::intermediate::Operation;
using isoline::compiler::intermediate::OperationCount;
using isoline::compiler::intermediate::OperationTable;
using isoline::compiler::intermediate::print;
using isoline::compiler::intermediate::ResultRule;
using isoline::compiler::intermediate::Type;
using isoline::compiler::intermediate::ValueId;
using isoline::compiler::intermediate::verify;

namespace
{

/// The operand type a WebAssembly value kind becomes.
OperandType operandTypeOf(isoline::binary::ValueKind kind)
{
    using isoline::binary::ValueKind;
    switch (kind)
    {
        case ValueKind::I32: return OperandType::I32;
        case ValueKind::I64: return OperandType::I64;
        case ValueKind::F32: return OperandType::F32;
        case ValueKind::F64: return OperandType::F64;
        default: FAIL("no scalar kind"); return OperandType::Any;
    }
}

/// The result rule a WebAssembly value kind becomes.
ResultRule resultRuleOf(isoline::binary::ValueKind kind)
{
    using isoline::binary::ValueKind;
    switch (kind)
    {
        case ValueKind::I32: return ResultRule::I32;
        case ValueKind::I64: return ResultRule::I64;
        case ValueKind::F32: return ResultRule::F32;
        case ValueKind::F64: return ResultRule::F64;
        default: FAIL("no scalar kind"); return ResultRule::None;
    }
}

/// The value of @p value as an immediate.
uint64_t immediate(auto value)
{
    return static_cast<uint64_t>(value);
}

/// The immediates of a constant @p value.
std::array<uint64_t, 1> constant(uint64_t value)
{
    return { value };
}

constexpr auto I32Result = std::array { Type::I32 };

/// An arena for one test: what every Function of it allocates from.
struct Arena
{
    std::pmr::monotonic_buffer_resource memory { std::pmr::new_delete_resource() };
};

/// Makes @p resource the default resource for its lifetime, restoring the previous one however the scope
/// is left, a failed REQUIRE included.
class DefaultResourceScope
{
  public:
    explicit DefaultResourceScope(std::pmr::memory_resource* resource):
        _previous { std::pmr::set_default_resource(resource) }
    {
    }
    DefaultResourceScope(DefaultResourceScope const&) = delete;
    DefaultResourceScope& operator=(DefaultResourceScope const&) = delete;
    DefaultResourceScope(DefaultResourceScope&&) = delete;
    DefaultResourceScope& operator=(DefaultResourceScope&&) = delete;
    ~DefaultResourceScope() { std::pmr::set_default_resource(_previous); }

  private:
    std::pmr::memory_resource* _previous;
};

} // namespace

TEST_CASE("Function.operationTableIsComplete")
{
    // One row per enumerator, in order (also a static_assert), each text unique.
    auto texts = std::set<std::string_view> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, OperationCount))
    {
        auto const& row = OperationTable[index];
        INFO(row.text);
        CHECK(std::to_underlying(row.operation) == index);
        CHECK(texts.insert(row.text).second);
        CHECK(nameOf(row.operation) == row.text);
        CHECK(effectOf(row.operation) == row.effect);

        auto const& operands = operandSignatureRow(row.operands);
        auto const& schema = immediateSchemaRow(row.immediates);
        // A rule that reads the first operand has one; one that reads a type immediate has a fixed one.
        if (row.result == ResultRule::SameAsOperand0)
            CHECK(operands.count >= 1);
        auto const fixedType = std::ranges::count(schema.fixed, ImmediateField::Type);
        CHECK((row.result == ResultRule::FromImmediate) == (fixedType == 1));
        CHECK(std::ranges::count(schema.repeated, ImmediateField::Type) == 0);
        // Any number of results only for what takes any number of operands.
        if (row.result == ResultRule::Multiple)
            CHECK(operands.tail == OperandTail::Variadic);
        // A terminator defines nothing; an operation with targets is a terminator whose operands go on.
        if (isTerminator(row.operation))
            CHECK(row.result == ResultRule::None);
        auto const targets = std::ranges::count(schema.fixed, ImmediateField::Target)
                             + std::ranges::count(schema.repeated, ImmediateField::Target);
        if (targets != 0)
        {
            CHECK(isTerminator(row.operation));
            CHECK(operands.tail == OperandTail::Variadic);
        }
        // SameAsOperand0 operand types refer back to a first operand of their own.
        for (auto const position: std::views::iota(std::size_t { 0 }, std::size_t { operands.count }))
            if (operands.types[position] == OperandType::SameAsOperand0)
                CHECK(position > 0);
    }
    CHECK(OperationTable[0].operation == Operation::Removed);

    // Every schema pairs each Target with the ArgumentCount right after it.
    for (auto const& schema: ImmediateSchemaTable)
        for (auto const fields: { schema.fixed, schema.repeated })
            for (auto const index: std::views::iota(std::size_t { 0 }, fields.size()))
                if (fields[index] == ImmediateField::Target)
                {
                    REQUIRE(index + 1 < fields.size());
                    CHECK(fields[index + 1] == ImmediateField::ArgumentCount);
                }

    // One row per scalar numeric WebAssembly instruction the compiler compiles, named as the text format
    // names it, with the instruction's operand and result types; the trapping ones, and only they, MayTrap.
    auto numeric = std::size_t { 0 };
    for (auto const& info: isoline::binary::OpcodeTable)
    {
        using isoline::binary::OpcodeSignature;
        if (info.signature == OpcodeSignature::Special || info.signature == OpcodeSignature::None
            || !isoline::compiler::compiledFeatures().includes(info.required))
            continue;
        INFO(info.text);
        ++numeric;
        auto const found = std::ranges::find(
            OperationTable, info.text, &isoline::compiler::intermediate::OperationRow::text);
        REQUIRE(found != OperationTable.end());
        auto const& signature = isoline::binary::SignatureTable[std::to_underlying(info.signature)];
        auto const& operands = operandSignatureRow(found->operands);
        CHECK(operands.tail == OperandTail::Fixed);
        REQUIRE(operands.count == signature.paramCount);
        for (auto const position: std::views::iota(std::size_t { 0 }, std::size_t { operands.count }))
            CHECK(operands.types[position] == operandTypeOf(signature.params[position]));
        REQUIRE(signature.resultCount == 1);
        CHECK(found->result == resultRuleOf(signature.results[0]));
        auto const text = std::string_view { info.text };
        auto const traps = text.contains(".div_") || text.contains(".rem_")
                           || (text.contains(".trunc_f") && !text.contains("_sat_"));
        CHECK((found->effect == Effect::MayTrap) == traps);
        CHECK((found->effect == Effect::Pure) == !traps);
    }
    CHECK(numeric == 140);
}

TEST_CASE("Function.buildsBlockParameters")
{
    // A diamond whose merge takes the value either arm computed as a block parameter.
    auto arena = Arena {};
    auto function = Function { arena.memory };
    auto builder = Builder { function };
    auto const entry = builder.createBlock();
    auto const left = builder.createBlock();
    auto const right = builder.createBlock();
    auto const merge = builder.createBlock();
    auto const condition = builder.addParameter(entry, Type::I32);
    auto const merged = builder.addParameter(merge, Type::I32);
    function.block(right).hotness = Hotness::Cold;

    builder.switchTo(entry);
    builder.branch(condition, left, {}, right, {});
    builder.switchTo(left);
    auto const one = builder.emit(Operation::I32Const, I32Result, {}, constant(1))[0];
    builder.jump(merge, std::array { one });
    builder.switchTo(right);
    auto const two = builder.emit(Operation::I32Const, I32Result, {}, constant(2))[0];
    builder.jump(merge, std::array { two });
    builder.switchTo(merge);
    auto const sum = builder.emit(Operation::I32Add, I32Result, std::array { merged, condition })[0];
    builder.ret(std::array { sum });

    REQUIRE(verify(function).has_value());
    CHECK(print(function)
          == "b0(v0:i32):\n"
             "  branch v0, b1(), b2()\n"
             "b1():\n"
             "  v2 = i32.const [value=1]\n"
             "  jump b3(v2)\n"
             "b2() cold:\n"
             "  v3 = i32.const [value=2]\n"
             "  jump b3(v3)\n"
             "b3(v1:i32):\n"
             "  v4 = i32.add v1, v0\n"
             "  return v4\n");
    CHECK(function.successors(entry) == std::pmr::vector<BlockId> { { left, right }, &arena.memory });
    CHECK(function.successors(merge).empty());
    auto const& jump = function.instruction(function.block(left).instructions.back());
    auto const targets = function.targets(jump);
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].block == merge);
    CHECK(std::ranges::equal(function.operands(targets[0].arguments), std::array { one }));

    // compact() renumbers values in block order: the merge's parameter becomes the last but one.
    function.compact();
    REQUIRE(verify(function).has_value());
    CHECK(print(function)
          == "b0(v0:i32):\n"
             "  branch v0, b1(), b2()\n"
             "b1():\n"
             "  v1 = i32.const [value=1]\n"
             "  jump b3(v1)\n"
             "b2() cold:\n"
             "  v2 = i32.const [value=2]\n"
             "  jump b3(v2)\n"
             "b3(v3:i32):\n"
             "  v4 = i32.add v3, v0\n"
             "  return v4\n");
}

TEST_CASE("Function.printsEveryFormOfInstruction")
{
    auto arena = Arena {};
    auto function = Function { arena.memory };
    auto builder = Builder { function };
    auto const entry = builder.createBlock();
    auto const exit = builder.createBlock();
    auto const index = builder.addParameter(entry, Type::I32);
    auto const base = builder.addParameter(entry, Type::Pointer);
    auto const bound = builder.addParameter(entry, Type::I64);
    builder.switchTo(entry);
    auto const address = builder.emit(Operation::CheckMemory,
                                      std::array { Type::Pointer },
                                      std::array { index, base, bound },
                                      std::array<uint64_t, 3> { 0, 16, 4 })[0];
    auto const loaded = builder.emit(Operation::Load,
                                     std::array { Type::I64 },
                                     std::array { address },
                                     std::array { immediate(0),
                                                  immediate(0),
                                                  immediate(1),
                                                  immediate(1),
                                                  immediate(std::to_underlying(Type::I64)) })[0];
    auto const results = builder.emit(Operation::Call,
                                      std::array { Type::I32, Type::F64 },
                                      std::array { loaded, index },
                                      std::array<uint64_t, 1> { 7 });
    auto const resultIndex = results[0];
    builder.switchOn(resultIndex,
                     std::array { isoline::compiler::intermediate::BranchTo { .block = exit } },
                     isoline::compiler::intermediate::BranchTo { .block = exit });
    builder.switchTo(exit);
    builder.trap(isoline::runtime::TrapCode::Unreachable);

    REQUIRE(verify(function).has_value());
    CHECK(print(function)
          == "b0(v0:i32, v1:ptr, v2:i64):\n"
             "  v3 = check_memory v0, v1, v2 [memory=0, offset=16, size=4]\n"
             "  v4 = load v3 [memory=0, offset=0, size=1, extension=1, type=i64]\n"
             "  v5, v6 = call v4, v0 [function=7]\n"
             "  switch v5, b1(), b1()\n"
             "b1():\n"
             "  trap [trap=0]\n");
    function.remove(function.block(entry).instructions[1]);
    CHECK(print(function).contains("  removed\n"));
}

TEST_CASE("Function.variadicOperandsAndPools")
{
    // A call with 1'000 arguments and 1'000 results, whose results are returned: every one round-trips
    // through the operand pool, each block's parameters stay one run while instructions interleave.
    constexpr auto Count = std::size_t { 1'000 };
    auto arena = Arena {};
    auto function = Function { arena.memory };
    auto builder = Builder { function };
    auto const entry = builder.createBlock();
    auto const exit = builder.createBlock();
    auto types = std::vector<Type> {};
    auto arguments = std::vector<ValueId> {};
    auto exitParameters = std::vector<ValueId> {};
    builder.switchTo(entry);
    for (auto const index: std::views::iota(std::size_t { 0 }, Count))
    {
        auto const type = index % 2 == 0 ? Type::I64 : Type::F32;
        types.push_back(type);
        arguments.push_back(builder.addParameter(entry, type));
        // Interleaved: a constant in the entry, then a parameter of the exit, so that neither block's
        // parameters sit at the pool's end when the next is added.
        std::ignore = builder.emit(Operation::I32Const, I32Result, {}, constant(index));
        exitParameters.push_back(builder.addParameter(exit, type));
    }
    auto const callResults = builder.emit(Operation::Call, types, arguments, std::array<uint64_t, 1> { 1 });
    auto const results = std::vector<ValueId>(callResults.begin(), callResults.end());
    builder.jump(exit, results);
    builder.switchTo(exit);
    builder.ret(exitParameters);

    REQUIRE(verify(function).has_value());
    CHECK(std::ranges::equal(function.operands(function.block(entry).parameters), arguments));
    CHECK(std::ranges::equal(function.operands(function.block(exit).parameters), exitParameters));
    auto const& call = function.instruction(function.block(entry).instructions[Count]);
    REQUIRE(call.operation == Operation::Call);
    CHECK(std::ranges::equal(function.operands(call.operands), arguments));
    REQUIRE(call.results.count == Count);
    for (auto const index: std::views::iota(std::size_t { 0 }, Count))
        CHECK(function.typeOf(function.operands(call.results)[index]) == types[index]);
    CHECK(call.type == Type::I64);
    auto const& jump = function.instruction(function.block(entry).instructions.back());
    CHECK(std::ranges::equal(function.operands(function.targets(jump)[0].arguments), results));

    // compact() keeps every one of them, and the order; compacting again changes nothing.
    function.compact();
    REQUIRE(verify(function).has_value());
    CHECK(function.valueCount() == 4 * Count);
    // Parameters of both blocks, the constants' results, the call's operands and results, the jump's
    // and the return's operands: no slot left over from a parameter run that moved.
    CHECK(function.operandPoolSize() == 7 * Count);
    auto const& compacted = function.instruction(function.block(entry).instructions[Count]);
    CHECK(compacted.operands.count == Count);
    CHECK(compacted.results.count == Count);
    auto const compactedText = print(function);
    function.compact();
    CHECK(print(function) == compactedText);
}

TEST_CASE("Function.compactDropsRemovedInstructions")
{
    auto arena = Arena {};
    auto function = Function { arena.memory };
    auto builder = Builder { function };
    auto const entry = builder.createBlock();
    builder.switchTo(entry);
    auto const unused = builder.emit(Operation::I32Const, I32Result, {}, constant(5))[0];
    auto const kept = builder.emit(Operation::I32Const, I32Result, {}, constant(6))[0];
    builder.ret(std::array { kept });
    CHECK(unused == ValueId { 0 });
    function.remove(function.block(entry).instructions[0]);
    REQUIRE(verify(function).has_value());
    function.compact();
    REQUIRE(verify(function).has_value());
    CHECK(function.instructionCount() == 2);
    CHECK(function.valueCount() == 1);
    CHECK(function.immediatePoolSize() == 1);
    CHECK(print(function) == "b0():\n  v0 = i32.const [value=6]\n  return v0\n");
}

TEST_CASE("Function.allocatesOnlyFromItsResource")
{
    // With the default resource refusing, building, verifying, analysing and compacting a function
    // succeed: every container takes the function's resource, not the default one.
    auto arena = Arena {};
    auto const refusingDefault = DefaultResourceScope { std::pmr::null_memory_resource() };
    auto function = Function { arena.memory };
    auto builder = Builder { function };
    auto const entry = builder.createBlock();
    auto const loop = builder.createBlock();
    auto const exit = builder.createBlock();
    auto const counter = builder.addParameter(loop, Type::I32);
    builder.switchTo(entry);
    auto const zero = builder.emit(Operation::I32Const, I32Result, {}, constant(0))[0];
    builder.jump(loop, std::array { zero });
    builder.switchTo(loop);
    builder.branch(counter, loop, std::array { counter }, exit, {});
    builder.switchTo(exit);
    builder.ret({});
    auto const verified = verify(function);
    auto const graph = isoline::compiler::intermediate::controlFlowGraphOf(function, arena.memory);
    auto const dominators = isoline::compiler::intermediate::DominatorTree { graph, arena.memory };
    auto const loops = isoline::compiler::intermediate::LoopNesting { graph, dominators, arena.memory };
    function.compact();
    CHECK(verified.has_value());
    CHECK(loops.isHeader(loop));
}

TEST_CASE("Function.parameterGrowthDoublesTheRoom")
{
    // A block's parameters take 4 slots of room first, then move to twice their room each time it fills:
    // parameterGrowth() says how many pool entries the next addParameter() appends.
    auto arena = Arena {};
    auto function = Function { arena.memory };
    auto builder = Builder { function };
    auto const block = builder.createBlock();
    auto growths = std::vector<uint64_t> {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 17))
    {
        auto const growth = function.parameterGrowth(block);
        auto const before = function.operandPoolSize();
        std::ignore = builder.addParameter(block, Type::I32);
        CHECK(function.operandPoolSize() - before == growth);
        growths.push_back(growth);
    }
    CHECK(growths == std::vector<uint64_t> { 4, 0, 0, 0, 8, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 32 });
}

TEST_CASE("Function.moveAssignmentWithinItsArena")
{
    auto arena = Arena {};
    auto function = Function { arena.memory };
    auto builder = Builder { function };
    builder.switchTo(builder.createBlock());
    builder.ret({});
    auto other = Function { arena.memory };
    other = std::move(function);
    CHECK(other.blockCount() == 1);
    CHECK(&other.memory() == &arena.memory);
    CHECK(verify(other).has_value());
}

// The contract test below ends the process at a failed Require; tests/CMakeLists.txt runs it alone.

TEST_CASE("Contract.Function.moveAssignmentStaysInItsArena", "[.contract]")
{
    // Moving a function of one arena into a function of another would copy into the second arena while
    // memory() named the first.
    isoline::runtime::testing::endAtContractViolation();
    auto first = Arena {};
    auto second = Arena {};
    auto target = Function { first.memory };
    target = Function { second.memory };
    FAIL("SURVIVED: a function moved between arenas");
}

TEST_CASE("Function.schemaTypeColumnsMatchTheirFields")
{
    // A schema narrows the types of a `type` field only when it has one, and checks an access width only
    // against a `size` field and the type it names: the `type` field's for OfType, none for OfValue.
    using isoline::compiler::intermediate::AccessWidth;
    using isoline::compiler::intermediate::AnyType;
    for (auto const& schema: ImmediateSchemaTable)
    {
        INFO("schema " << static_cast<int>(schema.schema));
        auto const hasType = std::ranges::count(schema.fixed, ImmediateField::Type) == 1;
        auto const hasSize = std::ranges::count(schema.fixed, ImmediateField::Size) == 1;
        CHECK((hasType || schema.types == AnyType));
        CHECK((schema.width == AccessWidth::None || hasSize));
        CHECK((schema.width != AccessWidth::OfType || hasType));
        CHECK((schema.width != AccessWidth::OfValue || !hasType));
    }
}

TEST_CASE("Function.terminatorOfAnOpenBlock")
{
    // A block without a terminator has none: neither an empty one, nor one that ends in a value, nor one
    // whose only terminator was removed.
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto function = Function { memory };
    auto builder = Builder { function };
    auto const empty = builder.createBlock();
    CHECK_FALSE(function.terminator(empty).has_value());
    auto const open = builder.createBlock();
    builder.switchTo(open);
    std::ignore =
        builder.emit(Operation::I32Const, std::array { Type::I32 }, {}, std::array<uint64_t, 1> { 1 });
    CHECK_FALSE(function.terminator(open).has_value());
    builder.ret({});
    REQUIRE(function.terminator(open).has_value());
    function.instruction(*function.terminator(open)).operation = Operation::Removed;
    CHECK_FALSE(function.terminator(open).has_value());
}

TEST_CASE("Function.tableLookupsAtRunTime")
{
    // What the tables answer when asked at run time rather than in a static_assert: the rows in their
    // enumerator order, and the rules that fix no result type.
    using isoline::compiler::intermediate::fixedTypeOf;
    CHECK(isoline::compiler::intermediate::detail::isInEnumeratorOrder(
        OperationTable,
        [](isoline::compiler::intermediate::OperationRow const& row) { return row.operation; }));
    CHECK_FALSE(isoline::compiler::intermediate::detail::isInEnumeratorOrder(
        OperationTable,
        [](isoline::compiler::intermediate::OperationRow const&) { return Operation::Trap; }));
    for (auto const rule:
         { ResultRule::None, ResultRule::SameAsOperand0, ResultRule::FromImmediate, ResultRule::Multiple })
        CHECK_FALSE(fixedTypeOf(rule).has_value());
    CHECK(fixedTypeOf(ResultRule::Pointer) == Type::Pointer);
}
