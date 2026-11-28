// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/Code.hpp>
#include <isoline/interpreter/Listing.hpp>
#include <isoline/interpreter/Lowering.hpp>
#include <isoline/interpreter/LoweringTable.hpp>
#include <isoline/interpreter/SlotCodec.hpp>
#include <isoline/interpreter/testing/HostileModules.hpp>
#include <isoline/interpreter/testing/Lowering.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::FeatureSet;
using isoline::binary::DecodeLimits;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::HeapType;
using isoline::binary::Mutability;
using isoline::binary::Opcode;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::drop;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::localSet;
using isoline::binary::testing::ModuleBuilder;
using isoline::binary::testing::unreachable;
using isoline::interpreter::bodyUnits;
using isoline::interpreter::Continuation;
using isoline::interpreter::continuationAfter;
using isoline::interpreter::describe;
using isoline::interpreter::Dispatch;
using isoline::interpreter::LocalInit;
using isoline::interpreter::lower;
using isoline::interpreter::LoweringConfig;
using isoline::interpreter::LoweringError;
using isoline::interpreter::LoweringFailure;
using isoline::interpreter::loweringReservation;
using isoline::interpreter::MaxFrameSlots;
using isoline::interpreter::NoFunction;
using isoline::interpreter::nullSlots;
using isoline::interpreter::Word;
using isoline::interpreter::WorkPerBodyUnit;
using isoline::interpreter::testing::arenaOf;
using isoline::interpreter::testing::DefaultBudget;
using isoline::interpreter::testing::functionOf;
using isoline::interpreter::testing::listingOf;
using isoline::interpreter::testing::lowered;
using isoline::interpreter::testing::loweredWithinTheBounds;
using isoline::interpreter::testing::op;
using isoline::interpreter::testing::validated;
using isoline::runtime::CountingArena;
using isoline::runtime::ExecutionMode;
using isoline::validate::ValidatedModule;

namespace binary = isoline::binary;
namespace runtime = isoline::runtime;

namespace
{

/// A memory resource the host has no memory for: every allocation throws.
class ExhaustedResource final: public std::pmr::memory_resource
{
  private:
    void* do_allocate(std::size_t /*bytes*/, std::size_t /*alignment*/) override { throw std::bad_alloc(); }
    void do_deallocate(void* /*pointer*/, std::size_t /*bytes*/, std::size_t /*alignment*/) override {}
    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
    {
        return this == &other;
    }
};

/// A memory resource over the global heap whose host has memory for @p allowed allocations only.
class RefusingResource final: public std::pmr::memory_resource
{
  public:
    /// A resource refusing every allocation after the first @p allowed.
    explicit RefusingResource(std::size_t allowed) noexcept: _allowed { allowed } {}

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        if (_allocations == _allowed)
            throw std::bad_alloc();
        ++_allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
    {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
    {
        return this == &other;
    }

    std::size_t _allowed;
    std::size_t _allocations = 0;
};

} // namespace

TEST_CASE("Lowering.forwardsLocalGet")
{
    // Both operands are read from the params' own slots: no copy is made of either.
    auto const addParams = functionOf(
        { binary::I32, binary::I32 }, { binary::I32 }, {}, code({ localGet(0), localGet(1), op(0x6A) }));
    CHECK(listingOf(addParams) == "i32.add s2, s0, s1\nreturn s2\n");
}

TEST_CASE("Lowering.foldsConstantOperand")
{
    auto const addFive =
        functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), i32Const(5), op(0x6A) }));
    CHECK(listingOf(addFive) == "i32.add.imm s1, s0, #5\nreturn s1\n");
}

TEST_CASE("Lowering.materializesConstantsWithoutAnImmediateTwin")
{
    SECTION("a constant left operand")
    {
        auto const sevenMinus =
            functionOf({ binary::I32 }, { binary::I32 }, {}, code({ i32Const(7), localGet(0), op(0x6B) }));
        CHECK(listingOf(sevenMinus) == "const64 s1, #7\ni32.sub s1, s1, s0\nreturn s1\n");
    }
    SECTION("a constant right operand of a float operator")
    {
        auto const plusOne = functionOf({ binary::F32 },
                                        { binary::F32 },
                                        {},
                                        code({ localGet(0), { 0x43, 0x00, 0x00, 0x80, 0x3F }, op(0x92) }));
        CHECK(listingOf(plusOne) == "const64 s2, #1065353216\nf32.add s1, s0, s2\nreturn s1\n");
    }
    SECTION("the operand of a unary operator")
    {
        auto const isZero = functionOf({}, { binary::I32 }, {}, code({ i32Const(0), op(0x45) }));
        CHECK(listingOf(isZero) == "const64 s0, #0\ni32.eqz s0, s0\nreturn s0\n");
    }
    SECTION("a constant result")
    {
        auto const minusOne = functionOf({}, { binary::I32 }, {}, i32Const(-1));
        CHECK(listingOf(minusOne) == "const64 s0, #4294967295\nreturn s0\n");
    }
}

TEST_CASE("Lowering.materializesBeforeLocalSet")
{
    // (local.get 1) (local.get 0) (i32.const 1) (local.set 0) (return): the forwarder of local 0's old
    // value is copied before local 0 changes; local 1's forwarder is left alone until the return.
    auto const overwrite = functionOf({ binary::I32, binary::I32 },
                                      { binary::I32, binary::I32 },
                                      {},
                                      code({ localGet(1), localGet(0), i32Const(1), localSet(0), op(0x0F) }));
    CHECK(listingOf(overwrite) == "move64 s3, s0\nconst64 s0, #1\nmove64 s2, s1\nreturn s2\n");
}

TEST_CASE("Lowering.readsAnEarlierResultFromItsSlot")
{
    auto const chain = functionOf({ binary::I32, binary::I32 },
                                  { binary::I32 },
                                  {},
                                  code({ localGet(0), localGet(1), op(0x6A), localGet(0), op(0x6C) }));
    CHECK(listingOf(chain) == "i32.add s2, s0, s1\ni32.mul s2, s2, s0\nreturn s2\n");
}

