// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <cfenv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/FastInterpreter.hpp>
#include <isoline/fastinterpreter/Listing.hpp>
#include <isoline/fastinterpreter/LoweringTable.hpp>
#include <isoline/fastinterpreter/testing/Engines.hpp>
#include <isoline/fastinterpreter/testing/Lowering.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>
#include <isoline/runtime/testing/FloatEnvironment.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

using isoline::binary::AbstractHeapType;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::HeapType;
using isoline::binary::Mutability;
using isoline::binary::Opcode;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;
using isoline::binary::testing::br;
using isoline::binary::testing::brIf;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::drop;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::i64Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::localSet;
using isoline::binary::testing::ModuleBuilder;
using isoline::binary::testing::unreachable;
using isoline::fastinterpreter::availableDispatches;
using isoline::fastinterpreter::describe;
using isoline::fastinterpreter::FusionTable;
using isoline::fastinterpreter::LoweredFunction;
using isoline::fastinterpreter::LoweredModule;
using isoline::fastinterpreter::LoweringRows;
using isoline::fastinterpreter::MinimumChunkSlots;
using isoline::fastinterpreter::OperationForm;
using isoline::fastinterpreter::OperationId;
using isoline::fastinterpreter::OperationTable;
using isoline::fastinterpreter::testing::encodingOf;
using isoline::fastinterpreter::testing::EngineUnderTest;
using isoline::fastinterpreter::testing::fast;
using isoline::fastinterpreter::testing::functionOf;
using isoline::fastinterpreter::testing::Harness;
using isoline::fastinterpreter::testing::op;
using isoline::fastinterpreter::testing::operationsOf;
using isoline::fastinterpreter::testing::Reference;
using isoline::fastinterpreter::testing::typesOf;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Limit;
using isoline::runtime::PollSite;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::StoreLimits;
using isoline::runtime::TrapCode;
using isoline::runtime::V128;
using isoline::runtime::Value;
using isoline::runtime::testing::PollCounter;

namespace binary = isoline::binary;

