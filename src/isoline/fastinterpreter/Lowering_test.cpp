// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
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
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Listing.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/fastinterpreter/LoweringArena.hpp>
#include <isoline/fastinterpreter/LoweringTable.hpp>
#include <isoline/fastinterpreter/SlotCodec.hpp>
#include <isoline/fastinterpreter/testing/HostileModules.hpp>
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
using isoline::binary::OpcodeTable;
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
using isoline::fastinterpreter::bodyUnits;
using isoline::fastinterpreter::codeBoundWords;
using isoline::fastinterpreter::CountingArena;
using isoline::fastinterpreter::describe;
using isoline::fastinterpreter::Dispatch;
using isoline::fastinterpreter::FunctionOverheadWords;
using isoline::fastinterpreter::LocalInit;
using isoline::fastinterpreter::lower;
using isoline::fastinterpreter::LoweredModule;
using isoline::fastinterpreter::LoweringConfig;
using isoline::fastinterpreter::LoweringError;
using isoline::fastinterpreter::LoweringFailure;
using isoline::fastinterpreter::loweringReservation;
using isoline::fastinterpreter::loweringRow;
using isoline::fastinterpreter::LoweringRows;
using isoline::fastinterpreter::MaxFrameSlots;
using isoline::fastinterpreter::MaxWordsPerBodyUnit;
using isoline::fastinterpreter::NoFunction;
using isoline::fastinterpreter::nullSlots;
using isoline::fastinterpreter::OperationForm;
using isoline::fastinterpreter::operationRow;
using isoline::fastinterpreter::Word;
using isoline::fastinterpreter::WorkPerBodyUnit;
using isoline::runtime::ExecutionMode;
using isoline::validate::ValidatedModule;

namespace binary = isoline::binary;
namespace runtime = isoline::runtime;

namespace
{

/// The compile budget the lowering tests run under unless they test the budget: P4's default.
constexpr auto DefaultBudget = uint64_t { 256 } * 1024 * 1024;

/// An instruction of no immediates.
[[nodiscard]] Bytes op(uint8_t opcode)
{
    return { opcode };
}

/// An instruction of one u32 immediate.
[[nodiscard]] Bytes op(uint8_t opcode, uint32_t immediate)
{
    return code({ { opcode }, encodeU32(immediate) });
}

/// The bytes of a function of @p params to @p results, declaring @p locals, with body @p body.
[[nodiscard]] ModuleBuilder functionOf(std::vector<ValueType> params,
                                       std::vector<ValueType> results,
                                       std::vector<ValueType> locals,
                                       Bytes body)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = std::move(params), .results = std::move(results) });
    builder.function(0, std::move(locals), std::move(body));
    return builder;
}

/// @p builder's module, decoded and validated with every feature.
[[nodiscard]] std::shared_ptr<ValidatedModule const> validated(ModuleBuilder const& builder)
{
    auto result = ValidatedModule::load(builder.build(), FeatureSet::all(), DecodeLimits {});
    REQUIRE(result.has_value());
    return *result;
}

/// A counting arena of @p budget bytes over the global heap.
[[nodiscard]] std::shared_ptr<CountingArena> arenaOf(uint64_t budget = DefaultBudget)
{
    return std::make_shared<CountingArena>(budget, *std::pmr::new_delete_resource());
}

/// @p module lowered with @p config under the default budget.
[[nodiscard]] std::shared_ptr<LoweredModule const> lowered(
    std::shared_ptr<ValidatedModule const> const& module, LoweringConfig const& config = {})
{
    auto result = lower(*module, config, arenaOf());
    REQUIRE(result.has_value());
    return *result;
}

/// The listing of the first function of @p builder's module.
[[nodiscard]] std::string listingOf(ModuleBuilder const& builder, LoweringConfig const& config = {})
{
    auto const module = lowered(validated(builder), config);
    REQUIRE_FALSE(module->functions().empty());
    return describe(module->functions().front());
}

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

/// The value types of a signature's kinds.
[[nodiscard]] std::vector<ValueType> typesOf(std::span<ValueKind const> kinds)
{
    auto types = std::vector<ValueType> {};
    for (auto const kind: kinds)
        types.push_back(ValueType { .kind = kind });
    return types;
}

/// The binary encoding of @p opcode, with no immediates.
[[nodiscard]] Bytes encodingOf(Opcode opcode)
{
    auto const& info = binary::opcodeInfo(opcode);
    if (info.prefix == binary::NoPrefix)
        return { static_cast<uint8_t>(info.code) };
    return code({ { info.prefix }, encodeU32(info.code) });
}