TEST_CASE("Lowering.writesAResultStraightIntoTheLocalSetOrTeedNext")
{
    SECTION("a folded immediate and a unary go straight to the local")
    {
        auto const increment =
            functionOf({ binary::I32 }, {}, {}, code({ localGet(0), i32Const(1), op(0x6A), localSet(0) }));
        CHECK(listingOf(increment) == "i32.add.imm s0, s0, #1\nreturn s1\n");
        auto const zero = functionOf({ binary::I32 }, {}, {}, code({ localGet(0), op(0x45), localSet(0) }));
        CHECK(listingOf(zero).starts_with("i32.eqz s0, s0\n"));
    }
    SECTION("a local whose old value an operand still forwards is set by a move")
    {
        // (local.get 0) (local.get 0) (i32.const 1) (i32.add) (local.set 0) (return): the forwarder below is
        // copied first, so the sum is moved into local 0 after it.
        auto const kept =
            functionOf({ binary::I32 },
                       { binary::I32 },
                       {},
                       code({ localGet(0), localGet(0), i32Const(1), op(0x6A), localSet(0), op(0x0F) }));
        CHECK(listingOf(kept) == "i32.add.imm s2, s0, #1\nmove64 s1, s0\nmove64 s0, s2\nreturn s1\n");
    }
    SECTION("a result teed into a local goes to the local, and the operand forwards it")
    {
        // (local.get 0) (i32.const 1) (i32.add) (local.tee 0) (i32.const 2) (i32.mul): the product reads
        // the sum from local 0.
        auto const tee =
            functionOf({ binary::I32 },
                       { binary::I32 },
                       {},
                       code({ localGet(0), i32Const(1), op(0x6A), op(0x22, 0), i32Const(2), op(0x6C) }));
        CHECK(listingOf(tee) == "i32.add.imm s0, s0, #1\ni32.mul.imm s1, s0, #2\nreturn s1\n");
    }
    SECTION("a result neither set nor teed next stays an operand")
    {
        auto const kept = functionOf({ binary::I32 },
                                     { binary::I32 },
                                     {},
                                     code({ localGet(0), i32Const(1), op(0x6A), op(0x1A), localGet(0) }));
        CHECK(listingOf(kept).starts_with("i32.add.imm s1, s0, #1\n"));
    }
}

TEST_CASE("Lowering.skipsAnEmptyLocalRun")
{
    // A declaration of no locals of one type, then one i64: the i64 is local 0, in slot 0.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { binary::I64 } });
    builder.rawSection(3, { 0x01, 0x00 });
    builder.rawSection(10, { 0x01, 0x08, 0x02, 0x00, 0x7F, 0x01, 0x7E, 0x20, 0x00, 0x0B });
    auto const module = lowered(validated(builder));
    auto const& function = module->functions()[0];
    CHECK(describe(function) == "move64 s1, s0\nreturn s1\n");
    CHECK(function.localSlots == 1);
    REQUIRE(function.localInit.size() == 1);
    CHECK(function.localInit[0].slotBase == 0);
    CHECK(function.localInit[0].slotCount == 1);
}

TEST_CASE("Lowering.localSetFromEachKindOfEntry")
{
    SECTION("from a slot")
    {
        auto const sum = functionOf(
            { binary::I32, binary::I32 }, {}, {}, code({ localGet(0), localGet(1), op(0x6A), localSet(1) }));
        // The sum goes straight to local 1: no move.
        CHECK(listingOf(sum) == "i32.add s1, s0, s1\nreturn s2\n");
    }
    SECTION("from another local")
    {
        auto const copy =
            functionOf({ binary::I32, binary::I32 }, {}, {}, code({ localGet(1), localSet(0) }));
        CHECK(listingOf(copy) == "move64 s0, s1\nreturn s2\n");
    }
    SECTION("from the local itself")
    {
        auto const same = functionOf({ binary::I32 }, {}, {}, code({ localGet(0), localSet(0) }));
        CHECK(listingOf(same) == "return s1\n");
    }
    SECTION("a v128 local")
    {
        auto const copy =
            functionOf({ binary::V128, binary::V128 }, {}, {}, code({ localGet(1), localSet(0) }));
        CHECK(listingOf(copy) == "move128 s0, s2\nreturn s4\n");
    }
}

TEST_CASE("Lowering.teeKeepsTheValueOnTheStack")
{
    SECTION("a forwarded local stays forwarded")
    {
        auto const teeAdd = functionOf({ binary::I32, binary::I32 },
                                       { binary::I32 },
                                       {},
                                       code({ localGet(1), op(0x22, 0), localGet(0), op(0x6A) }));
        CHECK(listingOf(teeAdd) == "move64 s0, s1\ni32.add s2, s1, s0\nreturn s2\n");
    }
    SECTION("a constant stays foldable")
    {
        auto const teeConstant = functionOf(
            { binary::I32 }, { binary::I32 }, {}, code({ i32Const(3), op(0x22, 0), i32Const(4), op(0x6A) }));
        CHECK(listingOf(teeConstant)
              == "const64 s0, #3\nconst64 s1, #3\ni32.add.imm s1, s1, #4\nreturn s1\n");
    }
    SECTION("a tee of the local's own forwarder writes nothing, and leaves its forwarders forwarded")
    {
        auto const teeSelf = functionOf({ binary::I32 },
                                        { binary::I32, binary::I32 },
                                        {},
                                        code({ localGet(0), localGet(0), op(0x22, 0) }));
        // Both moves are the return's; the tee emits none.
        CHECK(listingOf(teeSelf) == "move64 s2, s0\nmove64 s1, s0\nreturn s1\n");
    }
    SECTION("a set of the local's own forwarder writes nothing, and leaves its forwarders forwarded")
    {
        auto const setSelf =
            functionOf({ binary::I32 },
                       { binary::I32 },
                       {},
                       code({ localGet(0), localGet(0), localSet(0), i32Const(1), localSet(0) }));
        // The older forwarder is materialized by the second set, which changes local 0, not by the first.
        CHECK(listingOf(setSelf) == "move64 s1, s0\nconst64 s0, #1\nreturn s1\n");
    }
}

TEST_CASE("Lowering.dropLeavesNoForwarder")
{
    // The dropped forwarder of local 0 is gone from its chain, so setting local 0 copies nothing.
    auto const dropped = functionOf(
        { binary::I32 }, {}, {}, code({ localGet(0), drop(), i32Const(1), localSet(0), op(0x01) }));
    CHECK(listingOf(dropped) == "const64 s0, #1\nreturn s1\n");
}

TEST_CASE("Lowering.selectsByWidth")
{
    SECTION("numbers")
    {
        auto const pick = functionOf({ binary::I64, binary::I64, binary::I32 },
                                     { binary::I64 },
                                     {},
                                     code({ localGet(0), localGet(1), localGet(2), op(0x1B) }));
        CHECK(listingOf(pick) == "select64 s3, s0, s1, s2\nreturn s3\n");
    }
    SECTION("vectors, and a constant condition")
    {
        auto const pick = functionOf({ binary::V128, binary::V128 },
                                     { binary::V128 },
                                     {},
                                     code({ localGet(0), localGet(1), i32Const(1), op(0x1B) }));
        CHECK(listingOf(pick) == "const64 s8, #1\nselect128 s4, s0, s2, s8\nreturn s4\n");
    }
    SECTION("references, typed")
    {
        auto const pick = functionOf({ binary::FuncRef, binary::FuncRef, binary::I32 },
                                     { binary::FuncRef },
                                     {},
                                     code({ localGet(0), localGet(1), localGet(2), { 0x1C, 0x01, 0x70 } }));
        CHECK(listingOf(pick) == "select128 s5, s0, s2, s4\nreturn s5\n");
    }
}

