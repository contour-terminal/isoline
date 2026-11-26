// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::binary::AddressType;
using isoline::binary::ExternKind;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::binary::testing::ModuleBuilder;
using isoline::runtime::AllocationError;
using isoline::runtime::AllocationFailure;
using isoline::runtime::I32;
using isoline::runtime::InstanceDemand;
using isoline::runtime::InstantiationError;
using isoline::runtime::Limit;
using isoline::runtime::LimitExceeded;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Refusal;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::Trap;
using isoline::runtime::UnlimitedAllocator;
using isoline::runtime::Value;

namespace
{

/// The default page size, 64 KiB.
constexpr auto PageSize = isoline::binary::pageSize(MemoryType {});

/// Limits of at least @p min, unbounded, of i32 addresses.
Limits atLeast(uint64_t min)
{
    return Limits { .min = min, .max = std::nullopt, .addressType = AddressType::I32 };
}

/// The null funcref, which a new table's slots hold.
constexpr auto NullFunction = Ref { .kind = RefKind::Null, .heap = FuncRef.heap, .address = 0 };

/// A store of given limits, and an interpreter to instantiate modules in it.
class LimitedStore
{
  public:
    explicit LimitedStore(StoreLimits limits): _store { _allocator, limits } {}

    /// Decodes, validates and instantiates @p builder's module, which must be valid.
    /// @param builder The module.
    /// @param imports What its imports bind to.
    /// @return The instance's address, or why instantiation failed.
    std::expected<uint32_t, InstantiationError> instantiate(
        ModuleBuilder const& builder, std::vector<isoline::runtime::ExternValue> const& imports = {})
    {
        auto validated = isoline::validate::ValidatedModule::load(
            builder.build(), isoline::FeatureSet::all(), isoline::binary::DecodeLimits {});
        REQUIRE(validated.has_value());
        return isoline::runtime::instantiate(_store, _interpreter, std::move(*validated), imports);
    }

    [[nodiscard]] Store& store() noexcept { return _store; }

  private:
    UnlimitedAllocator _allocator;
    Store _store;
    isoline::interpreter::ReferenceInterpreter _interpreter;
};

/// A module of @p count memories of @p pages pages each.
ModuleBuilder moduleWithMemories(uint32_t count, uint64_t pages)
{
    auto builder = ModuleBuilder {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        builder.memory(atLeast(pages));
    return builder;
}

/// A module of one funcref table of @p elements elements for each of @p tables.
ModuleBuilder moduleWithTables(std::vector<uint64_t> const& tables)
{
    auto builder = ModuleBuilder {};
    for (auto const elements: tables)
        builder.table(TableType { .element = FuncRef, .limits = atLeast(elements) });
    return builder;
}

/// The LimitExceeded an instantiation failed with, which it must have.
LimitExceeded limitOf(std::expected<uint32_t, InstantiationError> const& result)
{
    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::holds_alternative<LimitExceeded>(result.error()));
    return std::get<LimitExceeded>(result.error());
}

} // namespace

TEST_CASE("Store.constAccessors")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const one = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 };
    auto const function = store.addHostFunction(
        FuncType {},
        [](std::span<Value const>, std::span<Value>) -> std::expected<void, Trap> { return {}; });
    auto const table = store.addTable(TableType { .element = FuncRef, .limits = one },
                                      Ref { .kind = RefKind::Null, .heap = FuncRef.heap, .address = 0 });
    auto const memory = store.addMemory(MemoryType { .limits = one });
    auto const global = store.addGlobal(
        GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Const }, I32 { .bits = 7 });
    REQUIRE(table.has_value());
    REQUIRE(memory.has_value());

    // A const store answers through the const overloads, with the same instances.
    auto const& view = static_cast<Store const&>(store);
    CHECK(view.function(function).isHost());
    CHECK(view.table(*table).elements.size() == 1);
    CHECK(view.memory(*memory).bytes.size() == PageSize);
    CHECK(view.global(global).value == Value { I32 { .bits = 7 } });
    CHECK(&view.global(global) == &store.global(global));
}

