// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/EngineRows.hpp>
#include <isoline/api/testing/RuntimeAccess.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

using isoline::FeatureSet;
using isoline::api::Abrupt;
using isoline::api::AllocationFailure;
using isoline::api::BulkGranule;
using isoline::api::BulkPollBytes;
using isoline::api::CallError;
using isoline::api::CallFailure;
using isoline::api::Engine;
using isoline::api::EngineConfig;
using isoline::api::EngineMismatch;
using isoline::api::ExecutionMode;
using isoline::api::ExternKind;
using isoline::api::ForeignImport;
using isoline::api::Function;
using isoline::api::FuncType;
using isoline::api::GrowthRecord;
using isoline::api::HeapMemoryAllocator;
using isoline::api::HostContext;
using isoline::api::HostFailure;
using isoline::api::HostFailureKind;
using isoline::api::I32;
using isoline::api::I64;
using isoline::api::Imports;
using isoline::api::Instance;
using isoline::api::InstantiateError;
using isoline::api::InvocationScope;
using isoline::api::IPollObserver;
using isoline::api::Limit;
using isoline::api::Linkage;
using isoline::api::LinkError;
using isoline::api::LinkErrorCode;
using isoline::api::makeHostFunction;
using isoline::api::MemoryAccessError;
using isoline::api::MemorySize;
using isoline::api::MismatchKind;
using isoline::api::Module;
using isoline::api::PollSite;
using isoline::api::Ref;
using isoline::api::RefKind;
using isoline::api::Refusal;
using isoline::api::Store;
using isoline::api::StoreLimits;
using isoline::api::StoreOptions;
using isoline::api::Trap;
using isoline::api::TrapCode;
using isoline::api::UncaughtException;
using isoline::api::V128;
using isoline::api::Value;
using isoline::api::ValueMismatch;
using isoline::api::testing::engineRows;
using isoline::api::testing::runtimeOf;
using isoline::api::testing::contract::configOf;
using isoline::binary::GlobalType;
using isoline::binary::Limits;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::call;
using isoline::binary::testing::code;
using isoline::binary::testing::drop;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::memoryAccess;
using isoline::binary::testing::memoryGrow;
using isoline::binary::testing::ModuleBuilder;
using isoline::binary::testing::tableGrow;
using isoline::binary::testing::unreachable;

namespace
{

constexpr auto TypeI32 = isoline::binary::I32;
constexpr auto TypeI64 = isoline::binary::I64;

/// A size of @p count pages (or elements, for a table), unbounded, of 32-bit addresses.
Limits pages(uint64_t count)
{
    return Limits { .min = count, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 };
}

/// The embedder's own failure @p code.
HostFailure embedderFailure(uint32_t code)
{
    return HostFailure { .kind = HostFailureKind::Embedder, .code = code, .mismatch = std::nullopt };
}

/// What a host function returns.
using Outcome = std::expected<void, Abrupt>;

/// An engine, an allocator and a store of that engine.
struct Fixture
{
    /// A store of an engine of @p config.
    explicit Fixture(EngineConfig const& config = {}): engine { config } {}

    Engine engine;
    HeapMemoryAllocator allocator;
    Store store { engine, allocator };

    /// Loads @p builder's module with the fixture's engine.
    [[nodiscard]] Module load(ModuleBuilder const& builder) const
    {
        auto module = engine.load(builder.build());
        REQUIRE(module.has_value());
        return *std::move(module);
    }

    /// Loads and instantiates @p builder's module, which must succeed.
    [[nodiscard]] Instance instantiate(ModuleBuilder const& builder, Imports const& imports = {})
    {
        auto instance = store.instantiate(load(builder), imports);
        REQUIRE(instance.has_value());
        return *instance;
    }
};

/// Instance @p instance's exported function @p name, which must exist.
Function exportedFunction(Instance const& instance, std::string_view name)
{
    auto const function = instance.function(name);
    REQUIRE(function.has_value());
    return *function;
}

/// The error a call or instantiation that must fail failed with.
template <typename T, typename E>
E errorOf(std::expected<T, E> const& outcome)
{
    REQUIRE_FALSE(outcome.has_value());
    return outcome.error();
}

/// The CallError an instantiation that must fail as a call failed with.
CallError callErrorOf(std::expected<Instance, InstantiateError> const& outcome)
{
    auto const error = errorOf(outcome);
    REQUIRE(std::holds_alternative<CallError>(error));
    return std::get<CallError>(error);
}

/// An externref to host object @p address.
Value externRef(uint32_t address)
{
    return Ref { .kind = RefKind::Extern,
                 .heap = { .kind = isoline::binary::AbstractHeapType::Extern },
                 .address = address };
}

/// How a host function re-enters the guest: as Contour does, through the Instance it holds; or
/// through the HostContext it is given.
enum class ReentryPath : uint8_t
{
    InstanceCall,
    HostContextInvoke,
};

/// A module of the re-entry cases: host env.ping(i32) re-enters the guest by its argument (0: "fail",
/// 2: "grow-big"; 1 fails on its own account). One page of memory, which may not grow past a
/// one-page store.
///
/// Functions: 0 ping; 1 "fail" pings 1; 2 "absorb" pings 0 and returns 7; 3 "grow-absorb-then-trap"
/// grows by 4 (refused), pings 0, then traps; 4 "grow-big" grows by 4 and returns the result; 5
/// "reenter-then-trap" pings 2, then traps.
ModuleBuilder reentryModule()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = {} })
        .type(FuncType {})
        .type(FuncType { .params = {}, .results = { TypeI32 } })
        .importFunction("env", "ping", 0)
        .memory(pages(1))
        .function(1, {}, code({ i32Const(1), call(0) }))
        .function(2, {}, code({ i32Const(0), call(0), i32Const(7) }))
        .function(1, {}, code({ i32Const(4), memoryGrow(), drop(), i32Const(0), call(0), unreachable() }))
        .function(2, {}, code({ i32Const(4), memoryGrow() }))
        .function(1, {}, code({ i32Const(2), call(0), unreachable() }))
        .exportFunction("fail", 1)
        .exportFunction("absorb", 2)
        .exportFunction("grow-absorb-then-trap", 3)
        .exportFunction("grow-big", 4)
        .exportFunction("reenter-then-trap", 5);
    return builder;
}

/// The re-entry cases' store and instance, re-entering along one path.
class Reentry
{
  public:
    explicit Reentry(ReentryPath path): _path { path }
    {
        auto config = EngineConfig {};
        config.store.maxMemoryBytes = 65'536;
        _fixture.emplace(config);
        auto imports = Imports {};
        imports.define("env",
                       "ping",
                       FuncType { .params = { TypeI32 }, .results = {} },
                       makeHostFunction([this](HostContext& context,
                                               std::span<Value const> args,
                                               std::span<Value>) noexcept -> Outcome {
                           return ping(context, std::get<I32>(args[0]).bits);
                       }));
        _instance = _fixture->instantiate(reentryModule(), imports);
    }

    [[nodiscard]] Instance& instance() noexcept { return *_instance; }

    [[nodiscard]] Function function(std::string_view name) const
    {
        return exportedFunction(*_instance, name);
    }

    /// The results "grow-big" returned when ping re-entered it.
    [[nodiscard]] std::vector<Value> const& grownResults() const noexcept { return _grownResults; }

  private:
    Outcome ping(HostContext& context, uint32_t which) noexcept
    {
        if (which == 1)
            return std::unexpected { Abrupt { embedderFailure(1) } };
        auto const target = function(which == 0 ? "fail" : "grow-big");
        auto results = std::vector<Value>(target.type().results.size());
        // The nested call's outcome is absorbed: the host carries on as if nothing failed.
        if (_path == ReentryPath::InstanceCall)
        {
            [[maybe_unused]] auto const nested = _instance->call(target, {}, results);
        }
        else
        {
            [[maybe_unused]] auto const nested = context.invoke(target.address(), {}, results);
        }
        if (which == 2)
            _grownResults = results;
        return {};
    }

    ReentryPath _path;
    std::optional<Fixture> _fixture;
    std::optional<Instance> _instance;
    std::vector<Value> _grownResults;
};

/// Each re-entry path in turn, as a Catch2 SECTION.
ReentryPath reentryPath()
{
    auto path = ReentryPath::InstanceCall;
    SECTION("through Instance::call")
    {
        path = ReentryPath::InstanceCall;
    }
    SECTION("through HostContext::invoke")
    {
        path = ReentryPath::HostContextInvoke;
    }
    return path;
}

/// A module exporting "add" (i32, i32) -> i32 and "trap", which traps.
ModuleBuilder addAndTrapModule()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32, TypeI32 }, .results = { TypeI32 } })
        .type(FuncType {})
        .function(0, {}, code({ { 0x20, 0x00, 0x20, 0x01, 0x6A } })) // local.get 0 local.get 1 i32.add
        .function(1, {}, unreachable())
        .exportFunction("add", 0)
        .exportFunction("trap", 1);
    return builder;
}

} // namespace

