// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/translation/MemoryTranslation.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/execution/RuntimeStubs.hpp>
#include <isoline/execution/testing/CompileOne.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

using isoline::compiler::ExecutionState;
using isoline::compiler::FloatRoundOperation;
using isoline::compiler::InstanceContext;
using isoline::compiler::PendingKind;
using isoline::compiler::RuntimeStub;
using isoline::compiler::StubSucceeded;
using isoline::execution::operandCountOf;
using isoline::execution::rowOf;
using isoline::execution::RuntimeStubTable;
using isoline::execution::Settles;
using isoline::execution::StubEntry;
using isoline::execution::StubStatus;
using isoline::execution::testing::callStub;
using isoline::execution::testing::CompiledInstance;
using isoline::execution::testing::hostConfig;
using isoline::execution::testing::hostRunsCompiledCode;
using isoline::execution::testing::InstanceOptions;
using isoline::execution::testing::moduleOf;
using isoline::execution::testing::runReference;
using isoline::runtime::Abrupt;
using isoline::runtime::BudgetState;
using isoline::runtime::HostFailure;
using isoline::runtime::PollSite;
using isoline::runtime::StoreLimits;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::PollCounter;

namespace
{

constexpr auto Deterministic = isoline::runtime::ExecutionMode::Deterministic;

/// A store with a memory of one page, a table of 4 slots, a passive data and a passive element segment.
constexpr auto StubModule = std::string_view { R"((module
  (memory 1)
  (table 4 funcref)
  (data "abcdefgh")
  (elem func $f)
  (func $f))
)" };

/// The status word of a stub that left its Abrupt pending.
constexpr auto Pending = uint64_t { std::to_underlying(StubStatus::Pending) };

/// An instance of @p wat for calling stubs directly.
std::unique_ptr<CompiledInstance> instanceOf(std::string_view wat, InstanceOptions options = {})
{
    auto created = CompiledInstance::create(moduleOf(wat), hostConfig(Deterministic, {}), std::move(options));
    REQUIRE(created.has_value());
    return std::move(*created);
}

/// Calls @p stub with @p words within @p instance's lent budget, then checks the lease is lent again.
/// @return The stub's result.
isoline::compiler::StubResult runStub(CompiledInstance& instance,
                                      RuntimeStub stub,
                                      std::vector<uint64_t> const& words)
{
    auto result = isoline::compiler::StubResult {};
    auto budget = BudgetState::Settled;
    instance.lent([&](ExecutionState& state, InstanceContext const& context) {
        result = callStub(rowOf(stub).entry, state, context, words);
        budget = instance.store().budgetState();
    });
    // Whatever the stub returned, generated code goes on (or leaves) under a lease.
    CHECK(budget == BudgetState::Lent);
    return result;
}

/// The trap @p instance's record holds for the stub that returned Pending last.
std::optional<TrapCode> pendingTrapOf(CompiledInstance& instance)
{
    auto const& abrupt = instance.record().abrupt;
    if (!abrupt)
        return std::nullopt;
    auto const* trap = std::get_if<Trap>(&*abrupt);
    return trap != nullptr ? std::optional { trap->code } : std::nullopt;
}

} // namespace

