// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/Bounds.hpp>
#include <isoline/interpreter/Code.hpp>
#include <isoline/interpreter/Dispatch.hpp>
#include <isoline/interpreter/Listing.hpp>
#include <isoline/interpreter/testing/Engines.hpp>
#include <isoline/interpreter/testing/Lowering.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

// Tables, references and the table bulk operations on the interpreter, against the reference: the same
// results, traps, granules and partial writes; references that keep their heap; and the Spectre select of a
// table index.

using isoline::binary::AbstractHeapType;
using isoline::binary::AddressType;
using isoline::binary::FuncType;
using isoline::binary::HeapType;
using isoline::binary::Nullable;
using isoline::binary::TableType;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;
using isoline::binary::testing::brIf;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::interpreter::availableDispatches;
using isoline::interpreter::elementMask;
using isoline::interpreter::headerWord;
using isoline::interpreter::indexPairWord;
using isoline::interpreter::LoweredModule;
using isoline::interpreter::OperationId;
using isoline::interpreter::testing::EngineUnderTest;
using isoline::interpreter::testing::functionOf;
using isoline::interpreter::testing::handWritten;
using isoline::interpreter::testing::Harness;
using isoline::interpreter::testing::interpreterOfEachDispatch;
using isoline::interpreter::testing::interpreterUnder;
using isoline::interpreter::testing::op;
using isoline::interpreter::testing::prefixed;
using isoline::interpreter::testing::Reference;
using isoline::runtime::BulkPollElements;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Limit;
using isoline::runtime::PollSite;
using isoline::runtime::PollSiteTable;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Refusal;
using isoline::runtime::StoreLimits;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::PollCounter;
using isoline::runtime::testing::PollTrigger;

namespace binary = isoline::binary;

