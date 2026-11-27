// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>

using isoline::binary::AddressType;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::runtime::I32;
using isoline::runtime::InvocationScope;
using isoline::runtime::IPollObserver;
using isoline::runtime::ObserverRefusedEntryPoints;
using isoline::runtime::PollSite;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;

// IPollObserver's contract, row by row: an observer may read the store and never write it. Each entry point
// of runtime::ObserverRefusedEntryPoints is called from an observer by one [.contract] case below, which must
// end on the store's Require (tests/CMakeLists.txt, the "Contract.Observer." rows); the reads an observer may
// make succeed.

namespace
{

/// One page, unbounded.
constexpr auto OnePage = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 };

/// A null funcref.
constexpr auto NullFunction = Ref { .kind = RefKind::Null, .heap = FuncRef.heap, .address = 0 };

/// A mutable i32 global.
constexpr auto MutableI32 = GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Var };

/// A host function that does nothing.
constexpr auto NoHost =
    [](isoline::runtime::HostContext&,
       std::span<isoline::runtime::Value const>,
       std::span<isoline::runtime::Value>) noexcept -> std::expected<void, isoline::runtime::Abrupt> {
    return {};
};

/// A poll observer that runs its act once, at the first poll it sees.
class ActingObserver final: public IPollObserver
{
  public:
    /// Runs @p act at the next poll.
    void actAtNextPoll(std::function<void()> act) { _act = std::move(act); }

    void onPoll(PollSite /*site*/) noexcept override
    {
        if (auto act = std::exchange(_act, {}); act)
            act();
    }

  private:
    std::function<void()> _act;
};

/// A runtime store with a memory, a table, a global, a type and a tag, polled under an observer.
struct ObservedRuntime
{
    isoline::runtime::HeapMemoryAllocator allocator;
    ActingObserver observer;
    isoline::runtime::Store store { isoline::runtime::StoreLimits {},
                                    isoline::runtime::StoreServices { .allocator = allocator,
                                                                      .pollObserver = &observer } };
    isoline::runtime::TypeId type = store.internType(FuncType {});
    isoline::runtime::TagAddr tag = store.addTag(type).value();

    ObservedRuntime()
    {
        REQUIRE(store.addMemory(MemoryType { .limits = OnePage }).has_value());
        REQUIRE(
            store.addTable(TableType { .element = FuncRef, .limits = OnePage }, NullFunction).has_value());
        std::ignore = store.addGlobal(MutableI32, I32 { .bits = 7 });
    }

    /// Polls, with @p act run by the observer.
    void pollActing(std::function<void(ObservedRuntime&)> act)
    {
        observer.actAtNextPoll([this, act = std::move(act)] { act(*this); });
        std::ignore = store.poll(PollSite::LoopBackEdge);
    }
};

/// An api store with a memory, a table and a global, and an instance whose exported function polls at its
/// entry: called, with an act the observer runs.
struct ObservedApi
{
    isoline::api::Engine engine { isoline::api::EngineConfig {} };
    isoline::runtime::HeapMemoryAllocator allocator;
    ActingObserver observer;
    isoline::api::Store store {
        engine, allocator, isoline::api::StoreOptions { .limits = std::nullopt, .observer = &observer }
    };
    isoline::api::Memory memory = store.defineMemory(MemoryType { .limits = OnePage }).value();
    isoline::api::Table table =
        store.defineTable(TableType { .element = FuncRef, .limits = OnePage }, NullFunction).value();
    isoline::api::Global global = store.defineGlobal(MutableI32, I32 { .bits = 7 }).value();

    /// Calls a function that polls, with @p act run by the observer.
    void callActing(std::function<void(ObservedApi&)> act)
    {
        auto builder = isoline::binary::testing::ModuleBuilder {};
        builder.type(FuncType {}).function(0, {}, {}).exportFunction("polls", 0);
        auto const module = engine.load(builder.build());
        REQUIRE(module.has_value());
        auto const instance = store.instantiate(*module, isoline::api::Imports {});
        REQUIRE(instance.has_value());
        auto const function = instance->function("polls");
        REQUIRE(function.has_value());
        observer.actAtNextPoll([this, act = std::move(act)] { act(*this); });
        REQUIRE(store.call(*function, {}, {}).has_value());
    }
};