/// @p builder's module lowered under the default budget, checked against every published bound: its
/// code per function and in all, its work, what the arena kept and its peak.
///
/// The budget is the module's reservations, room for the exact copy of the largest function's code (at most
/// its code bound), and @p temporaries, the allowance for the abstract stack, the forwarding chains and the
/// locals' runs: a lowering that allocates past that is refused, not merely measured.
[[nodiscard]] std::shared_ptr<LoweredModule const> loweredWithinTheBounds(ModuleBuilder const& builder,
                                                                          uint64_t temporaries)
{
    auto const module = validated(builder);
    auto const& source = module->module();
    auto budget = temporaries;
    auto largestCode = uint64_t { 0 };
    for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(source.code.size())))
    {
        budget += loweringReservation(source, index);
        largestCode = std::max(largestCode, codeBoundWords(source.code[index]) * sizeof(Word));
    }
    budget += largestCode;
    auto const arena = arenaOf(budget);
    auto const result = lower(*module, LoweringConfig {}, arena);
    if (!result)
        UNSCOPED_INFO("refused at " << result.error().codeIndex << ": " << result.error().requested
                                    << " bytes of " << result.error().limit << ", peak " << arena->peak());
    REQUIRE(result.has_value());
    auto const& lowered = **result;
    auto reservations = uint64_t { 0 };
    auto codeWords = uint64_t { 0 };
    for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(source.code.size())))
    {
        reservations += loweringReservation(source, index);
        codeWords += lowered.functions()[index].code.size();
        CHECK(lowered.functions()[index].code.size() <= codeBoundWords(source.code[index]));
    }
    auto const& stats = lowered.stats();
    CHECK(codeWords
          <= (stats.bodyUnits * MaxWordsPerBodyUnit) + (source.code.size() * FunctionOverheadWords));
    CHECK(lowered.loweredBytes() <= reservations);
    CHECK(stats.work <= WorkPerBodyUnit * stats.bodyUnits);
    CHECK(arena->peak() <= arena->budget());
    return *result;
}

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
        CHECK(listingOf(sum) == "i32.add s2, s0, s1\nmove64 s1, s2\nreturn s2\n");
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
            auto const value = isoline::fastinterpreter::loadValue(type, slots);
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
    CHECK(result.error().codeIndex == 0);
    CHECK(arena->charged() == 0);
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
    namespace hostile = isoline::fastinterpreter::testing;
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

TEST_CASE("LoweringTable.everyTableRowLowersToItsOperation")
{
    // One function per Unary, Binary and BinaryImmediate row, over params of its operand types: the
    // listing names the row's operation, and a folding row's twin when its right operand is a constant.
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
        auto const constant = params[1].kind == ValueKind::I32 ? i32Const(5) : code({ { 0x42 }, { 0x05 } });
        CHECK(listingOf(
                  functionOf(params, results, {}, code({ localGet(0), constant, encodingOf(row.opcode) })))
              == std::format("{} s2, s0, #5\nreturn s2\n", operationRow(row.immediate).text));
    }
    // An opcode without a row is Special, which a later part of lowering handles.
    CHECK(loweringRow(Opcode::Block).form == OperationForm::Special);
    CHECK(OpcodeTable.size() == isoline::fastinterpreter::LoweringTable.size());
}

// The contract tests below end the process at a failed Require, so they are hidden from the unit run:
// tests/CMakeLists.txt runs each in a process of its own and expects the Require's text
// (isoline::runtime::testing::endAtContractViolation()).

TEST_CASE("Contract.Lowering.dispatchMustBeAvailable", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto const module = validated(functionOf({}, {}, {}, {}));
    // No build of P4's Task 6 compiles TailCall; Task 8 adds the probe that may.
    std::ignore = lower(*module, LoweringConfig { .dispatch = Dispatch::TailCall }, arenaOf());
    FAIL("SURVIVED: lowering for a dispatch this build did not compile");
}

TEST_CASE("Contract.Listing.wholeOperations", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto resource = std::pmr::monotonic_buffer_resource {};
    auto function = isoline::fastinterpreter::LoweredFunction(&resource);
    // An i32.add header without its operand words.
    function.code.push_back(std::to_underlying(isoline::fastinterpreter::OperationId::I32Add));
    std::ignore = describe(function);
    FAIL("SURVIVED: a listing of a truncated operation");
}