TEST_CASE("Store.addressTypesBoundGrowth")
{
    // A guest can name sizes no host can hold; the store refuses them without allocating.
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const at64 = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I64 };
    auto const memory = store.addMemory(MemoryType { .limits = at64 });
    auto const table = store.addTable(TableType { .element = FuncRef, .limits = at64 },
                                      Ref { .kind = RefKind::Null, .heap = FuncRef.heap, .address = 0 });
    REQUIRE(memory.has_value());
    REQUIRE(table.has_value());
    // Past 2^48 pages, the memory64 reach; past the store's 4 GiB of memory; past 2^64 - 1 slots, and
    // past what a byte count of slots can express.
    CHECK(store.growMemory(*memory, uint64_t { 1 } << 48) == std::unexpected { Refusal::TypeMaximum });
    CHECK(store.growMemory(*memory, uint64_t { 1 } << 20) == std::unexpected { Refusal::Limit });
    CHECK(store.growTable(*table, ~uint64_t { 0 }, Ref {}) == std::unexpected { Refusal::TypeMaximum });
    CHECK(store.growTable(*table, uint64_t { 1 } << 62, Ref {}) == std::unexpected { Refusal::TypeMaximum });
    CHECK(store.memory(*memory).bytes.size() == PageSize);
    CHECK(store.table(*table).elements.size() == 1);
    // What the store grants, it grows by.
    CHECK(store.growMemory(*memory, 1) == 1U);
    CHECK(store.growTable(*table, 1, Ref {}) == 1U);
}