namespace
{

/// The elements of the large table: two granules and a part.
constexpr auto LargeElements = uint32_t { (2 * BulkPollElements) + 100 };

/// A passive element segment of @p count references to function 0.
[[nodiscard]] Bytes functionSegment(uint32_t count)
{
    auto bytes = code({ { 0x01, 0x00 }, encodeU32(count) });
    bytes.insert(bytes.end(), count, uint8_t { 0x00 });
    return bytes;
}

/// The large table's module: a funcref table of LargeElements elements of up to LargeElements + 2, element
/// segment 0 of LargeElements references to function 0, and three functions of no params: 0 initializes the
/// whole table from the segment (table.init), 1 copies its elements [0, LargeElements - 1) one up
/// (table.copy, the destination above), 2 grows it by its i32 param with function 0, returning table.grow's
/// result.
[[nodiscard]] ModuleBuilder largeTable()
{
    auto module = ModuleBuilder {};
    module.type(FuncType {});
    module.type(FuncType { .params = { binary::I32 }, .results = { binary::I32 } });
    module.table(TableType {
        .element = binary::FuncRef,
        .limits = { .min = LargeElements, .max = LargeElements + 2, .addressType = AddressType::I32 } });
    module.element(functionSegment(LargeElements));
    module.function(0,
                    {},
                    code({ i32Const(0),
                           i32Const(0),
                           i32Const(static_cast<int32_t>(LargeElements)),
                           prefixed(12, { 0, 0 }) }));
    module.function(0,
                    {},
                    code({ i32Const(1),
                           i32Const(0),
                           i32Const(static_cast<int32_t>(LargeElements - 1)),
                           prefixed(14, { 0, 0 }) }));
    module.function(1, {}, code({ { 0xD2, 0x00 }, localGet(0), prefixed(15, { 0 }) }));
    return module;
}

/// The elements of table 0 of @p harness.
[[nodiscard]] std::vector<Ref>& tableOf(Harness& harness)
{
    return harness.store().table(harness.instance().tables.at(0)).elements;
}

/// A funcref to function 0 of @p harness, null where @p present is false.
[[nodiscard]] Ref functionOrNull(Harness& harness, bool present)
{
    if (!present)
        return Ref { .kind = RefKind::Null, .heap = { .kind = AbstractHeapType::Func }, .address = 0 };
    return Ref { .kind = RefKind::Func,
                 .heap = { .kind = AbstractHeapType::Func },
                 .address = harness.function(0) };
}

/// Sets the elements of table 0 of @p harness to alternate: function 0 at the indices of @p parity.
void alternate(Harness& harness, uint32_t parity)
{
    auto& elements = tableOf(harness);
    for (auto const index: std::views::iota(std::size_t { 0 }, elements.size()))
        elements[index] = functionOrNull(harness, index % 2 == parity);
}

/// Whether each element of @p elements is null.
[[nodiscard]] std::vector<bool> nullsOf(std::span<Ref const> elements)
{
    auto nulls = std::vector<bool> {};
    for (auto const& element: elements)
        nulls.push_back(element.kind == RefKind::Null);
    return nulls;
}

/// The BulkGranule polls instantiating the large table's module takes: evaluating its segment's items, in
/// granules of BulkPollElements.
constexpr auto InstantiationGranules = uint64_t { 3 };

/// The first index at which @p left and @p right differ; their size where they do not. Compared so, a
/// failure names an index, not two tables.
[[nodiscard]] std::size_t firstDifference(std::vector<bool> const& left, std::vector<bool> const& right)
{
    REQUIRE(left.size() == right.size());
    return static_cast<std::size_t>(std::ranges::mismatch(left, right).in1 - left.begin());
}

/// What running a function of the large table's module on one engine left.
struct TableRun
{
    std::optional<TrapCode> trap; ///< How it ended: none for a return.
    std::vector<bool> nulls;      ///< Which elements of table 0 are null after it.
    std::vector<uint64_t> polls;  ///< Its polls per site, in PollSiteTable's order, instantiation's included.
    uint64_t steps = 0;           ///< Store::used(Limit::Steps).
};

/// Runs function @p function of the large table's module on @p engine, its table alternating first (function
/// 0 at the odd indices), observed by a counter that interrupts at its @p interruptAt-th granule poll, if
/// any, counting from the call's first.
[[nodiscard]] TableRun runOnLargeTable(uint32_t function,
                                       EngineUnderTest engine,
                                       std::optional<uint64_t> interruptAt)
{
    auto trigger = std::optional<PollTrigger> {};
    if (interruptAt)
        trigger =
            PollTrigger { .site = PollSite::BulkGranule, .ordinal = InstantiationGranules + *interruptAt };
    auto counter = PollCounter { trigger };
    auto harness = Harness { largeTable(), engine, StoreLimits {}, &counter };
    REQUIRE(counter.count(PollSite::BulkGranule) == InstantiationGranules);
    counter.interruptAtTrigger(harness.store().interrupter());
    alternate(harness, 1);
    auto run = TableRun {};
    auto const outcome = harness.outcome(function);
    if (!outcome)
    {
        REQUIRE(std::holds_alternative<isoline::runtime::Trap>(outcome.error()));
        run.trap = std::get<isoline::runtime::Trap>(outcome.error()).code;
    }
    run.nulls = nullsOf(tableOf(harness));
    for (auto const& row: PollSiteTable)
        run.polls.push_back(counter.count(row.site));
    run.steps = harness.store().used(Limit::Steps);
    return run;
}

/// The nulls the alternating table holds after the first @p granules granules of the init (function 0 from
/// index 0 up) or of the copy one up (from the top granule down).
[[nodiscard]] std::vector<bool> expectedNulls(uint32_t function, uint64_t granules)
{
    auto nulls = std::vector<bool>(LargeElements);
    for (auto const index: std::views::iota(std::size_t { 0 }, nulls.size()))
        nulls[index] = index % 2 == 0;
    if (function == 0)
    {
        for (auto const index: std::views::iota(
                 std::size_t { 0 }, std::min<std::size_t>(granules * BulkPollElements, LargeElements)))
            nulls[index] = false;
        return nulls;
    }
    auto const count = std::size_t { LargeElements - 1 };
    auto const total = (count + BulkPollElements - 1) / BulkPollElements;
    auto const before = nulls;
    for (auto const done: std::views::iota(std::size_t { 0 }, granules))
    {
        auto const granule = total - 1 - done;
        auto const first = granule * BulkPollElements;
        auto const last = std::min<uint64_t>(count, first + BulkPollElements);
        for (auto const index: std::views::iota(first, last))
            nulls[index + 1] = before[index];
    }
    return nulls;
}

} // namespace

