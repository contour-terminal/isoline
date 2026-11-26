// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::binary::FuncType;
using isoline::interpreter::ReferenceInterpreter;
using isoline::runtime::Abrupt;
using isoline::runtime::checkHostOutcome;
using isoline::runtime::Entry;
using isoline::runtime::ExnAddr;
using isoline::runtime::F32;
using isoline::runtime::FuncAddr;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::HostCallable;
using isoline::runtime::HostCallScope;
using isoline::runtime::HostContext;
using isoline::runtime::HostFailure;
using isoline::runtime::HostFailureKind;
using isoline::runtime::I32;
using isoline::runtime::invokeChecked;
using isoline::runtime::makeHostFunction;
using isoline::runtime::MismatchKind;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::Thrown;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::ValueMismatch;

namespace
{

constexpr auto TypeI32 = isoline::binary::I32;

/// What a host function returns.
using Outcome = std::expected<void, Abrupt>;

/// A host failure of @p kind, with @p mismatch.
Abrupt hostFailure(HostFailureKind kind, std::optional<ValueMismatch> mismatch = std::nullopt)
{
    return HostFailure { .kind = kind, .code = 0, .mismatch = mismatch };
}

/// A store of default limits, with its allocator and an interpreter to run it.
struct Fixture
{
    HeapMemoryAllocator allocator;
    Store store { StoreLimits {}, StoreServices { .allocator = allocator } };
    ReferenceInterpreter interpreter;

    /// Adds a host function of @p type implemented by @p function.
    template <HostCallable F>
    FuncAddr host(FuncType type, F function)
    {
        auto const address = store.addHostFunction(std::move(type), makeHostFunction(std::move(function)));
        REQUIRE(address.has_value());
        return *address;
    }

    /// Invokes @p function with @p args and @p resultCount result slots, through invokeChecked.
    Outcome invoke(FuncAddr function, std::vector<Value> const& args = {}, std::size_t resultCount = 0)
    {
        auto results = std::vector<Value>(resultCount);
        return invokeChecked(store, interpreter, function, args, results).outcome;
    }
};

} // namespace