/// One row of ObserverRefusedEntryPoints, called from an observer.
struct RefusedAct
{
    std::string_view entryPoint;                      ///< The row's entry point.
    std::function<void(ObservedRuntime&)> runtimeAct; ///< A call of it on a runtime store; or none.
    std::function<void(ObservedApi&)> apiAct;         ///< A call of it on an api store; or none.
};

/// A call of every refused entry point, by row.
std::vector<RefusedAct> const& refusedActs()
{
    using Runtime = ObservedRuntime;
    static auto const acts = std::vector<RefusedAct> {
        { "runtime::Store::growMemory", [](Runtime& r) { std::ignore = r.store.growMemory(0, 1); }, {} },
        { "runtime::Store::growTable",
          [](Runtime& r) { std::ignore = r.store.growTable(0, 1, NullFunction); },
          {} },
        { "runtime::Store::internType",
          [](Runtime& r) {
              std::ignore =
                  r.store.internType(FuncType { .params = { isoline::binary::I32 }, .results = {} });
          },
          {} },
        { "runtime::Store::addHostFunction",
          [](Runtime& r) {
              std::ignore = r.store.addHostFunction(FuncType {}, isoline::runtime::makeHostFunction(NoHost));
          },
          {} },
        { "runtime::Store::addModuleFunction",
          [](Runtime& r) { std::ignore = r.store.addModuleFunction(r.type, 0, 0); },
          {} },
        { "runtime::Store::addTable",
          [](Runtime& r) {
              std::ignore =
                  r.store.addTable(TableType { .element = FuncRef, .limits = OnePage }, NullFunction);
          },
          {} },
        { "runtime::Store::addMemory",
          [](Runtime& r) { std::ignore = r.store.addMemory(MemoryType { .limits = OnePage }); },
          {} },
        { "runtime::Store::addGlobal",
          [](Runtime& r) { std::ignore = r.store.addGlobal(MutableI32, I32 { .bits = 1 }); },
          {} },
        { "runtime::Store::addTag", [](Runtime& r) { std::ignore = r.store.addTag(r.type); }, {} },
        { "runtime::Store::addException",
          [](Runtime& r) { std::ignore = r.store.addException(r.tag, {}); },
          {} },
        { "runtime::Store::addModuleInstance",
          [](Runtime& r) { std::ignore = r.store.addModuleInstance(isoline::runtime::ModuleInstance {}); },
          {} },
        { "runtime::Store::releaseTrailing", [](Runtime& r) { r.store.releaseTrailing(1, 1); }, {} },
        { "runtime::Store::poll",
          [](Runtime& r) { std::ignore = r.store.poll(PollSite::LoopBackEdge); },
          {} },
        { "runtime::Store::chargeFrame", [](Runtime& r) { std::ignore = r.store.chargeFrame(1); }, {} },
        { "runtime::Store::releaseFrame", [](Runtime& r) { r.store.releaseFrame(0); }, {} },
        { "runtime::Store::bindDispatch",
          [](Runtime& r) { r.store.bindDispatch(0, 0, nullptr, nullptr); },
          {} },
        { "runtime::InvocationScope",
          [](Runtime& r) { auto const invocation = InvocationScope { r.store }; },
          {} },
        { "api::Store::setGlobal",
          {},
          [](ObservedApi& a) { std::ignore = a.store.setGlobal(a.global, I32 { .bits = 1 }); } },
        { "api::Store::tableSet",
          {},
          [](ObservedApi& a) { std::ignore = a.store.tableSet(a.table, 0, NullFunction); } },
        { "api::Store::memoryRange",
          {},
          [](ObservedApi& a) { std::ignore = a.store.memoryRange(a.memory, 0, 4); } },
    };
    return acts;
}

/// Calls @p entryPoint from an observer, which must end the process on the store's Require.
void refuse(std::string_view entryPoint)
{
    isoline::runtime::testing::endAtContractViolation();
    auto const found = std::ranges::find(refusedActs(), entryPoint, &RefusedAct::entryPoint);
    REQUIRE(found != refusedActs().end());
    if (found->runtimeAct)
    {
        auto observed = ObservedRuntime {};
        observed.pollActing(found->runtimeAct);
    }
    else
    {
        auto observed = ObservedApi {};
        observed.callActing(found->apiAct);
    }
    FAIL("SURVIVED: a poll observer called " << entryPoint);
}

} // namespace

