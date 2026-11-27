// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/testing/Engines.hpp>
#include <isoline/fastinterpreter/testing/Lowering.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

// The bulk memory operations on the fast interpreter, against the reference: the same granules, polled at
// the same sites, in the same order, leaving the same partial writes; bounds checked before any write.

using isoline::binary::AddressType;
using isoline::binary::FuncType;
using isoline::binary::Limits;
using isoline::binary::Opcode;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::fastinterpreter::availableDispatches;
using isoline::fastinterpreter::headerWord;
using isoline::fastinterpreter::LoweredModule;
using isoline::fastinterpreter::OperationId;
using isoline::fastinterpreter::testing::EngineUnderTest;
using isoline::fastinterpreter::testing::fast;
using isoline::fastinterpreter::testing::fastEngineOfEachDispatch;
using isoline::fastinterpreter::testing::handWritten;
using isoline::fastinterpreter::testing::Harness;
using isoline::fastinterpreter::testing::memoryInstruction;
using isoline::fastinterpreter::testing::op;
using isoline::fastinterpreter::testing::prefixed;
using isoline::fastinterpreter::testing::Reference;
using isoline::runtime::BulkPollBytes;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Limit;
using isoline::runtime::PollSite;
using isoline::runtime::PollSiteTable;
using isoline::runtime::StoreLimits;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::PollCounter;
using isoline::runtime::testing::PollTrigger;

namespace binary = isoline::binary;