TEST_CASE("Lowering.globalsByModuleIndex")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { binary::I64 } });
    builder.importGlobal(
        "host", "counter", GlobalType { .type = binary::I32, .mutability = Mutability::Var });
    builder.global(GlobalType { .type = binary::I64, .mutability = Mutability::Var },
                   code({ { 0x42, 0x00 } }));
    builder.function(0, {}, code({ i32Const(9), op(0x24, 0), op(0x23, 0), drop(), op(0x23, 1) }));
    auto const module = lowered(validated(builder));
    CHECK(describe(module->functions()[0])
          == "const64 s0, #9\nglobal.set s0, #0\nglobal.get s0, #0\nglobal.get s0, #1\nreturn s0\n");
}

TEST_CASE("Lowering.nothingAfterTheFunctionLeaves")
{
    SECTION("unreachable")
    {
        auto const trap = functionOf({}, {}, {}, code({ unreachable(), i32Const(1), drop() }));
        CHECK(listingOf(trap) == "unreachable\n");
    }
    SECTION("return, with operands left below the results")
    {
        auto const early = functionOf({ binary::I32 },
                                      { binary::I32 },
                                      {},
                                      code({ i32Const(1), localGet(0), op(0x0F), i32Const(2), op(0x01) }));
        CHECK(listingOf(early) == "move64 s2, s0\nreturn s2\n");
    }
}

TEST_CASE("Lowering.v128AndRefTakeTwoSlots")
{
    auto const module = lowered(validated(functionOf({ binary::V128, binary::FuncRef, binary::I32 },
                                                     { binary::I32 },
                                                     { binary::ExternRef, binary::I64 },
                                                     localGet(2))));
    auto const& function = module->functions()[0];
    CHECK(describe(function) == "move64 s8, s4\nreturn s8\n");
    CHECK(function.paramSlots == 5);
    CHECK(function.localSlots == 8);
    CHECK(function.frameSlots == 9);
    CHECK(function.resultSlots == 1);
}

TEST_CASE("Lowering.bothModesUseCanonicalFloats")
{
    // P4 lowers every float operator to the canonical-NaN kernel in either mode (plan Decision C2).
    auto const add = functionOf(
        { binary::F32, binary::F32 }, { binary::F32 }, {}, code({ localGet(0), localGet(1), op(0x92) }));
    auto const module = validated(add);
    auto const production = lowered(module, LoweringConfig { .mode = ExecutionMode::Production });
    auto const deterministic = lowered(module, LoweringConfig { .mode = ExecutionMode::Deterministic });
    CHECK(production->mode() == ExecutionMode::Production);
    CHECK(deterministic->mode() == ExecutionMode::Deterministic);
    CHECK(describe(production->functions()[0]) == "f32.add s2, s0, s1\nreturn s2\n");
    CHECK(describe(deterministic->functions()[0]) == describe(production->functions()[0]));
    CHECK(production->dispatch() == Dispatch::Switch);
}

TEST_CASE("Lowering.carriesTheFrameCost")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { binary::I32 }, .results = { binary::I32 } });
    builder.function(0, {}, localGet(0));
    builder.function(0, { binary::I64, binary::V128 }, code({ localGet(0), localGet(0), op(0x6A) }));
    builder.function(0, {}, code({ i32Const(1), i32Const(2), i32Const(3), op(0x1B) }));
    auto const module = validated(builder);
    auto const code = lowered(module);
    REQUIRE(code->functions().size() == 3);
    for (auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 3 }))
    {
        CHECK(code->functions()[index].codeIndex == index);
        CHECK(code->functions()[index].frameCost == module->frameCost(index));
    }
}

TEST_CASE("Lowering.refLocalsStartAsTypedNull")
{
    auto const concrete =
        ValueType { .kind = ValueKind::Ref,
                    .heap = HeapType { .kind = binary::AbstractHeapType::Concrete, .typeIndex = 0 } };
    auto const module = lowered(validated(functionOf({ binary::I32 },
                                                     {},
                                                     { binary::I32,
                                                       concrete,
                                                       binary::FuncRef,
                                                       binary::FuncRef,
                                                       binary::ExternRef,
                                                       binary::ExnRef,
                                                       binary::V128,
                                                       binary::F64 },
                                                     {})));
    auto const& runs = module->functions()[0].localInit;
    // Adjacent locals that start alike share a run; each reference heap its own.
    auto const expected = std::to_array<std::tuple<uint32_t, uint32_t, LocalInit, ValueType>>({
        { 1, 1, LocalInit::Zero, binary::I32 },
        { 2, 2, LocalInit::Null, concrete },
        { 4, 4, LocalInit::Null, binary::FuncRef },
        { 8, 2, LocalInit::Null, binary::ExternRef },
        { 10, 2, LocalInit::Null, binary::ExnRef },
        { 12, 3, LocalInit::Zero, binary::V128 },
    });
    REQUIRE(runs.size() == expected.size());
    auto const instanceTypes = std::to_array<uint32_t>({ 0 });
    for (auto const index: std::views::iota(std::size_t { 0 }, expected.size()))
    {
        auto const& [slotBase, slotCount, kind, type] = expected[index];
        CHECK(runs[index].slotBase == slotBase);
        CHECK(runs[index].slotCount == slotCount);
        CHECK(runs[index].kind == kind);
        if (kind == LocalInit::Null)
        {
            auto const slots = nullSlots(runs[index].heap, instanceTypes);
            auto const value = isoline::interpreter::loadValue(type, slots);
            CHECK(value
                  == runtime::Value { runtime::Ref { .kind = runtime::RefKind::Null, .heap = type.heap } });
        }
    }
}

TEST_CASE("Lowering.refusesAtTheFirstOverrun")
{
    // A 100-instruction body under a 64-byte budget: refused at the reservation, before any work.
    auto body = Bytes {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 50))
        body = code({ body, i32Const(1), drop() });
    auto const module = validated(functionOf({}, {}, {}, body));
    REQUIRE(module->module().code[0].body.size() == 101);
    auto const expectedReservation = loweringReservation(module->module(), 0);
    auto const arena = arenaOf(64);
    auto const result = lower(*module, LoweringConfig {}, arena);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LoweringError { .codeIndex = 0, .requested = expectedReservation, .limit = 64 });
    CHECK(arena->peak() <= 64);
    CHECK(arena->charged() == 0);
}