TEST_CASE("RuntimeStubs.tableIsComplete")
{
    // One row per RuntimeStub, each taking the operands translation passes it (MemoryTranslation's
    // StubCallTable: its indices, then its WebAssembly operands; PollSlow its site, CallHost the function,
    // FloatRound the operation and the bits).
    auto names = std::set<std::string_view> {};
    auto entries = std::set<void const*> {};
    for (auto const& row: RuntimeStubTable)
    {
        CAPTURE(row.name);
        CHECK(!row.name.empty());
        names.insert(row.name);
        auto const* entry = isoline::execution::entryAddress(row.entry);
        CHECK(entry != nullptr);
        entries.insert(entry);
        CHECK(isoline::execution::stubEntries()[static_cast<std::size_t>(row.stub)] == entry);
        auto const call = std::ranges::find(isoline::compiler::translation::StubCallTable,
                                            row.stub,
                                            &isoline::compiler::translation::StubCallRow::stub);
        auto expected = std::size_t { 0 };
        if (call != isoline::compiler::translation::StubCallTable.end())
            expected = isoline::compiler::translation::indexCountOf(call->indices) + call->operands;
        else if (row.stub == RuntimeStub::FloatRound)
            expected = 2;
        else
        {
            CHECK((row.stub == RuntimeStub::PollSlow || row.stub == RuntimeStub::CallHost));
            expected = 1;
        }
        CHECK(operandCountOf(row.entry) == expected);
    }
    CHECK(RuntimeStubTable.size() == static_cast<std::size_t>(isoline::compiler::LastRuntimeStub) + 1);
    CHECK(names.size() == RuntimeStubTable.size());
    CHECK(entries.size() == RuntimeStubTable.size());
    CHECK(isoline::execution::stubEntries().size() == RuntimeStubTable.size());
    // What settles is what polls, charges or calls out.
    for (auto const stub: { RuntimeStub::PollSlow,
                            RuntimeStub::MemoryFill,
                            RuntimeStub::MemoryCopy,
                            RuntimeStub::MemoryInit,
                            RuntimeStub::TableFill,
                            RuntimeStub::TableCopy,
                            RuntimeStub::TableInit,
                            RuntimeStub::CallHost })
        CHECK(rowOf(stub).settles == Settles::Yes);
    CHECK(rowOf(RuntimeStub::FloatRound).settles == Settles::No);
}

TEST_CASE("RuntimeStubs.boundaryTypesAreWordSized")
{
    // (amended for P6, 2026-11-26: I8) every stub parameter and result is a 32/64-bit integer or a pointer,
    // and each is a non-member function: what compiler::x86_64::HostBoundaryFunction states.
    using isoline::compiler::x86_64::HostBoundaryFunction;
    STATIC_CHECK(HostBoundaryFunction<isoline::execution::StubFunction<0>>);
    STATIC_CHECK(HostBoundaryFunction<isoline::execution::StubFunction<1>>);
    STATIC_CHECK(HostBoundaryFunction<isoline::execution::StubFunction<2>>);
    STATIC_CHECK(HostBoundaryFunction<isoline::execution::StubFunction<3>>);
    STATIC_CHECK(HostBoundaryFunction<isoline::execution::StubFunction<4>>);
    STATIC_CHECK(HostBoundaryFunction<isoline::execution::StubFunction<5>>);
    STATIC_CHECK(HostBoundaryFunction<isoline::compiler::EntryTrampoline>);
    STATIC_CHECK(std::variant_size_v<StubEntry> == 6);
    for (auto const& row: RuntimeStubTable)
        CHECK(std::visit([](auto function) { return HostBoundaryFunction<decltype(function)>; }, row.entry));
    STATIC_CHECK(sizeof(isoline::compiler::StubResult) == 16);
}