namespace
{

/// A block, loop or if of the encoded @p blockType around @p body.
[[nodiscard]] Bytes blockOf(uint8_t opcode, Bytes const& blockType, Bytes const& body)
{
    return code({ { opcode }, blockType, body, { 0x0B } });
}

/// The block type naming no values.
[[nodiscard]] Bytes noValues()
{
    return { 0x40 };
}

/// The block type naming type index @p index (small indices only: one LEB128 byte).
[[nodiscard]] Bytes typeIndex(uint8_t index)
{
    return { index };
}

/// br_table over @p labels, the last the default.
[[nodiscard]] Bytes brTable(std::vector<uint8_t> const& labels)
{
    auto bytes = Bytes { 0x0E, static_cast<uint8_t>(labels.size() - 1) };
    bytes.insert(bytes.end(), labels.begin(), labels.end());
    return bytes;
}

[[nodiscard]] Value i32(int32_t value)
{
    return I32 { static_cast<uint32_t>(value) };
}

[[nodiscard]] Value i64(int64_t value)
{
    return I64 { static_cast<uint64_t>(value) };
}

/// The null funcref.
[[nodiscard]] Value nullFunc()
{
    return Ref { .kind = RefKind::Null, .heap = { .kind = AbstractHeapType::Func }, .address = 0 };
}

/// The funcref of @p harness's function 0.
[[nodiscard]] Value funcRefOf(Harness& harness)
{
    return Ref { .kind = RefKind::Func,
                 .heap = { .kind = AbstractHeapType::Func },
                 .address = harness.function(0) };
}

/// The fast engine under every available dispatch, one per run of the test case.
[[nodiscard]] EngineUnderTest anyFastEngine()
{
    return fast(GENERATE(from_range(availableDispatches())));
}

// --- The programs ----------------------------------------------------------------------------------------

/// (i64) -> i64: n!, by a loop.
[[nodiscard]] ModuleBuilder factorial()
{
    return functionOf({ binary::I64 },
                      { binary::I64 },
                      { binary::I64 },
                      code({ i64Const(1),
                             localSet(1),
                             blockOf(0x02,
                                     noValues(),
                                     blockOf(0x03,
                                             noValues(),
                                             code({ localGet(0),
                                                    op(0x50),
                                                    brIf(1),
                                                    localGet(1),
                                                    localGet(0),
                                                    op(0x7E),
                                                    localSet(1),
                                                    localGet(0),
                                                    i64Const(1),
                                                    op(0x7D),
                                                    localSet(0),
                                                    br(0) }))),
                             localGet(1) }));
}

/// (i32) -> i64: the n-th Fibonacci number, by a loop.
[[nodiscard]] ModuleBuilder fibonacci()
{
    return functionOf({ binary::I32 },
                      { binary::I64 },
                      { binary::I64, binary::I64, binary::I64 },
                      code({ i64Const(1),
                             localSet(2),
                             blockOf(0x02,
                                     noValues(),
                                     blockOf(0x03,
                                             noValues(),
                                             code({ localGet(0),
                                                    op(0x45),
                                                    brIf(1),
                                                    localGet(1),
                                                    localGet(2),
                                                    op(0x7C),
                                                    localSet(3),
                                                    localGet(2),
                                                    localSet(1),
                                                    localGet(3),
                                                    localSet(2),
                                                    localGet(0),
                                                    i32Const(1),
                                                    op(0x6B),
                                                    localSet(0),
                                                    br(0) }))),
                             localGet(1) }));
}

/// A count-down loop of one shape: its module, and the argument that makes it take 1'000 back-edges.
struct CountDown
{
    std::string name;     ///< What transfers control back to the loop.
    ModuleBuilder module; ///< (i32) -> i32, returning what is left of the count.
    int32_t argument;     ///< The count that takes 1'000 back-edges.
};

/// Every shape of loop back-edge the lowering emits: each must poll, once per iteration.
[[nodiscard]] std::vector<CountDown> countDowns()
{
    auto shapes = std::vector<CountDown> {};
    // jump.loop: br 0 at the loop's end, the exit a fused i32.eqz.br_if to the block.
    shapes.push_back({ .name = "jump.loop",
                       .module = functionOf({ binary::I32 },
                                            { binary::I32 },
                                            {},
                                            code({ blockOf(0x02,
                                                           noValues(),
                                                           blockOf(0x03,
                                                                   noValues(),
                                                                   code({ localGet(0),
                                                                          op(0x45),
                                                                          brIf(1),
                                                                          localGet(0),
                                                                          i32Const(1),
                                                                          op(0x6B),
                                                                          localSet(0),
                                                                          br(0) }))),
                                                   localGet(0) })),
                       .argument = 1'000 });
    // br_if.loop: the condition a local.tee's result.
    shapes.push_back({ .name = "br_if.loop",
                       .module = functionOf(
                           { binary::I32 },
                           { binary::I32 },
                           {},
                           code({ blockOf(0x03,
                                          noValues(),
                                          code({ localGet(0), i32Const(1), op(0x6B), op(0x22, 0), brIf(0) })),
                                  localGet(0) })),
                       .argument = 1'001 });
    // i32.gt_s.imm.br_if.loop: a comparison with a constant, fused with the br_if.
    shapes.push_back({ .name = "i32.gt_s.imm.br_if.loop",
                       .module = functionOf({ binary::I32 },
                                            { binary::I32 },
                                            {},
                                            code({ blockOf(0x03,
                                                           noValues(),
                                                           code({ localGet(0),
                                                                  i32Const(1),
                                                                  op(0x6B),
                                                                  localSet(0),
                                                                  localGet(0),
                                                                  i32Const(0),
                                                                  op(0x4A),
                                                                  brIf(0) })),
                                                   localGet(0) })),
                       .argument = 1'001 });
    // br_table: label 0 is the loop's, the default the block's.
    shapes.push_back({ .name = "br_table to a loop",
                       .module = functionOf({ binary::I32 },
                                            { binary::I32 },
                                            {},
                                            code({ blockOf(0x02,
                                                           noValues(),
                                                           blockOf(0x03,
                                                                   noValues(),
                                                                   code({ localGet(0),
                                                                          i32Const(1),
                                                                          op(0x6B),
                                                                          op(0x22, 0),
                                                                          op(0x45),
                                                                          brTable({ 0, 1 }) }))),
                                                   localGet(0) })),
                       .argument = 1'001 });
    // br_if.move.loop: the loop's param travels from above a value the branch leaves behind.
    auto moving = ModuleBuilder {};
    moving.type(FuncType { .params = { binary::I32 }, .results = { binary::I32 } });
    moving.type(FuncType { .params = { binary::I32 }, .results = {} });
    moving.function(0,
                    { binary::I32 },
                    code({ localGet(0),
                           blockOf(0x03,
                                   typeIndex(1),
                                   code({ i32Const(1),
                                          op(0x6B),
                                          op(0x22, 1),
                                          localGet(1),
                                          localGet(1),
                                          brIf(0),
                                          drop(),
                                          drop() })),
                           localGet(1) }));
    shapes.push_back({ .name = "br_if.move.loop", .module = std::move(moving), .argument = 1'001 });
    return shapes;
}

/// The listing of the fast engine's lowering of @p harness's function 0.
[[nodiscard]] std::string listingOf(Harness const& harness)
{
    REQUIRE(harness.code() != nullptr);
    return describe(harness.code()->functions().front());
}

} // namespace

TEST_CASE("FastInterpreter.runsArithmeticAndLoops")
{
    auto const engine = anyFastEngine();
    CHECK(Harness { factorial(), engine }.result(0, { i64(10) }) == i64(3'628'800));
    CHECK(Harness { fibonacci(), engine }.result(0, { i32(40) }) == i64(102'334'155));
    CHECK(Harness { factorial(), engine }.result(0, { i64(0) }) == i64(1));
}

TEST_CASE("FastInterpreter.pollsLikeTheReference")
{
    auto const engine = anyFastEngine();
    for (auto const& shape: countDowns())
    {
        INFO(shape.name);
        auto referenceCounter = PollCounter {};
        auto fastCounter = PollCounter {};
        auto reference = Harness { shape.module, Reference, {}, &referenceCounter };
        auto fastHarness = Harness { shape.module, engine, {}, &fastCounter };
        CHECK(listingOf(fastHarness).contains(shape.name.starts_with("br_table") ? "br_table" : shape.name));
        CHECK(reference.result(0, { i32(shape.argument) }) == fastHarness.result(0, { i32(shape.argument) }));
        for (auto const& row: isoline::runtime::PollSiteTable)
        {
            INFO(row.name);
            CHECK(fastCounter.count(row.site) == referenceCounter.count(row.site));
        }
        CHECK(fastCounter.count(PollSite::LoopBackEdge) == 1'000);
        CHECK(fastCounter.count(PollSite::FunctionEntry) == 1);
    }
}

TEST_CASE("FastInterpreter.stepLimitStopsAtTheSamePoll")
{
    auto const engine = anyFastEngine();
    auto const limits = StoreLimits { .maxSteps = 500 };
    for (auto const& shape: countDowns())
    {
        INFO(shape.name);
        {
            // Unobserved: the countdown is the executor's alone until it commits.
            auto reference = Harness { shape.module, Reference, limits };
            auto fastHarness = Harness { shape.module, engine, limits };
            CHECK(reference.trap(0, { i32(shape.argument) }) == TrapCode::StepLimitExceeded);
            CHECK(fastHarness.trap(0, { i32(shape.argument) }) == TrapCode::StepLimitExceeded);
            CHECK(fastHarness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
            CHECK(fastHarness.store().used(Limit::Steps) == 500);
        }
        {
            // Observed: every poll goes through the store, which checks the countdown is its own.
            auto referenceCounter = PollCounter {};
            auto fastCounter = PollCounter {};
            auto reference = Harness { shape.module, Reference, limits, &referenceCounter };
            auto fastHarness = Harness { shape.module, engine, limits, &fastCounter };
            CHECK(reference.trap(0, { i32(shape.argument) }) == TrapCode::StepLimitExceeded);
            CHECK(fastHarness.trap(0, { i32(shape.argument) }) == TrapCode::StepLimitExceeded);
            CHECK(fastHarness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
            CHECK(fastCounter.count(PollSite::LoopBackEdge)
                  == referenceCounter.count(PollSite::LoopBackEdge));
        }
    }
}

TEST_CASE("FastInterpreter.commitsTheStepsOnEveryWayOut")
{
    // On a store without an observer the countdown lives in the executor alone: what the store counts is
    // what the executor committed on its way out, by a return, a trap in code, or a trap at the entry.
    auto const engine = anyFastEngine();
    SECTION("a return, after every loop shape")
    {
        for (auto const& shape: countDowns())
        {
            INFO(shape.name);
            auto reference = Harness { shape.module, Reference };
            auto fastHarness = Harness { shape.module, engine };
            CHECK(fastHarness.result(0, { i32(shape.argument) })
                  == reference.result(0, { i32(shape.argument) }));
            CHECK(fastHarness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
            CHECK(fastHarness.store().used(Limit::Steps) == 1'001);
        }
    }
    SECTION("a trap in code, after a loop")
    {
        auto const shape = countDowns().front();
        auto module = functionOf({ binary::I32 },
                                 {},
                                 {},
                                 code({ blockOf(0x02,
                                                noValues(),
                                                blockOf(0x03,
                                                        noValues(),
                                                        code({ localGet(0),
                                                               op(0x45),
                                                               brIf(1),
                                                               localGet(0),
                                                               i32Const(1),
                                                               op(0x6B),
                                                               localSet(0),
                                                               br(0) }))),
                                        unreachable() }));
        auto reference = Harness { module, Reference };
        auto fastHarness = Harness { module, engine };
        CHECK(reference.trap(0, { i32(shape.argument) }) == TrapCode::Unreachable);
        CHECK(fastHarness.trap(0, { i32(shape.argument) }) == TrapCode::Unreachable);
        CHECK(fastHarness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
        CHECK(fastHarness.store().used(Limit::Steps) == 1'001);
    }
    SECTION("a trap at the entry, after its poll")
    {
        auto const limits = StoreLimits { .maxCallDepth = 0 };
        auto reference = Harness { factorial(), Reference, limits };
        auto fastHarness = Harness { factorial(), engine, limits };
        CHECK(reference.trap(0, { i64(3) }) == TrapCode::CallStackExhausted);
        CHECK(fastHarness.trap(0, { i64(3) }) == TrapCode::CallStackExhausted);
        CHECK(fastHarness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
        CHECK(fastHarness.store().used(Limit::Steps) == 1);
    }
}

namespace
{

/// Interrupts a store from a thread of its own after a delay, and joins that thread however the test ends,
/// a failed REQUIRE's exception included.
class InterruptAfter
{
  public:
    /// Starts the thread, which interrupts @p interrupter after @p delay.
    InterruptAfter(std::shared_ptr<isoline::runtime::Interrupter> interrupter,
                   std::chrono::milliseconds delay):
        _thread { [interrupter = std::move(interrupter), delay] {
            std::this_thread::sleep_for(delay);
            interrupter->interrupt();
        } }
    {
    }

    InterruptAfter(InterruptAfter const&) = delete;
    InterruptAfter(InterruptAfter&&) = delete;
    InterruptAfter& operator=(InterruptAfter const&) = delete;
    InterruptAfter& operator=(InterruptAfter&&) = delete;

    ~InterruptAfter() { _thread.join(); }

  private:
    std::thread _thread;
};

} // namespace

TEST_CASE("FastInterpreter.interruptStopsASpin", "[threading]")
{
    // (loop (br 0)) on a store without an observer, so that the poll's own fast path sees the interrupt. The
    // interrupt is the only thing that stops a spinning guest, so the bound on a broken one is its step
    // budget: 10^8 steps take a few seconds under ASan, far longer than the 5 ms the interrupt comes after,
    // and a guest that ran out of them ends StepLimitExceeded, which fails the test.
    auto const engine = anyFastEngine();
    auto harness = Harness { functionOf({}, {}, {}, blockOf(0x03, noValues(), br(0))),
                             engine,
                             StoreLimits { .maxSteps = 100'000'000 } };
    auto const interrupt = InterruptAfter { harness.store().interrupter(), std::chrono::milliseconds { 5 } };
    CHECK(harness.trap(0) == TrapCode::Interrupted);
}

TEST_CASE("FastInterpreter.floatEnvironmentIsPinned")
{
    auto const engine = anyFastEngine();
    auto harness = Harness { functionOf({ binary::F32, binary::F32 },
                                        { binary::F32 },
                                        {},
                                        code({ localGet(0), localGet(1), op(0x92) })),
                             engine };
    auto const environment = isoline::runtime::testing::HostFloatEnvironment {};
    isoline::runtime::testing::departFromIeeeDefaults();
    // 1 + 2^-24 is a tie that rounds to 1 to nearest, and to the next float upward.
    CHECK(harness.result(0, { F32 { 0x3F80'0000 }, F32 { 0x3380'0000 } }) == Value { F32 { 0x3F80'0000 } });
    CHECK(std::fegetround() == FE_UPWARD);
}

TEST_CASE("FastInterpreter.runsAFrameOverSixtyFiveThousandSlots")
{
    // 35'000 v128 locals take two slots each, within the decoder's 50'000 locals: the three i64 locals after
    // them sit past slot 70'000, which no 16-bit slot index names.
    auto const engine = anyFastEngine();
    constexpr auto Vectors = uint32_t { 35'000 };
    auto locals = std::vector<ValueType>(Vectors, binary::V128);
    locals.insert(locals.end(), 3, binary::I64);
    auto const last = Vectors + 2;
    auto module = functionOf({},
                             { binary::I64 },
                             std::move(locals),
                             code({ i64Const(5),
                                    localSet(last),
                                    i64Const(7),
                                    localSet(last - 1),
                                    localGet(last),
                                    localGet(last - 1),
                                    op(0x7C),
                                    localGet(last - 2),
                                    op(0x7C) }));
    auto reference = Harness { module, Reference };
    auto harness = Harness { module, engine };
    CHECK(harness.code()->functions().front().localSlots > 70'000);
    CHECK(harness.code()->functions().front().frameSlots > MinimumChunkSlots);
    CHECK(harness.result(0) == i64(12));
    CHECK(reference.result(0) == i64(12));
}

TEST_CASE("FastInterpreter.entryChargesAndReleases")
{
    auto const engine = anyFastEngine();
    SECTION("a return")
    {
        auto harness = Harness { factorial(), engine };
        CHECK(harness.result(0, { i64(5) }) == i64(120));
        CHECK(harness.store().chargedStackValues() == 0);
    }
    SECTION("a trap in code")
    {
        auto harness = Harness { functionOf({}, {}, {}, unreachable()), engine };
        CHECK(harness.trap(0) == TrapCode::Unreachable);
        CHECK(harness.store().chargedStackValues() == 0);
        auto divides = Harness {
            functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), i32Const(0), op(0x6D) })),
            engine
        };
        CHECK(divides.trap(0, { i32(7) }) == TrapCode::IntegerDivideByZero);
        CHECK(divides.store().chargedStackValues() == 0);
    }
    SECTION("a trap at the entry: the depth, then the stack charge")
    {
        for (auto const& limits: { StoreLimits { .maxCallDepth = 0 }, StoreLimits { .maxStackValues = 1 } })
        {
            auto reference = Harness { factorial(), Reference, limits };
            auto harness = Harness { factorial(), engine, limits };
            CHECK(harness.trap(0, { i64(5) }) == reference.trap(0, { i64(5) }));
            CHECK(harness.store().chargedStackValues() == 0);
        }
    }
}

namespace
{

/// Counts the allocations it passes on to the global heap.
class CountingResource final: public std::pmr::memory_resource
{
  public:
    /// The allocations so far.
    [[nodiscard]] uint64_t allocations() const noexcept { return _allocations; }

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
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

    uint64_t _allocations = 0;
};

} // namespace

TEST_CASE("FastInterpreter.entryAllocatesOnlyOnceAdmitted")
{
    // The frame's slots are reserved after the FunctionEntry poll, the depth check and the charge: a frame
    // any of them refuses allocates nothing, as the reference's refuses before its stacks grow. (An outermost
    // invocation's step budget is fresh, and instantiation's own poll refuses a budget of none: a
    // step-exhausted entry is a call's, which comes with calls.)
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto const refusals = std::vector<std::pair<StoreLimits, TrapCode>> {
        { StoreLimits { .maxCallDepth = 0 }, TrapCode::CallStackExhausted },
        { StoreLimits { .maxStackValues = 1 }, TrapCode::CallStackExhausted },
    };
    for (auto const& [limits, code]: refusals)
    {
        auto memory = CountingResource {};
        auto harness =
            Harness { factorial(), EngineUnderTest { .dispatch = dispatch, .slotMemory = &memory }, limits };
        CHECK(harness.trap(0, { i64(3) }) == code);
        CHECK(memory.allocations() == 0);
    }
    {
        auto memory = CountingResource {};
        auto harness =
            Harness { factorial(), EngineUnderTest { .dispatch = dispatch, .slotMemory = &memory } };
        harness.store().interrupter()->interrupt();
        CHECK(harness.trap(0, { i64(3) }) == TrapCode::Interrupted);
        CHECK(memory.allocations() == 0);
    }
    auto memory = CountingResource {};
    auto harness = Harness { factorial(), EngineUnderTest { .dispatch = dispatch, .slotMemory = &memory } };
    CHECK(harness.result(0, { i64(3) }) == i64(6));
    CHECK(memory.allocations() == 1);
}

TEST_CASE("FastInterpreter.runsTheOperationsNoLoweringEmitsYet")
{
    // nop lowers to nothing and v128.const comes with the vector operations, so code written by hand runs
    // them: (param v128) (result v128), its body nop, const128 s2 #lanes, return s2.
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto const byHand = [dispatch](LoweredModule const& lowered) {
        auto arena = std::make_shared<std::pmr::monotonic_buffer_resource>();
        auto functions = std::pmr::vector<LoweredFunction>(arena.get());
        auto const& model = lowered.functions().front();
        auto& function = functions.emplace_back(arena.get());
        function.codeIndex = model.codeIndex;
        function.frameCost = model.frameCost;
        function.paramSlots = model.paramSlots;
        function.localSlots = model.localSlots;
        function.frameSlots = model.frameSlots;
        function.resultSlots = model.resultSlots;
        REQUIRE(function.frameSlots >= 4);
        function.code = { isoline::fastinterpreter::headerWord(dispatch, OperationId::Nop),
                          isoline::fastinterpreter::headerWord(dispatch, OperationId::Const128),
                          2,
                          0x1111,
                          0x2222,
                          isoline::fastinterpreter::headerWord(dispatch, OperationId::Return),
                          2 };
        return std::make_shared<LoweredModule const>(std::move(arena),
                                                     std::move(functions),
                                                     dispatch,
                                                     lowered.mode(),
                                                     0,
                                                     isoline::fastinterpreter::LoweringStats {});
    };
    auto harness =
        Harness { functionOf({ binary::V128 }, { binary::V128 }, {}, localGet(0)), fast(dispatch), byHand };
    CHECK(harness.result(0, { V128 { .lanes = { 7, 8 } } })
          == Value { V128 { .lanes = { 0x1111, 0x2222 } } });
}

TEST_CASE("FastInterpreter.runsTheDispatchItWasMadeFor")
{
    for (auto const dispatch: availableDispatches())
    {
        CHECK(isoline::fastinterpreter::FastInterpreter { dispatch }.dispatch() == dispatch);
        CHECK(isoline::fastinterpreter::FastInterpreter { dispatch }.engine()
              == isoline::runtime::ExecutionEngine::Fast);
    }
}

TEST_CASE("FastInterpreter.nullLocalsNameTheRunningInstancesTypes")
{
    // Type 1 repeats type 0, so the store interns both as one TypeId and type index 1 is not its TypeId: a
    // (ref null 1) local starts as the null of that TypeId, which a code shared by every store cannot bake
    // in.
    auto const engine = anyFastEngine();
    auto const concrete =
        ValueType { .kind = ValueKind::Ref,
                    .heap = HeapType { .kind = AbstractHeapType::Concrete, .typeIndex = 1 } };
    auto module = ModuleBuilder {};
    module.type(FuncType {});
    module.type(FuncType {});
    module.type(FuncType { .params = {}, .results = { concrete } });
    module.function(2, { concrete }, localGet(0));
    auto reference = Harness { module, Reference };
    auto harness = Harness { module, engine };
    auto const typeId = harness.instance().types.at(1);
    REQUIRE(typeId != 1);
    auto const result = harness.result(0);
    CHECK(result == reference.result(0));
    CHECK(result
          == Value { Ref { .kind = RefKind::Null,
                           .heap = HeapType { .kind = AbstractHeapType::Concrete, .typeIndex = typeId },
                           .address = 0 } });
}

TEST_CASE("FastInterpreter.brTableTakesTheDefaultPastTheLabels")
{
    auto const engine = anyFastEngine();
    SECTION("every index past the labels, up to the largest")
    {
        // block $2 (block $1 (block $0 (br_table $0 $1 $2 (local.get 0))) 100 return) 101 return) 102
        auto const body = code(
            { blockOf(
                  0x02,
                  noValues(),
                  code(
                      { blockOf(0x02,
                                noValues(),
                                code({ blockOf(0x02, noValues(), code({ localGet(0), brTable({ 0, 1, 2 }) })),
                                       i32Const(100),
                                       op(0x0F) })),
                        i32Const(101),
                        op(0x0F) })),
              i32Const(102) });
        auto const module = functionOf({ binary::I32 }, { binary::I32 }, {}, body);
        auto reference = Harness { module, Reference };
        auto harness = Harness { module, engine };
        auto const expected = std::vector<std::pair<uint32_t, int32_t>> {
            { 0, 100 }, { 1, 101 }, { 2, 102 }, { 3, 102 }, { std::numeric_limits<uint32_t>::max(), 102 },
        };
        for (auto const& [index, result]: expected)
        {
            INFO("index " << index);
            CHECK(harness.result(0, { I32 { index } }) == i32(result));
            CHECK(reference.result(0, { I32 { index } }) == i32(result));
        }
    }
    SECTION("each label's values in its own slots")
    {
        // block (result i64) (block (result i64) 99 7 (br_table 0 1 (local.get 0))) 1 i64.add): the branch
        // leaves 99 behind, so the 7 moves down to whichever label it goes to.
        auto const module = functionOf(
            { binary::I32 },
            { binary::I64 },
            {},
            blockOf(0x02,
                    { 0x7E },
                    code({ blockOf(0x02,
                                   { 0x7E },
                                   code({ i64Const(99), i64Const(7), localGet(0), brTable({ 0, 1 }) })),
                           i64Const(1),
                           op(0x7C) })));
        auto reference = Harness { module, Reference };
        auto harness = Harness { module, engine };
        for (auto const& [index, result]: std::vector<std::pair<uint32_t, int64_t>> {
                 { 0, 8 }, { 1, 7 }, { 2, 7 }, { std::numeric_limits<uint32_t>::max(), 7 } })
        {
            INFO("index " << index);
            CHECK(harness.result(0, { I32 { index } }) == i64(result));
            CHECK(reference.result(0, { I32 { index } }) == i64(result));
        }
    }
}

TEST_CASE("FastInterpreter.moveRangeCopiesOverlappingSlots")
{
    // block (result i64 i64 i64) 1 10 20 30 br 0: the three values move down one slot, over themselves.
    auto const engine = anyFastEngine();
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = {}, .results = { binary::I64, binary::I64, binary::I64 } });
    module.function(
        0,
        {},
        blockOf(0x02, typeIndex(0), code({ i64Const(1), i64Const(10), i64Const(20), i64Const(30), br(0) })));
    auto harness = Harness { module, engine };
    CHECK(listingOf(harness).contains("move.range s0, s1, #3"));
    auto const results = harness.outcome(0);
    REQUIRE(results.has_value());
    CHECK(*results == std::vector<Value> { i64(10), i64(20), i64(30) });
}

TEST_CASE("FastInterpreter.immediateComparesReadThirtyTwoBits")
{
    // An i32 immediate is stored zero-extended: -1 is 0xFFFF'FFFF, which compares as -1 only when read as 32
    // bits. As 64 bits, 1 < -1 would hold.
    auto const engine = anyFastEngine();
    auto const compared =
        functionOf({ binary::I32 }, { binary::I32 }, {}, code({ localGet(0), i32Const(-1), op(0x48) }));
    auto const fused = functionOf(
        { binary::I32 },
        { binary::I32 },
        {},
        code({ blockOf(0x02,
                       noValues(),
                       code({ localGet(0), i32Const(-1), op(0x48), brIf(0), i32Const(0), op(0x0F) })),
               i32Const(1) }));
    auto const added = functionOf(
        { binary::I32 }, { binary::I64 }, {}, code({ localGet(0), i32Const(-1), op(0x6A), op(0xAD) }));
    auto comparedHarness = Harness { compared, engine };
    auto fusedHarness = Harness { fused, engine };
    auto addedHarness = Harness { added, engine };
    CHECK(listingOf(comparedHarness).contains("i32.lt_s.imm s1, s0, #4294967295"));
    CHECK(listingOf(fusedHarness).contains("i32.lt_s.imm.br_if s0, #4294967295"));
    CHECK(comparedHarness.result(0, { i32(1) }) == i32(0));
    CHECK(comparedHarness.result(0, { i32(-5) }) == i32(1));
    CHECK(fusedHarness.result(0, { i32(1) }) == i32(0));
    CHECK(fusedHarness.result(0, { i32(-5) }) == i32(1));
    // 5 + -1 is 4, its upper 32 bits zero, as i64.extend_i32_u shows.
    CHECK(addedHarness.result(0, { i32(5) }) == i64(4));
}

namespace
{

/// Sample operands of each number kind: zeros, ones, extremes, a NaN, infinities and values just past the
/// integer ranges, so that every kernel's traps and edge cases run.
[[nodiscard]] std::vector<Value> samplesOf(ValueKind kind)
{
    switch (kind)
    {
        case ValueKind::I32:
            return { i32(0),
                     i32(1),
                     i32(7),
                     i32(31),
                     i32(-1),
                     i32(std::numeric_limits<int32_t>::min()),
                     i32(std::numeric_limits<int32_t>::max()) };
        case ValueKind::I64:
            return { i64(0),
                     i64(1),
                     i64(63),
                     i64(-1),
                     i64(std::numeric_limits<int64_t>::min()),
                     i64(std::numeric_limits<int64_t>::max()),
                     i64(0x1'0000'0001) };
        case ValueKind::F32:
            return { F32 { 0 },           F32 { 0x8000'0000 }, F32 { 0x3FC0'0000 }, F32 { 0xC020'0000 },
                     F32 { 0x7FC0'0000 }, F32 { 0x7F80'0000 }, F32 { 0x4F00'0000 }, F32 { 0x5F80'0000 } };
        case ValueKind::F64:
            return { F64 { 0 },
                     F64 { 0x8000'0000'0000'0000 },
                     F64 { 0x3FF8'0000'0000'0000 },
                     F64 { 0xC004'0000'0000'0000 },
                     F64 { 0x7FF8'0000'0000'0000 },
                     F64 { 0x7FF0'0000'0000'0000 },
                     F64 { 0x41E0'0000'0000'0000 },
                     F64 { 0x43F0'0000'0000'0000 } };
        case ValueKind::V128:
        case ValueKind::Ref: break;
    }
    return {};
}

/// The constants a folding row's right operand is tried with, as i32.const or i64.const of @p kind.
[[nodiscard]] std::vector<std::pair<Bytes, Value>> constantsOf(ValueKind kind)
{
    auto constants = std::vector<std::pair<Bytes, Value>> {};
    for (auto const value: { int64_t { 0 },
                             int64_t { 1 },
                             int64_t { 31 },
                             int64_t { -1 },
                             int64_t { std::numeric_limits<int32_t>::min() } })
    {
        if (kind == ValueKind::I32)
            constants.emplace_back(i32Const(static_cast<int32_t>(value)), i32(static_cast<int32_t>(value)));
        else
            constants.emplace_back(i64Const(value), i64(value));
    }
    return constants;
}

/// Every combination of one sample per kind of @p kinds.
[[nodiscard]] std::vector<std::vector<Value>> argumentsOf(std::vector<ValueKind> const& kinds)
{
    auto sets = std::vector<std::vector<Value>> { {} };
    for (auto const kind: kinds)
    {
        auto extended = std::vector<std::vector<Value>> {};
        for (auto const& set: sets)
        {
            for (auto const& sample: samplesOf(kind))
            {
                auto longer = set;
                longer.push_back(sample);
                extended.push_back(std::move(longer));
            }
        }
        sets = std::move(extended);
    }
    return sets;
}

/// What the cases of everyOperationAgreesWithTheReference share: each runs one module on both engines with
/// argument sets, and the operations of its lowering count as executed.
class Agreement
{
  public:
    explicit Agreement(EngineUnderTest engine): _engine { engine } {}

    /// Runs @p module's function 0 on both engines with each of @p argumentSets: the outcomes and the steps
    /// they took must be equal.
    void check(ModuleBuilder const& module,
               std::vector<std::vector<Value>> const& argumentSets,
               StoreLimits limits = {})
    {
        auto reference = Harness { module, Reference, limits };
        auto harness = Harness { module, _engine, limits };
        _executed.merge(operationsOf(*harness.code()));
        for (auto const& args: argumentSets)
        {
            CHECK(harness.outcome(0, args) == reference.outcome(0, args));
            CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
        }
    }

    /// Runs @p module's function 0 on both engines with the arguments @p arguments makes of the reference's
    /// harness, which may name its entities: the stores are alike, so the same addresses name the same.
    void checkWith(ModuleBuilder const& module,
                   std::function<std::vector<std::vector<Value>>(Harness&)> const& arguments,
                   StoreLimits limits = {})
    {
        auto reference = Harness { module, Reference, limits };
        check(module, arguments(reference), limits);
    }

    /// The operations every module checked so far was lowered to.
    [[nodiscard]] std::set<OperationId> const& executed() const noexcept { return _executed; }

  private:
    EngineUnderTest _engine;
    std::set<OperationId> _executed;
};

/// The loop label's type of one i32 param, as type index 1 after a function's type 0.
[[nodiscard]] ModuleBuilder withLoopParam(FuncType function,
                                          ValueType param,
                                          std::vector<ValueType> locals,
                                          Bytes body)
{
    auto module = ModuleBuilder {};
    module.type(std::move(function));
    module.type(FuncType { .params = { param }, .results = {} });
    module.function(0, std::move(locals), std::move(body));
    return module;
}

} // namespace

TEST_CASE("FastInterpreter.everyOperationAgreesWithTheReference")
{
    // Each row of every lowering table runs on both engines, over sample operands: the same results, the
    // same traps, the same steps. Together the modules run every operation lowering emits.
    auto agreement = Agreement { anyFastEngine() };

    // Every Unary, Binary and BinaryImmediate row, over params of its operand types; a folding row's twin
    // with constants for its right operand.
    for (auto const& row: LoweringRows)
    {
        if (row.form == OperationForm::Special)
            continue;
        auto const& info = binary::opcodeInfo(row.opcode);
        INFO(info.text);
        auto const& signature = binary::signatureInfo(info.signature);
        auto const params = typesOf(signature.paramKinds());
        auto const results = typesOf(signature.resultKinds());
        auto kinds = std::vector<ValueKind> {};
        for (auto const& param: params)
            kinds.push_back(param.kind);
        auto const body = params.size() == 1 ? code({ localGet(0), encodingOf(row.opcode) })
                                             : code({ localGet(0), localGet(1), encodingOf(row.opcode) });
        agreement.check(functionOf(params, results, {}, body), argumentsOf(kinds));
        if (row.form != OperationForm::BinaryImmediate)
            continue;
        for (auto const& [constant, value]: constantsOf(params[1].kind))
        {
            agreement.check(
                functionOf(
                    { params[0] }, results, {}, code({ localGet(0), constant, encodingOf(row.opcode) })),
                argumentsOf({ params[0].kind }));
        }
    }

    // Every FusionTable row: br_if to a block and to a loop, if, each over slots and over a constant.
    for (auto const& row: FusionTable)
    {
        auto const& info = binary::opcodeInfo(row.compare);
        INFO(info.text << " with " << binary::opcodeInfo(row.branch).text);
        auto const params = typesOf(binary::signatureInfo(info.signature).paramKinds());
        auto const kind = params.front().kind;
        auto const unary = params.size() == 1;
        auto operandSets = std::vector<std::pair<Bytes, std::vector<ValueType>>> {};
        operandSets.emplace_back(unary ? localGet(0) : code({ localGet(0), localGet(1) }), params);
        if (!unary)
        {
            for (auto const& [constant, value]: constantsOf(kind))
                operandSets.emplace_back(code({ localGet(0), constant }),
                                         std::vector<ValueType> { params[0] });
        }
        for (auto const& [operands, operandParams]: operandSets)
        {
            auto kinds = std::vector<ValueKind>(operandParams.size(), kind);
            auto const compared = code({ operands, encodingOf(row.compare) });
            if (row.branch == Opcode::BrIf)
            {
                agreement.check(
                    functionOf(
                        operandParams,
                        { binary::I32 },
                        {},
                        code({ blockOf(0x02, noValues(), code({ compared, brIf(0), i32Const(0), op(0x0F) })),
                               i32Const(1) })),
                    argumentsOf(kinds));
                // To a loop: a comparison that holds spins until the step budget runs out.
                agreement.check(
                    functionOf(operandParams,
                               { binary::I32 },
                               {},
                               code({ blockOf(0x03, noValues(), code({ compared, brIf(0) })), i32Const(1) })),
                    argumentsOf(kinds),
                    StoreLimits { .maxSteps = 20 });
            }
            else
                agreement.check(
                    functionOf(
                        operandParams,
                        { binary::I32 },
                        {},
                        code({ compared,
                               blockOf(0x04, { 0x7F }, code({ i32Const(1), { 0x05 }, i32Const(0) })) })),
                    argumentsOf(kinds));
        }
    }

    // The operations of a body of their own and the conditional branches, a case each.
    auto const i32s = argumentsOf({ ValueKind::I32 });
    auto const nullOrNot = [](Harness& harness) {
        return std::vector<std::vector<Value>> { { nullFunc() }, { funcRefOf(harness) } };
    };
    auto const nullOrNotWithCount = [](Harness& harness) {
        return std::vector<std::vector<Value>> { { nullFunc(), i32(3) }, { funcRefOf(harness), i32(3) } };
    };
    agreement.check(functionOf({}, {}, {}, unreachable()), { {} });
    agreement.check(functionOf({ binary::I32 }, { binary::I32 }, {}, localGet(0)), i32s);
    agreement.check(functionOf({}, { binary::I32 }, {}, i32Const(7)), { {} });
    agreement.checkWith(functionOf({ binary::FuncRef }, { binary::FuncRef }, {}, localGet(0)), nullOrNot);
    agreement.check(functionOf({ binary::I32, binary::I32, binary::I32 },
                               { binary::I32 },
                               {},
                               code({ localGet(0), localGet(1), localGet(2), op(0x1B) })),
                    argumentsOf({ ValueKind::I32, ValueKind::I32, ValueKind::I32 }));
    agreement.check(functionOf({ binary::V128, binary::V128, binary::I32 },
                               { binary::V128 },
                               {},
                               code({ localGet(0), localGet(1), localGet(2), op(0x1B) })),
                    { { V128 { .lanes = { 1, 2 } }, V128 { .lanes = { 3, 4 } }, i32(0) },
                      { V128 { .lanes = { 1, 2 } }, V128 { .lanes = { 3, 4 } }, i32(1) } });
    {
        auto globals = functionOf(
            {}, { binary::I64 }, {}, code({ op(0x23, 0), i64Const(1), op(0x7C), op(0x24, 0), op(0x23, 0) }));
        globals.global(GlobalType { .type = binary::I64, .mutability = Mutability::Var }, i64Const(5));
        agreement.check(globals, { {}, {}, {} });
    }
    agreement.check(
        functionOf({}, { binary::I32 }, {}, code({ blockOf(0x02, noValues(), br(0)), i32Const(3) })), { {} });
    agreement.check(
        functionOf(
            { binary::I32 },
            { binary::I32 },
            {},
            code({ localGet(0), blockOf(0x04, { 0x7F }, code({ i32Const(1), { 0x05 }, i32Const(2) })) })),
        i32s);
    agreement.check(
        functionOf({ binary::I32 },
                   { binary::I32 },
                   {},
                   code({ blockOf(0x02, noValues(), code({ localGet(0), brIf(0), i32Const(5), op(0x0F) })),
                          i32Const(6) })),
        i32s);
    // br_if.move: the branch leaves 99 behind.
    agreement.check(
        functionOf(
            { binary::I32, binary::I32 },
            { binary::I32 },
            {},
            blockOf(0x02, { 0x7F }, code({ i32Const(99), localGet(0), localGet(1), brIf(0), drop() }))),
        argumentsOf({ ValueKind::I32, ValueKind::I32 }));
    for (auto const& shape: countDowns())
        agreement.check(shape.module, { { i32(shape.argument) }, { i32(1) }, { i32(2) } });
    // br_on_null, to a block and to a loop, moving a value it leaves 99 under, or none.
    agreement.checkWith(
        functionOf({ binary::FuncRef },
                   { binary::I32 },
                   {},
                   code({ blockOf(0x02,
                                  noValues(),
                                  code({ localGet(0), { 0xD5, 0x00 }, drop(), i32Const(1), op(0x0F) })),
                          i32Const(0) })),
        nullOrNot);
    agreement.checkWith(
        functionOf(
            { binary::FuncRef },
            { binary::I32 },
            {},
            code({ blockOf(0x03, noValues(), code({ localGet(0), { 0xD5, 0x00 }, drop() })), i32Const(1) })),
        nullOrNot,
        StoreLimits { .maxSteps = 20 });
    agreement.checkWith(
        functionOf({ binary::FuncRef },
                   { binary::I32 },
                   {},
                   blockOf(0x02,
                           { 0x7F },
                           code({ i32Const(99), i32Const(5), localGet(0), { 0xD5, 0x00 }, drop(), drop() }))),
        nullOrNot);
    agreement.checkWith(
        withLoopParam(
            FuncType { .params = { binary::FuncRef, binary::I32 }, .results = { binary::I32 } },
            binary::I32,
            {},
            code({ localGet(1),
                   blockOf(0x03,
                           typeIndex(1),
                           code({ localGet(1), localGet(0), { 0xD5, 0x00 }, drop(), drop(), drop() })),
                   i32Const(7) })),
        nullOrNotWithCount,
        StoreLimits { .maxSteps = 20 });
    // br_on_non_null, to a block and to a loop, moving the reference over a value it leaves behind, or not.
    agreement.checkWith(
        functionOf({ binary::FuncRef },
                   { binary::FuncRef },
                   {},
                   blockOf(0x02, { 0x70 }, code({ localGet(0), { 0xD6, 0x00 }, localGet(0) }))),
        nullOrNot);
    agreement.checkWith(
        functionOf({ binary::FuncRef },
                   { binary::FuncRef },
                   {},
                   blockOf(0x02,
                           { 0x70 },
                           code({ i32Const(99), localGet(0), { 0xD6, 0x00 }, drop(), localGet(0) }))),
        nullOrNot);
    agreement.checkWith(
        withLoopParam(
            FuncType { .params = { binary::FuncRef }, .results = { binary::I32 } },
            binary::FuncRef,
            {},
            code({ localGet(0), blockOf(0x03, typeIndex(1), code({ { 0xD6, 0x00 } })), i32Const(1) })),
        nullOrNot,
        StoreLimits { .maxSteps = 20 });
    agreement.checkWith(
        withLoopParam(FuncType { .params = { binary::FuncRef }, .results = { binary::I32 } },
                      binary::FuncRef,
                      {},
                      code({ localGet(0),
                             blockOf(0x03, typeIndex(1), code({ localGet(0), { 0xD6, 0x00 }, drop() })),
                             i32Const(1) })),
        nullOrNot,
        StoreLimits { .maxSteps = 20 });
    // br to the function's own label, a return.
    agreement.check(functionOf({}, { binary::I32 }, {}, code({ i32Const(5), br(0) })), { {} });
    // Dead code cuts a block's two results, one entry, in the middle: the second block takes one as its
    // param.
    {
        auto module = ModuleBuilder {};
        module.type(FuncType { .params = {}, .results = { binary::I32 } });
        module.type(FuncType { .params = {}, .results = { binary::I32, binary::I32 } });
        module.type(FuncType { .params = { binary::I32 }, .results = {} });
        module.function(0,
                        {},
                        code({ blockOf(0x02, typeIndex(1), code({ i32Const(1), i32Const(2), br(0) })),
                               blockOf(0x02, typeIndex(2), unreachable()),
                               drop(),
                               i32Const(3) }));
        agreement.check(module, { {} });
    }
    // move.range and br_table.
    {
        auto module = ModuleBuilder {};
        module.type(FuncType { .params = {}, .results = { binary::I64, binary::I64, binary::I64 } });
        module.function(0,
                        {},
                        blockOf(0x02,
                                typeIndex(0),
                                code({ i64Const(1), i64Const(10), i64Const(20), i64Const(30), br(0) })));
        agreement.check(module, { {} });
    }

    // No lowering emits these yet: nop lowers to nothing, and v128.const is lowered with the vector
    // operations.
    auto const notEmitted = std::set<OperationId> { OperationId::Nop, OperationId::Const128 };
    for (auto const& row: OperationTable)
    {
        INFO(row.name);
        CHECK(agreement.executed().contains(row.id) != notEmitted.contains(row.id));
    }
}

// The contract test below ends the process at a failed Require, so it is hidden from the unit run:
// tests/CMakeLists.txt runs it in a process of its own and expects the Require's text.

TEST_CASE("Contract.FastInterpreter.dispatchMustBeAvailable", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    // One past the last Dispatch: no build compiles it.
    auto const uncompiled = static_cast<isoline::fastinterpreter::Dispatch>(
        std::to_underlying(isoline::fastinterpreter::Dispatch::TailCall) + 1);
    std::ignore = isoline::fastinterpreter::FastInterpreter { uncompiled };
    FAIL("SURVIVED: an interpreter for a dispatch this build did not compile");
}