TEST_CASE("Lowering.refusesPastTheReservation")
{
    // The budget holds the reservation, but not lowering's temporaries: refused at the first
    // allocation past it, and the arena never charged more than its budget.
    auto const module = validated(
        functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), i32Const(1), op(0x6A) })));
    auto const budget = loweringReservation(module->module(), 0);
    auto const arena = arenaOf(budget);
    auto const result = lower(*module, LoweringConfig {}, arena);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().codeIndex == 0);
    CHECK(result.error().reason == LoweringFailure::Budget);
    CHECK(result.error().limit == budget);
    CHECK(result.error().requested > 0);
    CHECK(arena->peak() <= budget);
    CHECK(arena->charged() == 0);
    CHECK(arena->reserved() == 0);
}

TEST_CASE("Lowering.refusalNamesTheFunction")
{
    // The second function's reservation is what exceeds the budget.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = {} });
    builder.function(0, {}, {});
    auto big = Bytes {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 1'000))
        big = code({ big, i32Const(1), drop() });
    builder.function(0, {}, big);
    auto const module = validated(builder);
    auto const budget = loweringReservation(module->module(), 0) + 4'096;
    auto const arena = arenaOf(budget);
    auto const result = lower(*module, LoweringConfig {}, arena);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == LoweringError {
              .codeIndex = 1, .requested = loweringReservation(module->module(), 1), .limit = budget });
}

TEST_CASE("Lowering.aRefusedModuleTableNamesNoFunction")
{
    // The imported globals' types are the module's, allocated before any function is lowered.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = {} });
    for (auto const index: std::views::iota(0, 10))
        builder.importGlobal("host", std::format("g{}", index), GlobalType { .type = binary::I32 });
    builder.function(0, {}, {});
    auto const module = validated(builder);
    auto const arena = arenaOf(16);
    auto const result = lower(*module, LoweringConfig {}, arena);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().codeIndex == NoFunction);
    CHECK(result.error().reason == LoweringFailure::Budget);
    CHECK(arena->charged() == 0);
}

TEST_CASE("Lowering.refusesAFrameOfMoreSlotsThanAnOperandNames")
{
    // Decode limits raised past their defaults admit 2^31 v128 locals: 2^32 slots, one more than a
    // 32-bit slot operand names. Refused before anything is reserved.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = {} });
    builder.rawSection(3, { 0x01, 0x00 });
    builder.rawSection(10, code({ { 0x01, 0x08, 0x01 }, encodeU32(uint32_t { 1 } << 31U), { 0x7B, 0x0B } }));
    auto const limits = DecodeLimits { .maxLocals = std::numeric_limits<uint32_t>::max() };
    auto loaded = ValidatedModule::load(builder.build(), FeatureSet::all(), limits);
    REQUIRE(loaded.has_value());
    auto const arena = arenaOf();
    auto const result = lower(**loaded, LoweringConfig {}, arena);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == LoweringError { .codeIndex = 0,
                             .requested = uint64_t { 1 } << 32U,
                             .limit = MaxFrameSlots,
                             .reason = LoweringFailure::FrameSlots });
    CHECK(arena->peak() == 0);
}

TEST_CASE("Lowering.refusesWhenTheHostHasNoMemory")
{
    auto exhausted = ExhaustedResource {};
    auto const arena = std::make_shared<CountingArena>(DefaultBudget, exhausted);
    auto const module = validated(functionOf({}, {}, {}, {}));
    auto const result = lower(*module, LoweringConfig {}, arena);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().reason == LoweringFailure::HostMemory);
    // The first allocation is the module's table of its types' slots, which no function owns.
    CHECK(result.error().codeIndex == NoFunction);
    CHECK(arena->charged() == 0);
}

TEST_CASE("Lowering.everyHostRefusalIsHostMemory")
{
    // The host refuses its n-th allocation, for each n until lowering needs no more: every one through the
    // arena, as the module's own record comes from the global heap, and each refusal is HostMemory.
    auto const module = validated(
        functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), i32Const(1), op(0x6A) })));
    auto lastRefusal = std::optional<LoweringError> {};
    auto lowered = false;
    for (auto const allowed: std::views::iota(std::size_t { 0 }, std::size_t { 100 }))
    {
        auto host = RefusingResource { allowed };
        auto const arena = std::make_shared<CountingArena>(DefaultBudget, host);
        auto const result = lower(*module, LoweringConfig {}, arena);
        if (result.has_value())
        {
            lowered = true;
            break;
        }
        CHECK(result.error().reason == LoweringFailure::HostMemory);
        lastRefusal = result.error();
    }
    REQUIRE(lowered);
    REQUIRE(lastRefusal.has_value());
}

TEST_CASE("Lowering.refusedRecordLeavesTheArenaWhole")
{
    // The record's memory refuses after every function is lowered: lower() throws std::bad_alloc, and the
    // functions, unwound after the throw, return their memory to an arena still alive, though lower() held
    // its only owner (AddressSanitizer reports a return to a freed arena).
    auto const module = validated(
        functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), i32Const(1), op(0x6A) })));
    auto refusing = RefusingResource { 0 };
    CHECK_THROWS_AS(std::ignore = lower(*module, LoweringConfig {}, arenaOf(), refusing), std::bad_alloc);
}

TEST_CASE("Lowering.recordsWhatItTook")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { binary::I32 }, .results = { binary::I32 } });
    builder.function(0, { binary::I32 }, code({ localGet(0), localSet(1), localGet(1) }));
    builder.function(0, {}, code({ localGet(0), i32Const(2), op(0x6C) }));
    auto const module = validated(builder);
    auto const arena = arenaOf();
    auto const result = lower(*module, LoweringConfig {}, arena);
    REQUIRE(result.has_value());
    auto const& code = **result;
    CHECK(code.stats().bodyUnits
          == bodyUnits(module->module().code[0]) + bodyUnits(module->module().code[1]));
    CHECK(code.stats().work > 0);
    CHECK(code.stats().work <= WorkPerBodyUnit * code.stats().bodyUnits);
    // What the module keeps is what the arena still charges: its code, no temporaries, no reservation.
    CHECK(code.loweredBytes() == arena->charged());
    CHECK(arena->reserved() == 0);
    CHECK(arena->peak() >= arena->charged());
    for (auto const& function: code.functions())
        CHECK(function.code.capacity() == function.code.size());
}