TEST_CASE("Store.nestedFailureStaysNested")
{
    auto reentry = Reentry { reentryPath() };
    auto results = std::array<Value, 1> {};
    REQUIRE(reentry.instance().call(reentry.function("absorb"), {}, results).has_value());
    CHECK(results[0] == Value { I32 { 7 } });
    auto const failed = reentry.instance().call(reentry.function("fail"), {}, {});
    CHECK(errorOf(failed).failure() == CallFailure::HostFailure);
    CHECK(errorOf(failed).detail == CallError::Detail { embedderFailure(1) });
}

TEST_CASE("Store.outerGrowthRecordSurvivesNestedCall", "[memory]")
{
    auto reentry = Reentry { reentryPath() };
    auto const failed = reentry.instance().call(reentry.function("grow-absorb-then-trap"), {}, {});
    CHECK(errorOf(failed).failure() == CallFailure::Trap);
    CHECK(errorOf(failed).refusedGrowth == GrowthRecord { .memory = Refusal::Limit, .table = {} });
}

TEST_CASE("Store.instantiationReportsItsRefusedGrowth", "[memory]")
{
    // A start function refused a memory.grow by the store's limit, and then trapping: the error says
    // why the guest gave up, as a call's does.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .memory(pages(1))
        .function(0, {}, code({ i32Const(4), memoryGrow(), drop(), unreachable() }))
        .start(0);
    auto config = EngineConfig {};
    config.store.maxMemoryBytes = 65'536;
    auto fixture = Fixture { config };
    auto const failed = fixture.store.instantiate(fixture.load(builder), {});
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(std::holds_alternative<CallError>(failed.error()));
    auto const& error = std::get<CallError>(failed.error());
    CHECK(error.failure() == CallFailure::Trap);
    CHECK(error.refusedGrowth == GrowthRecord { .memory = Refusal::Limit, .table = {} });
}

TEST_CASE("Store.nestedRefusalDoesNotLeak", "[memory]")
{
    auto reentry = Reentry { reentryPath() };
    auto const failed = reentry.instance().call(reentry.function("reenter-then-trap"), {}, {});
    CHECK(errorOf(failed).failure() == CallFailure::Trap);
    CHECK(errorOf(failed).refusedGrowth == GrowthRecord {});
    // The nested call was refused its growth: -1.
    CHECK(reentry.grownResults() == std::vector<Value> { I32 { 0xFFFF'FFFF } });
}

TEST_CASE("Store.badArgumentIsRefusedNotAborted")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { isoline::binary::FuncRef }, .results = {} })
        .type(FuncType { .params = {}, .results = { TypeI32 } })
        .function(0, {}, {})
        .function(1, {}, i32Const(3))
        .exportFunction("takes-funcref", 0)
        .exportFunction("returns-i32", 1);
    auto fixture = Fixture {};
    auto instance = fixture.instantiate(builder);
    auto const externArgs = std::array { externRef(1) };
    auto const wrongKind = instance.call(exportedFunction(instance, "takes-funcref"), externArgs, {});
    CHECK(errorOf(wrongKind).failure() == CallFailure::ArgumentMismatch);
    CHECK(errorOf(wrongKind).detail
          == CallError::Detail { ValueMismatch { .kind = MismatchKind::Heap, .index = 0 } });
    auto const noSlot = instance.call(exportedFunction(instance, "returns-i32"), {}, {});
    CHECK(errorOf(noSlot).failure() == CallFailure::ArgumentMismatch);
    CHECK(errorOf(noSlot).detail
          == CallError::Detail { ValueMismatch { .kind = MismatchKind::Count, .index = 0 } });
    // Refused calls ran nothing; the instance still answers.
    auto results = std::array<Value, 1> {};
    REQUIRE(instance.call(exportedFunction(instance, "returns-i32"), {}, results).has_value());
    CHECK(results[0] == Value { I32 { 3 } });
}

TEST_CASE("Store.reportsWhatItHolds")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).memory(pages(1)).function(0, {}, {}).exportFunction("f", 0);
    auto fixture = Fixture {};
    CHECK(fixture.store.used(Limit::Memories) == 0);
    auto const instance = fixture.instantiate(builder);
    CHECK(fixture.store.used(Limit::Memories) == 1);
    CHECK(fixture.store.used(Limit::MemoryBytes) == 65'536);
    CHECK(fixture.store.used(Limit::Instances) == 1);
    // Instantiation's one poll, on entry: its own budget.
    CHECK(fixture.store.used(Limit::Steps) == 1);
    REQUIRE(instance.call(exportedFunction(instance, "f"), {}, {}).has_value());
    // A fresh budget for the call, of which the function entry took one step; kept after it ended.
    CHECK(fixture.store.used(Limit::Steps) == 1);
    CHECK(fixture.store.used(Limit::NestedInvocations) == 0);
    CHECK(fixture.store.used(Limit::StackValues) == 0);
}

TEST_CASE("Store.staleHandleIsNoLaterStores")
{
    // A handle that outlives its store is never taken for a later store's, wherever the allocator
    // places the later store's identity: the check compares serial numbers, never addresses.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .function(0, {}, {})
        .exportFunction("f", 0)
        .memory(pages(1))
        .exportEntity("memory", ExternKind::Memory, 0);
    auto fixture = std::optional<Fixture> { std::in_place };
    auto const instance = fixture->instantiate(builder);
    auto const function = exportedFunction(instance, "f");
    auto const memory = instance.memory("memory");
    REQUIRE(memory.has_value());
    auto const firstSerial = fixture->store.id().serial();
    CHECK(function.isOf(fixture->store.id()));
    CHECK(memory->isOf(fixture->store.id()));
    CHECK(&memory->store() == &fixture->store.id());
    CHECK(&function.store() == &fixture->store.id());
    fixture.reset();
    fixture.emplace();
    CHECK(fixture->store.id().serial() != firstSerial);
    CHECK_FALSE(function.isOf(fixture->store.id()));
    CHECK_FALSE(memory->isOf(fixture->store.id()));
}

TEST_CASE("Store.foreignInstanceDoesNotLink")
{
    auto exporter = ModuleBuilder {};
    exporter.type(FuncType {}).function(0, {}, {}).exportFunction("f", 0);
    auto importer = ModuleBuilder {};
    importer.type(FuncType {}).importFunction("env", "f", 0);

    auto storeA = Fixture {};
    auto storeB = Fixture {};
    auto const instanceOfA = storeA.instantiate(exporter);
    auto imports = Imports {};
    imports.define("env", "f", instanceOfA, "f");

    CHECK(errorOf(storeB.store.instantiate(storeB.load(importer), imports))
          == InstantiateError { ForeignImport { .index = 0 } });
    // The store holding it links it.
    CHECK(storeA.store.instantiate(storeA.load(importer), imports).has_value());
}

TEST_CASE("Store.hostFunctionUnderTwoTypesIsTwoFunctions")
{
    auto calls = 0;
    auto const host =
        makeHostFunction([&calls](HostContext&, std::span<Value const>, std::span<Value>) noexcept {
            ++calls;
            return Outcome {};
        });
    auto const takesI32 = FuncType { .params = { TypeI32 }, .results = {} };
    auto const takesI64 = FuncType { .params = { TypeI64 }, .results = {} };
    auto imports = Imports {};
    imports.define("env", "narrow", takesI32, host).define("env", "wide", takesI64, host);
    auto builder = ModuleBuilder {};
    builder.type(takesI32)
        .type(takesI64)
        .importFunction("env", "narrow", 0)
        .importFunction("env", "wide", 1)
        .exportFunction("narrow", 0)
        .exportFunction("wide", 1);

    auto fixture = Fixture {};
    auto const first = fixture.instantiate(builder, imports);
    auto const narrow = exportedFunction(first, "narrow");
    auto const wide = exportedFunction(first, "wide");
    CHECK(narrow.address() != wide.address());
    CHECK(narrow.type() == takesI32);
    CHECK(wide.type() == takesI64);
    // A second instantiation binds the same two functions: each definition is added once.
    auto const second = fixture.instantiate(builder, imports);
    CHECK(exportedFunction(second, "narrow").address() == narrow.address());
    CHECK(exportedFunction(second, "wide").address() == wide.address());
    CHECK(runtimeOf(fixture.store).functionCount() == 2);

    auto const narrowArgs = std::array<Value, 1> { I32 { 1 } };
    auto const wideArgs = std::array<Value, 1> { I64 { 1 } };
    CHECK(first.call(narrow, narrowArgs, {}).has_value());
    CHECK(first.call(wide, wideArgs, {}).has_value());
    CHECK(calls == 2);
}

TEST_CASE("Store.interruptedBeforeInstantiate")
{
    auto calls = 0;
    auto imports = Imports {};
    imports.define(
        "env",
        "start",
        FuncType {},
        makeHostFunction([&calls](HostContext&, std::span<Value const>, std::span<Value>) noexcept {
            ++calls;
            return Outcome {};
        }));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("env", "start", 0).start(0);
    auto fixture = Fixture {};
    auto const interrupter = fixture.store.interrupter();
    REQUIRE(interrupter != nullptr);
    interrupter->interrupt();
    auto const error = callErrorOf(fixture.store.instantiate(fixture.load(builder), imports));
    CHECK(error.failure() == CallFailure::Interrupted);
    CHECK(error.detail == CallError::Detail { Trap { TrapCode::Interrupted } });
    CHECK(calls == 0);
}