namespace
{

/// The granules of the large operations: 64 MiB in granules of BulkPollBytes.
constexpr auto LargeGranules = uint64_t { 1'024 };

/// The bytes the large operations move: 64 MiB.
constexpr auto LargeBytes = LargeGranules * BulkPollBytes;

/// How far above its source the overlapping copy writes.
constexpr auto CopyShift = uint64_t { 4'096 };

/// The large operations, each the function of the same index of largeOperations().
enum class Large : uint8_t
{
    Fill, ///< memory.fill of 64 MiB at 0 with 0xAB.
    Copy, ///< memory.copy of 64 MiB from 0 to CopyShift: overlapping, the destination above.
};

/// A module of one memory of 64 MiB and a page, and two functions of no params: Large::Fill and Large::Copy.
[[nodiscard]] ModuleBuilder largeOperations()
{
    auto module = ModuleBuilder {};
    module.type(FuncType {});
    module.memory(Limits { .min = LargeGranules + 1, .max = std::nullopt, .addressType = AddressType::I32 });
    module.function(0,
                    {},
                    code({ i32Const(0),
                           i32Const(0xAB),
                           i32Const(static_cast<int32_t>(LargeBytes)),
                           prefixed(11, { 0 }) }));
    module.function(0,
                    {},
                    code({ i32Const(static_cast<int32_t>(CopyShift)),
                           i32Const(0),
                           i32Const(static_cast<int32_t>(LargeBytes)),
                           prefixed(10, { 0, 0 }) }));
    return module;
}

/// The bytes of memory 0 of @p harness.
[[nodiscard]] std::span<std::byte> memoryOf(Harness& harness)
{
    return harness.store().memory(harness.instance().memories.at(0)).bytes();
}

/// Byte @p index of the pattern the copies start from: no two granules alike.
[[nodiscard]] std::byte patternAt(std::size_t index)
{
    return static_cast<std::byte>((index % 251) + 1);
}

/// Fills @p bytes with the pattern: its first period byte by byte, then by doubling copies, so that 64 MiB
/// take a few dozen copies even in an unoptimized build under an emulator.
void writePattern(std::span<std::byte> bytes)
{
    constexpr auto Period = std::size_t { 251 };
    auto const first = std::min(bytes.size(), Period);
    for (auto const index: std::views::iota(std::size_t { 0 }, first))
        bytes[index] = patternAt(index);
    // A whole number of periods doubles each round, so byte i stays patternAt(i).
    auto filled = first;
    while (filled < bytes.size())
    {
        auto const size = std::min(filled, bytes.size() - filled);
        std::memcpy(bytes.data() + filled, bytes.data(), size);
        filled += size;
    }
}

/// The memory @p operation leaves after its first @p granules granules, as a model of its own: a fill upwards
/// from 0, a copy from the top granule down.
[[nodiscard]] std::vector<std::byte> expectedAfter(Large operation, uint64_t granules, std::size_t size)
{
    auto bytes = std::vector<std::byte>(size);
    if (operation == Large::Fill)
    {
        std::fill_n(bytes.begin(), granules * BulkPollBytes, std::byte { 0xAB });
        return bytes;
    }
    writePattern(bytes);
    for (auto const done: std::views::iota(uint64_t { 0 }, granules))
    {
        auto const granule = LargeGranules - 1 - done;
        std::memmove(bytes.data() + CopyShift + (granule * BulkPollBytes),
                     bytes.data() + (granule * BulkPollBytes),
                     BulkPollBytes);
    }
    return bytes;
}

/// The first index at which @p left and @p right differ; their size where they do not. Compared so, a
/// failure names an offset, not 64 MiB of bytes.
[[nodiscard]] std::size_t firstDifference(std::span<std::byte const> left, std::span<std::byte const> right)
{
    REQUIRE(left.size() == right.size());
    if (left.empty() || std::memcmp(left.data(), right.data(), left.size()) == 0)
        return left.size();
    return static_cast<std::size_t>(std::ranges::mismatch(left, right).in1 - left.begin());
}

/// What running a large operation on one engine left.
struct LargeRun
{
    std::optional<TrapCode> trap;  ///< How it ended: none for a return.
    std::vector<std::byte> memory; ///< Memory 0 after it.
    std::vector<uint64_t> polls;   ///< Its polls per site, in PollSiteTable's order.
    uint64_t steps = 0;            ///< Store::used(Limit::Steps).
};

/// Runs @p operation on @p engine, observed by a counter that interrupts at @p interruptAt, if any.
[[nodiscard]] LargeRun runLarge(Large operation,
                                EngineUnderTest engine,
                                std::optional<PollTrigger> interruptAt)
{
    auto counter = PollCounter { interruptAt };
    auto harness = Harness { largeOperations(), engine, StoreLimits {}, &counter };
    counter.interruptAtTrigger(harness.store().interrupter());
    if (operation == Large::Copy)
        writePattern(memoryOf(harness));
    auto run = LargeRun {};
    auto const outcome = harness.outcome(std::to_underlying(operation));
    if (!outcome)
    {
        REQUIRE(std::holds_alternative<isoline::runtime::Trap>(outcome.error()));
        run.trap = std::get<isoline::runtime::Trap>(outcome.error()).code;
    }
    auto const bytes = memoryOf(harness);
    run.memory.assign(bytes.begin(), bytes.end());
    for (auto const& row: PollSiteTable)
        run.polls.push_back(counter.count(row.site));
    run.steps = harness.store().used(Limit::Steps);
    return run;
}

/// @p module with a memory of one page of @p type, holding the bytes 1, 2, … from address 0 of its 16 first
/// bytes, and a passive data segment of 8 bytes.
[[nodiscard]] ModuleBuilder withSmallMemory(ModuleBuilder module, AddressType type)
{
    module.memory(Limits { .min = 1, .max = std::nullopt, .addressType = type });
    module.dataCount(1);
    module.data({ 0x01, 0x08, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38 });
    return module;
}

/// The value of an address of @p type.
[[nodiscard]] Value addressOf(AddressType type, uint64_t value)
{
    return type == AddressType::I64 ? Value { I64 { value } }
                                    : Value { I32 { static_cast<uint32_t>(value) } };
}

} // namespace

TEST_CASE("Bulk.granulesMatchReference", "[memory]")
{
    // Review Focus 2: a 64 MiB memory.fill and a 64 MiB memory.copy whose destination is above its source,
    // within one memory, poll at every one of their 1'024 granules, as the reference does. Interrupted at
    // the k-th granule poll, each leaves the k - 1 granules before it written: the fill the first, the copy
    // the last, which a model of their own confirms; and leaves exactly the reference's memory.
    // Memory-bound, so tagged [memory]: it runs apart, under the hard cap of the isoline_test_memory row; at
    // most three 64 MiB memories are live at once.
    auto const engine = fastEngineOfEachDispatch();
    for (auto const operation: { Large::Fill, Large::Copy })
    {
        INFO((operation == Large::Fill ? "memory.fill" : "memory.copy, the destination above the source"));
        {
            auto const reference = runLarge(operation, Reference, std::nullopt);
            auto const fastRun = runLarge(operation, engine, std::nullopt);
            CHECK_FALSE(fastRun.trap.has_value());
            CHECK(fastRun.polls == reference.polls);
            CHECK(fastRun.polls[std::to_underlying(PollSite::BulkGranule)] == LargeGranules);
            CHECK(fastRun.steps == reference.steps);
            CHECK(firstDifference(fastRun.memory, reference.memory) == fastRun.memory.size());
            CHECK(firstDifference(fastRun.memory,
                                  expectedAfter(operation, LargeGranules, fastRun.memory.size()))
                  == fastRun.memory.size());
        }
        for (auto const k: { uint64_t { 1 }, uint64_t { 513 }, LargeGranules })
        {
            INFO("interrupted at granule poll " << k);
            auto const trigger = PollTrigger { .site = PollSite::BulkGranule, .ordinal = k };
            auto const reference = runLarge(operation, Reference, trigger);
            auto const fastRun = runLarge(operation, engine, trigger);
            CHECK(fastRun.trap == TrapCode::Interrupted);
            CHECK(reference.trap == TrapCode::Interrupted);
            CHECK(fastRun.polls == reference.polls);
            CHECK(fastRun.steps == reference.steps);
            CHECK(firstDifference(fastRun.memory, expectedAfter(operation, k - 1, fastRun.memory.size()))
                  == fastRun.memory.size());
            CHECK(firstDifference(fastRun.memory, reference.memory) == fastRun.memory.size());
        }
    }
}

TEST_CASE("Bulk.stepLimitStopsAtTheSameGranule", "[memory]")
{
    // Unobserved, the countdown is the executor's alone until it commits: a step budget spent within a fill
    // stops it at the same granule as the reference, with the same steps taken and the same bytes written.
    auto const engine = fastEngineOfEachDispatch();
    auto const limits = StoreLimits { .maxSteps = 100 };
    auto reference = Harness { largeOperations(), Reference, limits };
    auto harness = Harness { largeOperations(), engine, limits };
    CHECK(reference.trap(std::to_underlying(Large::Fill)) == TrapCode::StepLimitExceeded);
    CHECK(harness.trap(std::to_underlying(Large::Fill)) == TrapCode::StepLimitExceeded);
    CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
    CHECK(harness.store().used(Limit::Steps) == 100);
    auto const written = memoryOf(harness);
    CHECK(firstDifference(written, memoryOf(reference)) == written.size());
    // The entry's poll took one step, each granule's one: 99 granules filled.
    CHECK(written[(99 * BulkPollBytes) - 1] == std::byte { 0xAB });
    CHECK(written[99 * BulkPollBytes] == std::byte { 0 });
}

TEST_CASE("Bulk.boundsCheckedBeforeAnyWrite")
{
    // Every range is checked before the first write, as the reference checks it: a trapping fill, copy or
    // init writes nothing, and no range end overflows, a memory64 one whose end is past 2^64 included.
    auto const engine = fastEngineOfEachDispatch();
    constexpr auto Page = uint64_t { 65'536 };
    constexpr auto All = std::numeric_limits<uint64_t>::max();
    for (auto const type: { AddressType::I32, AddressType::I64 })
    {
        INFO((type == AddressType::I64 ? "memory64" : "memory32"));
        auto const a = type == AddressType::I64 ? binary::I64 : binary::I32;
        auto const at = [type](uint64_t value) {
            return addressOf(type, value);
        };
        auto const threeOperands = code({ localGet(0), localGet(1), localGet(2) });
        struct Case
        {
            ModuleBuilder module;
            std::vector<std::vector<Value>> arguments;
        };
        auto cases = std::vector<Case> {};
        cases.push_back(
            { .module = withSmallMemory(
                  isoline::fastinterpreter::testing::functionOf(
                      { a, binary::I32, a }, {}, {}, code({ threeOperands, prefixed(11, { 0 }) })),
                  type),
              .arguments = { { at(1), I32 { 9 }, at(Page) },
                             { at(Page), I32 { 9 }, at(1) },
                             { at(Page - 1), I32 { 9 }, at(All) },
                             { at(All), I32 { 9 }, at(2) } } });
        cases.push_back({ .module = withSmallMemory(
                              isoline::fastinterpreter::testing::functionOf(
                                  { a, a, a }, {}, {}, code({ threeOperands, prefixed(10, { 0, 0 }) })),
                              type),
                          .arguments = { { at(0), at(1), at(Page) },
                                         { at(1), at(0), at(Page) },
                                         { at(0), at(All - 7), at(16) },
                                         { at(All - 7), at(0), at(16) },
                                         { at(8), at(0), at(All) } } });
        cases.push_back({ .module = withSmallMemory(isoline::fastinterpreter::testing::functionOf(
                                                        { a, binary::I32, binary::I32 },
                                                        {},
                                                        {},
                                                        code({ threeOperands, prefixed(8, { 0, 0 }) })),
                                                    type),
                          .arguments = { { at(Page - 7), I32 { 0 }, I32 { 8 } },
                                         { at(0), I32 { 1 }, I32 { 8 } },
                                         { at(0), I32 { 0xFFFF'FFFF }, I32 { 2 } },
                                         { at(All), I32 { 0 }, I32 { 1 } } } });
        for (auto& [module, argumentSets]: cases)
        {
            for (auto const& arguments: argumentSets)
            {
                auto reference = Harness { module, Reference };
                auto harness = Harness { module, engine };
                CHECK(reference.trap(0, arguments) == TrapCode::OutOfBoundsMemoryAccess);
                CHECK(harness.trap(0, arguments) == TrapCode::OutOfBoundsMemoryAccess);
                CHECK(
                    std::ranges::all_of(memoryOf(harness), [](std::byte b) { return b == std::byte { 0 }; }));
                CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
            }
        }
    }
}

TEST_CASE("Bulk.memory32OperandsIgnoreTheSlotsHighBits")
{
    // Code written by hand fills i32 memory 0 from slots holding i64s, high bits set: an operand of a memory
    // of i32 addresses is read as its low 32 bits, so destination 0xFFFF'FFFF'0000'0010 and count
    // 0xFFFF'FFFF'0000'0004 fill 4 bytes at 16. Read whole, the destination would be out of bounds.
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto module = withSmallMemory(
        isoline::fastinterpreter::testing::functionOf({ binary::I64, binary::I32, binary::I64 },
                                                      { binary::I32 },
                                                      {},
                                                      code({ localGet(0),
                                                             op(0xA7),
                                                             localGet(1),
                                                             localGet(2),
                                                             op(0xA7),
                                                             prefixed(11, { 0 }),
                                                             localGet(0),
                                                             op(0xA7),
                                                             memoryInstruction(Opcode::I32Load, 0) })),
        AddressType::I32);
    // By hand: memory.fill s0, s1, s2, #0; i32.load s3, s0, #0; return s3.
    auto const byHand = [dispatch](LoweredModule const& lowered) {
        REQUIRE(lowered.functions().front().frameSlots >= 4);
        return handWritten(lowered,
                           { headerWord(dispatch, OperationId::MemoryFill),
                             isoline::fastinterpreter::indexPairWord(0, 1),
                             2,
                             0,
                             headerWord(dispatch, OperationId::I32Load),
                             isoline::fastinterpreter::indexPairWord(3, 0),
                             0,
                             headerWord(dispatch, OperationId::Return),
                             3 });
    };
    auto harness = Harness { module, fast(dispatch), byHand };
    CHECK(harness.result(0, { I64 { 0xFFFF'FFFF'0000'0010 }, I32 { 0x5A }, I64 { 0xFFFF'FFFF'0000'0004 } })
          == Value { I32 { 0x5A5A'5A5A } });
    CHECK(harness.trap(0, { I64 { 0x0000'0000'0001'0000 }, I32 { 0x5A }, I64 { 0xFFFF'FFFF'0000'0001 } })
          == TrapCode::OutOfBoundsMemoryAccess);
}

TEST_CASE("Bulk.everyMemory32OperandIgnoresTheSlotsHighBits")
{
    // Code written by hand runs memory.copy and memory.init over i32 memory 0 from three slots holding i64s,
    // high bits planted in one operand at a time: each operand, the destination, the source and the count, is
    // read as its low 32 bits, so the operation goes on; read whole, any of them would be out of bounds.
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    constexpr auto High = uint64_t { 0xFFFF'FFFF'0000'0000 };
    struct Row
    {
        char const* name;  ///< The operation.
        OperationId id;    ///< Its operation.
        Bytes instruction; ///< Its instruction, which the lowered body ends with.
        uint64_t indices;  ///< Its immediate (indexPairWord()).
    };
    auto const rows = std::vector<Row> {
        { .name = "memory.copy",
          .id = OperationId::MemoryCopy,
          .instruction = prefixed(10, { 0, 0 }),
          .indices = 0 },
        { .name = "memory.init",
          .id = OperationId::MemoryInit,
          .instruction = prefixed(8, { 0, 0 }),
          .indices = 0 },
        { .name = "memory.fill",
          .id = OperationId::MemoryFill,
          .instruction = prefixed(11, { 0 }),
          .indices = 0 },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const module = withSmallMemory(
            isoline::fastinterpreter::testing::functionOf({ binary::I64, binary::I64, binary::I64 },
                                                          {},
                                                          {},
                                                          code({ localGet(0),
                                                                 op(0xA7),
                                                                 localGet(1),
                                                                 op(0xA7),
                                                                 localGet(2),
                                                                 op(0xA7),
                                                                 row.instruction })),
            AddressType::I32);
        // By hand: the operation over s0, s1, s2; return s3.
        auto const byHand = [dispatch, &row](LoweredModule const& lowered) {
            REQUIRE(lowered.functions().front().frameSlots >= 4);
            return handWritten(lowered,
                               { headerWord(dispatch, row.id),
                                 isoline::fastinterpreter::indexPairWord(0, 1),
                                 2,
                                 row.indices,
                                 headerWord(dispatch, OperationId::Return),
                                 3 });
        };
        auto harness = Harness { module, fast(dispatch), byHand };
        // Destination 16, source 0 (of the segment, for memory.init), count 4.
        auto const plain = std::array<uint64_t, 3> { 16, 0, 4 };
        for (auto const planted: std::views::iota(std::size_t { 0 }, plain.size()))
        {
            INFO("high bits in operand " << planted);
            auto arguments = std::vector<Value> {};
            for (auto const operand: std::views::iota(std::size_t { 0 }, plain.size()))
                arguments.emplace_back(I64 { plain[operand] | (operand == planted ? High : 0) });
            CHECK(harness.outcome(0, arguments).has_value());
        }
    }
}