TEST_CASE("Lowering.hostileShapesStayLinear")
{
    // Under the memory caps: 50'000 locals and 1M local.get/local.set over a 50'000-high stack, and a
    // 7 MB body of i32.const/drop. The code, the work and the peak stay within their published bounds.
    namespace hostile = isoline::interpreter::testing;
    // 8 MiB for the temporaries: the 50'000-entry stack, the chains and 50'000 local runs take less
    // than 4 MiB.
    constexpr auto Temporaries = uint64_t { 8 } * 1024 * 1024;
    SECTION("a small forwarded stack, which a quadratic lowering already fails quickly")
    {
        std::ignore = loweredWithinTheBounds(hostile::forwardedStack(1'000, 2'000, 20'000), Temporaries);
    }
    SECTION("forwarded stack")
    {
        auto const module =
            loweredWithinTheBounds(hostile::forwardedStack(50'000, 50'000, 1'000'000), Temporaries);
        CHECK(module->functions()[0].frameSlots > 65'535);
    }
    SECTION("constants and drops")
    {
        auto const module = loweredWithinTheBounds(hostile::constantsAndDrops(7'000'000), Temporaries);
        CHECK(describe(module->functions()[0]) == "return s0\n");
    }
}

namespace
{

/// A block, loop or if (@p opcode) of block type @p blockType around @p body.
[[nodiscard]] Bytes blockOf(uint8_t opcode, uint8_t blockType, Bytes const& body)
{
    return code({ { opcode, blockType }, body, { 0x0B } });
}

/// @p builder with a second function type, index 1, which a block names.
[[nodiscard]] ModuleBuilder withBlockType(ModuleBuilder builder, FuncType type)
{
    builder.type(std::move(type));
    return builder;
}

/// How many lines of @p listing start with @p text and a space.
[[nodiscard]] std::size_t linesStartingWith(std::string_view listing, std::string_view text)
{
    auto count = std::size_t { 0 };
    for (auto const line: std::views::split(listing, '\n'))
        if (std::string_view { line }.starts_with(std::format("{} ", text)))
            ++count;
    return count;
}

/// The compile budget's allowance for the temporaries of the hostile control-flow shapes: their stacks
/// of blocks and values take less than half of it.
constexpr auto ControlTemporaries = uint64_t { 32 } * 1024 * 1024;

} // namespace

TEST_CASE("Lowering.fusesCompareAndBranch")
{
    SECTION("a loop counting down: the comparison with its constant and the br_if are one operation")
    {
        auto const countDown = functionOf({ binary::I32 },
                                          {},
                                          {},
                                          blockOf(0x03,
                                                  0x40,
                                                  code({ localGet(0),
                                                         i32Const(1),
                                                         op(0x6B),
                                                         localSet(0),
                                                         localGet(0),
                                                         i32Const(0),
                                                         op(0x47),
                                                         op(0x0D, 0) })));
        CHECK(listingOf(countDown)
              == "i32.sub.imm s0, s0, #1\ni32.ne.imm.br_if.loop s0, #0 → @0\nreturn s1\n");
    }
    SECTION("not where the br_if moves its values: the comparison's result is its condition")
    {
        // The block's result is one slot above its slot, past a value below it.
        auto const moving = functionOf({ binary::I32, binary::I32 },
                                       {},
                                       {},
                                       code({ blockOf(0x02,
                                                      0x7F,
                                                      code({ localGet(0),
                                                             localGet(1),
                                                             localGet(0),
                                                             localGet(1),
                                                             op(0x46),
                                                             op(0x0D, 0),
                                                             op(0x0C, 0) })),
                                              drop() }));
        CHECK(listingOf(moving)
              == "i32.eq s4, s0, s1\nmove64 s3, s1\nmove64 s2, s0\nbr_if.move s4, s2, s3, #1 → @16\n"
                 "move.range s2, s3, #1\njump → @16\nreturn s2\n");
    }
}

TEST_CASE("Lowering.everyLoopTransferPolls")
{
    // Each transfer of control to a loop label is its polling twin, or a br_table record of LoopPoll::Yes;
    // to a block label it is not.
    SECTION("br")
    {
        CHECK(listingOf(functionOf({}, {}, {}, blockOf(0x03, 0x40, op(0x0C, 0))))
              == "jump.loop → @0\nreturn s0\n");
        CHECK(listingOf(functionOf({}, {}, {}, blockOf(0x02, 0x40, op(0x0C, 0))))
              == "jump → @2\nreturn s0\n");
    }
    SECTION("br_if")
    {
        auto const body = code({ localGet(0), op(0x0D, 0) });
        CHECK(listingOf(functionOf({ binary::I32 }, {}, {}, blockOf(0x03, 0x40, body)))
              == "br_if.loop s0 → @0\nreturn s1\n");
        CHECK(listingOf(functionOf({ binary::I32 }, {}, {}, blockOf(0x02, 0x40, body)))
              == "br_if s0 → @3\nreturn s1\n");
    }
    SECTION("a br_table record")
    {
        // Labels 0 (the loop), 1 (the block) and the default, 1.
        auto const table = code({ localGet(0), { 0x0E, 0x02, 0x00, 0x01, 0x01 } });
        CHECK(listingOf(functionOf({ binary::I32 }, {}, {}, blockOf(0x02, 0x40, blockOf(0x03, 0x40, table))))
              == "br_table s0, s1, #0, #2 → @0.loop s1, @10 s1, @10 s1\nreturn s1\n");
    }
    SECTION("a fused comparison")
    {
        auto const body = code({ localGet(0), op(0x45), op(0x0D, 0) });
        CHECK(listingOf(functionOf({ binary::I32 }, {}, {}, blockOf(0x03, 0x40, body)))
              == "i32.eqz.br_if.loop s0 → @0\nreturn s1\n");
        CHECK(listingOf(functionOf({ binary::I32 }, {}, {}, blockOf(0x02, 0x40, body)))
              == "i32.eqz.br_if s0 → @3\nreturn s1\n");
    }
    SECTION("br_on_null")
    {
        auto const body = code({ localGet(0), { 0xD5, 0x00 }, drop() });
        CHECK(listingOf(functionOf({ binary::FuncRef }, {}, {}, blockOf(0x03, 0x40, body)))
              == "br_on_null.loop s0 → @0\nreturn s2\n");
        CHECK(listingOf(functionOf({ binary::FuncRef }, {}, {}, blockOf(0x02, 0x40, body)))
              == "br_on_null s0 → @3\nreturn s2\n");
    }
    SECTION("br_on_non_null")
    {
        // The loop takes the reference as its param, which the branch carries back to it.
        auto const looping = withBlockType(
            functionOf(
                { binary::FuncRef }, {}, {}, code({ localGet(0), blockOf(0x03, 0x01, { 0xD6, 0x00 }) })),
            FuncType { .params = { binary::FuncRef }, .results = {} });
        CHECK(listingOf(looping) == "move128 s2, s0\nbr_on_non_null.loop s2 → @2\nreturn s2\n");
        auto const leaving = functionOf(
            { binary::FuncRef },
            {},
            {},
            code({ blockOf(0x02, 0x70, code({ localGet(0), { 0xD6, 0x00 }, localGet(0) })), drop() }));
        CHECK(listingOf(leaving) == "move128 s2, s0\nbr_on_non_null s2 → @7\nmove128 s2, s0\nreturn s2\n");
    }
}

TEST_CASE("Lowering.branchTableRecordsNameEachLabelsSlots")
{
    // Each record names the slot its own label's values go to, not the default's: the table's one
    // value run, here s2, moves to s2 for the inner block and to s1 for the label below it.
    auto const table = Bytes { 0x0E, 0x01, 0x00, 0x01 }; // br_table 0 1
    SECTION("two blocks of one result, at different heights")
    {
        auto const nested =
            functionOf({ binary::I32 },
                       {},
                       {},
                       code({ blockOf(0x02,
                                      0x7F,
                                      code({ i32Const(9),
                                             blockOf(0x02, 0x7F, code({ i32Const(7), localGet(0), table })),
                                             drop() })),
                              drop() }));
        CHECK(listingOf(nested)
              == "const64 s1, #9\nconst64 s2, #7\nbr_table s0, s2, #1, #1 → @14 s2, @14 s1\nreturn s1\n");
    }
    SECTION("a record to the function's own label returns through the table")
    {
        auto const returning = functionOf(
            { binary::I32 },
            { binary::I32 },
            {},
            code({ i32Const(5), blockOf(0x02, 0x7F, code({ i32Const(7), localGet(0), table })), op(0x6A) }));
        CHECK(listingOf(returning)
              == "const64 s1, #5\nconst64 s2, #7\nbr_table s0, s2, #1, #1 → @14 s2, @17 s1\n"
                 "i32.add s1, s1, s2\nreturn s1\n");
    }
}

TEST_CASE("Lowering.conditionalBranchesMoveTheirValuesOnce")
{
    // A conditional branch whose values are not in its label's slots moves them as it branches, in one
    // operation: the condition, the label's slots, the values' slots and their count.
    SECTION("br_if to a loop")
    {
        auto const module = withBlockType(functionOf({ binary::I32 },
                                                     {},
                                                     {},
                                                     code({ localGet(0),
                                                            blockOf(0x03,
                                                                    0x01,
                                                                    code({ localGet(0),
                                                                           localGet(0),
                                                                           localGet(0),
                                                                           op(0x0D, 0),
                                                                           drop(),
                                                                           drop(),
                                                                           drop() })) })),
                                          FuncType { .params = { binary::I32 }, .results = {} });
        CHECK(listingOf(module)
              == "move64 s1, s0\nmove64 s3, s0\nmove64 s2, s0\nbr_if.move.loop s0, s1, s3, #1 → @2\nreturn "
                 "s1\n");
    }
    SECTION("br_on_null to a block and to a loop")
    {
        auto const leaving = functionOf(
            { binary::I32, binary::FuncRef },
            {},
            {},
            code({ blockOf(
                       0x02,
                       0x7F,
                       code({ localGet(0), localGet(0), localGet(1), { 0xD5, 0x00 }, drop(), op(0x0C, 0) })),
                   drop() }));
        CHECK(listingOf(leaving)
              == "move64 s4, s0\nmove64 s3, s0\nbr_on_null.move s1, s3, s4, #1 → @13\nmove.range s3, s4, #1\n"
                 "jump → @13\nreturn s3\n");
        auto const looping = withBlockType(functionOf({ binary::I32, binary::FuncRef },
                                                      {},
                                                      {},
                                                      code({ localGet(0),
                                                             blockOf(0x03,
                                                                     0x01,
                                                                     code({ localGet(0),
                                                                            localGet(0),
                                                                            localGet(1),
                                                                            { 0xD5, 0x00 },
                                                                            drop(),
                                                                            drop(),
                                                                            drop(),
                                                                            drop() })) })),
                                           FuncType { .params = { binary::I32 }, .results = {} });
        CHECK(listingOf(looping)
              == "move64 s3, s0\nmove64 s5, s0\nmove64 s4, s0\nbr_on_null.move.loop s1, s3, s5, #1 → "
                 "@2\nreturn s3\n");
    }
    SECTION("br_on_non_null to a block and to a loop, the reference among the values")
    {
        auto const leaving =
            withBlockType(functionOf({ binary::I32, binary::FuncRef },
                                     {},
                                     {},
                                     code({ blockOf(0x02,
                                                    0x01,
                                                    code({ localGet(0),
                                                           localGet(0),
                                                           localGet(1),
                                                           { 0xD6, 0x00 },
                                                           localGet(1),
                                                           op(0x0C, 0) })),
                                            drop(),
                                            drop() })),
                          FuncType { .params = {}, .results = { binary::I32, binary::FuncRef } });
        CHECK(listingOf(leaving)
              == "move128 s5, s1\nmove64 s4, s0\nmove64 s3, s0\nbr_on_non_null.move s5, s3, s4, #3 → @17\n"
                 "move128 s5, s1\nmove.range s3, s4, #3\njump → @17\nreturn s3\n");
        auto const looping =
            withBlockType(functionOf({ binary::I32, binary::FuncRef },
                                     {},
                                     {},
                                     code({ localGet(0),
                                            localGet(1),
                                            blockOf(0x03,
                                                    0x01,
                                                    code({ localGet(0),
                                                           localGet(0),
                                                           localGet(1),
                                                           { 0xD6, 0x00 },
                                                           drop(),
                                                           drop(),
                                                           drop(),
                                                           drop() })) })),
                          FuncType { .params = { binary::I32, binary::FuncRef }, .results = {} });
        CHECK(listingOf(looping)
              == "move128 s4, s1\nmove64 s3, s0\nmove128 s8, s1\nmove64 s7, s0\nmove64 s6, s0\n"
                 "br_on_non_null.move.loop s8, s3, s7, #3 → @4\nreturn s3\n");
    }
}

TEST_CASE("Lowering.branchMovesItsValuesOnce")
{
    // (block (result i32 i32) (i32.const 7) (local.get 0) (local.get 1) (br 0)): the two values are
    // materialized, then moved down past the constant in one move.range, then the jump. The constant,
    // dead past the br, is never materialized.
    auto const module = withBlockType(
        functionOf({ binary::I32, binary::I32 },
                   {},
                   {},
                   code({ blockOf(0x02, 0x01, code({ i32Const(7), localGet(0), localGet(1), op(0x0C, 0) })),
                          drop(),
                          drop() })),
        FuncType { .params = {}, .results = { binary::I32, binary::I32 } });
    auto const listing = listingOf(module);
    CHECK(listing == "move64 s4, s1\nmove64 s3, s0\nmove.range s2, s3, #2\njump → @9\nreturn s2\n");
    CHECK(linesStartingWith(listing, "move.range") == 1);
}

TEST_CASE("Lowering.blockBoundariesMaterialize")
{
    // A loop's start follows the moves of the entries below it, so no iteration copies a local again
    // that the loop body changed; a block's end has its results in their slots.
    auto const counting = FuncType { .params = { binary::I32 }, .results = { binary::I32 } };
    auto const body = [](uint8_t opcode) {
        return code({ localGet(0), blockOf(opcode, 0x01, code({ i32Const(1), op(0x6A) })) });
    };
    CHECK(listingOf(withBlockType(functionOf({ binary::I32 }, { binary::I32 }, {}, body(0x02)), counting))
          == "move64 s1, s0\ni32.add.imm s1, s1, #1\nreturn s1\n");
    CHECK(listingOf(withBlockType(functionOf({ binary::I32 }, { binary::I32 }, {}, body(0x03)), counting))
          == "move64 s1, s0\ni32.add.imm s1, s1, #1\nreturn s1\n");
    SECTION("an if without else keeps its params as its results")
    {
        auto const unchanged = withBlockType(
            functionOf(
                { binary::I32 },
                { binary::I32 },
                {},
                code({ localGet(0), localGet(0), blockOf(0x04, 0x01, code({ i32Const(1), op(0x6A) })) })),
            counting);
        CHECK(listingOf(unchanged) == "move64 s1, s0\nif s0 → @8\ni32.add.imm s1, s1, #1\nreturn s1\n");
    }
    SECTION("an else starts from the if's params again")
    {
        auto const branches = withBlockType(functionOf({ binary::I32 },
                                                       { binary::I32 },
                                                       {},
                                                       code({ localGet(0),
                                                              localGet(0),
                                                              { 0x04, 0x01 },
                                                              i32Const(1),
                                                              op(0x6A),
                                                              op(0x05),
                                                              i32Const(2),
                                                              op(0x6A),
                                                              op(0x0B) })),
                                            counting);
        CHECK(listingOf(branches)
              == "move64 s1, s0\nif s0 → @10\ni32.add.imm s1, s1, #1\njump → @13\ni32.add.imm s1, s1, "
                 "#2\nreturn s1\n");
    }
    SECTION("a br_if to the function's body returns at its end")
    {
        auto const early =
            functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), localGet(0), op(0x0D, 0) }));
        CHECK(listingOf(early) == "move64 s1, s0\nbr_if s0 → @5\nreturn s1\n");
    }
}