TEST_CASE("Observer.everyRefusedEntryPointHasACase")
{
    // The table the documentation states and the acts below are the same rows, in the same order.
    REQUIRE(refusedActs().size() == ObserverRefusedEntryPoints.size());
    for (auto const position: std::views::iota(std::size_t { 0 }, refusedActs().size()))
    {
        auto const& act = refusedActs()[position];
        CHECK(ObserverRefusedEntryPoints[position].entryPoint == act.entryPoint);
        CHECK(static_cast<bool>(act.runtimeAct) != static_cast<bool>(act.apiAct));
    }
}

TEST_CASE("Observer.mayReadTheStore")
{
    // A global's value, a table element, a memory's size and its read-only bytes: all succeed from an
    // observer, and the call it observes returns.
    auto observed = ObservedApi {};
    auto value = std::optional<isoline::runtime::Value> {};
    auto element = std::optional<Ref> {};
    auto bytes = std::size_t { 0 };
    observed.callActing([&](ObservedApi& api) {
        auto const& store = std::as_const(api.store);
        value = api.store.globalValue(api.global).value();
        element = api.store.tableGet(api.table, 0).value();
        bytes = store.memoryRange(api.memory, 0, 4).value().size();
    });
    REQUIRE(value.has_value());
    CHECK(std::get<I32>(*value).bits == 7);
    REQUIRE(element.has_value());
    CHECK(element->kind == RefKind::Null);
    CHECK(bytes == 4);
}

TEST_CASE("Contract.Observer.growMemory", "[.contract]")
{
    refuse("runtime::Store::growMemory");
}
TEST_CASE("Contract.Observer.growTable", "[.contract]")
{
    refuse("runtime::Store::growTable");
}
TEST_CASE("Contract.Observer.internType", "[.contract]")
{
    refuse("runtime::Store::internType");
}
TEST_CASE("Contract.Observer.addHostFunction", "[.contract]")
{
    refuse("runtime::Store::addHostFunction");
}
TEST_CASE("Contract.Observer.addModuleFunction", "[.contract]")
{
    refuse("runtime::Store::addModuleFunction");
}
TEST_CASE("Contract.Observer.addTable", "[.contract]")
{
    refuse("runtime::Store::addTable");
}
TEST_CASE("Contract.Observer.addMemory", "[.contract]")
{
    refuse("runtime::Store::addMemory");
}
TEST_CASE("Contract.Observer.addGlobal", "[.contract]")
{
    refuse("runtime::Store::addGlobal");
}
TEST_CASE("Contract.Observer.addTag", "[.contract]")
{
    refuse("runtime::Store::addTag");
}
TEST_CASE("Contract.Observer.addException", "[.contract]")
{
    refuse("runtime::Store::addException");
}
TEST_CASE("Contract.Observer.addModuleInstance", "[.contract]")
{
    refuse("runtime::Store::addModuleInstance");
}
TEST_CASE("Contract.Observer.releaseTrailing", "[.contract]")
{
    refuse("runtime::Store::releaseTrailing");
}
TEST_CASE("Contract.Observer.poll", "[.contract]")
{
    refuse("runtime::Store::poll");
}
TEST_CASE("Contract.Observer.chargeFrame", "[.contract]")
{
    refuse("runtime::Store::chargeFrame");
}
TEST_CASE("Contract.Observer.releaseFrame", "[.contract]")
{
    refuse("runtime::Store::releaseFrame");
}
TEST_CASE("Contract.Observer.bindDispatch", "[.contract]")
{
    refuse("runtime::Store::bindDispatch");
}
TEST_CASE("Contract.Observer.invocation", "[.contract]")
{
    refuse("runtime::InvocationScope");
}
TEST_CASE("Contract.Observer.setGlobal", "[.contract]")
{
    refuse("api::Store::setGlobal");
}
TEST_CASE("Contract.Observer.tableSet", "[.contract]")
{
    refuse("api::Store::tableSet");
}
TEST_CASE("Contract.Observer.memoryRange", "[.contract]")
{
    refuse("api::Store::memoryRange");
}