TEST_CASE("Tables.opsMatchReference")
{
    auto const engine = interpreterOfEachDispatch();
    SECTION("table.grow refused at the store's limit: -1, the refusal recorded as Limit")
    {
        auto const limits = StoreLimits { .maxTableElements = LargeElements + 1 };
        for (auto const& candidate: { Reference, engine })
        {
            auto harness = Harness { largeTable(), candidate, limits };
            auto results = std::vector<Value>(1);
            auto const args = std::vector<Value> { I32 { 2 } };
            auto const invoked = isoline::runtime::invokeChecked(
                harness.store(), harness.executor(), harness.function(2), args, results);
            REQUIRE(invoked.outcome.has_value());
            CHECK(results.front() == Value { I32 { 0xFFFF'FFFF } });
            CHECK(invoked.refusedGrowth.table == Refusal::Limit);
            CHECK(tableOf(harness).size() == LargeElements);
        }
        // Past the type's maximum instead, and granted.
        for (auto const& candidate: { Reference, engine })
        {
            auto harness = Harness { largeTable(), candidate };
            auto results = std::vector<Value>(1);
            auto args = std::vector<Value> { I32 { 3 } };
            auto const refused = isoline::runtime::invokeChecked(
                harness.store(), harness.executor(), harness.function(2), args, results);
            REQUIRE(refused.outcome.has_value());
            CHECK(results.front() == Value { I32 { 0xFFFF'FFFF } });
            CHECK(refused.refusedGrowth.table == Refusal::TypeMaximum);
            CHECK(harness.result(2, { I32 { 2 } }) == Value { I32 { LargeElements } });
            CHECK(tableOf(harness).size() == LargeElements + 2);
            CHECK(tableOf(harness).back() == functionOrNull(harness, true));
        }
    }
    SECTION(
        "table.init and a backward table.copy: their granules, polled and interrupted like the reference's")
    {
        for (auto const function: { 0U, 1U })
        {
            INFO((function == 0 ? "table.init" : "table.copy one up"));
            {
                auto const reference = runOnLargeTable(function, Reference, std::nullopt);
                auto const interpreterRun = runOnLargeTable(function, engine, std::nullopt);
                CHECK_FALSE(interpreterRun.trap.has_value());
                CHECK(interpreterRun.polls == reference.polls);
                CHECK(interpreterRun.polls[std::to_underlying(PollSite::BulkGranule)]
                      == InstantiationGranules + 3);
                CHECK(interpreterRun.steps == reference.steps);
                CHECK(firstDifference(interpreterRun.nulls, reference.nulls) == LargeElements);
                CHECK(firstDifference(interpreterRun.nulls, expectedNulls(function, 3)) == LargeElements);
            }
            for (auto const k: { uint64_t { 1 }, uint64_t { 2 }, uint64_t { 3 } })
            {
                INFO("interrupted at granule poll " << k);
                auto const reference = runOnLargeTable(function, Reference, k);
                auto const interpreterRun = runOnLargeTable(function, engine, k);
                CHECK(interpreterRun.trap == TrapCode::Interrupted);
                CHECK(interpreterRun.polls == reference.polls);
                CHECK(interpreterRun.steps == reference.steps);
                CHECK(firstDifference(interpreterRun.nulls, expectedNulls(function, k - 1)) == LargeElements);
                CHECK(firstDifference(interpreterRun.nulls, reference.nulls) == LargeElements);
            }
        }
    }
}

TEST_CASE("Tables.referencesKeepTheirHeap")
{
    // R6: type 1 repeats type 0, so the store interns both as one TypeId and type index 1 is not its TypeId.
    // A (ref null 1) table is set from an uninitialized (ref null 1) local, and from ref.null 1, then read
    // back: the null reaching the embedder, and the one the table holds, name the TypeId, as the reference's.
    auto const engine = interpreterOfEachDispatch();
    auto const concrete =
        ValueType { .kind = ValueKind::Ref,
                    .nullable = Nullable::Yes,
                    .heap = HeapType { .kind = AbstractHeapType::Concrete, .typeIndex = 1 } };
    auto module = ModuleBuilder {};
    module.type(FuncType {});
    module.type(FuncType {});
    module.type(FuncType { .params = {}, .results = { concrete } });
    module.table(TableType { .element = concrete,
                             .limits = { .min = 2, .max = std::nullopt, .addressType = AddressType::I32 } });
    // Function 0: table[0] = the local; table[1] = ref.null 1; return table[0]. Function 1: return table[1].
    module.function(2,
                    { concrete },
                    code({ i32Const(0),
                           localGet(0),
                           { 0x26, 0x00 },
                           i32Const(1),
                           { 0xD0, 0x01 },
                           { 0x26, 0x00 },
                           i32Const(0),
                           { 0x25, 0x00 } }));
    module.function(2, {}, code({ i32Const(1), { 0x25, 0x00 } }));
    auto reference = Harness { module, Reference };
    auto harness = Harness { module, engine };
    auto const typeId = harness.instance().types.at(1);
    REQUIRE(typeId != 1);
    auto const typedNull =
        Value { Ref { .kind = RefKind::Null,
                      .heap = HeapType { .kind = AbstractHeapType::Concrete, .typeIndex = typeId },
                      .address = 0 } };
    CHECK(harness.result(0) == typedNull);
    CHECK(harness.result(1) == typedNull);
    CHECK(reference.result(0) == typedNull);
    CHECK(reference.result(1) == typedNull);
    for (auto const& element: tableOf(harness))
        CHECK(Value { element } == typedNull);
}

TEST_CASE("Tables.brOnNullToALoopPolls")
{
    // Review Focus 2: br_on_null to a loop label, taken 100 times, polls at the loop's back-edge each time,
    // exactly where the reference does: (param i32 n) (local funcref r) loop: if n == 0 leave; n -= 1;
    // br_on_null r to the loop.
    auto const engine = interpreterOfEachDispatch();
    auto module = functionOf({ binary::I32 },
                             { binary::I32 },
                             { binary::FuncRef },
                             code({ { 0x02, 0x40 },
                                    { 0x03, 0x40 },
                                    localGet(0),
                                    op(0x45),
                                    brIf(1),
                                    localGet(0),
                                    i32Const(1),
                                    op(0x6B),
                                    op(0x21, 0),
                                    localGet(1),
                                    { 0xD5, 0x00 },
                                    op(0x1A),
                                    { 0x0B },
                                    { 0x0B },
                                    localGet(0) }));
    auto referenceCounter = PollCounter {};
    auto interpreterCounter = PollCounter {};
    auto reference = Harness { module, Reference, StoreLimits {}, &referenceCounter };
    auto harness = Harness { module, engine, StoreLimits {}, &interpreterCounter };
    REQUIRE(harness.code() != nullptr);
    CHECK(isoline::interpreter::describe(harness.code()->functions().front()).contains("br_on_null.loop"));
    CHECK(harness.result(0, { I32 { 100 } }) == reference.result(0, { I32 { 100 } }));
    for (auto const& row: PollSiteTable)
    {
        INFO(row.name);
        CHECK(interpreterCounter.count(row.site) == referenceCounter.count(row.site));
    }
    CHECK(interpreterCounter.count(PollSite::LoopBackEdge) == 100);
    // Unobserved, the steps are the same too.
    auto unobservedReference = Harness { module, Reference };
    auto unobserved = Harness { module, engine };
    CHECK(unobserved.result(0, { I32 { 100 } }) == unobservedReference.result(0, { I32 { 100 } }));
    CHECK(unobserved.store().used(Limit::Steps) == unobservedReference.store().used(Limit::Steps));
}

TEST_CASE("Tables.elementMaskAtTheEdges")
{
    // The Spectre mask of a table index: all ones exactly where the element exists, zero elsewhere, indices
    // near 2^32, 2^63 and 2^64 and a table of no elements included.
    struct Row
    {
        uint64_t index; ///< The index.
        uint64_t size;  ///< The table's elements.
        uint64_t mask;  ///< The mask expected.
    };
    constexpr auto All = std::numeric_limits<uint64_t>::max();
    constexpr auto Half = uint64_t { 1 } << 63U;
    constexpr auto Size = uint64_t { 10 };
    constexpr auto Table32 = uint64_t { 0xFFFF'FFFF };
    constexpr auto Rows = std::to_array<Row>({
        { .index = 0, .size = Size, .mask = All },
        { .index = Size - 1, .size = Size, .mask = All },
        { .index = Size, .size = Size, .mask = 0 },
        { .index = Size + 1, .size = Size, .mask = 0 },
        { .index = Table32, .size = Size, .mask = 0 },
        { .index = Table32 - 1, .size = Table32, .mask = All },
        { .index = Table32, .size = Table32, .mask = 0 },
        { .index = Table32, .size = Table32 + 1, .mask = All },
        { .index = Half - 1, .size = Size, .mask = 0 },
        { .index = Half, .size = Size, .mask = 0 },
        { .index = Half + Size, .size = Size, .mask = 0 },
        { .index = All, .size = Size, .mask = 0 },
        { .index = All - 1, .size = Size, .mask = 0 },
        { .index = 0, .size = 0, .mask = 0 },
        { .index = Half - 1, .size = 0, .mask = 0 },
        { .index = All, .size = 0, .mask = 0 },
        { .index = 0, .size = 1, .mask = All },
        { .index = 1, .size = 1, .mask = 0 },
    });
    for (auto const& row: Rows)
    {
        INFO("index " << row.index << ", size " << row.size);
        CHECK(elementMask(row.index, row.size) == row.mask);
        CHECK(isoline::interpreter::elementChecked(row.index, row.size).has_value() == (row.mask == All));
    }
    // Constant evaluation agrees, so the rows also pin what the compiler folds.
    static_assert(elementMask(Size - 1, Size) == All);
    static_assert(elementMask(All, Size) == 0);
    static_assert(elementMask(0, 0) == 0);
}

TEST_CASE("Tables.table32OperandsIgnoreTheSlotsHighBits")
{
    // Code written by hand reads table32 indices and deltas from slots holding i64s, high bits set: they are
    // read as their low 32 bits, and table.grow's result is written as 32 bits, its -1 included.
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto const table = TableType { .element = binary::FuncRef,
                                   .limits = { .min = 4, .max = 5, .addressType = AddressType::I32 } };
    auto const withTable = [&table](ModuleBuilder module) {
        module.table(table);
        // Active at 1: function 0.
        module.element(code({ { 0x00 }, i32Const(1), { 0x0B, 0x01, 0x00 } }));
        return module;
    };
    SECTION("table.get reads element 0xFFFF'FFFF'0000'0001 as 1")
    {
        auto const module = withTable(functionOf(
            { binary::I64 }, { binary::FuncRef }, {}, code({ localGet(0), op(0xA7), { 0x25, 0x00 } })));
        // By hand: table.get s1, s0, #0; return s1.
        auto const byHand = [dispatch](LoweredModule const& lowered) {
            REQUIRE(lowered.functions().front().frameSlots >= 3);
            return handWritten(lowered,
                               { headerWord(dispatch, OperationId::TableGet),
                                 indexPairWord(1, 0),
                                 0,
                                 headerWord(dispatch, OperationId::Return),
                                 1 });
        };
        auto harness = Harness { module, interpreterUnder(dispatch), byHand };
        auto const result = harness.result(0, { I64 { 0xFFFF'FFFF'0000'0001 } });
        CHECK(result
              == Value { Ref { .kind = RefKind::Func,
                               .heap = { .kind = AbstractHeapType::Func },
                               .address = harness.function(0) } });
        CHECK(harness.trap(0, { I64 { 0x0000'0001'0000'0004 } }) == TrapCode::OutOfBoundsTableAccess);
    }
    SECTION("call_indirect calls element 0xFFFF'FFFF'0000'0001 as 1")
    {
        // Function 0, of (i64) -> i32, calls element 1 of type 0, () -> i32: function 1, which returns 7.
        auto module = ModuleBuilder {};
        module.type(FuncType { .params = {}, .results = { binary::I32 } });
        module.type(FuncType { .params = { binary::I64 }, .results = { binary::I32 } });
        module.function(1, {}, code({ localGet(0), op(0xA7), { 0x11, 0x00, 0x00 } }));
        module.function(0, {}, i32Const(7));
        module.table(table);
        module.element(code({ { 0x00 }, i32Const(1), { 0x0B, 0x01, 0x01 } }));
        // By hand: call_indirect s1, s0, #0, #0; return s1.
        auto const byHand = [dispatch](LoweredModule const& lowered) {
            REQUIRE(lowered.functions().front().frameSlots >= 2);
            return handWritten(lowered,
                               { headerWord(dispatch, OperationId::CallIndirect),
                                 indexPairWord(1, 0),
                                 0,
                                 0,
                                 headerWord(dispatch, OperationId::Return),
                                 1 });
        };
        auto harness = Harness { module, interpreterUnder(dispatch), byHand };
        CHECK(harness.result(0, { I64 { 0xFFFF'FFFF'0000'0001 } }) == Value { I32 { 7 } });
        CHECK(harness.trap(0, { I64 { 0x0000'0001'0000'0004 } }) == TrapCode::UndefinedElement);
    }
    SECTION("table.copy from a table32 to a table64 reads its count as 32 bits")
    {
        // Table 0 of i64 addresses, table 1 of i32: the count is of the narrower type, i32.
        auto module = functionOf(
            { binary::I64, binary::I64, binary::I64 },
            {},
            {},
            code({ localGet(0), localGet(1), op(0xA7), localGet(2), op(0xA7), prefixed(14, { 0, 1 }) }));
        module.table(
            TableType { .element = binary::FuncRef,
                        .limits = { .min = 4, .max = std::nullopt, .addressType = AddressType::I64 } });
        module.table(table);
        // By hand: table.copy s0, s1, s2, #(0, 1); return s3.
        auto const byHand = [dispatch](LoweredModule const& lowered) {
            REQUIRE(lowered.functions().front().frameSlots >= 4);
            return handWritten(lowered,
                               { headerWord(dispatch, OperationId::TableCopy),
                                 indexPairWord(0, 1),
                                 2,
                                 indexPairWord(0, 1),
                                 headerWord(dispatch, OperationId::Return),
                                 3 });
        };
        auto harness = Harness { module, interpreterUnder(dispatch), byHand };
        CHECK(harness.outcome(0, { I64 { 0 }, I64 { 0xFFFF'FFFF'0000'0000 }, I64 { 0xFFFF'FFFF'0000'0004 } })
                  .has_value());
        CHECK(harness.trap(0, { I64 { 0 }, I64 { 0 }, I64 { 0x0000'0000'0000'0005 } })
              == TrapCode::OutOfBoundsTableAccess);
    }
    SECTION("table.fill and table.init read every operand as 32 bits")
    {
        // High bits planted in one operand at a time: read whole, any would be out of bounds. table.fill's
        // value is a funcref of two slots; table.init's source and count are i32s of its segment.
        constexpr auto High = uint64_t { 0xFFFF'FFFF'0000'0000 };
        auto const null =
            Value { Ref { .kind = RefKind::Null, .heap = { .kind = AbstractHeapType::Func }, .address = 0 } };
        {
            auto const module = withTable(functionOf(
                { binary::I64, binary::FuncRef, binary::I64 },
                {},
                {},
                code({ localGet(0), op(0xA7), localGet(1), localGet(2), op(0xA7), prefixed(17, { 0 }) })));
            // By hand: table.fill s0, s1, s3, #0; return s4.
            auto const byHand = [dispatch](LoweredModule const& lowered) {
                REQUIRE(lowered.functions().front().frameSlots >= 5);
                return handWritten(lowered,
                                   { headerWord(dispatch, OperationId::TableFill),
                                     indexPairWord(0, 1),
                                     3,
                                     0,
                                     headerWord(dispatch, OperationId::Return),
                                     4 });
            };
            auto harness = Harness { module, interpreterUnder(dispatch), byHand };
            CHECK(harness.outcome(0, { I64 { High | 1 }, null, I64 { 2 } }).has_value());
            CHECK(harness.outcome(0, { I64 { 1 }, null, I64 { High | 2 } }).has_value());
        }
        {
            auto module = withTable(functionOf({ binary::I64, binary::I64, binary::I64 },
                                               {},
                                               {},
                                               code({ localGet(0),
                                                      op(0xA7),
                                                      localGet(1),
                                                      op(0xA7),
                                                      localGet(2),
                                                      op(0xA7),
                                                      prefixed(12, { 1, 0 }) })));
            // Segment 1, passive: function 0 twice.
            module.element({ 0x01, 0x00, 0x02, 0x00, 0x00 });
            // By hand: table.init s0, s1, s2, #(1, 0); return s3.
            auto const byHand = [dispatch](LoweredModule const& lowered) {
                REQUIRE(lowered.functions().front().frameSlots >= 4);
                return handWritten(lowered,
                                   { headerWord(dispatch, OperationId::TableInit),
                                     indexPairWord(0, 1),
                                     2,
                                     indexPairWord(1, 0),
                                     headerWord(dispatch, OperationId::Return),
                                     3 });
            };
            auto harness = Harness { module, interpreterUnder(dispatch), byHand };
            CHECK(harness.outcome(0, { I64 { High | 1 }, I64 { 0 }, I64 { 2 } }).has_value());
            CHECK(harness.outcome(0, { I64 { 1 }, I64 { High }, I64 { 2 } }).has_value());
            CHECK(harness.outcome(0, { I64 { 1 }, I64 { 0 }, I64 { High | 2 } }).has_value());
        }
    }
    SECTION("table.grow reads its delta as 32 bits and writes -1 as 32 bits")
    {
        auto const module = withTable(
            functionOf({ binary::FuncRef, binary::I64 },
                       { binary::I64 },
                       {},
                       code({ localGet(0), localGet(1), op(0xA7), prefixed(15, { 0 }), op(0xAD) })));
        // By hand: table.grow s3, s0, s2, #0; return s3, the result read back whole as an i64.
        auto const byHand = [dispatch](LoweredModule const& lowered) {
            REQUIRE(lowered.functions().front().frameSlots >= 4);
            return handWritten(lowered,
                               { headerWord(dispatch, OperationId::TableGrow),
                                 indexPairWord(3, 0),
                                 2,
                                 0,
                                 headerWord(dispatch, OperationId::Return),
                                 3 });
        };
        auto harness = Harness { module, interpreterUnder(dispatch), byHand };
        auto const null =
            Value { Ref { .kind = RefKind::Null, .heap = { .kind = AbstractHeapType::Func }, .address = 0 } };
        CHECK(harness.result(0, { null, I64 { 0xFFFF'FFFF'0000'0001 } }) == Value { I64 { 4 } });
        CHECK(harness.result(0, { null, I64 { 0xFFFF'FFFF'0000'0001 } }) == Value { I64 { 0xFFFF'FFFF } });
    }
}

TEST_CASE("Tables.importedTablesComeFirst")
{
    // Table 0 is imported, of i64 addresses and 2 elements; table 1 the module's own, of i32 addresses and 3:
    // table.size and table.grow take each table's own address type, the imported one's first.
    auto const engine = interpreterOfEachDispatch();
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = {}, .results = { binary::I64, binary::I32, binary::I64 } });
    module.importTable(
        "env",
        "table",
        TableType { .element = binary::FuncRef,
                    .limits = { .min = 2, .max = std::nullopt, .addressType = AddressType::I64 } });
    module.table(TableType { .element = binary::FuncRef,
                             .limits = { .min = 3, .max = std::nullopt, .addressType = AddressType::I32 } });
    module.function(0,
                    {},
                    code({ prefixed(16, { 0 }),
                           prefixed(16, { 1 }),
                           { 0xD0, 0x70 },
                           { 0x42, 0x01 },
                           prefixed(15, { 0 }) }));
    auto const imports = [](isoline::runtime::Store& store) {
        auto const table = store.addTable(
            TableType { .element = binary::FuncRef,
                        .limits = { .min = 2, .max = std::nullopt, .addressType = AddressType::I64 } },
            Ref { .kind = RefKind::Null, .heap = { .kind = AbstractHeapType::Func }, .address = 0 });
        REQUIRE(table.has_value());
        return std::vector { isoline::runtime::ExternValue { .kind = binary::ExternKind::Table,
                                                             .address = *table } };
    };
    auto reference = Harness { module, Reference, imports };
    auto harness = Harness { module, engine, imports };
    auto const expected = std::vector<Value> { I64 { 2 }, I32 { 3 }, I64 { 2 } };
    auto const results = harness.outcome(0);
    REQUIRE(results.has_value());
    CHECK(*results == expected);
    auto const referenceResults = reference.outcome(0);
    REQUIRE(referenceResults.has_value());
    CHECK(*referenceResults == expected);
}