TEST_CASE("RuntimeStubs.trappingStubsReturn")
{
    SECTION("an out-of-bounds memory.fill returns Pending, writes nothing and lends the budget again")
    {
        auto instance = instanceOf(StubModule);
        auto const result = runStub(*instance, RuntimeStub::MemoryFill, { 0, 65'000, 1, 1'000 });
        CHECK(result.status == Pending);
        CHECK(pendingTrapOf(*instance) == TrapCode::OutOfBoundsMemoryAccess);
        CHECK(instance->state().pending.kind == PendingKind::StubAbrupt);
        auto const bytes = instance->store().memory(instance->store().moduleInstance(0).memories[0]).bytes();
        CHECK(std::ranges::all_of(bytes, [](std::byte byte) { return byte == std::byte { 0 }; }));
        CHECK(instance->store().budgetState() == BudgetState::Settled);
        CHECK(instance->store().chargedStackValues() == 0);
    }
    SECTION("an out-of-bounds table.get and table.set, a dropped segment's init")
    {
        auto instance = instanceOf(StubModule);
        CHECK(runStub(*instance, RuntimeStub::TableGet, { 0, 4 }).status == Pending);
        CHECK(pendingTrapOf(*instance) == TrapCode::OutOfBoundsTableAccess);
        CHECK(runStub(*instance, RuntimeStub::TableSet, { 0, 4, 0 }).status == Pending);
        CHECK(pendingTrapOf(*instance) == TrapCode::OutOfBoundsTableAccess);
        CHECK(runStub(*instance, RuntimeStub::DataDrop, { 0 }).status == StubSucceeded);
        CHECK(runStub(*instance, RuntimeStub::MemoryInit, { 0, 0, 0, 0, 1 }).status == Pending);
        CHECK(pendingTrapOf(*instance) == TrapCode::OutOfBoundsMemoryAccess);
        CHECK(runStub(*instance, RuntimeStub::ElementDrop, { 0 }).status == StubSucceeded);
        CHECK(runStub(*instance, RuntimeStub::TableInit, { 0, 0, 0, 0, 1 }).status == Pending);
        CHECK(pendingTrapOf(*instance) == TrapCode::OutOfBoundsTableAccess);
        CHECK(runStub(*instance, RuntimeStub::MemoryCopy, { 0, 0, 65'000, 0, 1'000 }).status == Pending);
        CHECK(runStub(*instance, RuntimeStub::TableCopy, { 0, 0, 3, 0, 2 }).status == Pending);
        CHECK(runStub(*instance, RuntimeStub::TableFill, { 0, 3, 0, 2 }).status == Pending);
        CHECK(pendingTrapOf(*instance) == TrapCode::OutOfBoundsTableAccess);
    }
    SECTION("compiled code: a failing host and an out-of-bounds fill end the call, nothing left charged")
    {
        if (!hostRunsCompiledCode())
            SKIP("the code is x86-64's");
        constexpr auto Failing = std::string_view { R"((module
  (import "host" "fail" (func $fail (param i32) (result i32)))
  (memory 1)
  (func (export "host") (result i32) (call $fail (i32.const 5)))
  (func (export "fill") (memory.fill (i32.const 65000) (i32.const 1) (i32.const 1000))))
)" };
        auto received = std::vector<Value> {};
        auto options = InstanceOptions {};
        options.imports = [&](isoline::runtime::Store& store) {
            auto const address = store.addHostFunction(
                isoline::binary::FuncType { .params = { isoline::binary::I32 },
                                            .results = { isoline::binary::I32 } },
                isoline::runtime::makeHostFunction([&](isoline::runtime::HostContext& /*context*/,
                                                       std::span<Value const> arguments,
                                                       std::span<Value> /*results*/) noexcept
                                                       -> std::expected<void, Abrupt> {
                    received.assign(arguments.begin(), arguments.end());
                    return std::unexpected { Abrupt { HostFailure { .code = 7, .mismatch = std::nullopt } } };
                }));
            REQUIRE(address.has_value());
            return std::vector { isoline::runtime::ExternValue { .kind = isoline::binary::ExternKind::Func,
                                                                 .address = *address } };
        };
        auto instance = instanceOf(Failing, std::move(options));
        auto const host = instance->call(0, {});
        REQUIRE_FALSE(host.has_value());
        CHECK(host.error() == Abrupt { HostFailure { .code = 7, .mismatch = std::nullopt } });
        CHECK(received == std::vector<Value> { isoline::runtime::I32 { .bits = 5 } });
        CHECK(instance->store().chargedStackValues() == 0);
        CHECK(instance->store().budgetState() == BudgetState::Settled);
        auto const fill = instance->call(1, {});
        REQUIRE_FALSE(fill.has_value());
        CHECK(fill.error() == Abrupt { Trap { TrapCode::OutOfBoundsMemoryAccess } });
        CHECK(instance->store().chargedStackValues() == 0);
    }
}

