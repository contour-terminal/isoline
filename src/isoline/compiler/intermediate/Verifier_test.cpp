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
#include <isoline/compiler/intermediate/Guards.hpp>
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
using isoline::runtime::PollSite;
using isoline::runtime::TrapCode;

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

/// poll(@p countdown) at a loop's back edge, in the current block.
ValueId poll(Fixture& fixture, ValueId countdown)
{
    return fixture.builder.emit(Operation::Poll,
                                std::array { Type::I64 },
                                std::array { countdown },
                                std::array { uint64_t { std::to_underlying(PollSite::LoopBackEdge) } })[0];
}

/// storeContext(@p countdown) of the step countdown, in the current block.
void storeCountdown(Fixture& fixture, ValueId countdown)
{
    std::ignore =
        fixture.builder.emit(Operation::StoreContext,
                             {},
                             std::array { countdown },
                             std::array { uint64_t { std::to_underlying(ContextValue::StepCountdown) } });
}

/// loadContext of the step countdown, in the current block.
ValueId reloadCountdown(Fixture& fixture)
{
    return fixture.builder.emit(Operation::LoadContext,
                                std::array { Type::I64 },
                                {},
                                std::array<uint64_t, 2> { std::to_underlying(ContextValue::StepCountdown),
                                                          std::to_underlying(Type::I64) })[0];
}

/// The immediates of a load of @p size bytes into @p type, extended as @p extension says.
std::array<uint64_t, 4> loadOf(uint64_t size, Extension extension, Type type)
{
    return { 0, size, std::to_underlying(extension), std::to_underlying(type) };
}

/// @p memory's base, in the current block: memory 0's context value, memoryBase of any other.
ValueId baseOf(Fixture& fixture, uint64_t memory)
{
    if (memory == 0)
        return fixture.builder.emit(Operation::LoadContext,
                                    std::array { Type::Pointer },
                                    {},
                                    std::array<uint64_t, 2> { std::to_underlying(ContextValue::Memory0Base),
                                                              std::to_underlying(Type::Pointer) })[0];
    return fixture.builder.emit(
        Operation::MemoryBase, std::array { Type::Pointer }, {}, std::array<uint64_t, 1> { memory })[0];
}