TEST_CASE("Store.startFunctionFailuresAreCallErrors")
{
    auto imports = Imports {};
    imports.define("env",
                   "start",
                   FuncType {},
                   makeHostFunction([](HostContext&, std::span<Value const>, std::span<Value>) noexcept {
                       return Outcome { std::unexpected { Abrupt { embedderFailure(9) } } };
                   }));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("env", "start", 0).start(0);
    auto fixture = Fixture {};
    auto const error = callErrorOf(fixture.store.instantiate(fixture.load(builder), imports));
    CHECK(error.failure() == CallFailure::HostFailure);
    CHECK(error.detail == CallError::Detail { embedderFailure(9) });
}

TEST_CASE("Store.hostEndsTheCallAsATrap")
{
    auto imports = Imports {};
    imports.define("wasi", "proc_exit", FuncType {}, makeHostFunction([](HostContext&, auto, auto) noexcept {
                       return Outcome { std::unexpected { Abrupt { Trap { TrapCode::HostTrap, 42 } } } };
                   }));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("wasi", "proc_exit", 0)
        .function(0, {}, call(0))
        .exportFunction("run", 1);
    auto fixture = Fixture {};
    auto instance = fixture.instantiate(builder, imports);
    auto const failed = instance.call(exportedFunction(instance, "run"), {}, {});
    CHECK(errorOf(failed).failure() == CallFailure::Trap);
    // The embedder's own trap, subject included, comes back unchanged.
    CHECK(errorOf(failed).detail == CallError::Detail { Trap { TrapCode::HostTrap, 42 } });
}

TEST_CASE("Store.callableAfterTrap")
{
    auto fixture = Fixture {};
    auto instance = fixture.instantiate(addAndTrapModule());
    auto const trapped = instance.call(exportedFunction(instance, "trap"), {}, {});
    CHECK(errorOf(trapped).detail == CallError::Detail { Trap { TrapCode::Unreachable } });
    auto const args = std::array<Value, 2> { I32 { 1 }, I32 { 2 } };
    auto results = std::array<Value, 1> {};
    REQUIRE(instance.call(exportedFunction(instance, "add"), args, results).has_value());
    CHECK(results[0] == Value { I32 { 3 } });
}