TEST_CASE("RuntimeStubs.pollSlowKeepsTheOrder")
{
    auto const site = std::vector<uint64_t> { std::to_underlying(PollSite::LoopBackEdge) };
    // Generated code that took every step lent has a countdown of 0, which sends its next poll here.
    auto const exhausted = [&](CompiledInstance& instance) {
        auto status = uint64_t { 0 };
        instance.lent([&](ExecutionState& state, InstanceContext const& context) {
            state.stepsLeft = 0;
            status = callStub(rowOf(RuntimeStub::PollSlow).entry, state, context, site).status;
        });
        return status;
    };
    SECTION("lent 0 steps and interrupted: Interrupted, not the step limit")
    {
        auto instance = instanceOf(StubModule, InstanceOptions { .limits = StoreLimits { .maxSteps = 10 } });
        instance->store().interrupter()->interrupt();
        CHECK(exhausted(*instance) == Pending);
        CHECK(pendingTrapOf(*instance) == TrapCode::Interrupted);
    }
    SECTION("lent 0 steps: the step limit, no step charged past it")
    {
        auto instance = instanceOf(StubModule, InstanceOptions { .limits = StoreLimits { .maxSteps = 10 } });
        CHECK(exhausted(*instance) == Pending);
        CHECK(pendingTrapOf(*instance) == TrapCode::StepLimitExceeded);
        CHECK(instance->store().used(isoline::runtime::Limit::Steps) == 10);
    }
    SECTION("observed: through the store, a step charged, the countdown lent again")
    {
        auto polls = PollCounter {};
        auto instance = instanceOf(StubModule, InstanceOptions { .observer = &polls });
        auto stepsLent = uint64_t { 1 };
        instance->lent([&](ExecutionState& state, InstanceContext const& context) {
            CHECK(state.stepsLeft == 0); // an observed store lends no steps
            CHECK(callStub(rowOf(RuntimeStub::PollSlow).entry, state, context, site).status == StubSucceeded);
            stepsLent = state.stepsLeft;
        });
        CHECK(stepsLent == 0);
        CHECK(polls.count(PollSite::LoopBackEdge) == 1);
        CHECK(instance->store().used(isoline::runtime::Limit::Steps) == 1);
    }
    SECTION("unobserved: the countdown settled, charged and lent again")
    {
        auto instance = instanceOf(StubModule, InstanceOptions { .limits = StoreLimits { .maxSteps = 10 } });
        auto steps = std::pair<uint64_t, uint64_t> {};
        instance->lent([&](ExecutionState& state, InstanceContext const& context) {
            state.stepsLeft = 6; // generated code took 4
            CHECK(callStub(rowOf(RuntimeStub::PollSlow).entry, state, context, site).status == StubSucceeded);
            steps = { state.stepsLeft, instance->store().used(isoline::runtime::Limit::Steps) };
        });
        CHECK(steps == std::pair<uint64_t, uint64_t> { 5, 5 });
    }
}

