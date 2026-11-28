// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/reference/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostCall.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

using isoline::binary::FuncType;
using isoline::reference::ReferenceInterpreter;
using isoline::runtime::Abrupt;
using isoline::runtime::callHostFunction;
using isoline::runtime::ExnAddr;
using isoline::runtime::FuncAddr;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::HostCallable;
using isoline::runtime::HostContext;
using isoline::runtime::hostFrameCost;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::InvocationScope;
using isoline::runtime::makeHostFunction;
using isoline::runtime::PollSite;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::PollCounter;

namespace
{

/// What a host function returns.
using Outcome = std::expected<void, Abrupt>;

/// A store of @p limits whose polls a PollCounter counts, with an interpreter to execute it.
struct Fixture
{
    explicit Fixture(StoreLimits limits = {}):
        store { limits, StoreServices { .allocator = allocator, .pollObserver = &polls } }
    {
    }

    /// Adds a host function of @p type implemented by @p function.
    template <HostCallable F>
    FuncAddr host(FuncType type, F function)
    {
        auto const address = store.addHostFunction(std::move(type), makeHostFunction(std::move(function)));
        REQUIRE(address.has_value());
        return *address;
    }

    /// Calls the host function at @p function through callHostFunction(), the embedder its caller.
    Outcome call(FuncAddr function, std::span<Value const> args, std::span<Value> results)
    {
        auto const scope = InvocationScope { store };
        return callHostFunction(store, interpreter, store.function(function), std::nullopt, args, results);
    }

    HeapMemoryAllocator allocator;
    PollCounter polls;
    Store store;
    ReferenceInterpreter interpreter;
};

/// An exnref to the exception at @p address.
Ref exnRef(ExnAddr address)
{
    return Ref { .kind = RefKind::Exn, .heap = isoline::binary::ExnRef.heap, .address = address };
}

/// A host type of one i32 param and two results, an i32 and an i64: a frame cost of 4.
FuncType twoResults()
{
    return FuncType { .params = { isoline::binary::I32 },
                      .results = { isoline::binary::I32, isoline::binary::I64 } };
}

} // namespace

TEST_CASE("HostCall.ordersChargeCallAndReturnPoll")
{
    auto fixture = Fixture {};
    auto& store = fixture.store;
    auto chargedDuringCall = std::optional<uint64_t> {};
    auto hostCallsDuringCall = uint32_t { 0 };
    auto returnPollsDuringCall = std::optional<uint64_t> {};
    auto resultsOnEntry = std::vector<Value> {};
    auto interruptInside = false;
    auto const function = fixture.host(
        twoResults(),
        [&](HostContext& context, std::span<Value const> args, std::span<Value> results) noexcept -> Outcome {
            chargedDuringCall = context.store().chargedStackValues();
            hostCallsDuringCall = context.store().hostCallsInProgress();
            returnPollsDuringCall = fixture.polls.count(PollSite::HostReturn);
            resultsOnEntry.assign(results.begin(), results.end());
            if (interruptInside)
                store.interrupter()->interrupt();
            results[0] = args[0];
            results[1] = I64 { .bits = 9 };
            return {};
        });
    auto const args = std::vector<Value> { I32 { .bits = 5 } };
    auto results = std::vector<Value>(2);

    SECTION("charged while it runs, released after, polled at its return")
    {
        auto const outcome = fixture.call(function, args, results);
        REQUIRE(outcome.has_value());
        CHECK(chargedDuringCall == hostFrameCost(twoResults()));
        CHECK(hostCallsDuringCall == 1);
        CHECK(returnPollsDuringCall == 0);
        CHECK(resultsOnEntry == std::vector<Value> { I32 {}, I64 {} });
        CHECK(store.chargedStackValues() == 0);
        CHECK(store.hostCallsInProgress() == 0);
        CHECK(fixture.polls.count(PollSite::HostReturn) == 1);
        CHECK(results == std::vector<Value> { I32 { .bits = 5 }, I64 { .bits = 9 } });
    }

    SECTION("an interrupt raised inside the host ends the call at its return poll")
    {
        interruptInside = true;
        auto const outcome = fixture.call(function, args, results);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error() == Abrupt { Trap { TrapCode::Interrupted } });
        CHECK(store.chargedStackValues() == 0);
    }

    SECTION("a frame that does not fit runs nothing")
    {
        auto small = Fixture { StoreLimits { .maxStackValues = hostFrameCost(twoResults()) - 1 } };
        auto calls = 0;
        auto const tooWide = small.host(twoResults(), [&](HostContext&, auto, auto) noexcept -> Outcome {
            ++calls;
            return {};
        });
        auto const outcome = small.call(tooWide, args, results);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error() == Abrupt { Trap { TrapCode::CallStackExhausted } });
        CHECK(calls == 0);
        CHECK(small.polls.count(PollSite::HostReturn) == 0);
    }
}

TEST_CASE("HostCall.marksArgumentsEscaped")
{
    // An exnref handed to a host survives 20 rounds of dropped exceptions in a store of 16: the host
    // may keep it past its frame, so the call escapes it.
    auto fixture = Fixture { StoreLimits { .maxExceptions = 16 } };
    auto& store = fixture.store;
    auto const tag =
        store.addTag(store.internType(FuncType { .params = { isoline::binary::I32 }, .results = {} }))
            .value();
    auto const handed = store.addException(tag, { I32 { .bits = 42 } });
    REQUIRE(handed.has_value());
    auto const function = fixture.host(FuncType { .params = { isoline::binary::ExnRef }, .results = {} },
                                       [](HostContext&, auto, auto) noexcept -> Outcome { return {}; });
    auto const args = std::vector<Value> { exnRef(*handed) };
    REQUIRE(fixture.call(function, args, {}).has_value());
    for ([[maybe_unused]] auto const round: std::views::iota(0, 20))
        REQUIRE(store.addException(tag, { I32 { .bits = 7 } }).has_value());
    CHECK(store.isLiveException(*handed));
    CHECK(store.exception(*handed).payload == std::vector<Value> { I32 { .bits = 42 } });
}

// The contract checks: each ends the process on purpose, so the unit run skips them and CTest runs
// each alone (tests/CMakeLists.txt).

TEST_CASE("Contract.HostCall.onlyHostFunctions", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto fixture = Fixture {};
    auto module = fixture.store.function(
        fixture.host(FuncType {}, [](HostContext&, auto, auto) noexcept -> Outcome { return {}; }));
    module.host = nullptr;
    std::ignore = callHostFunction(fixture.store, fixture.interpreter, module, std::nullopt, {}, {});
    FAIL("SURVIVED: a module function called as a host function");
}

TEST_CASE("Contract.HostCall.argumentsAsWideAsTheParams", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto fixture = Fixture {};
    auto const function =
        fixture.host(twoResults(), [](HostContext&, auto, auto) noexcept -> Outcome { return {}; });
    auto results = std::vector<Value>(2);
    std::ignore = fixture.call(function, {}, results);
    FAIL("SURVIVED: a host call with too few arguments");
}

TEST_CASE("Contract.HostCall.resultsAsWideAsTheResults", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto fixture = Fixture {};
    auto const function =
        fixture.host(twoResults(), [](HostContext&, auto, auto) noexcept -> Outcome { return {}; });
    auto const args = std::vector<Value> { I32 { .bits = 5 } };
    auto results = std::vector<Value>(1);
    std::ignore = fixture.call(function, args, results);
    FAIL("SURVIVED: a host call with too few result slots");
}