/// A guarded address: checkMemory [memory, offset 0, size] of an entry parameter, of the memory's base.
ValueId guardedAddress(Fixture& fixture, uint64_t memory = 0, uint64_t size = 8)
{
    auto const index = fixture.builder.addParameter(fixture.entry, Type::I32);
    auto const base = baseOf(fixture, memory);
    auto const bound = fixture.builder.addParameter(fixture.entry, Type::I64);
    return fixture.builder.emit(Operation::CheckMemory,
                                std::array { Type::Pointer },
                                std::array { index, base, bound },
                                std::array<uint64_t, 3> { memory, 0, size })[0];
}

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
        { .code = VerifierErrorCode::UnguardedAddress,
          .build =
              [](Fixture& fixture) {
                  // A load of memory 0's base itself: an address no checkMemory guarded.
                  auto const base = fixture.builder.emit(
                      Operation::LoadContext,
                      std::array { Type::Pointer },
                      {},
                      std::array<uint64_t, 2> { std::to_underlying(ContextValue::Memory0Base),
                                                std::to_underlying(Type::Pointer) })[0];
                  std::ignore = fixture.builder.emit(Operation::Load,
                                                     std::array { Type::I32 },
                                                     std::array { base },
                                                     std::array<uint64_t, 4> { 0, 4, 0, 0 });
                  fixture.builder.ret({});
              } },
        { .code = VerifierErrorCode::CountdownNotStored,
          .build =
              [](Fixture& fixture) {
                  // A poll changes the countdown, and nothing stores it before the trap.
                  auto const countdown = fixture.builder.addParameter(fixture.entry, Type::I64);
                  std::ignore = poll(fixture, countdown);
                  fixture.builder.trap(TrapCode::Unreachable);
              } },
        { .code = VerifierErrorCode::AddressAcrossMemoryMotion,
          .build =
              [](Fixture& fixture) {
                  // A call between a check and its load: the memory may have moved.
                  auto const address = guardedAddress(fixture);
                  std::ignore = fixture.builder.emit(Operation::Call, {}, {}, std::array { uint64_t { 0 } });
                  std::ignore = fixture.builder.emit(Operation::Load,
                                                     std::array { Type::I64 },
                                                     std::array { address },
                                                     loadOf(8, Extension::Zero, Type::I64));
                  fixture.builder.ret({});
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
        { .code = VerifierErrorCode::UnguardedAddress, .text = "address not defined by its guard" },
        { .code = VerifierErrorCode::CountdownNotStored, .text = "trap site with the countdown not stored" },
        { .code = VerifierErrorCode::AddressAcrossMemoryMotion,
          .text = "guarded address used across memory motion" },
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

/// A load from a guarded address with @p immediates, its result of @p type.
void emitLoad(Fixture& fixture, Type type, std::array<uint64_t, 4> const& immediates)
{
    auto const address = guardedAddress(fixture);
    std::ignore =
        fixture.builder.emit(Operation::Load, std::array { type }, std::array { address }, immediates);
}

/// A store of @p size bytes of a parameter of @p type.
void emitStore(Fixture& fixture, Type type, uint64_t size)
{
    auto const address = guardedAddress(fixture);
    auto const value = fixture.builder.addParameter(fixture.entry, type);
    std::ignore = fixture.builder.emit(
        Operation::Store, {}, std::array { address, value }, std::array<uint64_t, 2> { 0, size });
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
        { .name = "store_global of a pointer",
          .code = VerifierErrorCode::OperandType,
          .build =
              [](Fixture& fixture) {
                  auto const address = fixture.builder.addParameter(fixture.entry, Type::Pointer);
                  std::ignore = fixture.builder.emit(
                      Operation::StoreGlobal, {}, std::array { address }, std::array<uint64_t, 1> { 0 });
              } },
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
            Operation::TableSize, std::array { Type::I64 }, {}, std::array<uint64_t, 1> { 0 });
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

TEST_CASE("Verifier.refusesTheRemainingShapes")
{
    SECTION("an access its guard does not cover")
    {
        // Each access through a checkMemory [memory 0, size 8], maybe moved by guardAddress: within the 8
        // bytes it is accepted; past them, of another memory, through an offset sum past 2^64, or from no
        // guard at all, refused.
        enum class Verdict : uint8_t
        {
            Refused,
            Accepted,
        };
        struct Case
        {
            std::string_view name;
            uint64_t checkedMemory;      ///< The checkMemory's memory.
            uint64_t checkedSize;        ///< Its size.
            std::vector<uint64_t> moves; ///< The guardAddress offsets on the way, in order.
            uint64_t memory;             ///< The load's memory.
            uint64_t size;               ///< Its width.
            Verdict verdict;             ///< Whether the verifier accepts it.
        };
        auto const cases = std::vector<Case> {
            { "the guard's own width", 0, 8, {}, 0, 8, Verdict::Accepted },
            { "4 bytes moved 4 bytes", 0, 8, { 4 }, 0, 4, Verdict::Accepted },
            { "two moves to the end", 0, 8, { 2, 2 }, 0, 4, Verdict::Accepted },
            { "a narrow guard, a wide load", 0, 1, {}, 0, 8, Verdict::Refused },
            { "another memory", 1, 8, {}, 0, 4, Verdict::Refused },
            { "a move past the guard", 0, 8, { uint64_t { 1 } << 40U }, 0, 1, Verdict::Refused },
            { "a move that leaves the load no room", 0, 8, { 4 }, 0, 8, Verdict::Refused },
            { "moves that sum past 2^64",
              0,
              8,
              { uint64_t { 1 } << 63U, uint64_t { 1 } << 63U },
              0,
              1,
              Verdict::Refused },
        };
        for (auto const& entry: cases)
        {
            INFO(entry.name);
            auto fixture = Fixture {};
            auto address = guardedAddress(fixture, entry.checkedMemory, entry.checkedSize);
            for (auto const move: entry.moves)
                address = fixture.builder.emit(Operation::GuardAddress,
                                               std::array { Type::Pointer },
                                               std::array { address },
                                               constant(move))[0];
            std::ignore = fixture.builder.emit(Operation::Load,
                                               std::array { Type::I64 },
                                               std::array { address },
                                               std::array<uint64_t, 4> { entry.memory,
                                                                         entry.size,
                                                                         std::to_underlying(Extension::Zero),
                                                                         std::to_underlying(Type::I64) });
            fixture.builder.ret({});
            if (entry.verdict == Verdict::Accepted)
                CHECK(verify(fixture.function).has_value());
            else
                CHECK(refusal(fixture.function).code == VerifierErrorCode::UnguardedAddress);
        }
    }
    SECTION("an address that skipped its guard")
    {
        // A select of two guarded addresses is no guard's result.
        auto other = Fixture {};
        auto const guarded = guardedAddress(other);
        auto const condition = other.i32(1);
        auto const chosen = other.builder.emit(
            Operation::Select, std::array { Type::Pointer }, std::array { guarded, guarded, condition })[0];
        std::ignore = other.builder.emit(Operation::Load,
                                         std::array { Type::I32 },
                                         std::array { chosen },
                                         loadOf(4, Extension::Zero, Type::I32));
        other.builder.ret({});
        CHECK(refusal(other.function).code == VerifierErrorCode::UnguardedAddress);
    }
    SECTION("a table element of another table, or moved")
    {
        // checkTable of table 1 guards no element of table 0, and guardAddress moves no table element.
        auto const elementOf = [](Fixture& fixture, uint64_t guardedTable, uint64_t movedBy) {
            auto const index = fixture.builder.addParameter(fixture.entry, Type::I32);
            auto element = fixture.builder.emit(Operation::CheckTable,
                                                std::array { Type::Pointer },
                                                std::array { index },
                                                std::array<uint64_t, 2> { guardedTable, 0 })[0];
            if (movedBy != 0)
                element = fixture.builder.emit(Operation::GuardAddress,
                                               std::array { Type::Pointer },
                                               std::array { element },
                                               constant(movedBy))[0];
            std::ignore = fixture.builder.emit(Operation::LoadTableElement,
                                               std::array { Type::Ref },
                                               std::array { element },
                                               std::array<uint64_t, 5> { 0, 0, 1, 4, 8 });
            fixture.builder.ret({});
        };
        auto same = Fixture {};
        elementOf(same, 0, 0);
        CHECK(verify(same.function).has_value());
        auto other = Fixture {};
        elementOf(other, 1, 0);
        CHECK(refusal(other.function).code == VerifierErrorCode::UnguardedAddress);
        auto moved = Fixture {};
        elementOf(moved, 0, 4);
        CHECK(refusal(moved.function).code == VerifierErrorCode::UnguardedAddress);
    }
    SECTION("a dispatch row's type read through a table element")
    {
        // loadDispatchType takes a checkDispatch's row: a checkTable's element, the same shape of pointer, is
        // refused.
        auto fixture = Fixture {};
        auto const index = fixture.builder.addParameter(fixture.entry, Type::I32);
        auto const element = fixture.builder.emit(Operation::CheckTable,
                                                  std::array { Type::Pointer },
                                                  std::array { index },
                                                  std::array<uint64_t, 2> { 0, 0 })[0];
        std::ignore = fixture.builder.emit(Operation::LoadDispatchType, I32Result, std::array { element });
        fixture.builder.ret({});
        CHECK(refusal(fixture.function).code == VerifierErrorCode::UnguardedAddress);
    }
    SECTION("a guardAddress chain that loops")
    {
        // guardAddress of its own result: no guard at the root, refused rather than walked forever.
        auto fixture = Fixture {};
        auto const address = guardedAddress(fixture);
        auto const moved = fixture.builder.emit(
            Operation::GuardAddress, std::array { Type::Pointer }, std::array { address }, constant(0))[0];
        fixture.function.operands(fixture.function.instruction(fixture.last()).operands)[0] = moved;
        fixture.builder.ret({});
        CHECK(refusal(fixture.function).code == VerifierErrorCode::UnguardedAddress);
    }
    SECTION("a block parameter that names no value")
    {
        auto fixture = Fixture {};
        std::ignore = fixture.builder.addParameter(fixture.entry, Type::I32);
        fixture.builder.ret({});
        auto const parameters = fixture.function.block(fixture.entry).parameters;
        fixture.function.operands(parameters)[0] = ValueId { 1'000 };
        CHECK(refusal(fixture.function).code == VerifierErrorCode::UndefinedValue);
    }
    SECTION("a select of two types")
    {
        auto fixture = Fixture {};
        auto const narrow = fixture.i32(1);
        auto const wide =
            fixture.builder.emit(Operation::I64Const, std::array { Type::I64 }, {}, constant(2))[0];
        std::ignore = fixture.builder.emit(Operation::Select, I32Result, std::array { narrow, wide, narrow });
        fixture.builder.ret({});
        CHECK(refusal(fixture.function).code == VerifierErrorCode::OperandType);
    }
    SECTION("a fixed result of another type")
    {
        auto fixture = Fixture {};
        auto const one = fixture.i32(1);
        std::ignore =
            fixture.builder.emit(Operation::I32Add, std::array { Type::I64 }, std::array { one, one });
        fixture.builder.ret({});
        CHECK(refusal(fixture.function).code == VerifierErrorCode::ResultType);
    }
    SECTION("an instruction whose type is not its first result's")
    {
        auto fixture = Fixture {};
        auto const one = fixture.i32(1);
        std::ignore = fixture.builder.emit(Operation::I32Add, I32Result, std::array { one, one });
        fixture.function.instruction(fixture.last()).type = Type::I64;
        fixture.builder.ret({});
        CHECK(refusal(fixture.function).code == VerifierErrorCode::ResultType);
    }
    SECTION("a branch whose two targets both miss an argument")
    {
        auto fixture = Fixture {};
        auto const condition = fixture.i32(1);
        auto const taken = fixture.builder.createBlock();
        auto const fallthrough = fixture.builder.createBlock();
        std::ignore = fixture.builder.addParameter(taken, Type::I32);
        std::ignore = fixture.builder.addParameter(fallthrough, Type::I32);
        fixture.builder.branch(condition, taken, {}, fallthrough, {});
        for (auto const block: { taken, fallthrough })
        {
            fixture.builder.switchTo(block);
            fixture.builder.ret({});
        }
        CHECK(refusal(fixture.function).code == VerifierErrorCode::ArgumentCount);
    }
}

TEST_CASE("Verifier.countdownIsStoredAtEveryTrapSite")
{
    // Every trap site (a trap, a trapIf, a MayTrap operation, a call) sees the live countdown stored, on
    // every path: after a poll, a store of its result; a call leaves the callee's stored.
    auto const trapIf = [](Fixture& fixture) {
        std::ignore =
            fixture.builder.emit(Operation::TrapIf,
                                 {},
                                 std::array { fixture.i32(1) },
                                 std::array { uint64_t { std::to_underlying(TrapCode::Unreachable) } });
        fixture.builder.ret({});
    };
    SECTION("a poll, then its result stored: accepted; not stored, or the old one stored: refused")
    {
        for (auto const store: { 0, 1, 2 })
        {
            INFO("store " << store);
            auto fixture = Fixture {};
            auto const countdown = fixture.builder.addParameter(fixture.entry, Type::I64);
            auto const next = poll(fixture, countdown);
            if (store == 1)
                storeCountdown(fixture, next);
            if (store == 2)
                storeCountdown(fixture, countdown);
            trapIf(fixture);
            if (store == 1)
                CHECK(verify(fixture.function).has_value());
            else
                CHECK(refusal(fixture.function).code == VerifierErrorCode::CountdownNotStored);
        }
    }
    SECTION("a call leaves the callee's countdown stored; storing the one from before the call is refused")
    {
        for (auto const storeStale: { false, true })
        {
            INFO("store the stale countdown " << storeStale);
            auto fixture = Fixture {};
            auto const countdown = fixture.builder.addParameter(fixture.entry, Type::I64);
            auto const polled = poll(fixture, countdown);
            storeCountdown(fixture, polled);
            std::ignore = fixture.builder.emit(Operation::Call, {}, {}, std::array { uint64_t { 0 } });
            if (storeStale)
                storeCountdown(fixture, polled);
            auto const divisor = fixture.i32(0);
            std::ignore =
                fixture.builder.emit(Operation::I32DivS, I32Result, std::array { divisor, divisor });
            fixture.builder.ret({});
            if (storeStale)
                CHECK(refusal(fixture.function).code == VerifierErrorCode::CountdownNotStored);
            else
                CHECK(verify(fixture.function).has_value());
        }
    }
    SECTION("a reload of the countdown is the live one: storing it is accepted")
    {
        auto fixture = Fixture {};
        std::ignore = fixture.builder.emit(Operation::Call, {}, {}, std::array { uint64_t { 0 } });
        storeCountdown(fixture, reloadCountdown(fixture));
        fixture.builder.trap(TrapCode::Unreachable);
        CHECK(verify(fixture.function).has_value());
    }
    SECTION(
        "a loop whose back edge polls: its header's trap sites need a store; a merge needs every edge stored")
    {
        // entry -> header(c); header: [store?] trapIf ...; branch -> latch | exit; latch: poll -> header.
        for (auto const store: { false, true })
        {
            INFO("store " << store);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto const countdown = builder.addParameter(fixture.entry, Type::I64);
            auto const condition = builder.addParameter(fixture.entry, Type::I32);
            auto const header = builder.createBlock();
            auto const latch = builder.createBlock();
            auto const exit = builder.createBlock();
            auto const carried = builder.addParameter(header, Type::I64);
            builder.jump(header, std::array { countdown });
            builder.switchTo(header);
            if (store)
                storeCountdown(fixture, carried);
            std::ignore = builder.emit(Operation::TrapIf,
                                       {},
                                       std::array { condition },
                                       std::array { uint64_t { std::to_underlying(TrapCode::Unreachable) } });
            builder.branch(condition, latch, {}, exit, {});
            builder.switchTo(latch);
            auto const next = poll(fixture, carried);
            builder.jump(header, std::array { next });
            builder.switchTo(exit);
            builder.ret({});
            if (store)
                CHECK(verify(fixture.function).has_value());
            else
                CHECK(refusal(fixture.function)
                      == VerifierError {
                          .code = VerifierErrorCode::CountdownNotStored, .block = header, .instruction = 1 });
        }
    }
    SECTION("the entry's countdown is the stored one, and a block no unstored edge enters keeps it")
    {
        auto fixture = Fixture {};
        auto const next = fixture.builder.createBlock();
        fixture.builder.jump(next, {});
        fixture.builder.switchTo(next);
        trapIf(fixture);
        CHECK(verify(fixture.function).has_value());
    }
}

TEST_CASE("Verifier.staleCountdownAcrossBlocks")
{
    // The live countdown is followed across blocks, through block parameters: a store in a successor of the
    // countdown a call or a poll in its predecessor superseded is refused, as it is within one block.
    auto const callThenJump = [](Fixture& fixture, ValueId polled) {
        storeCountdown(fixture, polled);
        std::ignore = fixture.builder.emit(Operation::Call, {}, {}, std::array { uint64_t { 0 } });
        auto const next = fixture.builder.createBlock();
        fixture.builder.jump(next, {});
        fixture.builder.switchTo(next);
    };
    SECTION("a call in block A, the pre-call countdown stored in block B: refused; a reload stored: accepted")
    {
        for (auto const storeStale: { false, true })
        {
            INFO("store the stale countdown " << storeStale);
            auto fixture = Fixture {};
            auto const countdown = fixture.builder.addParameter(fixture.entry, Type::I64);
            auto const polled = poll(fixture, countdown);
            callThenJump(fixture, polled);
            storeCountdown(fixture, storeStale ? polled : reloadCountdown(fixture));
            fixture.builder.trap(TrapCode::Unreachable);
            if (storeStale)
                CHECK(refusal(fixture.function)
                      == VerifierError { .code = VerifierErrorCode::CountdownNotStored,
                                         .block = BlockId { 1 },
                                         .instruction = fixture.last() - 1 });
            else
                CHECK(verify(fixture.function).has_value());
        }
    }
    SECTION("a call in block A, the countdown A started with stored in block B: refused")
    {
        // Nothing in A polled or reloaded: the entry's countdown is as stale after the call as a polled one.
        auto fixture = Fixture {};
        auto const countdown = fixture.builder.addParameter(fixture.entry, Type::I64);
        std::ignore = fixture.builder.emit(Operation::Call, {}, {}, std::array { uint64_t { 0 } });
        auto const next = fixture.builder.createBlock();
        fixture.builder.jump(next, {});
        fixture.builder.switchTo(next);
        storeCountdown(fixture, countdown);
        fixture.builder.trap(TrapCode::Unreachable);
        CHECK(refusal(fixture.function).code == VerifierErrorCode::CountdownNotStored);
    }
    SECTION("a poll in block A, the countdown it replaced stored in block B: refused")
    {
        for (auto const storeStale: { false, true })
        {
            INFO("store the stale countdown " << storeStale);
            auto fixture = Fixture {};
            auto const countdown = fixture.builder.addParameter(fixture.entry, Type::I64);
            auto const polled = poll(fixture, countdown);
            auto const next = fixture.builder.createBlock();
            fixture.builder.jump(next, {});
            fixture.builder.switchTo(next);
            storeCountdown(fixture, storeStale ? countdown : polled);
            fixture.builder.trap(TrapCode::Unreachable);
            if (storeStale)
                CHECK(refusal(fixture.function).code == VerifierErrorCode::CountdownNotStored);
            else
                CHECK(verify(fixture.function).has_value());
        }
    }
    SECTION("a loop: its header parameter is the live countdown, the one the loop was entered with is stale")
    {
        // entry -> header(c); header: store ?; trapIf; branch -> latch | exit; latch: poll -> header(next).
        for (auto const storeEntered: { false, true })
        {
            INFO("store the countdown the loop was entered with " << storeEntered);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto const countdown = builder.addParameter(fixture.entry, Type::I64);
            auto const condition = builder.addParameter(fixture.entry, Type::I32);
            auto const header = builder.createBlock();
            auto const latch = builder.createBlock();
            auto const exit = builder.createBlock();
            auto const carried = builder.addParameter(header, Type::I64);
            builder.jump(header, std::array { countdown });
            builder.switchTo(header);
            storeCountdown(fixture, storeEntered ? countdown : carried);
            std::ignore = builder.emit(Operation::TrapIf,
                                       {},
                                       std::array { condition },
                                       std::array { uint64_t { std::to_underlying(TrapCode::Unreachable) } });
            builder.branch(condition, latch, {}, exit, {});
            builder.switchTo(latch);
            builder.jump(header, std::array { poll(fixture, carried) });
            builder.switchTo(exit);
            builder.ret({});
            if (storeEntered)
            {
                auto const refused = refusal(fixture.function);
                INFO(describe(refused));
                CHECK(refused.code == VerifierErrorCode::CountdownNotStored);
                CHECK(refused.block == header);
            }
            else
                CHECK(verify(fixture.function).has_value());
        }
    }
    SECTION("a merge: the parameter both edges pass their live countdown to is live, neither edge's value is")
    {
        // entry: poll; branch -> left | right; left: poll -> merge(a); right: poll -> merge(b); merge(m):
        // store.
        for (auto const stored: { 0, 1, 2 })
        {
            INFO("store " << stored);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto const countdown = builder.addParameter(fixture.entry, Type::I64);
            auto const condition = builder.addParameter(fixture.entry, Type::I32);
            auto const left = builder.createBlock();
            auto const right = builder.createBlock();
            auto const merge = builder.createBlock();
            auto const merged = builder.addParameter(merge, Type::I64);
            auto const entryPolled = poll(fixture, countdown);
            storeCountdown(fixture, entryPolled);
            builder.branch(condition, left, {}, right, {});
            builder.switchTo(left);
            auto const leftPolled = poll(fixture, entryPolled);
            storeCountdown(fixture, leftPolled);
            builder.jump(merge, std::array { leftPolled });
            builder.switchTo(right);
            auto const rightPolled = poll(fixture, entryPolled);
            storeCountdown(fixture, rightPolled);
            builder.jump(merge, std::array { rightPolled });
            builder.switchTo(merge);
            auto const candidates = std::array { merged, countdown, entryPolled };
            storeCountdown(fixture, candidates[static_cast<std::size_t>(stored)]);
            builder.trap(TrapCode::Unreachable);
            if (stored == 0)
                CHECK(verify(fixture.function).has_value());
            else
                CHECK(refusal(fixture.function).code == VerifierErrorCode::CountdownNotStored);
        }
    }
    SECTION("a merge of an edge that polled and one that did not: the value the second passes is stale")
    {
        // entry: poll p; branch -> left | right; left: poll -> merge(a); right: -> merge(p); merge(m): store.
        for (auto const storeParameter: { false, true })
        {
            INFO("store the parameter " << storeParameter);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto const countdown = builder.addParameter(fixture.entry, Type::I64);
            auto const condition = builder.addParameter(fixture.entry, Type::I32);
            auto const left = builder.createBlock();
            auto const right = builder.createBlock();
            auto const merge = builder.createBlock();
            auto const merged = builder.addParameter(merge, Type::I64);
            auto const polled = poll(fixture, countdown);
            storeCountdown(fixture, polled);
            builder.branch(condition, left, {}, right, {});
            builder.switchTo(left);
            auto const leftPolled = poll(fixture, polled);
            storeCountdown(fixture, leftPolled);
            builder.jump(merge, std::array { leftPolled });
            builder.switchTo(right);
            builder.jump(merge, std::array { polled });
            builder.switchTo(merge);
            storeCountdown(fixture, storeParameter ? merged : polled);
            builder.trap(TrapCode::Unreachable);
            if (storeParameter)
                CHECK(verify(fixture.function).has_value());
            else
                CHECK(refusal(fixture.function).code == VerifierErrorCode::CountdownNotStored);
        }
    }
    SECTION("a countdown passed to more parameters than are followed leaves its target unconstrained")
    {
        // entry: poll p; jump -> next(p, p, p, p, p); next: store its last parameter. Following four values,
        // the verifier gives up on the fifth rather than refuse a store of it.
        auto fixture = Fixture {};
        auto& builder = fixture.builder;
        auto const countdown = builder.addParameter(fixture.entry, Type::I64);
        auto const next = builder.createBlock();
        auto parameters = std::vector<ValueId> {};
        for ([[maybe_unused]] auto const position: std::views::iota(0, 5))
            parameters.push_back(builder.addParameter(next, Type::I64));
        auto const polled = poll(fixture, countdown);
        storeCountdown(fixture, polled);
        builder.jump(next, std::vector<ValueId>(parameters.size(), polled));
        builder.switchTo(next);
        storeCountdown(fixture, parameters.back());
        builder.trap(TrapCode::Unreachable);
        CHECK(verify(fixture.function).has_value());
    }
    SECTION("edges that agree: their value and the parameter it is passed to are both live")
    {
        // entry: poll; branch -> merge(p) | merge(p); merge(m): store p or m, both accepted.
        for (auto const storeParameter: { false, true })
        {
            INFO("store the parameter " << storeParameter);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto const countdown = builder.addParameter(fixture.entry, Type::I64);
            auto const condition = builder.addParameter(fixture.entry, Type::I32);
            auto const merge = builder.createBlock();
            auto const merged = builder.addParameter(merge, Type::I64);
            auto const polled = poll(fixture, countdown);
            builder.branch(condition, merge, std::array { polled }, merge, std::array { polled });
            builder.switchTo(merge);
            storeCountdown(fixture, storeParameter ? merged : polled);
            builder.trap(TrapCode::Unreachable);
            CHECK(verify(fixture.function).has_value());
        }
    }
}

TEST_CASE("Verifier.guardedAddressesStayInTheirStretch")
{
    // A guarded pointer addresses the storage its guard saw: no operation that may grow or move a memory
    // (a call, a stub, a poll, whose observer is host code) may lie between the guard and a use of it, on any
    // path. storeContext moves nothing.
    auto const loadFrom = [](Fixture& fixture, ValueId address) {
        std::ignore = fixture.builder.emit(Operation::Load,
                                           std::array { Type::I64 },
                                           std::array { address },
                                           loadOf(8, Extension::Zero, Type::I64));
    };
    SECTION("an operation between the check and its load")
    {
        struct Between
        {
            std::string_view name;
            std::function<void(Fixture&)> emit;
            bool refused;
        };
        auto const cases = std::vector<Between> {
            { "nothing", [](Fixture&) {}, false },
            { "a call",
              [](Fixture& f) {
                  std::ignore = f.builder.emit(Operation::Call, {}, {}, std::array { uint64_t { 0 } });
              },
              true },
            { "a stub",
              [](Fixture& f) {
                  std::ignore = f.builder.emit(Operation::CallStub, {}, {}, std::array { uint64_t { 0 } });
              },
              true },
            { "a poll",
              [](Fixture& f) {
                  auto const polled = poll(f, reloadCountdown(f));
                  storeCountdown(f, polled);
              },
              true },
            { "a stored countdown", [](Fixture& f) { storeCountdown(f, reloadCountdown(f)); }, false },
        };
        for (auto const& entry: cases)
        {
            INFO(entry.name);
            auto fixture = Fixture {};
            auto const address = guardedAddress(fixture);
            entry.emit(fixture);
            loadFrom(fixture, address);
            fixture.builder.ret({});
            if (entry.refused)
                CHECK(refusal(fixture.function).code == VerifierErrorCode::AddressAcrossMemoryMotion);
            else
                CHECK(verify(fixture.function).has_value());
        }
    }
    SECTION("across blocks: a merge where one edge called is a new stretch; a block only a quiet edge enters "
            "is not")
    {
        // entry: check; branch -> calls | quiet; calls: call -> merge; quiet: [load] -> merge; merge: [load].
        for (auto const loadAtMerge: { false, true })
        {
            INFO("load at the merge " << loadAtMerge);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto const condition = builder.addParameter(fixture.entry, Type::I32);
            auto const address = guardedAddress(fixture);
            auto const calls = builder.createBlock();
            auto const quiet = builder.createBlock();
            auto const merge = builder.createBlock();
            builder.branch(condition, calls, {}, quiet, {});
            builder.switchTo(calls);
            std::ignore = builder.emit(Operation::Call, {}, {}, std::array { uint64_t { 0 } });
            builder.jump(merge, {});
            builder.switchTo(quiet);
            if (!loadAtMerge)
                loadFrom(fixture, address);
            builder.jump(merge, {});
            builder.switchTo(merge);
            if (loadAtMerge)
                loadFrom(fixture, address);
            builder.ret({});
            if (loadAtMerge)
                CHECK(refusal(fixture.function).code == VerifierErrorCode::AddressAcrossMemoryMotion);
            else
                CHECK(verify(fixture.function).has_value());
        }
    }
    SECTION("an in-bounds address used after a call")
    {
        auto fixture = Fixture {};
        auto const address = fixture.builder.emit(Operation::InBoundsAddress,
                                                  std::array { Type::Pointer },
                                                  std::array { baseOf(fixture, 0) },
                                                  std::array<uint64_t, 3> { 0, 0, 8 })[0];
        std::ignore = fixture.builder.emit(Operation::Call, {}, {}, std::array { uint64_t { 0 } });
        loadFrom(fixture, address);
        fixture.builder.ret({});
        auto const minimums = std::array { uint64_t { 65'536 } };
        auto const refused = verify(fixture.function, minimums);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == VerifierErrorCode::AddressAcrossMemoryMotion);
    }
}

TEST_CASE("Verifier.memoryGuardsTakeTheirMemorysBase")
{
    // checkMemory and inBoundsAddress add their offset to their memory's base: memory 0's context value or
    // memoryBase of the memory, directly or through block parameters every edge passes one to.
    auto const checkOf = [](Fixture& fixture, ValueId base, uint64_t memory) {
        auto const index = fixture.builder.addParameter(fixture.entry, Type::I32);
        auto const bound = fixture.builder.addParameter(fixture.entry, Type::I64);
        std::ignore = fixture.builder.emit(Operation::CheckMemory,
                                           std::array { Type::Pointer },
                                           std::array { index, base, bound },
                                           std::array<uint64_t, 3> { memory, 0, 4 });
    };
    SECTION("its own memory's base, memory 0's or another's: accepted")
    {
        for (auto const memory: { uint64_t { 0 }, uint64_t { 3 } })
        {
            auto fixture = Fixture {};
            checkOf(fixture, baseOf(fixture, memory), memory);
            fixture.builder.ret({});
            CHECK(verify(fixture.function).has_value());
        }
    }
    SECTION("a parameter of the entry, or another memory's base: refused")
    {
        auto parameter = Fixture {};
        checkOf(parameter, parameter.builder.addParameter(parameter.entry, Type::Pointer), 0);
        parameter.builder.ret({});
        CHECK(refusal(parameter.function).code == VerifierErrorCode::UnguardedAddress);
        auto other = Fixture {};
        checkOf(other, baseOf(other, 1), 0);
        other.builder.ret({});
        CHECK(refusal(other.function).code == VerifierErrorCode::UnguardedAddress);
    }
    SECTION(
        "through a merge: accepted when both edges pass memory 0's base, refused when one passes another's")
    {
        for (auto const mixed: { false, true })
        {
            INFO("mixed " << mixed);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto const condition = builder.addParameter(fixture.entry, Type::I32);
            auto const left = builder.createBlock();
            auto const right = builder.createBlock();
            auto const merge = builder.createBlock();
            auto const merged = builder.addParameter(merge, Type::Pointer);
            builder.branch(condition, left, {}, right, {});
            builder.switchTo(left);
            builder.jump(merge, std::array { baseOf(fixture, 0) });
            builder.switchTo(right);
            builder.jump(merge, std::array { baseOf(fixture, mixed ? 1 : 0) });
            builder.switchTo(merge);
            checkOf(fixture, merged, 0);
            builder.ret({});
            if (mixed)
                CHECK(refusal(fixture.function).code == VerifierErrorCode::UnguardedAddress);
            else
                CHECK(verify(fixture.function).has_value());
        }
    }
    SECTION("an in-bounds address of another memory's base: refused")
    {
        auto fixture = Fixture {};
        std::ignore = fixture.builder.emit(Operation::InBoundsAddress,
                                           std::array { Type::Pointer },
                                           std::array { baseOf(fixture, 1) },
                                           std::array<uint64_t, 3> { 0, 0, 8 });
        fixture.builder.ret({});
        auto const minimums = std::array { uint64_t { 65'536 }, uint64_t { 65'536 } };
        auto const refused = verify(fixture.function, minimums);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == VerifierErrorCode::UnguardedAddress);
    }
}

TEST_CASE("Verifier.everyMemoryMotionRowIsPinned")
{
    // MemoryMotionTable as it must read, hard-coded: every call (direct, indirect, import, stub) and every
    // poll, frame charge and frame release may run code that grows or moves a memory; nothing else does,
    // storeContext included. A wrong row in the shared table fails here, where the pass and the verifier,
    // which both read it, would agree with it.
    using isoline::compiler::intermediate::MemoryMotion;
    using isoline::compiler::intermediate::memoryMotionOf;
    auto const moves =
        std::set<Operation> { Operation::Call,       Operation::CallIndirect, Operation::CallImport,
                              Operation::CallStub,   Operation::Poll,         Operation::PollStored,
                              Operation::EnterFrame, Operation::LeaveFrame };
    for (auto const& row: isoline::compiler::intermediate::OperationTable)
    {
        INFO(isoline::compiler::intermediate::nameOf(row.operation));
        CHECK((memoryMotionOf(row.operation) == MemoryMotion::MayMove) == moves.contains(row.operation));
    }

    // And the verifier refuses a use across each one it can be given without a dispatch row.
    struct Between
    {
        Operation operation;
        std::vector<uint64_t> immediates;
    };
    for (auto const& between:
         { Between { Operation::CallImport, { 0 } },
           Between { Operation::PollStored, { std::to_underlying(PollSite::FunctionEntry) } },
           Between { Operation::EnterFrame, { 1 } },
           Between { Operation::LeaveFrame, { 1 } } })
    {
        INFO(isoline::compiler::intermediate::nameOf(between.operation));
        auto fixture = Fixture {};
        auto const address = guardedAddress(fixture);
        std::ignore = fixture.builder.emit(between.operation, {}, {}, between.immediates);
        std::ignore = fixture.builder.emit(Operation::Load,
                                           std::array { Type::I64 },
                                           std::array { address },
                                           loadOf(8, Extension::Zero, Type::I64));
        fixture.builder.ret({});
        CHECK(refusal(fixture.function).code == VerifierErrorCode::AddressAcrossMemoryMotion);
    }
}

TEST_CASE("Verifier.remainingCountdownAndStretchPaths")
{
    SECTION("a call after a poll whose countdown is not stored")
    {
        auto fixture = Fixture {};
        auto const countdown = fixture.builder.addParameter(fixture.entry, Type::I64);
        std::ignore = poll(fixture, countdown);
        std::ignore = fixture.builder.emit(Operation::Call, {}, {}, std::array { uint64_t { 0 } });
        fixture.builder.ret({});
        CHECK(refusal(fixture.function).code == VerifierErrorCode::CountdownNotStored);
    }
    SECTION("a block no edge enters has no stretch: its loads are not held to one")
    {
        auto fixture = Fixture {};
        auto const address = guardedAddress(fixture);
        std::ignore = fixture.builder.emit(Operation::Load,
                                           std::array { Type::I64 },
                                           std::array { address },
                                           loadOf(8, Extension::Zero, Type::I64));
        fixture.builder.ret({});
        auto const unreached = fixture.builder.createBlock();
        fixture.builder.switchTo(unreached);
        fixture.builder.ret({});
        CHECK(verify(fixture.function).has_value());
    }
    SECTION("a use read before its guard's own row: a guard short of immediates, a guardAddress of nothing")
    {
        // The entry jumps to the guard's block (a later ID) through the use's block (an earlier ID), which
        // the verifier reads first: the guard's row is read before its own check refuses it.
        for (auto const shape: { 0, 1 })
        {
            INFO("shape " << shape);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto const index = builder.addParameter(fixture.entry, Type::I32);
            auto const bound = builder.addParameter(fixture.entry, Type::I64);
            auto const base = baseOf(fixture, 0);
            auto const use = builder.createBlock();
            auto const guard = builder.createBlock();
            builder.jump(guard, {});
            builder.switchTo(guard);
            auto const checked = shape == 0 ? builder.emit(Operation::CheckMemory,
                                                           std::array { Type::Pointer },
                                                           std::array { index, base, bound },
                                                           {})[0]
                                            : builder.emit(Operation::GuardAddress,
                                                           std::array { Type::Pointer },
                                                           {},
                                                           std::array<uint64_t, 1> { 0 })[0];
            builder.jump(use, {});
            builder.switchTo(use);
            std::ignore = builder.emit(Operation::Load,
                                       std::array { Type::I64 },
                                       std::array { checked },
                                       loadOf(8, Extension::Zero, Type::I64));
            builder.ret({});
            CHECK_FALSE(verify(fixture.function).has_value());
        }
    }
}