TEST_CASE("RuntimeStubs.bulkFillPollsPerGranule")
{
    // A 1 MiB fill polls 16 times at BulkGranule, once per 64 KiB, as the reference does.
    constexpr auto Fill = std::string_view { R"((module
  (memory 16)
  (func (export "fill") (memory.fill (i32.const 0) (i32.const 7) (i32.const 1048576))))
)" };
    auto const module = moduleOf(Fill);
    auto referencePolls = PollCounter {};
    auto const reference = runReference(module, 0, {}, InstanceOptions { .observer = &referencePolls });
    REQUIRE(reference.outcome.has_value());
    CHECK(referencePolls.count(PollSite::BulkGranule) == 16);
    SECTION("the stub")
    {
        auto polls = PollCounter {};
        auto instance = instanceOf(Fill, InstanceOptions { .observer = &polls });
        CHECK(runStub(*instance, RuntimeStub::MemoryFill, { 0, 0, 7, 1'048'576 }).status == StubSucceeded);
        CHECK(polls.count(PollSite::BulkGranule) == referencePolls.count(PollSite::BulkGranule));
        auto const bytes = instance->store().memory(instance->store().moduleInstance(0).memories[0]).bytes();
        CHECK(std::ranges::count(bytes, std::byte { 7 }) == 1'048'576);
    }
    SECTION("compiled code: the same polls and steps as the reference")
    {
        if (!hostRunsCompiledCode())
            SKIP("the code is x86-64's");
        auto polls = PollCounter {};
        auto instance = instanceOf(Fill, InstanceOptions { .observer = &polls });
        REQUIRE(instance->call(0, {}).has_value());
        CHECK(polls.count(PollSite::BulkGranule) == referencePolls.count(PollSite::BulkGranule));
        CHECK(polls.count(PollSite::FunctionEntry) == referencePolls.count(PollSite::FunctionEntry));
        CHECK(instance->store().used(isoline::runtime::Limit::Steps) == reference.steps);
    }
}