TEST_CASE("Store.memoryRangeIsBoundsChecked")
{
    auto builder = ModuleBuilder {};
    builder.memory(pages(1)).exportEntity("memory", ExternKind::Memory, 0);
    auto fixture = Fixture {};
    auto instance = fixture.instantiate(builder);
    auto const memory = instance.memory("memory");
    REQUIRE(memory.has_value());
    CHECK(instance.memoryRange(*memory, 65'535, 1).transform([](auto bytes) { return bytes.size(); })
          == std::expected<std::size_t, MemoryAccessError> { 1 });
    CHECK(errorOf(instance.memoryRange(*memory, 65'535, 2)) == MemoryAccessError::OutOfBounds);
    CHECK(errorOf(instance.memoryRange(*memory, 65'536, 1)) == MemoryAccessError::OutOfBounds);
    CHECK(errorOf(instance.memoryRange(*memory, 0xFFFF'FFF0, 32)) == MemoryAccessError::OutOfBounds);
    CHECK(errorOf(instance.memoryRange(*memory, 1, std::numeric_limits<uint64_t>::max()))
          == MemoryAccessError::OutOfBounds);
    // Contour's rule: nothing is viewed at any address.
    for (auto const address: { uint64_t { 0 }, uint64_t { 70'000 }, std::numeric_limits<uint64_t>::max() })
    {
        auto const empty = instance.memoryRange(*memory, address, 0);
        REQUIRE(empty.has_value());
        CHECK(empty->empty());
    }
    CHECK_FALSE(instance.memory("absent").has_value());
    CHECK_FALSE(instance.function("memory").has_value()); // a memory, not a function
}

TEST_CASE("Store.grownMemoryIsVisible")
{
    // grow by one page, then i32.store 0x12345678 at 65'536.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .memory(pages(1))
        .function(0,
                  {},
                  code({ i32Const(1),
                         memoryGrow(),
                         drop(),
                         i32Const(65'536),
                         i32Const(0x1234'5678),
                         memoryAccess(0x36, 2, 0) }))
        .exportFunction("grow", 0)
        .exportEntity("memory", ExternKind::Memory, 0);
    auto fixture = Fixture {};
    auto instance = fixture.instantiate(builder);
    auto const memory = instance.memory("memory");
    REQUIRE(memory.has_value());
    REQUIRE(instance.call(exportedFunction(instance, "grow"), {}, {}).has_value());
    auto const range = instance.memoryRange(*memory, 65'536, 4);
    REQUIRE(range.has_value());
    CHECK(std::vector<std::byte>(range->begin(), range->end())
          == std::vector { std::byte { 0x78 }, std::byte { 0x56 }, std::byte { 0x34 }, std::byte { 0x12 } });
}

TEST_CASE("Store.unsatisfiedImportIsDescribedThenUnlinkable")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("env", "missing", 0);
    auto fixture = Fixture {};
    auto const module = fixture.load(builder);
    auto const imports = Imports {};
    CHECK(imports.resolve(module) == std::vector { Linkage::Unsatisfied });
    CHECK(errorOf(fixture.store.instantiate(module, imports))
          == InstantiateError { LinkError { LinkErrorCode::UnknownImport, 0 } });
}

TEST_CASE("Store.wrongKindOrTypeIsIncompatible")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = {} }).importFunction("env", "f", 0);
    auto fixture = Fixture {};
    auto imports = Imports {};
    imports.define("env", "f", FuncType {}, makeHostFunction([](HostContext&, auto, auto) noexcept {
                       return Outcome {};
                   }));
    CHECK(errorOf(fixture.store.instantiate(fixture.load(builder), imports))
          == InstantiateError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
    // A host function's name imported as another kind, a global, does not link either.
    auto global = ModuleBuilder {};
    global.importGlobal("env", "f", GlobalType { .type = TypeI32 });
    CHECK(errorOf(fixture.store.instantiate(fixture.load(global), imports))
          == InstantiateError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
}

TEST_CASE("Store.modesAgreeOnTheReferenceEngine")
{
    // f32.div 0 / 0 (a NaN, as bits), and f32x4.relaxed_madd(inf, 0, 1) (NaNs whatever the fusing).
    auto const f32Zero = Bytes { 0x43, 0x00, 0x00, 0x00, 0x00 };
    auto v128Const = [](uint32_t lane) {
        auto bytes = Bytes { 0xFD, 0x0C };
        for ([[maybe_unused]] auto const index: std::views::iota(0, 4))
            for (auto const shift: { 0U, 8U, 16U, 24U })
                bytes.push_back(static_cast<uint8_t>(lane >> shift));
        return bytes;
    };
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { TypeI32 } })
        .type(FuncType { .params = {}, .results = { isoline::binary::V128 } })
        .function(0, {}, code({ f32Zero, f32Zero, { 0x95, 0xBC } }))
        .function(
            1,
            {},
            code({ v128Const(0x7F80'0000), v128Const(0), v128Const(0x3F80'0000), { 0xFD, 0x85, 0x02 } }))
        .exportFunction("nan", 0)
        .exportFunction("madd", 1);
    auto const run = [&](ExecutionMode mode) {
        auto fixture = Fixture { EngineConfig { .mode = mode } };
        auto instance = fixture.instantiate(builder);
        auto nan = std::array<Value, 1> {};
        auto madd = std::array<Value, 1> {};
        REQUIRE(instance.call(exportedFunction(instance, "nan"), {}, nan).has_value());
        REQUIRE(instance.call(exportedFunction(instance, "madd"), {}, madd).has_value());
        REQUIRE(std::holds_alternative<V128>(madd[0]));
        return std::pair { nan[0], madd[0] };
    };
    CHECK(run(ExecutionMode::Production) == run(ExecutionMode::Deterministic));
}

TEST_CASE("Store.uncaughtExceptionIsCopiedOut")
{
    // tag $e (i32); "throw": throw $e (i32.const 5).
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = {} })
        .type(FuncType {})
        .tag(0)
        .function(1, {}, code({ i32Const(5), { 0x08, 0x00 } }))
        .exportFunction("throw", 0)
        .exportEntity("e", ExternKind::Tag, 0);
    auto config = EngineConfig {};
    config.store.maxExceptions = 1;
    auto fixture = Fixture { config };
    auto instance = fixture.instantiate(builder);
    auto const tag = instance.tag("e");
    REQUIRE(tag.has_value());
    auto const thrower = exportedFunction(instance, "throw");
    // Each throw's sweep reclaims the previous copied-out exception's slot: never ExceptionLimitExceeded.
    for ([[maybe_unused]] auto const round: std::views::iota(0, 21))
    {
        auto const failed = instance.call(thrower, {}, {});
        CHECK(errorOf(failed).failure() == CallFailure::UncaughtException);
        CHECK(errorOf(failed).detail
              == CallError::Detail { UncaughtException { .tag = tag->address(), .payload = { I32 { 5 } } } });
    }
    CHECK(runtimeOf(fixture.store).liveExceptions() == 1); // the latest, unswept
    CHECK_FALSE(instance.tag("throw").has_value());
}

TEST_CASE("Store.engineMismatchIsRefused")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, {});
    auto fixture =
        Fixture { EngineConfig { .features = FeatureSet::wasm3(), .mode = ExecutionMode::Production } };
    auto const wasm2 = Engine { EngineConfig { .features = FeatureSet::wasm2() } }.load(builder.build());
    auto const deterministic =
        Engine { EngineConfig { .mode = ExecutionMode::Deterministic } }.load(builder.build());
    REQUIRE(wasm2.has_value());
    REQUIRE(deterministic.has_value());
    CHECK(errorOf(fixture.store.instantiate(*wasm2, Imports {})) == InstantiateError { EngineMismatch {} });
    CHECK(errorOf(fixture.store.instantiate(*deterministic, Imports {}))
          == InstantiateError { EngineMismatch {} });
    // The store's own engine's module instantiates.
    CHECK(fixture.store.instantiate(fixture.load(builder), Imports {}).has_value());
}

TEST_CASE("Store.outlivesItsEngine")
{
    auto allocator = HeapMemoryAllocator {};
    auto engine = std::optional<Engine> { std::in_place, EngineConfig {} };
    auto module = engine->load(addAndTrapModule().build());
    REQUIRE(module.has_value());
    auto store = Store { *engine, allocator };
    engine.reset();
    auto instance = store.instantiate(*module, Imports {});
    REQUIRE(instance.has_value());
    auto const args = std::array<Value, 2> { I32 { 2 }, I32 { 3 } };
    auto results = std::array<Value, 1> {};
    REQUIRE(instance->call(exportedFunction(*instance, "add"), args, results).has_value());
    CHECK(results[0] == Value { I32 { 5 } });
}

TEST_CASE("Store.memoryAllocationFailureIsReported")
{
    auto builder = ModuleBuilder {};
    builder.memory(pages(2));
    auto config = EngineConfig {};
    config.store.maxMemoryBytes = 65'536;
    auto fixture = Fixture { config };
    auto const error = errorOf(fixture.store.instantiate(fixture.load(builder), Imports {}));
    CHECK(std::holds_alternative<isoline::api::LimitExceeded>(error));
    CHECK_FALSE(std::holds_alternative<AllocationFailure>(error));
}

TEST_CASE("Store.exnrefResultsEscape")
{
    // "catch" returns the exception it caught as an exnref; the embedder holds it, so no sweep may
    // reclaim it: in a store of one exception, the next throw finds the arena full.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .type(FuncType { .params = {}, .results = { isoline::binary::ExnRef } })
        .tag(0)
        // block (result exnref) try_table (catch_all_ref 0) throw $e end unreachable end
        .function(1, {}, code({ { 0x02, 0x69, 0x1F, 0x40, 0x01, 0x03, 0x00, 0x08, 0x00, 0x0B, 0x00, 0x0B } }))
        .function(0, {}, code({ { 0x08, 0x00 } }))
        .exportFunction("catch", 0)
        .exportFunction("throw", 1);
    auto config = EngineConfig {};
    config.store.maxExceptions = 1;
    auto fixture = Fixture { config };
    auto instance = fixture.instantiate(builder);
    auto results = std::array<Value, 1> {};
    REQUIRE(instance.call(exportedFunction(instance, "catch"), {}, results).has_value());
    REQUIRE(std::holds_alternative<Ref>(results[0]));
    auto const held = std::get<Ref>(results[0]);
    CHECK(held.kind == RefKind::Exn);
    auto const thrown = instance.call(exportedFunction(instance, "throw"), {}, {});
    CHECK(errorOf(thrown).detail == CallError::Detail { Trap { TrapCode::ExceptionLimitExceeded } });
    CHECK(runtimeOf(fixture.store).isLiveException(held.address));
}

TEST_CASE("Store.exnrefsInAnUncaughtPayloadEscape")
{
    // "nest" catches $e as an exnref and throws it uncaught as the payload of $f. The copy the embedder
    // gets holds that exnref, so it escapes: a later sweep reclaims $f's own slot, never $e's.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .type(FuncType { .params = { isoline::binary::ExnRef }, .results = {} })
        .tag(0)
        .tag(1)
        // block (result exnref) try_table (catch_all_ref 0) throw $e end unreachable end; throw $f
        .function(
            0,
            {},
            code({ { 0x02, 0x69, 0x1F, 0x40, 0x01, 0x03, 0x00, 0x08, 0x00, 0x0B, 0x00, 0x0B, 0x08, 0x01 } }))
        .function(0, {}, code({ { 0x08, 0x00 } }))
        .exportFunction("nest", 0)
        .exportFunction("throw", 1);
    auto config = EngineConfig {};
    config.store.maxExceptions = 2;
    auto fixture = Fixture { config };
    auto instance = fixture.instantiate(builder);
    auto const nested = instance.call(exportedFunction(instance, "nest"), {}, {});
    auto const error = errorOf(nested);
    REQUIRE(std::holds_alternative<UncaughtException>(error.detail));
    auto const& payload = std::get<UncaughtException>(error.detail).payload;
    REQUIRE(payload.size() == 1);
    REQUIRE(std::holds_alternative<Ref>(payload[0]));
    // The arena is full ($e, $f): this throw sweeps, reclaiming $f's slot but not the escaped $e's.
    CHECK(errorOf(instance.call(exportedFunction(instance, "throw"), {}, {})).failure()
          == CallFailure::UncaughtException);
    CHECK(runtimeOf(fixture.store).liveExceptions() == 2);
    CHECK(runtimeOf(fixture.store).isLiveException(std::get<Ref>(payload[0]).address));
}

TEST_CASE("Store.crossInstanceLinksOfEveryKind")
{
    // The exporter's memory, table, mutable global and tag, imported and used by another instance.
    auto exporting = ModuleBuilder {};
    exporting.type(FuncType { .params = {}, .results = { TypeI32 } })
        .type(FuncType { .params = { TypeI32 }, .results = {} })
        .table(TableType { .element = isoline::binary::FuncRef, .limits = pages(1) })
        .memory(pages(1))
        .tag(1)
        .global(GlobalType { .type = TypeI32, .mutability = Mutability::Var }, i32Const(5))
        .function(0, {}, code({ { 0x23, 0x00 } }))       // global.get 0
        .function(0, {}, code({ { 0xFC, 0x10, 0x00 } })) // table.size 0
        .exportFunction("counter", 0)
        .exportFunction("table-size", 1)
        .exportEntity("memory", ExternKind::Memory, 0)
        .exportEntity("table", ExternKind::Table, 0)
        .exportEntity("global", ExternKind::Global, 0)
        .exportEntity("e", ExternKind::Tag, 0);
    auto importing = ModuleBuilder {};
    importing.type(FuncType {})
        .type(FuncType { .params = {}, .results = { TypeI32 } })
        .type(FuncType { .params = { TypeI32 }, .results = {} })
        .importMemory("other", "memory", pages(1))
        .importTable("other", "table", TableType { .element = isoline::binary::FuncRef, .limits = pages(1) })
        .importGlobal("other", "global", GlobalType { .type = TypeI32, .mutability = Mutability::Var })
        .importTag("other", "e", 2)
        // i32.store8 0xAB at 0
        .function(0, {}, code({ i32Const(0), i32Const(0xAB), memoryAccess(0x3A, 0, 0) }))
        // global.get 0; i32.const 1; i32.add; global.set 0
        .function(0, {}, code({ { 0x23, 0x00 }, i32Const(1), { 0x6A, 0x24, 0x00 } }))
        // table.grow 0 (ref.null func, 2)
        .function(1, {}, code({ { 0xD0, 0x70 }, i32Const(2), { 0xFC, 0x0F, 0x00 } }))
        // throw $e (9)
        .function(0, {}, code({ i32Const(9), { 0x08, 0x00 } }))
        .exportFunction("store", 0)
        .exportFunction("bump", 1)
        .exportFunction("grow-table", 2)
        .exportFunction("throw", 3);

    auto fixture = Fixture {};
    auto exporter = fixture.instantiate(exporting);
    auto imports = Imports {};
    for (auto const* name: { "memory", "table", "global", "e" })
        imports.define("other", name, exporter, name);
    auto importer = fixture.instantiate(importing, imports);

    REQUIRE(importer.call(exportedFunction(importer, "store"), {}, {}).has_value());
    auto const memory = exporter.memory("memory");
    REQUIRE(memory.has_value());
    auto const byte = exporter.memoryRange(*memory, 0, 1);
    REQUIRE(byte.has_value());
    CHECK((*byte)[0] == std::byte { 0xAB });

    REQUIRE(importer.call(exportedFunction(importer, "bump"), {}, {}).has_value());
    auto counter = std::array<Value, 1> {};
    REQUIRE(exporter.call(exportedFunction(exporter, "counter"), {}, counter).has_value());
    CHECK(counter[0] == Value { I32 { 6 } });

    auto grown = std::array<Value, 1> {};
    REQUIRE(importer.call(exportedFunction(importer, "grow-table"), {}, grown).has_value());
    CHECK(grown[0] == Value { I32 { 1 } });
    auto size = std::array<Value, 1> {};
    REQUIRE(exporter.call(exportedFunction(exporter, "table-size"), {}, size).has_value());
    CHECK(size[0] == Value { I32 { 3 } });

    auto const thrown = importer.call(exportedFunction(importer, "throw"), {}, {});
    auto const tag = exporter.tag("e");
    REQUIRE(tag.has_value());
    CHECK(errorOf(thrown).detail
          == CallError::Detail { UncaughtException { .tag = tag->address(), .payload = { I32 { 9 } } } });
}

TEST_CASE("Store.definedHostFunctionLinksThroughImports")
{
    auto calls = 0;
    auto const host =
        makeHostFunction([&calls](HostContext&, std::span<Value const> args, std::span<Value>) noexcept {
            calls += static_cast<int>(std::get<I32>(args[0]).bits);
            return Outcome {};
        });
    auto const type = FuncType { .params = { TypeI32 }, .results = {} };
    auto fixture = Fixture {};
    auto const function = fixture.store.define(type, host);
    REQUIRE(function.has_value());
    CHECK(function->type() == type);
    // The same implementation and type is the same function, whether defined or bound.
    auto const again = fixture.store.define(type, host);
    REQUIRE(again.has_value());
    CHECK(again->address() == function->address());

    auto imports = Imports {};
    imports.define("env", "f", *function).define("env", "g", type, host);
    auto builder = ModuleBuilder {};
    builder.type(type)
        .type(FuncType {})
        .importFunction("env", "f", 0)
        .importFunction("env", "g", 0)
        .function(1, {}, code({ i32Const(3), call(0), i32Const(4), call(1) }))
        .exportFunction("run", 2)
        .exportFunction("g", 1);
    auto instance = fixture.instantiate(builder, imports);
    REQUIRE(instance.call(exportedFunction(instance, "run"), {}, {}).has_value());
    CHECK(calls == 7);
    CHECK(exportedFunction(instance, "g").address() == function->address());
    CHECK(runtimeOf(fixture.store).functionCount() == 2); // the host function, and run

    // Another store does not link it.
    auto other = Fixture {};
    CHECK(errorOf(other.store.instantiate(other.load(builder), imports))
          == InstantiateError { ForeignImport { .index = 0 } });
}

TEST_CASE("Store.refusedHostLinkingConsumesNothing")
{
    // An import of another type, or a later import with no definition, refuses the instantiation
    // before any host function is added: the store holds no function and no type for it.
    auto const takesI32 = FuncType { .params = { TypeI32 }, .results = {} };
    auto imports = Imports {};
    imports.define(
        "env", "f", takesI32, makeHostFunction([](HostContext&, auto, auto) noexcept { return Outcome {}; }));
    auto wrongType = ModuleBuilder {};
    wrongType.type(FuncType { .params = { TypeI64 }, .results = {} }).importFunction("env", "f", 0);
    auto unknownLater = ModuleBuilder {};
    unknownLater.type(takesI32).importFunction("env", "f", 0).importFunction("env", "missing", 0);
    auto fixture = Fixture {};
    auto const typesBefore = runtimeOf(fixture.store).typeCount();
    CHECK(errorOf(fixture.store.instantiate(fixture.load(wrongType), imports))
          == InstantiateError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
    CHECK(errorOf(fixture.store.instantiate(fixture.load(unknownLater), imports))
          == InstantiateError { LinkError { LinkErrorCode::UnknownImport, 1 } });
    CHECK(runtimeOf(fixture.store).functionCount() == 0);
    CHECK(runtimeOf(fixture.store).typeCount() == typesBefore);
}

TEST_CASE("Store.memorySizeFollowsGrowth")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .memory(pages(1))
        .function(0, {}, code({ i32Const(2), memoryGrow(), drop() }))
        .exportFunction("grow", 0)
        .exportEntity("memory", ExternKind::Memory, 0);
    auto fixture = Fixture {};
    auto instance = fixture.instantiate(builder);
    auto const memory = instance.memory("memory");
    REQUIRE(memory.has_value());
    CHECK(instance.memorySize(*memory) == MemorySize { .pages = 1, .bytes = 65'536 });
    REQUIRE(instance.call(exportedFunction(instance, "grow"), {}, {}).has_value());
    CHECK(instance.memorySize(*memory) == MemorySize { .pages = 3, .bytes = uint64_t { 3 } * 65'536 });
}

TEST_CASE("Store.newHostTypeOverTheTypeBudgetIsRefused")
{
    // A host function whose type is new to the store and costs more type units than the store has left
    // is refused, by instantiate() and by define() alike, and adds nothing.
    constexpr auto WideParams = std::size_t { 16 };
    auto config = EngineConfig {};
    config.store.maxTypes = WideParams; // a type of 16 params costs 17 units
    auto fixture = Fixture { config };
    auto const wide = FuncType { .params = std::vector(WideParams, TypeI32), .results = {} };
    auto const host = makeHostFunction(
        [](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> Outcome { return {}; });
    auto builder = ModuleBuilder {};
    builder.type(wide).importFunction("env", "wide", 0);
    auto imports = Imports {};
    imports.define("env", "wide", wide, host);
    auto const types = isoline::api::LimitExceeded { isoline::api::Limit::Types };
    CHECK(errorOf(fixture.store.instantiate(fixture.load(builder), imports)) == InstantiateError { types });
    CHECK(errorOf(fixture.store.define(wide, host)) == isoline::api::DefineError { types });
    CHECK(runtimeOf(fixture.store).functionCount() == 0);
}

namespace
{

/// A module of one page of memory, whose "outer" calls "inner": two steps per call.
ModuleBuilder memoryAndCallModule()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .memory(pages(1))
        .function(0, {}, {})
        .function(0, {}, code({ call(0) }))
        .exportFunction("outer", 1);
    return builder;
}

/// Store options of @p limits and no observer.
StoreOptions optionsOf(StoreLimits const& limits)
{
    return StoreOptions { .limits = limits, .observer = nullptr };
}

} // namespace

TEST_CASE("Store.limitOverridesAreLocal", "[api]")
{
    // A store's overrides bind that store alone: another store of the same engine keeps the engine's
    // limits, at instantiation (maxMemories) and at run time (maxSteps) alike.
    auto const row = GENERATE(from_range(engineRows()));
    auto const engine = Engine { configOf(StoreLimits {}, row) };
    auto allocator = HeapMemoryAllocator {};
    auto const module = engine.load(memoryAndCallModule().build());
    REQUIRE(module.has_value());
    auto plain = Store { engine, allocator };

    SECTION("maxMemories")
    {
        auto limits = engine.config().store;
        limits.maxMemories = 0;
        auto capped = Store { engine, allocator, optionsOf(limits) };
        CHECK(errorOf(capped.instantiate(*module, Imports {}))
              == InstantiateError { isoline::api::LimitExceeded { Limit::Memories } });
        CHECK(plain.instantiate(*module, Imports {}).has_value());
    }

    SECTION("maxSteps")
    {
        // One step instantiates; a call of "outer" takes two, one per function entered.
        auto limits = engine.config().store;
        limits.maxSteps = 1;
        auto capped = Store { engine, allocator, optionsOf(limits) };
        auto const cappedInstance = capped.instantiate(*module, Imports {});
        REQUIRE(cappedInstance.has_value());
        auto const refused = cappedInstance->call(exportedFunction(*cappedInstance, "outer"), {}, {});
        CHECK(errorOf(refused).detail == CallError::Detail { Trap { TrapCode::StepLimitExceeded } });

        auto const plainInstance = plain.instantiate(*module, Imports {});
        REQUIRE(plainInstance.has_value());
        CHECK(plainInstance->call(exportedFunction(*plainInstance, "outer"), {}, {}).has_value());
    }
}

TEST_CASE("Store.withoutOverridesKeepsItsEnginesLimits", "[api]")
{
    // No override is the engine's limits, not the library's defaults; an override replaces them whole,
    // and may raise one above the engine's.
    auto const row = GENERATE(from_range(engineRows()));
    auto engineLimits = StoreLimits {};
    engineLimits.maxMemories = 0;
    auto const engine = Engine { configOf(engineLimits, row) };
    auto allocator = HeapMemoryAllocator {};
    auto const module = engine.load(memoryAndCallModule().build());
    REQUIRE(module.has_value());
    auto const refused = InstantiateError { isoline::api::LimitExceeded { Limit::Memories } };

    auto defaulted = Store { engine, allocator };
    CHECK(errorOf(defaulted.instantiate(*module, Imports {})) == refused);
    auto none = Store { engine, allocator, StoreOptions { .limits = std::nullopt, .observer = nullptr } };
    CHECK(errorOf(none.instantiate(*module, Imports {})) == refused);

    auto raised = engineLimits;
    raised.maxMemories = 1;
    auto overriding = Store { engine, allocator, optionsOf(raised) };
    CHECK(overriding.instantiate(*module, Imports {}).has_value());
    CHECK(overriding.used(Limit::Memories) == 1);
    CHECK(errorOf(overriding.instantiate(*module, Imports {})) == refused);
}

TEST_CASE("Store.overridesKeepModulesCompatible", "[api]")
{
    // Limits are no part of EngineIdentity: a module instantiates into a store of other limits, whether
    // they are a store's overrides or another engine's configuration of the same identity.
    auto const row = GENERATE(from_range(engineRows()));
    auto otherLimits = StoreLimits {};
    otherLimits.maxMemories = 1;
    otherLimits.maxCallDepth = 7;
    otherLimits.maxSteps = 1'000;
    auto const engine = Engine { configOf(StoreLimits {}, row) };
    auto const otherEngine = Engine { configOf(otherLimits, row) };
    REQUIRE(engine.config().identity() == otherEngine.config().identity());
    auto allocator = HeapMemoryAllocator {};
    auto const module = engine.load(memoryAndCallModule().build());
    auto const otherModule = otherEngine.load(memoryAndCallModule().build());
    REQUIRE(module.has_value());
    REQUIRE(otherModule.has_value());

    auto overriding = Store { engine, allocator, optionsOf(otherLimits) };
    CHECK(overriding.instantiate(*module, Imports {}).has_value());
    auto otherOverriding = Store { otherEngine, allocator, optionsOf(StoreLimits {}) };
    CHECK(otherOverriding.instantiate(*module, Imports {}).has_value());
    auto plain = Store { engine, allocator };
    CHECK(plain.instantiate(*otherModule, Imports {}).has_value());
}

TEST_CASE("Store.nestingOverrideIsHonoured", "[api]")
{
    // maxNestedInvocations bounds Wasm → host → Wasm recursion on the host stack, so a store's override
    // of it must bind: past N nested invocations the next traps, while a store of the engine's limit
    // nests deeper. N is small, far from any real stack.
    constexpr auto Nesting = uint32_t { 4 };
    constexpr auto Ceiling = Nesting + 2; // where the host stops recursing on its own
    auto const row = GENERATE(from_range(engineRows()));
    auto const engine = Engine { configOf(StoreLimits {}, row) };
    REQUIRE(engine.config().store.maxNestedInvocations > Ceiling);
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("env", "reenter", 0)
        .function(0, {}, call(0))
        .exportFunction("nest", 1);
    auto const module = engine.load(builder.build());
    REQUIRE(module.has_value());

    // Each entry re-enters "nest", one invocation deeper, until Ceiling; a nested failure propagates.
    auto entries = uint32_t { 0 };
    auto nest = std::optional<Function> {};
    auto imports = Imports {};
    imports.define(
        "env",
        "reenter",
        FuncType {},
        makeHostFunction(
            [&](HostContext& context, std::span<Value const>, std::span<Value>) noexcept -> Outcome {
                ++entries;
                if (entries >= Ceiling)
                    return {};
                return context.invoke(nest->address(), {}, {});
            }));
    auto allocator = HeapMemoryAllocator {};
    auto const run = [&](Store& store) {
        entries = 0;
        auto const instance = store.instantiate(*module, imports);
        REQUIRE(instance.has_value());
        nest = exportedFunction(*instance, "nest");
        return instance->call(*nest, {}, {});
    };

    auto limits = engine.config().store;
    limits.maxNestedInvocations = Nesting;
    auto capped = Store { engine, allocator, optionsOf(limits) };
    auto const refused = run(capped);
    CHECK(errorOf(refused).detail == CallError::Detail { Trap { TrapCode::CallStackExhausted } });
    CHECK(entries == Nesting); // invocations 1..N each entered the host; N + 1 was refused

    auto plain = Store { engine, allocator };
    CHECK(run(plain).has_value());
    CHECK(entries == Ceiling);
}

namespace
{

/// Counts the polls of one site, and ignores the rest.
class SitePolls final: public IPollObserver
{
  public:
    explicit SitePolls(PollSite site) noexcept: _site { site } {}

    void onPoll(PollSite site) noexcept override
    {
        if (site == _site)
            ++_count;
    }

    /// The polls of the site seen so far.
    [[nodiscard]] uint64_t count() const noexcept { return _count; }

  private:
    PollSite _site;
    uint64_t _count = 0;
};

/// The calls one invocation of a component call makes, each a step (its function entry): "realloc"
/// refuses itself a page of memory, "callee" refuses itself table elements and then traps, and
/// "post-return" does nothing. Neither memory nor table may grow in a store of one page and one element.
ModuleBuilder invocationModule()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .memory(pages(1))
        .table(TableType { .element = isoline::binary::FuncRef, .limits = pages(1) })
        .function(0, {}, code({ i32Const(1), memoryGrow(), drop() }))
        .function(0, {}, code({ { 0xD0, 0x70 }, i32Const(1), tableGrow(0), drop(), unreachable() }))
        .function(0, {}, {})
        .exportFunction("realloc", 0)
        .exportFunction("callee", 1)
        .exportFunction("post-return", 2);
    return builder;
}

/// A store of @p row's engine under @p limits, with @p module instantiated in it.
struct InvocationFixture
{
    InvocationFixture(isoline::api::testing::EngineRow const& row,
                      StoreLimits const& limits,
                      ModuleBuilder const& module,
                      Imports const& imports = {}):
        engine { configOf(limits, row) }
    {
        auto loaded = engine.load(module.build());
        REQUIRE(loaded.has_value());
        auto instantiated = store.instantiate(*loaded, imports);
        REQUIRE(instantiated.has_value());
        instance = *instantiated;
    }

    /// Calls @p name, of no params and no results.
    [[nodiscard]] std::expected<void, CallError> call(std::string_view name)
    {
        return store.call(exportedFunction(*instance, name), {}, {});
    }

    Engine engine;
    HeapMemoryAllocator allocator;
    Store store { engine, allocator };
    std::optional<Instance> instance;
};

/// The limits of the invocation cases: @p maxSteps, one page of memory and one table element.
StoreLimits invocationLimits(uint64_t maxSteps)
{
    auto limits = StoreLimits {};
    limits.maxSteps = maxSteps;
    limits.maxMemoryBytes = 65'536;
    limits.maxTableElements = 1;
    return limits;
}

} // namespace

TEST_CASE("Store.bulkPollChargesSteps", "[api]")
{
    // A poll charges a step per BulkPollBytes of host work, at least one, whole or not at all: of 8
    // steps, two polls of three granules take 6, and a third is refused, charging nothing.
    auto const row = GENERATE(from_range(engineRows()));
    auto const engine = Engine { configOf(invocationLimits(8), row) };
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    {
        auto const invocation = store.openInvocation();
        CHECK(store.poll(BulkGranule { .bytes = 3 * BulkPollBytes }).has_value());
        CHECK(store.used(Limit::Steps) == 3);
        CHECK(store.poll(BulkGranule { .bytes = (4 * BulkPollBytes) - 1 }).has_value());
        CHECK(store.used(Limit::Steps) == 6);
        auto const refused = store.poll(BulkGranule { .bytes = 3 * BulkPollBytes });
        CHECK(errorOf(refused).detail == CallError::Detail { Trap { TrapCode::StepLimitExceeded } });
        CHECK(store.used(Limit::Steps) == 6);
        // What is left is still there to take: less than a granule is one step, as is none.
        CHECK(store.poll(BulkGranule { .bytes = BulkPollBytes }).has_value());
        CHECK(store.poll(BulkGranule { .bytes = 0 }).has_value());
        CHECK(store.used(Limit::Steps) == 8);
        CHECK_FALSE(store.poll(BulkGranule { .bytes = BulkPollBytes - 1 }).has_value());
    }
    // The next invocation starts a budget of its own.
    auto const invocation = store.openInvocation();
    CHECK(store.poll(BulkGranule { .bytes = 8 * BulkPollBytes }).has_value());
    CHECK(store.used(Limit::Steps) == 8);
}

TEST_CASE("Store.bulkPollNotifiesAndInterrupts", "[api]")
{
    // Each poll is seen once at PollSite::HostBulk, before the interrupt is checked; once interrupted,
    // a poll ends with Interrupted, which beats a spent budget.
    auto const row = GENERATE(from_range(engineRows()));
    auto const engine = Engine { configOf(invocationLimits(1), row) };
    auto allocator = HeapMemoryAllocator {};
    auto polls = SitePolls { PollSite::HostBulk };
    auto store = Store { engine, allocator, StoreOptions { .limits = std::nullopt, .observer = &polls } };
    auto const invocation = store.openInvocation();
    CHECK(store.poll(BulkGranule { .bytes = BulkPollBytes }).has_value());
    CHECK(polls.count() == 1);
    CHECK(errorOf(store.poll(BulkGranule { .bytes = 0 })).detail
          == CallError::Detail { Trap { TrapCode::StepLimitExceeded } });
    CHECK(polls.count() == 2);
    store.interrupter()->interrupt();
    auto const interrupted = store.poll(BulkGranule { .bytes = 0 });
    CHECK(errorOf(interrupted).failure() == CallFailure::Interrupted);
    CHECK(errorOf(interrupted).detail == CallError::Detail { Trap { TrapCode::Interrupted } });
    CHECK(polls.count() == 3);
}

TEST_CASE("Store.invocationScopeSharesBudget", "[api]")
{
    // realloc, the callee and post-return, a step each. Outside a scope each call has a budget of its
    // own, so a budget of 2 lets all three through; in one scope they share it, and the third is refused.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = InvocationFixture { row, invocationLimits(2), invocationModule() };
    CHECK(fixture.call("realloc").has_value());
    CHECK(fixture.call("post-return").has_value());
    CHECK(fixture.call("post-return").has_value());
    CHECK(fixture.store.used(Limit::Steps) == 1);
    {
        auto const invocation = fixture.store.openInvocation();
        CHECK(fixture.store.used(Limit::Steps) == 0);
        CHECK(fixture.call("realloc").has_value());
        CHECK(fixture.call("post-return").has_value());
        CHECK(fixture.store.used(Limit::Steps) == 2);
        CHECK(errorOf(fixture.call("post-return")).detail
              == CallError::Detail { Trap { TrapCode::StepLimitExceeded } });
        // A bulk poll charges the same budget.
        CHECK_FALSE(fixture.store.poll(BulkGranule { .bytes = 0 }).has_value());
    }
    // used(Steps) keeps the scope's total; the next call starts afresh.
    CHECK(fixture.store.used(Limit::Steps) == 2);
    CHECK(fixture.call("post-return").has_value());
    CHECK(fixture.store.used(Limit::Steps) == 1);
}

TEST_CASE("Store.invocationScopeSharesGrowth", "[api]")
{
    // realloc is refused a memory.grow, then the callee a table.grow: within one scope they share one
    // GrowthRecord, which the callee's error and the scope report; alone, each call has its own.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = InvocationFixture { row, invocationLimits(100), invocationModule() };
    auto const both = GrowthRecord { .memory = Refusal::Limit, .table = Refusal::Limit };
    CHECK(fixture.call("realloc").has_value());
    CHECK(errorOf(fixture.call("callee")).refusedGrowth
          == GrowthRecord { .memory = {}, .table = Refusal::Limit });
    {
        auto const invocation = fixture.store.openInvocation();
        CHECK(invocation.refusedGrowth() == GrowthRecord {});
        CHECK(fixture.call("realloc").has_value());
        CHECK(invocation.refusedGrowth() == GrowthRecord { .memory = Refusal::Limit, .table = {} });
        auto const failed = fixture.call("callee");
        CHECK(errorOf(failed).failure() == CallFailure::Trap);
        CHECK(errorOf(failed).refusedGrowth == both);
        CHECK(fixture.call("post-return").has_value());
        CHECK(invocation.refusedGrowth() == both);
    }
    CHECK(errorOf(fixture.call("callee")).refusedGrowth
          == GrowthRecord { .memory = {}, .table = Refusal::Limit });
}

TEST_CASE("Store.invocationScopeNesting", "[api]")
{
    // A call made directly in a scope is the outermost invocation, the scope adding none; each hop a
    // host function makes back into the guest adds one. Of 2 nested invocations, the call and one hop
    // run, and the second hop is refused, in a scope as without one.
    auto const row = GENERATE(from_range(engineRows()));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("env", "reenter", 0)
        .function(0, {}, call(0))
        .exportFunction("nest", 1);
    auto store = static_cast<Store*>(nullptr);
    auto nest = std::optional<Function> {};
    auto depths = std::vector<uint64_t> {};
    auto hop = std::optional<CallError> {};
    auto imports = Imports {};
    imports.define(
        "env",
        "reenter",
        FuncType {},
        makeHostFunction([&](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> Outcome {
            depths.push_back(store->used(Limit::NestedInvocations));
            if (auto const nested = store->call(*nest, {}, {}); !nested)
                hop = nested.error();
            return {};
        }));
    auto limits = invocationLimits(100);
    limits.maxNestedInvocations = 2;
    auto fixture = InvocationFixture { row, limits, builder, imports };
    store = &fixture.store;
    nest = exportedFunction(*fixture.instance, "nest");
    auto const run = [&] {
        depths.clear();
        hop.reset();
        CHECK(fixture.call("nest").has_value());
        CHECK(depths == std::vector<uint64_t> { 1, 2 });
        REQUIRE(hop.has_value());
        CHECK(hop->detail == CallError::Detail { Trap { TrapCode::CallStackExhausted } });
    };
    run();
    auto const invocation = fixture.store.openInvocation();
    CHECK(fixture.store.used(Limit::NestedInvocations) == 0);
    run();
    run();
}

TEST_CASE("Store.invocationScopeJoinsWhenNested", "[api]")
{
    // A scope opened while one is open at the same depth joins it: it counts nothing, keeps the budget
    // and reports the record of the scope it joined.
    auto const row = GENERATE(from_range(engineRows()));
    STATIC_CHECK_FALSE(std::is_copy_constructible_v<InvocationScope>);
    STATIC_CHECK_FALSE(std::is_copy_assignable_v<InvocationScope>);
    STATIC_CHECK(std::is_nothrow_move_constructible_v<InvocationScope>);
    auto fixture = InvocationFixture { row, invocationLimits(4), invocationModule() };
    {
        auto const outer = fixture.store.openInvocation();
        CHECK(fixture.call("post-return").has_value());
        {
            auto inner = fixture.store.openInvocation();
            auto const moved = std::move(inner);
            CHECK(fixture.store.used(Limit::Steps) == 1);
            CHECK(fixture.call("realloc").has_value());
            CHECK(moved.refusedGrowth() == outer.refusedGrowth());
        }
        // The joined scope closed and the outer one holds on: its budget and its record.
        CHECK(fixture.store.used(Limit::Steps) == 2);
        CHECK(outer.refusedGrowth() == GrowthRecord { .memory = Refusal::Limit, .table = {} });
        CHECK(fixture.call("post-return").has_value());
        CHECK(fixture.store.used(Limit::Steps) == 3);
    }
    // A scope moved from holds nothing: the invocation ends with the scope it moved to.
    {
        auto taken = std::optional<InvocationScope> {};
        {
            auto opened = fixture.store.openInvocation();
            CHECK(fixture.call("post-return").has_value());
            taken.emplace(std::move(opened));
        }
        CHECK(fixture.call("post-return").has_value());
        CHECK(fixture.store.used(Limit::Steps) == 2);
    }
    CHECK(fixture.call("post-return").has_value());
    CHECK(fixture.store.used(Limit::Steps) == 1);
}

TEST_CASE("Store.invocationScopeInAHostFunction", "[api]")
{
    // Opened from a host function, a scope holds a record of its own for the hops the host makes in
    // it, while the budget stays the call's: "grow-then-hop" is refused a page and calls the host, which
    // opens a scope and calls "grow-table", refused an element, twice. The scope sees the table refusal
    // only; the outer call, trapping after the host returns, the memory refusal only.
    auto const row = GENERATE(from_range(engineRows()));
    auto store = static_cast<Store*>(nullptr);
    auto growTable = std::optional<Function> {};
    auto stepsAtOpen = std::optional<uint64_t> {};
    auto scopeRecord = std::optional<GrowthRecord> {};
    auto imports = Imports {};
    imports.define(
        "env",
        "hop",
        FuncType {},
        makeHostFunction([&](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> Outcome {
            auto const scope = store->openInvocation();
            // The call's entry, then the host function's: the call's budget goes on.
            stepsAtOpen = store->used(Limit::Steps);
            CHECK(scope.refusedGrowth() == GrowthRecord {});
            CHECK(store->call(*growTable, {}, {}).has_value());
            CHECK(store->call(*growTable, {}, {}).has_value());
            scopeRecord = scope.refusedGrowth();
            return {};
        }));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("env", "hop", 0)
        .memory(pages(1))
        .table(TableType { .element = isoline::binary::FuncRef, .limits = pages(1) })
        .function(0, {}, code({ { 0xD0, 0x70 }, i32Const(1), tableGrow(0), drop() }))
        .function(0, {}, code({ i32Const(1), memoryGrow(), drop(), call(0), unreachable() }))
        .exportFunction("grow-table", 1)
        .exportFunction("grow-then-hop", 2);
    auto fixture = InvocationFixture { row, invocationLimits(100), builder, imports };
    store = &fixture.store;
    growTable = exportedFunction(*fixture.instance, "grow-table");
    auto const failed = fixture.call("grow-then-hop");
    CHECK(errorOf(failed).failure() == CallFailure::Trap);
    CHECK(errorOf(failed).refusedGrowth == GrowthRecord { .memory = Refusal::Limit, .table = {} });
    CHECK(stepsAtOpen == std::optional<uint64_t> { 2 });
    CHECK(scopeRecord
          == std::optional<GrowthRecord> { GrowthRecord { .memory = {}, .table = Refusal::Limit } });

    // Within an outer scope alike; once the host's scope ends, the outer one holds on, and a later call
    // made in it records into it.
    scopeRecord.reset();
    auto const outer = fixture.store.openInvocation();
    CHECK(errorOf(fixture.call("grow-then-hop")).refusedGrowth
          == GrowthRecord { .memory = Refusal::Limit, .table = {} });
    CHECK(scopeRecord
          == std::optional<GrowthRecord> { GrowthRecord { .memory = {}, .table = Refusal::Limit } });
    CHECK(fixture.store.call(*growTable, {}, {}).has_value());
    CHECK(outer.refusedGrowth() == GrowthRecord { .memory = Refusal::Limit, .table = Refusal::Limit });
}

TEST_CASE("Store.instantiationInsideAScope", "[api]")
{
    // An instantiation in a scope shares its budget, its start function included, but keeps its refused
    // grows to itself. Instantiating costs 3 steps (its entry, before the start function, and the start
    // function's entry): of 3, it fits alone, but not after a call in the scope took one.
    auto const row = GENERATE(from_range(engineRows()));
    auto starter = ModuleBuilder {};
    starter.type(FuncType {})
        .memory(pages(1))
        .function(0, {}, code({ i32Const(100), memoryGrow(), drop() }))
        .start(0);
    auto limits = invocationLimits(3);
    limits.maxMemoryBytes =
        uint64_t { 8 } * 65'536; // room for every instance's page, none for the start's 100
    auto fixture = InvocationFixture { row, limits, invocationModule() };
    auto const module = fixture.engine.load(starter.build());
    REQUIRE(module.has_value());
    CHECK(fixture.store.instantiate(*module, {}).has_value());
    CHECK(fixture.store.used(Limit::Steps) == 3);
    {
        auto const invocation = fixture.store.openInvocation();
        CHECK(fixture.call("post-return").has_value());
        auto const refused = callErrorOf(fixture.store.instantiate(*module, {}));
        CHECK(refused.detail == CallError::Detail { Trap { TrapCode::StepLimitExceeded } });
        CHECK(fixture.store.used(Limit::Steps) == 3);
    }
    {
        // Within the budget, the start function's refused page stays out of the scope's record.
        auto const invocation = fixture.store.openInvocation();
        CHECK(fixture.store.instantiate(*module, {}).has_value());
        CHECK(invocation.refusedGrowth() == GrowthRecord {});
    }
}

TEST_CASE("Store.hostPollsOutsideAScope", "[api]")
{
    // A host function polls with no scope open: the call's own budget pays. Of 4 steps, the call's
    // entry and the host function's take 2, a poll of two granules the rest, and one more is refused;
    // the next call starts a budget of its own.
    auto const row = GENERATE(from_range(engineRows()));
    auto store = static_cast<Store*>(nullptr);
    auto outcomes = std::vector<bool> {};
    auto imports = Imports {};
    imports.define(
        "env",
        "work",
        FuncType {},
        makeHostFunction([&](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> Outcome {
            outcomes.push_back(store->poll(BulkGranule { .bytes = 2 * BulkPollBytes }).has_value());
            outcomes.push_back(store->used(Limit::Steps) == 4);
            auto const refused = store->poll(BulkGranule { .bytes = 0 });
            outcomes.push_back(!refused.has_value()
                               && refused.error().detail
                                      == CallError::Detail { Trap { TrapCode::StepLimitExceeded } });
            return {};
        }));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("env", "work", 0)
        .function(0, {}, call(0))
        .exportFunction("work", 1);
    auto fixture = InvocationFixture { row, invocationLimits(4), builder, imports };
    store = &fixture.store;
    for ([[maybe_unused]] auto const round: std::views::iota(0, 2))
    {
        outcomes.clear();
        // The host return's step is past the budget: the call ends there.
        CHECK(errorOf(fixture.call("work")).detail
              == CallError::Detail { Trap { TrapCode::StepLimitExceeded } });
        CHECK(outcomes == std::vector<bool> { true, true, true });
    }
}

TEST_CASE("Store.scopeOutlivesItsStore", "[api]")
{
    // Destroying the store first lets go of its scope, whose end then does nothing (ASan sees no use
    // after free).
    auto const row = GENERATE(from_range(engineRows()));
    auto const engine = Engine { configOf(invocationLimits(10), row) };
    auto allocator = HeapMemoryAllocator {};
    auto store = std::make_unique<Store>(engine, allocator);
    auto scope = std::optional<InvocationScope> {};
    scope.emplace(store->openInvocation());
    CHECK(store->poll(BulkGranule { .bytes = 0 }).has_value());
    store.reset();
    scope.reset();
    CHECK_FALSE(scope.has_value());
}

TEST_CASE("Store.bulkPollInterruptedFromAnotherThread", "[api][threading]")
{
    // A host bulk-poll loop in a scope, interrupted by a watchdog thread once it runs: the loop ends with
    // Interrupted. The loop is bounded, so a missed interrupt fails rather than hangs.
    auto const row = GENERATE(from_range(engineRows()));
    auto const engine = Engine { configOf(invocationLimits(std::numeric_limits<uint64_t>::max()), row) };
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const interrupter = store.interrupter();
    auto started = std::atomic<bool> { false };
    auto watchdog = std::thread { [&] {
        while (!started.load(std::memory_order_acquire))
            std::this_thread::yield();
        interrupter->interrupt();
    } };
    auto outcome = std::optional<CallError> {};
    {
        auto const invocation = store.openInvocation();
        for ([[maybe_unused]] auto const round: std::views::iota(uint64_t { 0 }, uint64_t { 100'000'000 }))
        {
            started.store(true, std::memory_order_release);
            if (auto polled = store.poll(BulkGranule { .bytes = BulkPollBytes }); !polled)
            {
                outcome = polled.error();
                break;
            }
        }
    }
    started.store(true, std::memory_order_release);
    watchdog.join();
    REQUIRE(outcome.has_value());
    CHECK(outcome->failure() == CallFailure::Interrupted);
}

TEST_CASE("Store.hopOutsideAScopeKeepsItsRecord", "[api]")
{
    // In a scope opened outside every call, a host function hops into a guest without opening a scope
    // of its own: the hop is refused a table element and traps; its refusal is its own CallError's,
    // never the scope's.
    auto const row = GENERATE(from_range(engineRows()));
    auto store = static_cast<Store*>(nullptr);
    auto growTable = std::optional<Function> {};
    auto hop = std::optional<CallError> {};
    auto imports = Imports {};
    imports.define(
        "env",
        "hop",
        FuncType {},
        makeHostFunction([&](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> Outcome {
            if (auto const nested = store->call(*growTable, {}, {}); !nested)
                hop = nested.error();
            return {};
        }));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("env", "hop", 0)
        .table(TableType { .element = isoline::binary::FuncRef, .limits = pages(1) })
        .function(0, {}, code({ { 0xD0, 0x70 }, i32Const(1), tableGrow(0), drop(), unreachable() }))
        .function(0, {}, call(0))
        .exportFunction("grow-table-then-trap", 1)
        .exportFunction("hop", 2);
    auto fixture = InvocationFixture { row, invocationLimits(100), builder, imports };
    store = &fixture.store;
    growTable = exportedFunction(*fixture.instance, "grow-table-then-trap");
    auto const outer = fixture.store.openInvocation();
    CHECK(fixture.call("hop").has_value());
    REQUIRE(hop.has_value());
    CHECK(hop->failure() == CallFailure::Trap);
    CHECK(hop->refusedGrowth == GrowthRecord { .memory = {}, .table = Refusal::Limit });
    CHECK(outer.refusedGrowth() == GrowthRecord {});
}

TEST_CASE("Store.destroyedMidCallIsAContractViolation", "[.contract]")
{
    // Destroying the store from a host function while a call made in a scope runs ends the process
    // (Require): never a silent abandon of the scope. Hidden: CTest runs it alone and expects the
    // report (isoline_contract_store_destroyed_mid_call). The fail handler reports the check and ends
    // the process at once, as a failed check would, without the abort CTest counts as a crash.
    core::setFailHandler(
        [](std::string_view text, std::string_view message, std::string_view file, int line) {
            std::cerr << "contract check failed: [" << file << ':' << line << "] " << message << ' ' << text
                      << '\n'
                      << std::flush;
            std::_Exit(0);
        });
    auto const engine = Engine { configOf(invocationLimits(100), engineRows().front()) };
    auto allocator = HeapMemoryAllocator {};
    auto store = std::make_unique<Store>(engine, allocator);
    auto imports = Imports {};
    imports.define(
        "env",
        "destroy",
        FuncType {},
        makeHostFunction([&](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> Outcome {
            store.reset();
            return {};
        }));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("env", "destroy", 0)
        .function(0, {}, call(0))
        .exportFunction("destroy", 1);
    auto const module = engine.load(builder.build());
    REQUIRE(module.has_value());
    auto const instance = store->instantiate(*module, imports);
    REQUIRE(instance.has_value());
    auto const scope = store->openInvocation();
    CHECK(scope.refusedGrowth() == GrowthRecord {});
    std::ignore = instance->call(exportedFunction(*instance, "destroy"), {}, {});
    FAIL("the store was destroyed mid-call without a contract violation");
}