TEST_CASE("HostFunction.mustBeNoexcept")
{
    auto throwing = [](HostContext&, std::span<Value const>, std::span<Value>) -> Outcome {
        return {};
    };
    auto nothrow = [](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> Outcome {
        return {};
    };
    STATIC_CHECK_FALSE(HostCallable<decltype(throwing)>);
    STATIC_CHECK(HostCallable<decltype(nothrow)>);
}

TEST_CASE("HostFunction.badReentryIsAHostFailure")
{
    // A host function re-enters through its context with a wrong argument, too few result slots, or a
    // function that does not exist: each is refused as a host failure, and the process lives on.
    auto fixture = Fixture {};
    auto innerCalls = 0;
    auto const inner = fixture.host(
        FuncType { .params = { TypeI32 }, .results = { TypeI32 } },
        [&](HostContext&, std::span<Value const> args, std::span<Value> results) noexcept -> Outcome {
            ++innerCalls;
            results[0] = args[0];
            return {};
        });
    auto outcomes = std::vector<Outcome> {};
    auto caller = std::optional<uint32_t> { 0 };
    auto const outer = fixture.host(FuncType {}, [&](HostContext& context, auto, auto) noexcept -> Outcome {
        caller = context.callerInstance();
        auto one = std::vector<Value>(1);
        auto const wrongKind = std::vector<Value> { F32 {} };
        auto const right = std::vector<Value> { I32 { 41 } };
        outcomes.push_back(context.invoke(inner, wrongKind, one));
        outcomes.push_back(context.invoke(inner, right, {}));
        outcomes.push_back(context.invoke(999, right, one));
        outcomes.push_back(context.invoke(inner, right, one));
        if (one[0] != Value { I32 { 41 } })
            return std::unexpected { hostFailure(HostFailureKind::Embedder) };
        return {};
    });
    CHECK(fixture.invoke(outer).has_value());
    CHECK(caller == std::nullopt); // the embedder called it directly
    REQUIRE(outcomes.size() == 4);
    CHECK(outcomes[0]
          == std::unexpected { hostFailure(HostFailureKind::ArgumentMismatch,
                                           ValueMismatch { .kind = MismatchKind::Kind, .index = 0 }) });
    CHECK(outcomes[1]
          == std::unexpected { hostFailure(HostFailureKind::ArgumentMismatch,
                                           ValueMismatch { .kind = MismatchKind::Count, .index = 0 }) });
    CHECK(outcomes[2] == std::unexpected { hostFailure(HostFailureKind::ArgumentMismatch) });
    CHECK(outcomes[3].has_value());
    CHECK(innerCalls == 1); // the refused calls ran nothing
}

TEST_CASE("HostFunction.invokeCheckedRefusesBeforeEntering")
{
    // The embedder's call is checked as a host's is: refused without entering, or entered.
    auto fixture = Fixture {};
    auto calls = 0;
    auto const function = fixture.host(FuncType { .params = { TypeI32 }, .results = {} },
                                       [&](HostContext&, auto, auto) noexcept -> Outcome {
                                           ++calls;
                                           return {};
                                       });
    auto const wrong = std::vector<Value> { F32 {} };
    auto const refused = invokeChecked(fixture.store, fixture.interpreter, function, wrong, {});
    CHECK(refused.entry == Entry::Refused);
    CHECK(refused.outcome
          == std::unexpected { hostFailure(HostFailureKind::ArgumentMismatch,
                                           ValueMismatch { .kind = MismatchKind::Kind, .index = 0 }) });
    auto const right = std::vector<Value> { I32 { 1 } };
    auto const entered = invokeChecked(fixture.store, fixture.interpreter, function, right, {});
    CHECK(entered.entry == Entry::Entered);
    CHECK(entered.outcome.has_value());
    CHECK(calls == 1);
}

TEST_CASE("HostFunction.hostWritesAreChecked")
{
    // An exception a host allocates carries its tag's params' types; anything else is a host failure,
    // not a failed precondition.
    auto fixture = Fixture {};
    auto const tagged =
        fixture.store.addTag(fixture.store.internType(FuncType { .params = { TypeI32 }, .results = {} }));
    REQUIRE(tagged.has_value());
    auto const tag = *tagged;
    auto results = std::vector<std::expected<ExnAddr, Abrupt>> {};
    auto const host = fixture.host(FuncType {}, [&](HostContext& context, auto, auto) noexcept -> Outcome {
        results.push_back(context.addException(tag, { F32 {} }));
        results.push_back(context.addException(tag, {}));
        results.push_back(context.addException(tag + 1, { I32 { 1 } }));
        results.push_back(context.addException(tag, { I32 { 1 } }));
        return {};
    });
    CHECK(fixture.invoke(host).has_value());
    REQUIRE(results.size() == 4);
    CHECK(results[0]
          == std::unexpected { hostFailure(HostFailureKind::ArgumentMismatch,
                                           ValueMismatch { .kind = MismatchKind::Kind, .index = 0 }) });
    CHECK(results[1]
          == std::unexpected { hostFailure(HostFailureKind::ArgumentMismatch,
                                           ValueMismatch { .kind = MismatchKind::Count, .index = 0 }) });
    CHECK(results[2] == std::unexpected { hostFailure(HostFailureKind::ArgumentMismatch) });
    REQUIRE(results[3].has_value());
    CHECK(fixture.store.exception(*results[3]).payload == std::vector<Value> { I32 { 1 } });
}

TEST_CASE("HostFunction.hostExceptionsKeepToTheLimit")
{
    // A host's exceptions are charged as the guest's are: past the limit, a trap.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxExceptions = 1 }, StoreServices { .allocator = allocator } };
    auto interpreter = ReferenceInterpreter {};
    auto const tag = store.addTag(store.internType(FuncType {}));
    REQUIRE(tag.has_value());
    auto results = std::vector<std::expected<ExnAddr, Abrupt>> {};
    auto const host = store.addHostFunction(
        FuncType {}, makeHostFunction([&](HostContext& context, auto, auto) noexcept -> Outcome {
            results.push_back(context.addException(*tag, {}));
            results.push_back(context.addException(*tag, {}));
            return {};
        }));
    REQUIRE(host.has_value());
    CHECK(invokeChecked(store, interpreter, *host, {}, {}).outcome.has_value());
    REQUIRE(results.size() == 2);
    CHECK(results[0].has_value());
    CHECK(results[1] == std::unexpected { Abrupt { Trap { TrapCode::ExceptionLimitExceeded } } });
}

TEST_CASE("HostFunction.checkHostOutcome")
{
    auto fixture = Fixture {};
    auto const type = FuncType { .params = {}, .results = { TypeI32 } };
    auto const i32 = std::vector<Value> { I32 { 1 } };
    auto const f32 = std::vector<Value> { F32 {} };
    // Results of the type pass; others are a result mismatch, the count included.
    CHECK(checkHostOutcome(fixture.store, type, {}, i32).has_value());
    CHECK(checkHostOutcome(fixture.store, type, {}, f32)
          == std::unexpected { hostFailure(HostFailureKind::ResultMismatch,
                                           ValueMismatch { .kind = MismatchKind::Kind, .index = 0 }) });
    CHECK(checkHostOutcome(fixture.store, type, {}, {})
          == std::unexpected { hostFailure(HostFailureKind::ResultMismatch,
                                           ValueMismatch { .kind = MismatchKind::Count, .index = 0 }) });
    // An abrupt end passes through, whatever the results; a thrown exception must be the store's.
    auto const trapped = Outcome { std::unexpected { Abrupt { Trap { TrapCode::HostTrap } } } };
    CHECK(checkHostOutcome(fixture.store, type, trapped, f32) == trapped);
    auto const failed = Outcome { std::unexpected { hostFailure(HostFailureKind::Embedder) } };
    CHECK(checkHostOutcome(fixture.store, type, failed, f32) == failed);
    auto const forged = Outcome { std::unexpected { Abrupt { Thrown { .exception = 0 } } } };
    CHECK(checkHostOutcome(fixture.store, type, forged, {})
          == std::unexpected { hostFailure(HostFailureKind::ResultMismatch) });
    auto const tag = fixture.store.addTag(fixture.store.internType(FuncType {}));
    REQUIRE(tag.has_value());
    REQUIRE(fixture.store.addException(*tag, {}).has_value());
    CHECK(checkHostOutcome(fixture.store, type, forged, {}) == forged);
}

TEST_CASE("HostFunction.hostCallsAreCounted")
{
    // A host call in progress is counted, nested ones included, and every scope gives its count back.
    auto fixture = Fixture {};
    auto seen = std::vector<uint32_t> {};
    auto const inner = fixture.host(FuncType {}, [&](HostContext& context, auto, auto) noexcept -> Outcome {
        seen.push_back(context.store().hostCallsInProgress());
        return {};
    });
    auto const outer = fixture.host(FuncType {}, [&](HostContext& context, auto, auto) noexcept -> Outcome {
        seen.push_back(context.store().hostCallsInProgress());
        return context.invoke(inner, {}, {});
    });
    CHECK(fixture.store.hostCallsInProgress() == 0);
    CHECK(fixture.invoke(outer).has_value());
    CHECK(seen == std::vector<uint32_t> { 1, 2 });
    CHECK(fixture.store.hostCallsInProgress() == 0);
    {
        auto const scope = HostCallScope { fixture.store };
        CHECK(fixture.store.hostCallsInProgress() == 1);
    }
    CHECK(fixture.store.hostCallsInProgress() == 0);
}

TEST_CASE("HostFunction.callerMemoryNeedsACaller")
{
    // Called by the embedder, a host function has no caller, and so no caller memory.
    auto fixture = Fixture {};
    auto memory = std::optional<std::span<std::byte>> { std::span<std::byte> {} };
    auto const host = fixture.host(FuncType {}, [&](HostContext& context, auto, auto) noexcept -> Outcome {
        memory = context.callerMemory(0);
        return {};
    });
    CHECK(fixture.invoke(host).has_value());
    CHECK(memory == std::nullopt);
}

TEST_CASE("HostFunction.nanPayloadsPassThrough")
{
    // A host's float results reach the embedder bit for bit, signalling NaNs of any payload included.
    auto fixture = Fixture {};
    auto const nans = std::vector<Value> { isoline::runtime::F32 { 0x7FA0'0001 },
                                           isoline::runtime::F64 { 0xFFF0'0000'0000'0001 },
                                           isoline::runtime::V128 {
                                               .lanes = { 0x7FA0'0001'FFC0'0002, 0x7FF0'0000'0000'0003 } } };
    auto const host = fixture.host(
        FuncType { .params = {},
                   .results = { isoline::binary::F32, isoline::binary::F64, isoline::binary::V128 } },
        [&](HostContext&, std::span<Value const>, std::span<Value> results) noexcept -> Outcome {
            std::ranges::copy(nans, results.begin());
            return {};
        });
    auto results = std::vector<Value>(3);
    CHECK(invokeChecked(fixture.store, fixture.interpreter, host, {}, results).outcome.has_value());
    CHECK(results == nans);
}