TEST_CASE("RuntimeStubs.doTheirWork")
{
    auto instance = instanceOf(
        StubModule, InstanceOptions { .limits = StoreLimits { .maxMemoryBytes = uint64_t { 2 } * 65'536 } });
    auto& store = instance->store();
    auto const memory = store.moduleInstance(0).memories[0];
    auto const table = store.moduleInstance(0).tables[0];
    SECTION("grows: the old size, or all ones once refused")
    {
        CHECK(runStub(*instance, RuntimeStub::MemoryGrow, { 0, 1 }).value == 1);
        CHECK(runStub(*instance, RuntimeStub::MemoryGrow, { 0, 1 }).value == ~uint64_t { 0 });
        CHECK(store.memory(memory).bytes().size() == std::size_t { 2 } * 65'536);
        CHECK(runStub(*instance, RuntimeStub::TableGrow, { 0, 0, 2 }).value == 4);
        CHECK(store.table(table).elements().size() == 6);
        CHECK(runStub(*instance, RuntimeStub::TableGrow, { 0, 0, uint64_t { 1 } << 40U }).value
              == ~uint64_t { 0 });
    }
    SECTION("memory: init, copy and fill write what the reference writes")
    {
        CHECK(runStub(*instance, RuntimeStub::MemoryInit, { 0, 0, 10, 2, 4 }).status == StubSucceeded);
        CHECK(runStub(*instance, RuntimeStub::MemoryCopy, { 0, 0, 20, 10, 4 }).status == StubSucceeded);
        CHECK(runStub(*instance, RuntimeStub::MemoryFill, { 0, 30, 0x41, 2 }).status == StubSucceeded);
        auto const bytes = store.memory(memory).bytes();
        auto const text = [&](std::size_t offset, std::size_t size) {
            auto const range = bytes.subspan(offset, size);
            return std::string { reinterpret_cast<char const*>(range.data()), range.size() };
        };
        CHECK(text(10, 4) == "cdef");
        CHECK(text(20, 4) == "cdef");
        CHECK(text(30, 2) == "AA");
    }
    SECTION("tables: init, set, get, copy and fill keep a Ref's word")
    {
        auto const function = store.moduleInstance(0).functions[0];
        auto const word =
            uint64_t { std::to_underlying(isoline::runtime::RefKind::Func) } | (uint64_t { function } << 32U);
        CHECK(runStub(*instance, RuntimeStub::TableInit, { 0, 0, 1, 0, 1 }).status == StubSucceeded);
        CHECK(runStub(*instance, RuntimeStub::TableGet, { 0, 1 }).value == word);
        CHECK(runStub(*instance, RuntimeStub::TableGet, { 0, 0 }).value == 0);
        CHECK(runStub(*instance, RuntimeStub::TableSet, { 0, 0, word }).status == StubSucceeded);
        CHECK(runStub(*instance, RuntimeStub::TableCopy, { 0, 0, 2, 0, 2 }).status == StubSucceeded);
        CHECK(runStub(*instance, RuntimeStub::TableGet, { 0, 3 }).value == word);
        CHECK(runStub(*instance, RuntimeStub::TableFill, { 0, 0, 0, 4 }).status == StubSucceeded);
        CHECK(runStub(*instance, RuntimeStub::TableGet, { 0, 3 }).value == 0);
    }
    SECTION("FloatRound: numerics:: for every operation")
    {
        // Values on which the four roundings differ pairwise between them: ties both ways, and fractions
        // below and above a half, of either sign.
        namespace numerics = isoline::numerics;
        using F32 = isoline::runtime::F32;
        using F64 = isoline::runtime::F64;
        struct Kernel
        {
            FloatRoundOperation operation;
            uint64_t (*round)(double value);
        };
        constexpr auto Kernels = std::to_array<Kernel>({
            { FloatRoundOperation::F32Ceil,
              [](double value) -> uint64_t {
                  return numerics::ceil(F32 { std::bit_cast<uint32_t>(static_cast<float>(value)) }).bits;
              } },
            { FloatRoundOperation::F32Floor,
              [](double value) -> uint64_t {
                  return numerics::floor(F32 { std::bit_cast<uint32_t>(static_cast<float>(value)) }).bits;
              } },
            { FloatRoundOperation::F32Trunc,
              [](double value) -> uint64_t {
                  return numerics::trunc(F32 { std::bit_cast<uint32_t>(static_cast<float>(value)) }).bits;
              } },
            { FloatRoundOperation::F32Nearest,
              [](double value) -> uint64_t {
                  return numerics::nearest(F32 { std::bit_cast<uint32_t>(static_cast<float>(value)) }).bits;
              } },
            { FloatRoundOperation::F64Ceil,
              [](double value) -> uint64_t {
                  return numerics::ceil(F64 { std::bit_cast<uint64_t>(value) }).bits;
              } },
            { FloatRoundOperation::F64Floor,
              [](double value) -> uint64_t {
                  return numerics::floor(F64 { std::bit_cast<uint64_t>(value) }).bits;
              } },
            { FloatRoundOperation::F64Trunc,
              [](double value) -> uint64_t {
                  return numerics::trunc(F64 { std::bit_cast<uint64_t>(value) }).bits;
              } },
            { FloatRoundOperation::F64Nearest,
              [](double value) -> uint64_t {
                  return numerics::nearest(F64 { std::bit_cast<uint64_t>(value) }).bits;
              } },
        });
        for (auto const& [operation, round]: Kernels)
            for (auto const value: { 2.5, -2.5, 1.5, -1.5, 1.25, -1.75 })
            {
                auto const isSingle =
                    std::to_underlying(operation) < std::to_underlying(FloatRoundOperation::F64Ceil);
                auto const bits = isSingle ? uint64_t { std::bit_cast<uint32_t>(static_cast<float>(value)) }
                                           : std::bit_cast<uint64_t>(value);
                CAPTURE(std::to_underlying(operation), value);
                CHECK(
                    runStub(*instance, RuntimeStub::FloatRound, { std::to_underlying(operation), bits }).value
                    == round(value));
            }
    }
}

TEST_CASE("RuntimeStubs.callHostPollsAsTheReference")
{
    // A host call from compiled code: the callee's FunctionEntry poll, then the host frame and its HostReturn
    // poll, its arguments and results through the scratch, the steps the reference takes.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Calling = std::string_view { R"((module
  (import "host" "twice" (func $twice (param i64 f32) (result f32 i64)))
  (func (param i64) (result i64)
    (call $twice (local.get 0) (f32.const 1.5))
    (call $twice (i64.const 3) (f32.const 0))
    drop drop (local.set 0) drop (local.get 0)))
)" };
    auto const importsOf = [](isoline::runtime::Store& store) {
        auto const address = store.addHostFunction(
            isoline::binary::FuncType { .params = { isoline::binary::I64, isoline::binary::F32 },
                                        .results = { isoline::binary::F32, isoline::binary::I64 } },
            isoline::runtime::makeHostFunction(
                [](isoline::runtime::HostContext& /*context*/,
                   std::span<Value const> arguments,
                   std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                    results[0] = arguments[1];
                    results[1] =
                        isoline::runtime::I64 { .bits =
                                                    std::get<isoline::runtime::I64>(arguments[0]).bits * 2 };
                    return {};
                }));
        REQUIRE(address.has_value());
        return std::vector { isoline::runtime::ExternValue { .kind = isoline::binary::ExternKind::Func,
                                                             .address = *address } };
    };
    auto const arguments = std::vector<Value> { isoline::runtime::I64 { .bits = 21 } };
    auto referencePolls = PollCounter {};
    auto const reference =
        runReference(moduleOf(Calling),
                     0,
                     arguments,
                     InstanceOptions { .observer = &referencePolls, .imports = importsOf });
    REQUIRE(reference.outcome.has_value());
    SECTION("interrupted: the callee's entry poll ends the call before the host runs")
    {
        auto calls = 0;
        auto options = InstanceOptions { .interruption = isoline::execution::testing::Interruption::Pending };
        options.imports = [&](isoline::runtime::Store& store) {
            auto const address = store.addHostFunction(
                isoline::binary::FuncType {},
                isoline::runtime::makeHostFunction(
                    [&](isoline::runtime::HostContext& /*context*/,
                        std::span<Value const> /*arguments*/,
                        std::span<Value> /*results*/) noexcept -> std::expected<void, Abrupt> {
                        ++calls;
                        return {};
                    }));
            REQUIRE(address.has_value());
            return std::vector { isoline::runtime::ExternValue { .kind = isoline::binary::ExternKind::Func,
                                                                 .address = *address } };
        };
        auto instance = instanceOf(R"((module (import "host" "none" (func))))", std::move(options));
        auto const host = instance->store().moduleInstance(instance->context().moduleInstance).functions[0];
        CHECK(runStub(*instance, RuntimeStub::CallHost, { host }).status == Pending);
        CHECK(pendingTrapOf(*instance) == TrapCode::Interrupted);
        CHECK(calls == 0);
        CHECK(instance->store().chargedStackValues() == 0);
    }
    SECTION("observed: every poll through the store")
    {
        auto polls = PollCounter {};
        auto instance = instanceOf(Calling, InstanceOptions { .observer = &polls, .imports = importsOf });
        auto const called = instance->call(0, arguments);
        REQUIRE(called.has_value());
        CHECK(*called == *reference.outcome);
        for (auto const site: { PollSite::FunctionEntry, PollSite::HostReturn })
            CHECK(polls.count(site) == referencePolls.count(site));
        CHECK(instance->store().used(isoline::runtime::Limit::Steps) == reference.steps);
    }
    SECTION("unobserved: the countdown lent, the same steps")
    {
        auto const unobserved =
            runReference(moduleOf(Calling), 0, arguments, InstanceOptions { .imports = importsOf });
        auto instance = instanceOf(Calling, InstanceOptions { .imports = importsOf });
        auto const called = instance->call(0, arguments);
        REQUIRE(called.has_value());
        CHECK(*called == std::vector<Value> { isoline::runtime::I64 { .bits = 42 } });
        CHECK(instance->store().used(isoline::runtime::Limit::Steps) == unobserved.steps);
        CHECK(instance->store().chargedStackValues() == 0);
    }
}
