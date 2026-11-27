// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>

using isoline::compiler::intermediate::BlockId;
using isoline::compiler::intermediate::BranchTo;
using isoline::compiler::intermediate::Builder;
using isoline::compiler::intermediate::ContextValue;
using isoline::compiler::intermediate::describe;
using isoline::compiler::intermediate::Extension;
using isoline::compiler::intermediate::Function;
using isoline::compiler::intermediate::Hotness;
using isoline::compiler::intermediate::Operation;
using isoline::compiler::intermediate::OperationCount;
using isoline::compiler::intermediate::Type;
using isoline::compiler::intermediate::ValueId;
using isoline::compiler::intermediate::VerifierError;
using isoline::compiler::intermediate::VerifierErrorCode;
using isoline::compiler::intermediate::VerifierErrorCodeCount;
using isoline::compiler::intermediate::VerifierErrorTable;
using isoline::compiler::intermediate::verify;

namespace
{

constexpr auto I32Result = std::array { Type::I32 };

/// The immediates of a constant @p value.
std::array<uint64_t, 1> constant(uint64_t value)
{
    return { value };
}

/// A function under construction: an entry block that is the builder's current one.
struct Fixture
{
    std::pmr::monotonic_buffer_resource memory { std::pmr::new_delete_resource() };
    Function function { memory };
    Builder builder { function };
    BlockId entry = builder.createBlock();

    Fixture() { builder.switchTo(entry); }

    /// An I32 constant in the current block.
    ValueId i32(uint64_t value)
    {
        return builder.emit(Operation::I32Const, I32Result, {}, constant(value))[0];
    }

    /// The index of the last instruction appended.
    [[nodiscard]] uint32_t last() const { return function.instructionCount() - 1; }
};

/// verify()'s refusal of @p function; fails the test if it accepts.
VerifierError refusal(Function const& function)
{
    auto const verified = verify(function);
    REQUIRE_FALSE(verified.has_value());
    return verified.error();
}

/// One bad function and the code verify() must refuse it with.
struct Row
{
    VerifierErrorCode code;              ///< The refusal.
    std::function<void(Fixture&)> build; ///< Builds the function.
};

} // namespace

TEST_CASE("Verifier.acceptsAWellFormedFunction")
{
    auto fixture = Fixture {};
    auto& builder = fixture.builder;
    auto const loop = builder.createBlock();
    auto const exit = builder.createBlock();
    auto const counter = builder.addParameter(loop, Type::I32);
    auto const zero = fixture.i32(0);
    builder.jump(loop, std::array { zero });
    builder.switchTo(loop);
    auto const one = fixture.i32(1);
    auto const next = builder.emit(Operation::I32Sub, I32Result, std::array { counter, one })[0];
    builder.switchOn(next,
                     std::array { BranchTo { .block = loop, .arguments = std::array { next } } },
                     BranchTo { .block = exit });
    builder.switchTo(exit);
    builder.ret(std::array { counter });
    CHECK(verify(fixture.function).has_value());
}