TEST_CASE("Store.byteGranularMemory")
{
    // A memory of 1-byte pages holds as many bytes as pages, and grows by bytes.
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const memory =
        store.addMemory(MemoryType { .limits = Limits { .min = 3, .max = std::nullopt }, .pageSizeLog2 = 0 });
    REQUIRE(memory.has_value());
    CHECK(store.memory(*memory).bytes.size() == 3);
    CHECK(store.growMemory(*memory, 1) == 3U);
    CHECK(store.memory(*memory).bytes.size() == 4);
    // An i32 memory of 1-byte pages reaches 2^32 - 1 bytes, not 2^32.
    CHECK(store.growMemory(*memory, 0xFFFF'FFFC) == std::unexpected { Refusal::TypeMaximum });
    // With i64 addresses it reaches 2^64 - 1 pages, which no product overflows to grant: the store
    // still refuses what is past its 4 GiB.
    auto const wide = store.addMemory(
        MemoryType { .limits = Limits { .min = 0, .max = std::nullopt, .addressType = AddressType::I64 },
                     .pageSizeLog2 = 0 });
    REQUIRE(wide.has_value());
    CHECK(store.growMemory(*wide, ~uint64_t { 0 }) == std::unexpected { Refusal::Limit });
    CHECK(store.growMemory(*wide, (uint64_t { 1 } << 32) + 1) == std::unexpected { Refusal::Limit });
    CHECK(store.growMemory(*wide, 16) == 0U);
    CHECK(store.memory(*wide).bytes.size() == 16);
}

TEST_CASE("Store.internTypeHashConses")
{
    using isoline::binary::concreteRef;
    using isoline::binary::Nullable;
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const takesI32 = store.internType(FuncType { .params = { isoline::binary::I32 }, .results = {} });
    auto const returnsI32 = store.internType(FuncType { .params = {}, .results = { isoline::binary::I32 } });
    CHECK(store.internType(FuncType { .params = { isoline::binary::I32 }, .results = {} }) == takesI32);
    CHECK(returnsI32 != takesI32);
    // A type naming another names it by its TypeId, so two such types are equal when the ids are.
    auto const takesRef =
        store.internType(FuncType { .params = { concreteRef(takesI32, Nullable::No) }, .results = {} });
    CHECK(store.internType(FuncType { .params = { concreteRef(takesI32, Nullable::No) }, .results = {} })
          == takesRef);
    CHECK(store.internType(FuncType { .params = { concreteRef(returnsI32, Nullable::No) }, .results = {} })
          != takesRef);
    CHECK(store.internType(FuncType { .params = { concreteRef(takesI32, Nullable::Yes) }, .results = {} })
          != takesRef);
    // Functions intern their types.
    auto const host = store.addHostFunction(
        FuncType { .params = { isoline::binary::I32 }, .results = {} },
        [](std::span<Value const>, std::span<Value>) -> std::expected<void, Trap> { return {}; });
    CHECK(store.function(host).typeId == takesI32);
    auto const defined = store.addModuleFunction(returnsI32, 0, 0);
    CHECK(store.function(defined).typeId == returnsI32);
    CHECK(store.functionType(host) == FuncType { .params = { isoline::binary::I32 }, .results = {} });
}

TEST_CASE("Store.findTypeDoesNotIntern")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const takesI32 = FuncType { .params = { isoline::binary::I32 }, .results = {} };
    CHECK(store.typeCount() == 0);
    CHECK(store.findType(takesI32) == std::nullopt);
    CHECK(store.typeCount() == 0);
    // A new type gets the next TypeId, typeCount() before it, and is found from then on.
    auto const id = store.internType(takesI32);
    CHECK(id == 0);
    CHECK(store.typeCount() == 1);
    CHECK(store.findType(takesI32) == id);
    CHECK(store.internType(FuncType {}) == 1);
    CHECK(store.internType(takesI32) == id);
    CHECK(store.typeCount() == 2);
}

TEST_CASE("Store.typeRoundTrip")
{
    using isoline::binary::concreteRef;
    using isoline::binary::Nullable;
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const empty = FuncType {};
    auto const wide =
        FuncType { .params = { isoline::binary::I64, FuncRef }, .results = { isoline::binary::F32 } };
    auto const emptyId = store.internType(empty);
    auto const wideId = store.internType(wide);
    auto const typed = FuncType { .params = { concreteRef(wideId, Nullable::Yes) }, .results = {} };
    auto const typedId = store.internType(typed);
    CHECK(store.type(emptyId) == empty);
    CHECK(store.type(wideId) == wide);
    CHECK(store.type(typedId) == typed);
    // A reference stays valid as the store interns more.
    auto const& kept = store.type(wideId);
    for (auto const count: std::views::iota(uint32_t { 0 }, uint32_t { 100 }))
        std::ignore =
            store.internType(FuncType { .params = std::vector(count, isoline::binary::I32), .results = {} });
    CHECK(kept == wide);
}

TEST_CASE("Store.tagsAreDistinct")
{
    // Every tag added is a tag of its own, whatever its type; equal types share their TypeId.
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const type = FuncType { .params = { isoline::binary::I32 }, .results = {} };
    auto const first = store.addTag(store.internType(type));
    auto const second = store.addTag(store.internType(type));
    CHECK(first != second);
    CHECK(store.tag(first).typeId == store.tag(second).typeId);
    CHECK(store.findType(type) == store.tag(first).typeId);
}

TEST_CASE("Store.tagNamesItsTypeOnly")
{
    // A tag holds its TypeId and nothing else: its payload's types are read through the store, so a
    // tag of a wide type costs no more than one of an empty type.
    static_assert(sizeof(isoline::runtime::TagInstance) == sizeof(isoline::runtime::TypeId));
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const wide = FuncType { .params = std::vector(1000, isoline::binary::I32), .results = {} };
    auto const tag = store.addTag(store.internType(wide));
    CHECK(store.type(store.tag(tag).typeId) == wide);
    CHECK(store.addException(tag, std::vector<Value>(1000, I32 { .bits = 1 })).has_value());
}

TEST_CASE("Store.exceptionLimits")
{
    using isoline::runtime::TrapCode;
    auto const allocator = UnlimitedAllocator {};
    auto const payload = [](std::size_t count) {
        return std::vector<Value>(count, I32 { .bits = 7 });
    };

    SECTION("the number of exceptions")
    {
        auto store = Store { allocator, StoreLimits { .maxExceptions = 2 } };
        auto const tag = store.addTag(store.internType(FuncType {}));
        auto const first = store.addException(tag, {});
        REQUIRE(first.has_value());
        REQUIRE(store.addException(tag, {}).has_value());
        auto const third = store.addException(tag, {});
        REQUIRE_FALSE(third.has_value());
        CHECK(third.error() == Trap { TrapCode::ExceptionLimitExceeded });
        CHECK(isoline::runtime::specMessage(third.error()) == "exception limit exceeded");
        CHECK(store.exception(*first).tag == tag);
    }

    SECTION("the values of their payloads")
    {
        auto store = Store { allocator, StoreLimits { .maxExceptionValues = 3 } };
        using isoline::binary::I32;
        auto const none = store.addTag(store.internType(FuncType {}));
        auto const one = store.addTag(store.internType(FuncType { .params = { I32 }, .results = {} }));
        auto const two = store.addTag(store.internType(FuncType { .params = { I32, I32 }, .results = {} }));
        auto const first = store.addException(two, payload(2));
        REQUIRE(first.has_value());
        CHECK(store.exception(*first).payload == payload(2));
        CHECK(store.addException(two, payload(2)).error() == Trap { TrapCode::ExceptionLimitExceeded });
        CHECK(store.addException(one, payload(1)).has_value());
        CHECK(store.addException(none, {}).has_value());
        CHECK_FALSE(store.addException(one, payload(1)).has_value());
    }
}

TEST_CASE("Store.memoryLimitIsStoreWide")
{
    // 100 memories of one page in a store of 10 pages in all: refused before any is allocated.
    auto limited = LimitedStore { StoreLimits { .maxMemoryBytes = 10 * PageSize } };
    auto& store = limited.store();
    CHECK(limitOf(limited.instantiate(moduleWithMemories(100, 1))) == LimitExceeded { Limit::MemoryBytes });
    CHECK(store.moduleInstanceCount() == 0);
    CHECK(store.memoryCount() == 0);
    // The whole budget is still there.
    CHECK(limited.instantiate(moduleWithMemories(10, 1)).has_value());
    CHECK(store.memoryCount() == 10);
    // And now spent.
    CHECK(limitOf(limited.instantiate(moduleWithMemories(1, 1))) == LimitExceeded { Limit::MemoryBytes });
    CHECK(limited.instantiate(moduleWithMemories(1, 0)).has_value());
}

TEST_CASE("Store.growPastLimitIsRefused")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator, StoreLimits { .maxMemoryBytes = 2 * PageSize, .maxTableElements = 2 } };
    auto const memory = store.addMemory(MemoryType { .limits = atLeast(1) });
    REQUIRE(memory.has_value());
    CHECK(store.growMemory(*memory, 1) == 1U);
    CHECK(store.growMemory(*memory, 1) == std::unexpected { Refusal::Limit });
    // Past an i32 memory's 2^16 pages: the type's reach, whatever the budget.
    CHECK(store.growMemory(*memory, uint64_t { 1 } << 40) == std::unexpected { Refusal::TypeMaximum });
    CHECK(store.memory(*memory).bytes.size() == 2 * PageSize);

    auto const table = store.addTable(TableType { .element = FuncRef, .limits = atLeast(1) }, NullFunction);
    REQUIRE(table.has_value());
    CHECK(store.growTable(*table, 1, NullFunction) == 1U);
    CHECK(store.growTable(*table, 1, NullFunction) == std::unexpected { Refusal::Limit });
    CHECK(store.table(*table).elements.size() == 2);
}