TEST_CASE("Lowering.deadCodeAfterEveryTerminator")
{
    // Inside a block, each terminator and then code that would emit (const64, move64): what follows the
    // terminator up to the block's end emits nothing, so the listing is the one without it.
    struct Row
    {
        Opcode opcode;
        Bytes terminator;
    };
    auto const rows = std::to_array<Row>({
        { .opcode = Opcode::Unreachable, .terminator = unreachable() },
        { .opcode = Opcode::Br, .terminator = op(0x0C, 0) },
        { .opcode = Opcode::BrTable, .terminator = code({ localGet(0), { 0x0E, 0x00, 0x00 } }) },
        { .opcode = Opcode::Return, .terminator = op(0x0F) },
    });
    auto const dead = code({ i32Const(5), localSet(0), localGet(0), localSet(1) });
    for (auto const& row: rows)
    {
        INFO(binary::opcodeInfo(row.opcode).text);
        auto const with = functionOf(
            { binary::I32, binary::I32 }, {}, {}, blockOf(0x02, 0x40, code({ row.terminator, dead })));
        auto const without =
            functionOf({ binary::I32, binary::I32 }, {}, {}, blockOf(0x02, 0x40, row.terminator));
        CHECK(listingOf(with) == listingOf(without));
        CHECK(continuationAfter(row.opcode) == Continuation::Dead);
    }
    SECTION("the tail calls and the throws are terminators of the same table")
    {
        // return_call, return_call_indirect and return_call_ref, throw and throw_ref: lowered by their own
        // cases, and what follows them is skipped by this one table.
        for (auto const opcode: { Opcode::ReturnCall,
                                  Opcode::ReturnCallIndirect,
                                  Opcode::ReturnCallRef,
                                  Opcode::Throw,
                                  Opcode::ThrowRef })
        {
            INFO(binary::opcodeInfo(opcode).text);
            CHECK(continuationAfter(opcode) == Continuation::Dead);
        }
        CHECK(isoline::interpreter::Terminators.size() == rows.size() + 5);
    }
    SECTION("no other instruction is one")
    {
        for (auto const opcode:
             { Opcode::BrIf, Opcode::BrOnNull, Opcode::BrOnNonNull, Opcode::If, Opcode::Call })
            CHECK(continuationAfter(opcode) == Continuation::Next);
    }
    SECTION("a then branch's dead code ends at its else, and nested blocks in dead code are skipped")
    {
        auto const branches = functionOf({ binary::I32 },
                                         {},
                                         {},
                                         code({ localGet(0),
                                                { 0x04, 0x40 },
                                                op(0x0C, 0),
                                                blockOf(0x02, 0x40, code({ i32Const(5), localSet(0) })),
                                                i32Const(5),
                                                localSet(0),
                                                op(0x05),
                                                i32Const(6),
                                                localSet(0),
                                                op(0x0B) }));
        CHECK(listingOf(branches) == "if s0 → @5\njump → @8\nconst64 s0, #6\nreturn s1\n");
    }
}