TEST_CASE("Verifier.rejectsEachErrorCode")
{
    // One hand-built function per VerifierErrorCode, each faulty in that one way only.
    auto const rows = std::vector<Row> {
        { .code = VerifierErrorCode::NoBlocks,
          .build = [](Fixture& fixture) { fixture.function = Function { fixture.memory }; } },
        { .code = VerifierErrorCode::UnknownType,
          .build =
              [](Fixture& fixture) {
                  std::ignore = fixture.builder.addParameter(fixture.entry, static_cast<Type>(6));
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::RangeOutOfBounds,
          .build =
              [](Fixture& fixture) {
                  fixture.builder.ret({});
                  fixture.function.block(fixture.entry).parameters = { .begin = 1, .count = 1 };
              } },
        { .code = VerifierErrorCode::UnknownHotness,
          .build =
              [](Fixture& fixture) {
                  fixture.builder.ret({});
                  fixture.function.block(fixture.entry).hotness = static_cast<Hotness>(2);
              } },
        { .code = VerifierErrorCode::InstructionOutOfBounds,
          .build =
              [](Fixture& fixture) {
                  fixture.builder.ret({});
                  fixture.function.block(fixture.entry).instructions.push_back(1);
              } },
        { .code = VerifierErrorCode::InstructionListedTwice,
          .build =
              [](Fixture& fixture) {
                  fixture.builder.ret({});
                  fixture.function.block(fixture.entry).instructions.push_back(0);
              } },
        { .code = VerifierErrorCode::UnknownOperation,
          .build =
              [](Fixture& fixture) {
                  std::ignore = fixture.builder.emit(static_cast<Operation>(OperationCount), {}, {});
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::UndefinedValue,
          .build = [](Fixture& fixture) { fixture.builder.ret(std::array { ValueId { 77 } }); } },
        { .code = VerifierErrorCode::RedefinedValue,
          .build =
              [](Fixture& fixture) {
                  auto const first = fixture.i32(1);
                  std::ignore = fixture.i32(2);
                  fixture.function.operands(fixture.function.instruction(fixture.last()).results)[0] = first;
                  fixture.builder.ret(std::array { first });
              } },
        { .code = VerifierErrorCode::MissingTerminator,
          .build = [](Fixture& fixture) { std::ignore = fixture.i32(1); } },
        { .code = VerifierErrorCode::TerminatorNotLast,
          .build =
              [](Fixture& fixture) {
                  fixture.builder.ret({});
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::ImmediateCount,
          .build =
              [](Fixture& fixture) {
                  std::ignore = fixture.builder.emit(Operation::I32Const, I32Result, {});
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) {
                  // An access of 3 bytes.
                  auto const index = fixture.builder.addParameter(fixture.entry, Type::I32);
                  auto const base = fixture.builder.addParameter(fixture.entry, Type::Pointer);
                  auto const bound = fixture.builder.addParameter(fixture.entry, Type::I64);
                  std::ignore = fixture.builder.emit(Operation::CheckMemory,
                                                     std::array { Type::Pointer },
                                                     std::array { index, base, bound },
                                                     std::array<uint64_t, 3> { 0, 0, 3 });
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::TargetOutOfBounds,
          .build = [](Fixture& fixture) { fixture.builder.jump(BlockId { 50 }, {}); } },
        { .code = VerifierErrorCode::TargetIsEntry,
          .build = [](Fixture& fixture) { fixture.builder.jump(fixture.entry, {}); } },
        { .code = VerifierErrorCode::OperandCount,
          .build =
              [](Fixture& fixture) {
                  auto const one = fixture.i32(1);
                  std::ignore = fixture.builder.emit(Operation::I32Add, I32Result, std::array { one });
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::OperandType,
          .build =
              [](Fixture& fixture) {
                  auto const one = fixture.i32(1);
                  auto const wide =
                      fixture.builder.emit(Operation::I64Const, std::array { Type::I64 }, {}, constant(2))[0];
                  std::ignore = fixture.builder.emit(Operation::I32Add, I32Result, std::array { one, wide });
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::ResultCount,
          .build =
              [](Fixture& fixture) {
                  auto const one = fixture.i32(1);
                  std::ignore = fixture.builder.emit(Operation::I32Add, {}, std::array { one, one });
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::ResultType,
          .build =
              [](Fixture& fixture) {
                  auto const one = fixture.i32(1);
                  std::ignore = fixture.builder.emit(
                      Operation::I32Add, std::array { Type::F32 }, std::array { one, one });
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::ArgumentCount,
          .build =
              [](Fixture& fixture) {
                  auto const target = fixture.builder.createBlock();
                  std::ignore = fixture.builder.addParameter(target, Type::I32);
                  fixture.builder.jump(target, {});
                  fixture.builder.switchTo(target);
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::ArgumentType,
          .build =
              [](Fixture& fixture) {
                  auto const target = fixture.builder.createBlock();
                  std::ignore = fixture.builder.addParameter(target, Type::I32);
                  auto const wide =
                      fixture.builder.emit(Operation::I64Const, std::array { Type::I64 }, {}, constant(2))[0];
                  fixture.builder.jump(target, std::array { wide });
                  fixture.builder.switchTo(target);
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::ReturnsDisagree,
          .build =
              [](Fixture& fixture) {
                  // One arm returns an i32, the other an i64.
                  auto& builder = fixture.builder;
                  auto const condition = builder.addParameter(fixture.entry, Type::I32);
                  auto const left = builder.createBlock();
                  auto const right = builder.createBlock();
                  builder.branch(condition, left, {}, right, {});
                  builder.switchTo(left);
                  builder.ret(std::array { fixture.i32(1) });
                  builder.switchTo(right);
                  builder.ret(std::array {
                      builder.emit(Operation::I64Const, std::array { Type::I64 }, {}, constant(1))[0] });
              } },
        { .code = VerifierErrorCode::UseNotDominated,
          .build =
              [](Fixture& fixture) {
                  // A diamond whose merge uses what only its left arm defines.
                  auto& builder = fixture.builder;
                  auto const condition = builder.addParameter(fixture.entry, Type::I32);
                  auto const left = builder.createBlock();
                  auto const right = builder.createBlock();
                  auto const merge = builder.createBlock();
                  builder.branch(condition, left, {}, right, {});
                  builder.switchTo(left);
                  auto const leftValue = fixture.i32(1);
                  builder.jump(merge, {});
                  builder.switchTo(right);
                  builder.jump(merge, {});
                  builder.switchTo(merge);
                  builder.ret(std::array { leftValue });
              } },
    };
    REQUIRE(rows.size() == VerifierErrorCodeCount);
    auto covered = std::set<VerifierErrorCode> {};
    for (auto const& row: rows)
    {
        INFO(VerifierErrorTable[std::to_underlying(row.code)].text);
        CHECK(covered.insert(row.code).second);
        auto fixture = Fixture {};
        row.build(fixture);
        CHECK(refusal(fixture.function).code == row.code);
    }
}

TEST_CASE("Verifier.namesWhereTheFaultIs")
{
    auto fixture = Fixture {};
    auto const next = fixture.builder.createBlock();
    fixture.builder.jump(next, {});
    fixture.builder.switchTo(next);
    auto const one = fixture.i32(1);
    std::ignore = fixture.builder.emit(Operation::F32Neg, std::array { Type::F32 }, std::array { one });
    fixture.builder.ret({});
    CHECK(refusal(fixture.function)
          == VerifierError { .code = VerifierErrorCode::OperandType, .block = next, .instruction = 2 });
    CHECK(describe(refusal(fixture.function)) == "operand type mismatch in block 1 at instruction 2");
    CHECK(describe(VerifierError { .code = VerifierErrorCode::NoBlocks }) == "function has no blocks");
}

TEST_CASE("Verifier.hostileFunctionsAreRefusedNotRead")
{
    // Counts and indices at the edge of their types: each is refused, none read out of bounds (ASan).
    SECTION("a target that claims 2^32 - 1 arguments")
    {
        auto fixture = Fixture {};
        auto const next = fixture.builder.createBlock();
        fixture.builder.jump(next, {});
        fixture.builder.switchTo(next);
        fixture.builder.ret({});
        fixture.function.immediates(fixture.function.instruction(0).immediates)[1] =
            std::numeric_limits<uint32_t>::max();
        CHECK(refusal(fixture.function).code == VerifierErrorCode::OperandCount);
    }
    SECTION("a switch whose immediates end inside a target")
    {
        auto fixture = Fixture {};
        auto const index = fixture.i32(0);
        fixture.builder.switchOn(
            index, std::array { BranchTo { .block = fixture.entry } }, BranchTo { .block = fixture.entry });
        fixture.function.instruction(1).immediates.count = 3;
        CHECK(refusal(fixture.function).code == VerifierErrorCode::ImmediateCount);
    }
    SECTION("a range whose end wraps around 2^32")
    {
        auto fixture = Fixture {};
        auto const one = fixture.i32(1);
        std::ignore = fixture.builder.emit(Operation::I32Add, I32Result, std::array { one, one });
        fixture.builder.ret({});
        fixture.function.instruction(1).operands = { .begin = std::numeric_limits<uint32_t>::max(),
                                                     .count = 2 };
        CHECK(refusal(fixture.function).code == VerifierErrorCode::RangeOutOfBounds);
    }
    SECTION("a target past 2^32")
    {
        auto fixture = Fixture {};
        fixture.builder.jump(fixture.entry, {});
        fixture.function.immediates(fixture.function.instruction(0).immediates)[0] = uint64_t { 1 } << 40;
        CHECK(refusal(fixture.function).code == VerifierErrorCode::ImmediateValue);
    }
    SECTION("an empty block")
    {
        auto fixture = Fixture {};
        CHECK(refusal(fixture.function)
              == VerifierError { .code = VerifierErrorCode::MissingTerminator, .block = fixture.entry });
    }
    SECTION("an instruction that uses its own result")
    {
        auto fixture = Fixture {};
        auto const one = fixture.i32(1);
        auto const sum = fixture.builder.emit(Operation::I32Add, I32Result, std::array { one, one })[0];
        fixture.function.operands(fixture.function.instruction(1).operands)[1] = sum;
        fixture.builder.ret({});
        CHECK(refusal(fixture.function).code == VerifierErrorCode::UseNotDominated);
    }
    SECTION("a use of a removed instruction's result")
    {
        auto fixture = Fixture {};
        auto const one = fixture.i32(1);
        fixture.builder.ret(std::array { one });
        fixture.function.remove(0);
        CHECK(refusal(fixture.function).code == VerifierErrorCode::UndefinedValue);
    }
    SECTION("a removed instruction after the terminator")
    {
        // The terminator is the last instruction not removed: the block keeps its successor, so the
        // next block stays reachable and its use of the entry's value stays dominated.
        auto fixture = Fixture {};
        auto const one = fixture.i32(1);
        auto const next = fixture.builder.createBlock();
        fixture.builder.jump(next, {});
        std::ignore = fixture.i32(2);
        fixture.function.remove(fixture.last());
        fixture.builder.switchTo(next);
        auto const sum = fixture.builder.emit(Operation::I32Add, I32Result, std::array { one, one })[0];
        fixture.builder.ret(std::array { sum });
        CHECK(verify(fixture.function).has_value());
        CHECK(fixture.function.successors(fixture.entry)
              == std::pmr::vector<BlockId> { { next }, &fixture.memory });
        CHECK(fixture.function.terminator(fixture.entry) == 1);
    }
    SECTION("an unreachable block may use what it likes of the reachable ones")
    {
        auto fixture = Fixture {};
        auto const one = fixture.i32(1);
        auto const two = fixture.i32(2);
        fixture.builder.ret(std::array { two });
        auto const orphan = fixture.builder.createBlock();
        fixture.builder.switchTo(orphan);
        fixture.builder.ret(std::array { one });
        CHECK(verify(fixture.function).has_value());
    }
}

TEST_CASE("Verifier.everyCodeHasItsExactText")
{
    struct Expected
    {
        VerifierErrorCode code;
        std::string_view text;
    };
    auto const expected = std::to_array<Expected>({
        { .code = VerifierErrorCode::NoBlocks, .text = "function has no blocks" },
        { .code = VerifierErrorCode::UnknownType, .text = "value of unknown type" },
        { .code = VerifierErrorCode::RangeOutOfBounds, .text = "range out of its pool" },
        { .code = VerifierErrorCode::UnknownHotness, .text = "block of unknown hotness" },
        { .code = VerifierErrorCode::InstructionOutOfBounds, .text = "instruction index out of bounds" },
        { .code = VerifierErrorCode::InstructionListedTwice, .text = "instruction listed twice" },
        { .code = VerifierErrorCode::UnknownOperation, .text = "unknown operation" },
        { .code = VerifierErrorCode::UndefinedValue, .text = "undefined value" },
        { .code = VerifierErrorCode::RedefinedValue, .text = "value defined twice" },
        { .code = VerifierErrorCode::MissingTerminator, .text = "block does not end with a terminator" },
        { .code = VerifierErrorCode::TerminatorNotLast, .text = "terminator before the end of its block" },
        { .code = VerifierErrorCode::ImmediateCount, .text = "immediate count mismatch" },
        { .code = VerifierErrorCode::ImmediateValue, .text = "immediate out of range" },
        { .code = VerifierErrorCode::TargetOutOfBounds, .text = "branch target out of bounds" },
        { .code = VerifierErrorCode::TargetIsEntry, .text = "branch to the entry block" },
        { .code = VerifierErrorCode::OperandCount, .text = "operand count mismatch" },
        { .code = VerifierErrorCode::OperandType, .text = "operand type mismatch" },
        { .code = VerifierErrorCode::ResultCount, .text = "result count mismatch" },
        { .code = VerifierErrorCode::ResultType, .text = "result type mismatch" },
        { .code = VerifierErrorCode::ArgumentCount, .text = "branch argument count mismatch" },
        { .code = VerifierErrorCode::ArgumentType, .text = "branch argument type mismatch" },
        { .code = VerifierErrorCode::ReturnsDisagree, .text = "returns disagree" },
        { .code = VerifierErrorCode::UseNotDominated, .text = "use not dominated by its definition" },
    });
    REQUIRE(expected.size() == VerifierErrorCodeCount);
    for (auto const index: std::views::iota(std::size_t { 0 }, VerifierErrorCodeCount))
    {
        auto const& [code, text] = expected[index];
        INFO(text);
        CHECK(std::to_underlying(code) == index);
        CHECK(describe(VerifierError { .code = code }) == text);
    }
}

namespace
{

/// One instruction whose immediates disagree with its types, and the code verify() refuses it with.
struct TypedRow
{
    std::string_view name;               ///< What is wrong with it.
    VerifierErrorCode code;              ///< The refusal.
    std::function<void(Fixture&)> build; ///< Emits it into the entry block, which it does not end.
};

/// The immediates of a load of @p size bytes into @p type, extended as @p extension says.
std::array<uint64_t, 5> loadOf(uint64_t size, Extension extension, Type type)
{
    return { 0, 0, size, std::to_underlying(extension), std::to_underlying(type) };
}

/// A load from a Pointer parameter of the entry with @p immediates, its result of @p type.
void emitLoad(Fixture& fixture, Type type, std::array<uint64_t, 5> const& immediates)
{
    auto const address = fixture.builder.addParameter(fixture.entry, Type::Pointer);
    std::ignore =
        fixture.builder.emit(Operation::Load, std::array { type }, std::array { address }, immediates);
}

/// A store of @p size bytes of a parameter of @p type.
void emitStore(Fixture& fixture, Type type, uint64_t size)
{
    auto const address = fixture.builder.addParameter(fixture.entry, Type::Pointer);
    auto const value = fixture.builder.addParameter(fixture.entry, type);
    std::ignore = fixture.builder.emit(
        Operation::Store, {}, std::array { address, value }, std::array<uint64_t, 3> { 0, 0, size });
}

/// The immediates of a context value @p value of @p type.
std::array<uint64_t, 2> contextOf(ContextValue value, Type type)
{
    return { std::to_underlying(value), std::to_underlying(type) };
}

} // namespace

TEST_CASE("Verifier.typedImmediatesFitTheirTypes")
{
    // Each row breaks one rule between an instruction's immediates and its types, and nothing else; the
    // last section accepts the cases at the edges of those rules.
    auto const rows = std::vector<TypedRow> {
        { .name = "memory_size of an f32",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) {
                  std::ignore = fixture.builder.emit(
                      Operation::MemorySize, std::array { Type::F32 }, {}, std::array<uint64_t, 2> { 0, 2 });
              } },
        { .name = "table_size of a ref",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) {
                  std::ignore = fixture.builder.emit(
                      Operation::TableSize, std::array { Type::Ref }, {}, std::array<uint64_t, 2> { 0, 4 });
              } },
        { .name = "load_global of a pointer",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) {
                  std::ignore = fixture.builder.emit(Operation::LoadGlobal,
                                                     std::array { Type::Pointer },
                                                     {},
                                                     std::array<uint64_t, 2> { 0, 5 });
              } },
        { .name = "an 8-byte load into an i32",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) { emitLoad(fixture, Type::I32, loadOf(8, Extension::Zero, Type::I32)); } },
        { .name = "a 4-byte load into an f64",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) { emitLoad(fixture, Type::F64, loadOf(4, Extension::Zero, Type::F64)); } },
        { .name = "a sign-extending 4-byte load into an f64",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) { emitLoad(fixture, Type::F64, loadOf(4, Extension::Sign, Type::F64)); } },
        { .name = "a sign-extending load of an i32's full width",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) { emitLoad(fixture, Type::I32, loadOf(4, Extension::Sign, Type::I32)); } },
        { .name = "a load into a ref",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) { emitLoad(fixture, Type::Ref, loadOf(8, Extension::Zero, Type::Ref)); } },
        { .name = "a 2-byte store of an f64",
          .code = VerifierErrorCode::ImmediateValue,
          .build = [](Fixture& fixture) { emitStore(fixture, Type::F64, 2); } },
        { .name = "an 8-byte store of an i32",
          .code = VerifierErrorCode::ImmediateValue,
          .build = [](Fixture& fixture) { emitStore(fixture, Type::I32, 8); } },
        { .name = "a store of a ref",
          .code = VerifierErrorCode::OperandType,
          .build = [](Fixture& fixture) { emitStore(fixture, Type::Ref, 8); } },
        { .name = "load_context of the step countdown as a pointer",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) {
                  std::ignore = fixture.builder.emit(Operation::LoadContext,
                                                     std::array { Type::Pointer },
                                                     {},
                                                     contextOf(ContextValue::StepCountdown, Type::Pointer));
              } },
        { .name = "store_context of memory 0's base",
          .code = VerifierErrorCode::ImmediateValue,
          .build =
              [](Fixture& fixture) {
                  auto const base = fixture.builder.addParameter(fixture.entry, Type::Pointer);
                  std::ignore = fixture.builder.emit(
                      Operation::StoreContext,
                      {},
                      std::array { base },
                      std::array<uint64_t, 1> { std::to_underlying(ContextValue::Memory0Base) });
              } },
        { .name = "store_context of an i32 countdown",
          .code = VerifierErrorCode::OperandType,
          .build =
              [](Fixture& fixture) {
                  std::ignore = fixture.builder.emit(
                      Operation::StoreContext,
                      {},
                      std::array { fixture.i32(1) },
                      std::array<uint64_t, 1> { std::to_underlying(ContextValue::StepCountdown) });
              } },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto fixture = Fixture {};
        row.build(fixture);
        fixture.builder.ret({});
        CHECK(refusal(fixture.function).code == row.code);
    }
    SECTION("the edges of every rule are accepted")
    {
        auto fixture = Fixture {};
        emitLoad(fixture, Type::I32, loadOf(1, Extension::Sign, Type::I32));
        emitLoad(fixture, Type::I64, loadOf(8, Extension::Zero, Type::I64));
        emitLoad(fixture, Type::F32, loadOf(4, Extension::Zero, Type::F32));
        emitStore(fixture, Type::I64, 1);
        emitStore(fixture, Type::F64, 8);
        std::ignore = fixture.builder.emit(
            Operation::MemorySize, std::array { Type::I64 }, {}, std::array<uint64_t, 2> { 0, 1 });
        std::ignore = fixture.builder.emit(
            Operation::LoadGlobal, std::array { Type::Ref }, {}, std::array<uint64_t, 2> { 0, 4 });
        std::ignore = fixture.builder.emit(Operation::LoadContext,
                                           std::array { Type::Pointer },
                                           {},
                                           contextOf(ContextValue::Memory0Base, Type::Pointer));
        auto const countdown = fixture.builder.emit(Operation::LoadContext,
                                                    std::array { Type::I64 },
                                                    {},
                                                    contextOf(ContextValue::StepCountdown, Type::I64))[0];
        std::ignore =
            fixture.builder.emit(Operation::StoreContext,
                                 {},
                                 std::array { countdown },
                                 std::array<uint64_t, 1> { std::to_underlying(ContextValue::StepCountdown) });
        fixture.builder.ret({});
        auto const verified = verify(fixture.function);
        INFO((verified ? std::string {} : describe(verified.error())));
        CHECK(verified.has_value());
    }
}