TEST_CASE("Store.tableLimitIsStoreWide")
{
    auto limited = LimitedStore { StoreLimits { .maxTableElements = 1'024 } };
    auto& store = limited.store();
    CHECK(limitOf(limited.instantiate(moduleWithTables({ 600, 600 })))
          == LimitExceeded { Limit::TableElements });
    CHECK(store.moduleInstanceCount() == 0);
    CHECK(store.tableCount() == 0);
    CHECK(limited.instantiate(moduleWithTables({ 1'024 })).has_value());
    CHECK(store.tableCount() == 1);
}

TEST_CASE("Store.instanceCountIsCapped")
{
    auto limited = LimitedStore { StoreLimits { .maxInstances = 2 } };
    CHECK(limited.instantiate(ModuleBuilder {}).has_value());
    CHECK(limited.instantiate(ModuleBuilder {}).has_value());
    CHECK(limitOf(limited.instantiate(ModuleBuilder {})) == LimitExceeded { Limit::Instances });
    CHECK(limited.store().moduleInstanceCount() == 2);
}

TEST_CASE("Store.entityCountsAreCapped")
{
    auto limited = LimitedStore { StoreLimits { .maxMemories = 2, .maxTables = 1 } };
    auto& store = limited.store();
    CHECK(limitOf(limited.instantiate(moduleWithMemories(3, 0))) == LimitExceeded { Limit::Memories });
    CHECK(limitOf(limited.instantiate(moduleWithTables({ 0, 0 }))) == LimitExceeded { Limit::Tables });
    CHECK(store.memoryCount() == 0);
    CHECK(store.tableCount() == 0);
    CHECK(limited.instantiate(moduleWithMemories(2, 0)).has_value());
    // An embedder's own memory or table is capped alike.
    CHECK(store.addMemory(MemoryType { .limits = atLeast(0) })
          == std::unexpected { AllocationError { LimitExceeded { Limit::Memories } } });
    REQUIRE(store.addTable(TableType { .element = FuncRef, .limits = atLeast(0) }, NullFunction).has_value());
    CHECK(store.addTable(TableType { .element = FuncRef, .limits = atLeast(0) }, NullFunction)
          == std::unexpected { AllocationError { LimitExceeded { Limit::Tables } } });
    CHECK(store.memoryCount() == 2);
    CHECK(store.tableCount() == 1);
}

TEST_CASE("Store.addRefusesPastTheBudgetAndTheType")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator, StoreLimits { .maxMemoryBytes = PageSize, .maxTableElements = 4 } };
    CHECK(store.addMemory(MemoryType { .limits = atLeast(2) })
          == std::unexpected { AllocationError { LimitExceeded { Limit::MemoryBytes } } });
    CHECK(store.addTable(TableType { .element = FuncRef, .limits = atLeast(5) }, NullFunction)
          == std::unexpected { AllocationError { LimitExceeded { Limit::TableElements } } });
    // An embedder's memory type validation would refuse: past an i32 memory's 2^16 pages.
    CHECK(store.addMemory(MemoryType { .limits = atLeast((uint64_t { 1 } << 16) + 1) })
          == std::unexpected { AllocationError {
              AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::TypeMaximum } } });
    CHECK(store.memoryCount() == 0);
    CHECK(store.tableCount() == 0);
    // What fits is charged: one page and four elements leave nothing.
    REQUIRE(store.addMemory(MemoryType { .limits = atLeast(1) }).has_value());
    REQUIRE(store.addTable(TableType { .element = FuncRef, .limits = atLeast(4) }, NullFunction).has_value());
    CHECK(store.addMemory(MemoryType { .limits = atLeast(1) })
          == std::unexpected { AllocationError { LimitExceeded { Limit::MemoryBytes } } });
    CHECK(store.addTable(TableType { .element = FuncRef, .limits = atLeast(1) }, NullFunction)
          == std::unexpected { AllocationError { LimitExceeded { Limit::TableElements } } });
}

TEST_CASE("Store.checkInstanceDemandCommitsNothing")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator, StoreLimits { .maxMemoryBytes = PageSize, .maxTags = 1 } };
    auto const fitting = InstanceDemand { .memories = 1, .memoryBytes = PageSize, .tags = 1 };
    CHECK(store.checkInstanceDemand(fitting).has_value());
    CHECK(store.checkInstanceDemand(fitting).has_value());
    CHECK(store.checkInstanceDemand(InstanceDemand { .memoryBytes = PageSize + 1 })
          == std::unexpected { LimitExceeded { Limit::MemoryBytes } });
    CHECK(store.checkInstanceDemand(InstanceDemand { .tags = 2 })
          == std::unexpected { LimitExceeded { Limit::Tags } });
    // A saturated demand is refused, not wrapped.
    CHECK(store.checkInstanceDemand(InstanceDemand { .tableElements = ~uint64_t { 0 } })
          == std::unexpected { LimitExceeded { Limit::TableElements } });
    CHECK(store.memoryCount() == 0);
    CHECK(store.moduleInstanceCount() == 0);
}

TEST_CASE("Store.typeLimitIsStoreWide")
{
    // Types are weighted by what they hold: one of 1000 params takes 1'001 type units.
    auto const wide = [](std::size_t params) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = std::vector(params, isoline::binary::I32), .results = {} });
        return builder;
    };
    auto limited = LimitedStore { StoreLimits { .maxTypes = 1'000 } };
    auto& store = limited.store();
    CHECK(limitOf(limited.instantiate(wide(1'000))) == LimitExceeded { Limit::Types });
    CHECK(store.typeCount() == 0);
    CHECK(store.moduleInstanceCount() == 0);
    // 999 params take 1'000 units: the whole budget.
    CHECK(limited.instantiate(wide(999)).has_value());
    CHECK(store.typeCount() == 1);
    // A type the store holds already costs nothing more; a new one, however small, is refused.
    CHECK(limited.instantiate(wide(999)).has_value());
    CHECK(limitOf(limited.instantiate(wide(0))) == LimitExceeded { Limit::Types });
    CHECK(store.typeCount() == 1);
}

TEST_CASE("Store.tagLimitIsStoreWide")
{
    auto const tags = [](uint32_t count) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType {});
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
            builder.tag(0);
        return builder;
    };
    auto limited = LimitedStore { StoreLimits { .maxTags = 2 } };
    CHECK(limitOf(limited.instantiate(tags(3))) == LimitExceeded { Limit::Tags });
    CHECK(limited.store().moduleInstanceCount() == 0);
    CHECK(limited.instantiate(tags(2)).has_value());
    CHECK(limitOf(limited.instantiate(tags(1))) == LimitExceeded { Limit::Tags });
    CHECK(limited.instantiate(tags(0)).has_value());
}

TEST_CASE("Store.importsClaimNoBudget")
{
    // The store's whole memory budget and its one memory are spent on an embedder's memory; a module
    // that imports it, rather than defining one, still instantiates.
    auto limited = LimitedStore { StoreLimits { .maxMemoryBytes = PageSize, .maxMemories = 1 } };
    auto& store = limited.store();
    auto const memory = store.addMemory(MemoryType { .limits = atLeast(1) });
    REQUIRE(memory.has_value());
    auto importing = ModuleBuilder {};
    importing.importMemory("env", "memory", atLeast(1));
    auto const imports =
        std::vector { isoline::runtime::ExternValue { .kind = ExternKind::Memory, .address = *memory } };
    CHECK(limited.instantiate(importing, imports).has_value());
    CHECK(limitOf(limited.instantiate(moduleWithMemories(1, 0))) == LimitExceeded { Limit::Memories });
}