TEST_CASE("Lowering.deepNestingIsIterative")
{
    // Under the memory caps: 100'000 nested blocks, loops and ifs, each with a br 0. A lowering that
    // recurses overflows its stack; one that walks the open blocks at each branch fails the work bound
    // already at the small depth.
    namespace hostile = isoline::interpreter::testing;
    for (auto const depth: { uint32_t { 1'000 }, uint32_t { 100'000 } })
    {
        DYNAMIC_SECTION("depth " << depth)
        {
            auto const blocks =
                loweredWithinTheBounds(hostile::nestedBlocks(depth, 0x02), ControlTemporaries);
            // A jump per block and the return.
            CHECK(blocks->functions()[0].code.size() == (std::size_t { depth } * 2) + 2);
            auto const loops = loweredWithinTheBounds(hostile::nestedBlocks(depth, 0x03), ControlTemporaries);
            CHECK(loops->functions()[0].code.size() == (std::size_t { depth } * 2) + 2);
            auto const ifs = loweredWithinTheBounds(hostile::nestedBlocks(depth, 0x04), ControlTemporaries);
            // An if and the jumps of both branches per if, and the return.
            CHECK(ifs->functions()[0].code.size() == (std::size_t { depth } * 7) + 2);
        }
    }
}

TEST_CASE("Lowering.multiValueBranchesStayLinear")
{
    // Under the memory caps: values carried by many branches, a br_table to many labels of 1'000 values,
    // and many blocks of 1'000 params re-entered after dead code. The code and the work stay within their
    // bounds per body unit: one move per branch, never one per value. Each shape has a small size, at
    // which a lowering per value already fails.
    namespace hostile = isoline::interpreter::testing;
    SECTION("1'000 values, carried by br_ifs")
    {
        // Not 1M br_ifs: the validator pops and pushes a label's 1'000 types at every br_if, as the spec's
        // algorithm does, which takes 2·10⁹ steps and seven minutes under ASan before lowering starts (it
        // passes, measured once). The code size is exact at both sizes, so one move per value fails at the
        // small one already.
        for (auto const branches: { uint32_t { 10'000 }, uint32_t { 100'000 } })
        {
            auto const module =
                loweredWithinTheBounds(hostile::branchesWithValues(1'000, branches), ControlTemporaries);
            // The constants once, the one below the values too, then per br_if its condition and one
            // moving branch, then the br's move and jump, and the return.
            CHECK(module->functions()[0].code.size()
                  == (std::size_t { 1'001 } * 3) + (std::size_t { branches } * 7) + 3 + 2 + 2);
        }
    }
    SECTION("1'000 values, carried by a br_table of 65'520 labels")
    {
        for (auto const labels: { uint32_t { 1'000 }, uint32_t { 65'520 } })
        {
            auto const module =
                loweredWithinTheBounds(hostile::branchTableWithValues(1'000, labels), ControlTemporaries);
            CHECK(module->functions()[0].code.size()
                  == (std::size_t { 1'000 } * 3) + 3 + 4
                         + ((std::size_t { labels } + 1)
                            * std::size_t { isoline::interpreter::BranchTableEntryWords })
                         + 2);
        }
    }
    SECTION("blocks and ifs of 1'000 params, re-entered after their dead code")
    {
        for (auto const count: { uint32_t { 1'000 }, uint32_t { 20'000 } })
        {
            std::ignore =
                loweredWithinTheBounds(hostile::reenteredBlocks(1'000, count, 0x02), ControlTemporaries);
            std::ignore =
                loweredWithinTheBounds(hostile::reenteredBlocks(1'000, count, 0x04), ControlTemporaries);
        }
    }
}

namespace
{

/// A function of @p locals i32 locals whose body forwards @p forwarded of them by local.gets, then sets each
/// of @p absent, which nothing forwards, in turn @p sets times, then drops what it forwarded.
[[nodiscard]] ModuleBuilder forwardingThenSetting(uint32_t locals,
                                                  std::vector<uint32_t> const& forwarded,
                                                  std::vector<uint32_t> const& absent,
                                                  uint32_t sets)
{
    auto body = Bytes {};
    auto const append = [&body](Bytes const& instruction) {
        body.insert(body.end(), instruction.begin(), instruction.end());
    };
    for (auto const local: forwarded)
        append(localGet(local));
    for (auto const index: std::views::iota(uint32_t { 0 }, sets))
    {
        append(i32Const(0));
        append(op(0x21, absent[index % absent.size()]));
    }
    body.insert(body.end(), forwarded.size(), 0x1A);
    return functionOf({}, {}, std::vector<ValueType>(locals, binary::I32), body);
}

/// How long lowering @p builder's module takes, its validation not counted.
[[nodiscard]] std::chrono::steady_clock::duration loweringTime(ModuleBuilder const& builder)
{
    auto const module = validated(builder);
    auto const started = std::chrono::steady_clock::now();
    auto const result = lower(*module, LoweringConfig {}, arenaOf());
    auto const took = std::chrono::steady_clock::now() - started;
    REQUIRE(result.has_value());
    CHECK((*result)->stats().work <= isoline::interpreter::WorkPerBodyUnit * (*result)->stats().bodyUnits);
    return took;
}

} // namespace

TEST_CASE("Lowering.forwardedLocalsCostTheSameHoweverChosen")
{
    // 50'000 locals, the decoder's most, of which 16'383 are forwarded by local.gets, then 150'000 local.sets
    // of locals nothing forwards: a 1 MB body. Hostile, the forwarded locals are those whose Fibonacci hash
    // falls in the first half of a 32'768-bucket table, which an open-addressing hash map of the forwarded
    // locals fills into one probe run, and the set locals hash into that run, so each set's look-up of its
    // chain walked the run: 10^9 probes. Spread, the same counts are every third local. Lowering's work does
    // not depend on which locals a body forwards, so both take about as long, measured on the same machine
    // (0.27 s each under ASan); the clustered hash map took 27 times as long (7.8 s against 0.29 s), and more
    // the larger the body.
    constexpr auto Locals = uint32_t { 50'000 };
    constexpr auto Forwarded = uint32_t { 16'383 };
    constexpr auto Sets = uint32_t { 150'000 };
    auto const home = [](uint32_t local) {
        return static_cast<uint32_t>((uint64_t { local } * 0x9E37'79B9'7F4A'7C15) >> 49U);
    };
    auto clustered = std::vector<uint32_t> {};
    auto clusteredAbsent = std::vector<uint32_t> {};
    auto spread = std::vector<uint32_t> {};
    auto spreadAbsent = std::vector<uint32_t> {};
    for (auto const local: std::views::iota(uint32_t { 0 }, Locals))
    {
        if (home(local) < 16'384)
            (clustered.size() < Forwarded ? clustered : clusteredAbsent).push_back(local);
        if (local % 3 == 0 && spread.size() < Forwarded)
            spread.push_back(local);
        else if (local % 3 == 1)
            spreadAbsent.push_back(local);
    }
    REQUIRE(clustered.size() == Forwarded);
    REQUIRE(spread.size() == Forwarded);
    REQUIRE(clusteredAbsent.size() < spreadAbsent.size());
    spreadAbsent.resize(clusteredAbsent.size());
    auto const hostile = loweringTime(forwardingThenSetting(Locals, clustered, clusteredAbsent, Sets));
    auto const control = loweringTime(forwardingThenSetting(Locals, spread, spreadAbsent, Sets));
    INFO("hostile " << std::chrono::duration<double>(hostile).count() << " s, control "
                    << std::chrono::duration<double>(control).count() << " s");
    CHECK(hostile < 3 * control);
}

// The contract tests below end the process at a failed Require, so they are hidden from the unit run:
// tests/CMakeLists.txt runs each in a process of its own and expects the Require's text
// (isoline::runtime::testing::endAtContractViolation()).

TEST_CASE("Contract.Lowering.dispatchMustBeAvailable", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto const module = validated(functionOf({}, {}, {}, {}));
    // One past the last Dispatch: no build compiles it, whether or not a probe row passed for TailCall.
    auto const uncompiled = static_cast<Dispatch>(std::to_underlying(Dispatch::TailCall) + 1);
    std::ignore = lower(*module, LoweringConfig { .dispatch = uncompiled }, arenaOf());
    FAIL("SURVIVED: lowering for a dispatch this build did not compile");
}

TEST_CASE("Contract.Listing.wholeOperations", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto resource = std::pmr::monotonic_buffer_resource {};
    auto function = isoline::interpreter::LoweredFunction(&resource);
    // An i32.add header without its operand words.
    function.code.push_back(std::to_underlying(isoline::interpreter::OperationId::I32Add));
    std::ignore = describe(function);
    FAIL("SURVIVED: a listing of a truncated operation");
}
