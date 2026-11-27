// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
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
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>
#include <isoline/runtime/testing/ProvidedMemory.hpp>
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
using isoline::runtime::GrowthRecord;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::I32;
using isoline::runtime::IMemoryAllocator;
using isoline::runtime::InstanceDemand;
using isoline::runtime::InstantiationError;
using isoline::runtime::InvocationScope;
using isoline::runtime::Limit;
using isoline::runtime::LimitExceeded;
using isoline::runtime::PollSite;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Refusal;
using isoline::runtime::RefusalScope;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::GrowDecision;
using isoline::runtime::testing::ProvidedMemoryAllocator;
using isoline::runtime::testing::ReallocatingMemoryAllocator;

namespace
{

/// A host function that does nothing.
constexpr auto NoHost =
    [](isoline::runtime::HostContext&,
       std::span<isoline::runtime::Value const>,
       std::span<isoline::runtime::Value>) noexcept -> std::expected<void, isoline::runtime::Abrupt> {
    return {};
};

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
    /// @param limits The store's limits.
    /// @param allocator Allocates its memories; must outlive this.
    LimitedStore(StoreLimits limits, IMemoryAllocator& allocator):
        _store { limits, StoreServices { .allocator = allocator } }
    {
    }

    /// @param limits The store's limits; its memories are allocated on the heap.
    explicit LimitedStore(StoreLimits limits): LimitedStore { limits, _heap } {}

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
    HeapMemoryAllocator _heap;
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
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const one = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 };
    auto const function =
        store.addHostFunction(FuncType {}, isoline::runtime::makeHostFunction(NoHost)).value();
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
    CHECK(view.memory(*memory).bytes().size() == PageSize);
    CHECK(view.global(global).value() == Value { I32 { .bits = 7 } });
    CHECK(&view.global(global) == &store.global(global));
}

TEST_CASE("Store.addressTypesBoundGrowth", "[memory]")
{
    // A guest can name sizes no host can hold; the store refuses them without allocating.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
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
    CHECK(store.memory(*memory).bytes().size() == PageSize);
    CHECK(store.table(*table).elements.size() == 1);
    // What the store grants, it grows by.
    CHECK(store.growMemory(*memory, 1) == 1U);
    CHECK(store.growTable(*table, 1, Ref {}) == 1U);
}

TEST_CASE("Store.byteGranularMemory", "[memory]")
{
    // A memory of 1-byte pages holds as many bytes as pages, and grows by bytes.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const memory =
        store.addMemory(MemoryType { .limits = Limits { .min = 3, .max = std::nullopt }, .pageSizeLog2 = 0 });
    REQUIRE(memory.has_value());
    CHECK(store.memory(*memory).bytes().size() == 3);
    CHECK(store.growMemory(*memory, 1) == 3U);
    CHECK(store.memory(*memory).bytes().size() == 4);
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
    CHECK(store.memory(*wide).bytes().size() == 16);
}

TEST_CASE("Store.internTypeHashConses")
{
    using isoline::binary::concreteRef;
    using isoline::binary::Nullable;
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
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
    auto const host = store
                          .addHostFunction(FuncType { .params = { isoline::binary::I32 }, .results = {} },
                                           isoline::runtime::makeHostFunction(NoHost))
                          .value();
    CHECK(store.function(host).typeId == takesI32);
    auto const defined = store.addModuleFunction(returnsI32, 0, 0);
    CHECK(store.function(defined).typeId == returnsI32);
    CHECK(store.functionType(host) == FuncType { .params = { isoline::binary::I32 }, .results = {} });
}

TEST_CASE("Store.findTypeDoesNotIntern")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
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
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
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
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const type = FuncType { .params = { isoline::binary::I32 }, .results = {} };
    auto const first = store.addTag(store.internType(type)).value();
    auto const second = store.addTag(store.internType(type)).value();
    CHECK(first != second);
    CHECK(store.tag(first).typeId == store.tag(second).typeId);
    CHECK(store.findType(type) == store.tag(first).typeId);
}

TEST_CASE("Store.tagNamesItsTypeOnly")
{
    // A tag holds its TypeId and nothing else: its payload's types are read through the store, so a
    // tag of a wide type costs no more than one of an empty type.
    static_assert(sizeof(isoline::runtime::TagInstance) == sizeof(isoline::runtime::TypeId));
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const wide = FuncType { .params = std::vector(1000, isoline::binary::I32), .results = {} };
    auto const tag = store.addTag(store.internType(wide)).value();
    CHECK(store.type(store.tag(tag).typeId) == wide);
    CHECK(store.addException(tag, std::vector<Value>(1000, I32 { .bits = 1 })).has_value());
}

namespace
{

/// An exnref to the exception at @p address.
Ref exnRef(isoline::runtime::ExnAddr address)
{
    return Ref { .kind = RefKind::Exn, .heap = isoline::binary::ExnRef.heap, .address = address };
}

/// Exception roots of the values it holds, as an executor's stacks are.
class HeldRoots final: public isoline::runtime::IExceptionRoots
{
  public:
    /// @param held The values it keeps.
    explicit HeldRoots(std::vector<Value> held): _held { std::move(held) } {}

    void appendExceptionRoots(std::vector<std::span<Value const>>& ranges) const override
    {
        ranges.emplace_back(_held);
    }

  private:
    std::vector<Value> _held;
};

/// A store of 16 exceptions and a tag of one i32 param.
struct ExceptionArena
{
    ExceptionArena():
        store { StoreLimits { .maxExceptions = 16 }, StoreServices { .allocator = allocator } },
        tag { store.addTag(store.internType(FuncType { .params = { isoline::binary::I32 }, .results = {} }))
                  .value() }
    {
    }

    /// Allocates @p rounds exceptions, each dropped at once; every allocation must succeed.
    void dropRounds(int rounds)
    {
        for ([[maybe_unused]] auto const round: std::views::iota(0, rounds))
            REQUIRE(store.addException(tag, { I32 { .bits = 7 } }).has_value());
    }

    HeapMemoryAllocator allocator;
    Store store;
    isoline::runtime::TagAddr tag;
};

} // namespace

TEST_CASE("Store.exceptionLimits")
{
    using isoline::runtime::TrapCode;
    auto allocator = HeapMemoryAllocator {};
    auto const payload = [](std::size_t count) {
        return std::vector<Value>(count, I32 { .bits = 7 });
    };

    SECTION("the number of exceptions")
    {
        auto store = Store { StoreLimits { .maxExceptions = 2 }, StoreServices { .allocator = allocator } };
        auto const tag = store.addTag(store.internType(FuncType {})).value();
        // Escaped, so that the sweep a full arena takes reclaims neither.
        auto const first = store.addException(tag, {});
        REQUIRE(first.has_value());
        store.markEscaped(exnRef(*first));
        auto const second = store.addException(tag, {});
        REQUIRE(second.has_value());
        store.markEscaped(exnRef(*second));
        auto const third = store.addException(tag, {});
        REQUIRE_FALSE(third.has_value());
        CHECK(third.error() == Trap { TrapCode::ExceptionLimitExceeded });
        CHECK(isoline::runtime::specMessage(third.error()) == "exception limit exceeded");
        CHECK(store.exception(*first).tag == tag);
    }

    SECTION("the values of their payloads")
    {
        auto store =
            Store { StoreLimits { .maxExceptionValues = 3 }, StoreServices { .allocator = allocator } };
        using isoline::binary::I32;
        auto const none = store.addTag(store.internType(FuncType {})).value();
        auto const one =
            store.addTag(store.internType(FuncType { .params = { I32 }, .results = {} })).value();
        auto const two =
            store.addTag(store.internType(FuncType { .params = { I32, I32 }, .results = {} })).value();
        // Each escaped, so that no sweep reclaims it.
        auto const add = [&](isoline::runtime::TagAddr tag, std::size_t count) {
            auto const added = store.addException(tag, payload(count));
            if (added)
                store.markEscaped(exnRef(*added));
            return added;
        };
        auto const first = add(two, 2);
        REQUIRE(first.has_value());
        CHECK(store.exception(*first).payload == payload(2));
        CHECK(add(two, 2).error() == Trap { TrapCode::ExceptionLimitExceeded });
        CHECK(add(one, 1).has_value());
        CHECK(add(none, 0).has_value());
        CHECK_FALSE(add(one, 1).has_value());
    }
}

TEST_CASE("Store.escapedExceptionsAreKept")
{
    // An escaped exception survives the sweeps of 20 rounds of drops in a store of 16, its payload
    // intact; the dropped ones are reclaimed, so the arena never traps.
    auto arena = ExceptionArena {};
    auto& store = arena.store;
    auto const kept = store.addException(arena.tag, { I32 { .bits = 42 } }).value();
    store.markEscaped(exnRef(kept));
    arena.dropRounds(20);
    CHECK(store.isLiveException(kept));
    CHECK(store.exception(kept).payload == std::vector<Value> { I32 { .bits = 42 } });
    CHECK(store.liveExceptions() <= 16);
    CHECK(store.used(Limit::Exceptions) == store.liveExceptions());
    // Marking anything but a live exnref changes nothing.
    store.markEscaped(I32 { .bits = 1 });
    store.markEscaped(exnRef(1'000));
}

TEST_CASE("Store.sweepKeepsItsRoots")
{
    // What a registered root, a table, a global, an element segment or an escaped exception's payload
    // refers to survives 20 rounds of drops; what nothing refers to is reclaimed and its address
    // reused, lowest first.
    auto arena = ExceptionArena {};
    auto& store = arena.store;
    auto const exnTag =
        store.addTag(store.internType(FuncType { .params = { isoline::binary::ExnRef }, .results = {} }))
            .value();
    auto const rooted = store.addException(arena.tag, { I32 { .bits = 1 } }).value();
    auto const tabled = store.addException(arena.tag, { I32 { .bits = 2 } }).value();
    auto const globaled = store.addException(arena.tag, { I32 { .bits = 3 } }).value();
    auto const inPayload = store.addException(arena.tag, { I32 { .bits = 4 } }).value();
    auto const carrier = store.addException(exnTag, { exnRef(inPayload) }).value();
    store.markEscaped(exnRef(carrier));
    auto const table = store.addTable(TableType { .element = isoline::binary::ExnRef, .limits = atLeast(1) },
                                      exnRef(tabled));
    REQUIRE(table.has_value());
    std::ignore = store.addGlobal(
        GlobalType { .type = isoline::binary::ExnRef, .mutability = Mutability::Var }, exnRef(globaled));
    auto const segmented = store.addException(arena.tag, { I32 { .bits = 5 } }).value();
    auto segments = isoline::runtime::ModuleInstance {};
    segments.elements = { std::nullopt, std::vector { exnRef(segmented) } };
    std::ignore = store.addModuleInstance(std::move(segments));
    auto const roots =
        HeldRoots { { exnRef(rooted), I32 { .bits = 9 }, exnRef(rooted) } }; // twice: marked once
    auto const none = HeldRoots { {} };
    // Three registrations, the rooted one in the middle of the store's list, which ends first.
    using isoline::runtime::ExceptionRootsRegistration;
    auto oldest = std::optional<ExceptionRootsRegistration> {};
    oldest.emplace(store, none);
    auto middle = std::optional<ExceptionRootsRegistration> {};
    middle.emplace(store, roots);
    auto const newest = ExceptionRootsRegistration { store, none };
    arena.dropRounds(20);
    for (auto const& [address, bits]: std::to_array<std::pair<isoline::runtime::ExnAddr, uint32_t>>(
             { { rooted, 1 }, { tabled, 2 }, { globaled, 3 }, { inPayload, 4 }, { segmented, 5 } }))
    {
        CAPTURE(address);
        REQUIRE(store.isLiveException(address));
        CHECK(store.exception(address).payload == std::vector<Value> { I32 { .bits = bits } });
    }
    middle.reset();
    oldest.reset();
    // Unregistered, the root no longer keeps its exception: the next full arena reclaims it, and the
    // lowest address, its own, goes to the next exception.
    arena.dropRounds(16);
    REQUIRE(store.isLiveException(rooted));
    CHECK(store.exception(rooted).payload == std::vector<Value> { I32 { .bits = 7 } });
    CHECK(store.exception(carrier).payload == std::vector<Value> { exnRef(inPayload) });
}

namespace
{

/// A store of 2 exceptions whose sweep visits 3 × SweepPollItems + 2 items: the slots of a null exnref
/// table of 3 × 4096, and the 2 arena slots, holding exceptions nothing refers to.
struct MeteredSweep
{
    /// The polls the sweep takes: before items 0, 4096, 8192 and 12'288.
    static constexpr auto Polls = uint64_t { 4 };

    /// @param maxSteps The store's step budget.
    /// @param trigger Where its poll counter triggers; none when nullopt.
    explicit MeteredSweep(uint64_t maxSteps,
                          std::optional<isoline::runtime::testing::PollTrigger> trigger = {}):
        counter { trigger },
        store { StoreLimits { .maxExceptions = 2, .maxSteps = maxSteps },
                StoreServices { .allocator = allocator, .pollObserver = &counter } },
        tag { store.addTag(store.internType(FuncType {})).value() }
    {
        auto const nullExn =
            Ref { .kind = RefKind::Null, .heap = isoline::binary::ExnRef.heap, .address = 0 };
        REQUIRE(store
                    .addTable(TableType { .element = isoline::binary::ExnRef,
                                          .limits = atLeast(3 * isoline::runtime::SweepPollItems) },
                              nullExn)
                    .has_value());
        REQUIRE(store.addException(tag, {}).has_value());
        REQUIRE(store.addException(tag, {}).has_value());
    }

    HeapMemoryAllocator allocator;
    isoline::runtime::testing::PollCounter counter;
    Store store;
    isoline::runtime::TagAddr tag;
};

} // namespace

TEST_CASE("Store.sweepChargesItsItems")
{
    // 12'290 items: a poll before every 4096 of them, the first included, each charging a step.
    auto sweep = MeteredSweep { std::numeric_limits<uint64_t>::max() };
    auto& store = sweep.store;
    auto const invocation = InvocationScope { store };
    REQUIRE(store.used(Limit::Steps) == 0);
    REQUIRE(store.addException(sweep.tag, {}).has_value());
    CHECK(sweep.counter.count(PollSite::Sweep) == MeteredSweep::Polls);
    CHECK(store.used(Limit::Steps) == MeteredSweep::Polls);
    CHECK(store.liveExceptions() == 1);
}

TEST_CASE("Store.interruptEndsASweepWithinOneGranule")
{
    // Raised at the second poll, which observes it: the sweep ends there, reclaims nothing, and the
    // allocation reports Interrupted.
    auto sweep =
        MeteredSweep { std::numeric_limits<uint64_t>::max(),
                       isoline::runtime::testing::PollTrigger { .site = PollSite::Sweep, .ordinal = 2 } };
    auto& store = sweep.store;
    sweep.counter.interruptAtTrigger(store.interrupter());
    auto const added = store.addException(sweep.tag, {});
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error() == Trap { TrapCode::Interrupted });
    CHECK(sweep.counter.count(PollSite::Sweep) == 2);
    CHECK(sweep.counter.pollsAfterTrigger() == 0);
    CHECK(store.liveExceptions() == 2);
    CHECK(store.isLiveException(0));
    CHECK(store.isLiveException(1));
}

TEST_CASE("Store.abandonedSweepLeavesTheStoreUsable")
{
    // A budget of exactly the sweep's 4 steps, one of them spent: its last poll traps, and it reclaims
    // nothing. The next invocation's fresh budget lets the same sweep finish, and the allocation take
    // the lowest address it reclaimed.
    auto sweep = MeteredSweep { MeteredSweep::Polls };
    auto& store = sweep.store;
    {
        auto const invocation = InvocationScope { store };
        REQUIRE(store.poll(PollSite::FunctionEntry).has_value());
        auto const added = store.addException(sweep.tag, {});
        REQUIRE_FALSE(added.has_value());
        CHECK(added.error() == Trap { TrapCode::StepLimitExceeded });
        CHECK(store.liveExceptions() == 2);
        CHECK(store.isLiveException(0));
        CHECK(store.isLiveException(1));
    }
    auto const invocation = InvocationScope { store };
    auto const added = store.addException(sweep.tag, {});
    REQUIRE(added.has_value());
    CHECK(*added == 0);
    CHECK(store.used(Limit::Steps) == MeteredSweep::Polls);
    CHECK(store.liveExceptions() == 1);
    CHECK_FALSE(store.isLiveException(1));
}

TEST_CASE("Store.sweepAbandonedWhileTracingReclaimsNothing")
{
    // An escaped exception of 5000 values in a store of 2: the third allocation's sweep visits the 2
    // arena slots, then traces those values; its second poll, before item 4096, comes while tracing,
    // past a budget of one step. Nothing is reclaimed, the unescaped exception included.
    auto allocator = HeapMemoryAllocator {};
    auto store =
        Store { StoreLimits { .maxExceptions = 2, .maxSteps = 1 }, StoreServices { .allocator = allocator } };
    auto const wide = store
                          .addTag(store.internType(
                              FuncType { .params = std::vector(5000, isoline::binary::I32), .results = {} }))
                          .value();
    auto const empty = store.addTag(store.internType(FuncType {})).value();
    auto const escaped = store.addException(wide, std::vector<Value>(5000, I32 { .bits = 3 })).value();
    store.markEscaped(exnRef(escaped));
    auto const dropped = store.addException(empty, {}).value();
    auto const invocation = InvocationScope { store };
    auto const added = store.addException(empty, {});
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error() == Trap { TrapCode::StepLimitExceeded });
    CHECK(store.liveExceptions() == 2);
    CHECK(store.isLiveException(dropped));
    CHECK(store.exception(escaped).payload.size() == 5000);
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

TEST_CASE("Store.growPastLimitIsRefused", "[memory]")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxMemoryBytes = 2 * PageSize, .maxTableElements = 2 },
                         StoreServices { .allocator = allocator } };
    auto const memory = store.addMemory(MemoryType { .limits = atLeast(1) });
    REQUIRE(memory.has_value());
    CHECK(store.growMemory(*memory, 1) == 1U);
    CHECK(store.growMemory(*memory, 1) == std::unexpected { Refusal::Limit });
    // Past an i32 memory's 2^16 pages: the type's reach, whatever the budget.
    CHECK(store.growMemory(*memory, uint64_t { 1 } << 40) == std::unexpected { Refusal::TypeMaximum });
    CHECK(store.memory(*memory).bytes().size() == 2 * PageSize);

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
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxMemoryBytes = PageSize, .maxTableElements = 4 },
                         StoreServices { .allocator = allocator } };
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
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxMemoryBytes = PageSize, .maxTags = 1 },
                         StoreServices { .allocator = allocator } };
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

TEST_CASE("Store.hostAdditionsShareTheGuestsBudgets")
{
    // A guest's module takes 91 of the store's 100 type units and both its tags. The embedder's host
    // function of a new type past what is left, and a third tag, are refused, and the process lives
    // on; what still fits, and a type the store holds already, are added.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = std::vector(90, isoline::binary::I32), .results = {} }).tag(0).tag(0);
    auto limited = LimitedStore { StoreLimits { .maxTypes = 100, .maxTags = 2 } };
    auto& store = limited.store();
    auto const instance = limited.instantiate(builder);
    REQUIRE(instance.has_value());
    CHECK(store.used(Limit::Types) == 91);
    CHECK(store.used(Limit::Tags) == 2);

    auto const host = isoline::runtime::makeHostFunction(NoHost);
    auto const ofWidth = [](std::size_t params) {
        return FuncType { .params = std::vector(params, isoline::binary::I32), .results = {} };
    };
    CHECK(store.addHostFunction(ofWidth(9), host) == std::unexpected { LimitExceeded { Limit::Types } });
    CHECK(store.typeCount() == 1);
    CHECK(store.addHostFunction(ofWidth(90), host).has_value());
    CHECK(store.addHostFunction(ofWidth(8), host).has_value());
    CHECK(store.used(Limit::Types) == 100);
    CHECK(store.addHostFunction(FuncType {}, host) == std::unexpected { LimitExceeded { Limit::Types } });

    auto const typeId = store.tag(store.moduleInstance(*instance).tags.at(0)).typeId;
    CHECK(store.addTag(typeId) == std::unexpected { LimitExceeded { Limit::Tags } });
    CHECK(store.used(Limit::Tags) == 2);
}

TEST_CASE("Store.usedReadsEveryLimit")
{
    // What the store holds of each limit, in that limit's unit; with no invocation in progress, and
    // none taken yet, the invocations' limits read 0.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    REQUIRE(store.addMemory(MemoryType { .limits = atLeast(2) }).has_value());
    REQUIRE(store.addTable(TableType { .element = FuncRef, .limits = atLeast(3) }, NullFunction).has_value());
    REQUIRE(store.addTable(TableType { .element = FuncRef, .limits = atLeast(4) }, NullFunction).has_value());
    auto const tag =
        store.addTag(store.internType(FuncType { .params = { isoline::binary::I32 }, .results = {} }))
            .value();
    REQUIRE(store.addException(tag, { I32 { .bits = 1 } }).has_value());
    REQUIRE(store.addException(tag, { I32 { .bits = 2 } }).has_value());
    std::ignore = store.addModuleInstance(isoline::runtime::ModuleInstance {});
    struct Row
    {
        Limit limit;
        uint64_t used;
    };
    auto const rows = std::to_array<Row>({
        { .limit = Limit::CallDepth, .used = 0 },
        { .limit = Limit::StackValues, .used = 0 },
        { .limit = Limit::NestedInvocations, .used = 0 },
        { .limit = Limit::MemoryBytes, .used = 2 * PageSize },
        { .limit = Limit::TableElements, .used = 7 },
        { .limit = Limit::Memories, .used = 1 },
        { .limit = Limit::Tables, .used = 2 },
        { .limit = Limit::Instances, .used = 1 },
        { .limit = Limit::Exceptions, .used = 2 },
        { .limit = Limit::ExceptionValues, .used = 2 },
        { .limit = Limit::Types, .used = 2 },
        { .limit = Limit::Tags, .used = 1 },
        { .limit = Limit::Steps, .used = 0 },
    });
    static_assert(rows.size() == static_cast<std::size_t>(isoline::runtime::LastLimit) + 1);
    for (auto const& row: rows)
    {
        INFO("limit: " << static_cast<int>(row.limit));
        CHECK(store.used(row.limit) == row.used);
    }
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

namespace
{

/// Limits from @p min to @p max pages or elements, of i32 addresses.
Limits between(uint64_t min, uint64_t max)
{
    return Limits { .min = min, .max = max, .addressType = AddressType::I32 };
}

/// A memory of 1 to 4 pages.
MemoryType oneToFourPages()
{
    return MemoryType { .limits = between(1, 4) };
}

/// The embedder's decision that grants every grow.
GrowDecision grantAll(uint64_t /*newBytes*/)
{
    return GrowDecision::Grant;
}

} // namespace

TEST_CASE("Store.providedMemoryGuardTooSmallIsRefused")
{
    auto allocator = ProvidedMemoryAllocator { 4 * PageSize, /*guardBytes=*/4'096, grantAll };
    auto store =
        Store { StoreLimits {}, StoreServices { .allocator = allocator, .requiredGuardBytes = PageSize } };
    CHECK(store.addMemory(oneToFourPages())
          == std::unexpected { AllocationError {
              AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::Guard } } });
    CHECK(store.memoryCount() == 0);

    // The heap's memories have no guard at all.
    auto heap = HeapMemoryAllocator {};
    auto guarded = Store { StoreLimits {}, StoreServices { .allocator = heap, .requiredGuardBytes = 1 } };
    CHECK(guarded.addMemory(oneToFourPages())
          == std::unexpected { AllocationError {
              AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::Guard } } });

    // A guard as large as required is enough.
    auto exact = ProvidedMemoryAllocator { 4 * PageSize, PageSize, grantAll };
    auto store2 =
        Store { StoreLimits {}, StoreServices { .allocator = exact, .requiredGuardBytes = PageSize } };
    CHECK(store2.addMemory(oneToFourPages()).has_value());
}

TEST_CASE("Store.providedMemoryGrowthIsTheEmbeddersCall")
{
    auto decisions = std::vector<uint64_t> {};
    auto allocator = ProvidedMemoryAllocator { 4 * PageSize, 0, [&](uint64_t bytes) {
                                                  decisions.push_back(bytes);
                                                  return bytes <= 2 * PageSize ? GrowDecision::Grant
                                                                               : GrowDecision::Refuse;
                                              } };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const memory = store.addMemory(oneToFourPages());
    REQUIRE(memory.has_value());
    auto const* const data = store.memory(*memory).bytes().data();

    CHECK(store.growMemory(*memory, 1) == 1U);
    CHECK(decisions == std::vector { 2 * PageSize });
    CHECK(store.memory(*memory).bytes().size() == 2 * PageSize);
    // The embedder's memory never moves.
    CHECK(store.memory(*memory).bytes().data() == data);
    CHECK(store.growthRecord() == GrowthRecord {});

    CHECK(store.growMemory(*memory, 1) == std::unexpected { Refusal::Embedder });
    CHECK(decisions == std::vector { 2 * PageSize, 3 * PageSize });
    CHECK(store.memory(*memory).bytes().size() == 2 * PageSize);
    CHECK(store.growthRecord() == GrowthRecord { .memory = Refusal::Embedder, .table = std::nullopt });

    // Past the type's maximum the store refuses without asking the embedder.
    CHECK(store.growMemory(*memory, 3) == std::unexpected { Refusal::TypeMaximum });
    CHECK(decisions.size() == 2);
    // The record keeps the first refusal.
    CHECK(store.growthRecord() == GrowthRecord { .memory = Refusal::Embedder, .table = std::nullopt });
}

namespace
{

/// An embedder's storage that refuses with whatever it is told to, documented or not: a heap memory
/// whose every grow returns growRefusal, from an allocator whose allocate() returns allocateRefusal
/// when one is set.
class MisreportingAllocator final: public IMemoryAllocator
{
  public:
    /// @param allocateRefusal What allocate() returns, or nullopt to allocate.
    /// @param growRefusal What every grow() of an allocated memory returns.
    MisreportingAllocator(std::optional<Refusal> allocateRefusal, Refusal growRefusal):
        _allocateRefusal { allocateRefusal }, _growRefusal { growRefusal }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<isoline::runtime::ILinearMemory>, Refusal> allocate(
        isoline::runtime::MemoryRequest const& request) noexcept override
    {
        if (_allocateRefusal)
            return std::unexpected { *_allocateRefusal };
        return std::make_unique<Memory>(request.initialBytes, _growRefusal);
    }

  private:
    /// A memory of a fixed size, refusing every grow.
    class Memory final: public isoline::runtime::ILinearMemory
    {
      public:
        Memory(uint64_t size, Refusal growRefusal):
            _bytes(static_cast<std::size_t>(size)), _growRefusal { growRefusal }
        {
        }

        [[nodiscard]] std::span<std::byte> bytes() noexcept override { return _bytes; }
        [[nodiscard]] std::span<std::byte const> bytes() const noexcept override { return _bytes; }
        [[nodiscard]] uint64_t guardBytes() const noexcept override { return 0; }
        [[nodiscard]] uint64_t tailBytes() const noexcept override { return 0; }
        [[nodiscard]] std::expected<void, Refusal> grow(
            isoline::runtime::GrowRequest const& /*request*/) noexcept override
        {
            return std::unexpected { _growRefusal };
        }

      private:
        std::vector<std::byte> _bytes;
        Refusal _growRefusal;
    };

    std::optional<Refusal> _allocateRefusal;
    Refusal _growRefusal;
};

} // namespace

TEST_CASE("Store.storageRefusalsAreAttributedToTheEmbedder")
{
    // An allocator or a memory may report only its own refusals; one that claims the store's (Limit,
    // TypeMaximum) is refusing on its own account, so it reads as the embedder's, never as a store
    // limit the guest reached.
    struct Row
    {
        Refusal reported;   ///< What the allocator or memory returns.
        Refusal allocation; ///< What addMemory reports for it.
        Refusal growth;     ///< What growMemory reports for it.
    };
    auto const rows = std::to_array<Row>({
        { .reported = Refusal::TypeMaximum, .allocation = Refusal::Embedder, .growth = Refusal::Embedder },
        { .reported = Refusal::Limit, .allocation = Refusal::Embedder, .growth = Refusal::Embedder },
        { .reported = Refusal::HostMemory, .allocation = Refusal::HostMemory, .growth = Refusal::HostMemory },
        { .reported = Refusal::Guard, .allocation = Refusal::Guard, .growth = Refusal::Guard },
        { .reported = Refusal::Embedder, .allocation = Refusal::Embedder, .growth = Refusal::Embedder },
    });
    static_assert(rows.size() == static_cast<std::size_t>(isoline::runtime::LastRefusal) + 1);
    // One row per refusal, each at the index of its enumerator, so none is tested twice and none left out.
    for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
        CHECK(static_cast<std::size_t>(rows[index].reported) == index);
    for (auto const& row: rows)
    {
        INFO("reported: " << static_cast<int>(row.reported));
        auto refusing = MisreportingAllocator { row.reported, row.reported };
        auto refusingStore = Store { StoreLimits {}, StoreServices { .allocator = refusing } };
        CHECK(refusingStore.addMemory(oneToFourPages())
              == std::unexpected { AllocationError {
                  AllocationFailure { .kind = ExternKind::Memory, .reason = row.allocation } } });

        auto growing = MisreportingAllocator { std::nullopt, row.reported };
        auto growingStore = Store { StoreLimits {}, StoreServices { .allocator = growing } };
        auto const memory = growingStore.addMemory(oneToFourPages());
        REQUIRE(memory.has_value());
        CHECK(growingStore.growMemory(*memory, 1) == std::unexpected { row.growth });
        CHECK(growingStore.growthRecord() == GrowthRecord { .memory = row.growth, .table = std::nullopt });
    }
}

TEST_CASE("Store.growingByNothingAsksNobody")
{
    // memory.grow 0 returns the size, whatever the embedder would decide, and records nothing.
    auto decisions = std::vector<uint64_t> {};
    auto allocator = ProvidedMemoryAllocator { 4 * PageSize, 0, [&](uint64_t bytes) {
                                                  decisions.push_back(bytes);
                                                  return GrowDecision::Refuse;
                                              } };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const memory = store.addMemory(oneToFourPages());
    REQUIRE(memory.has_value());

    CHECK(store.growMemory(*memory, 0) == 1U);
    CHECK(decisions.empty());
    CHECK(store.growthRecord() == GrowthRecord {});
    CHECK(store.memory(*memory).bytes().size() == PageSize);
}

TEST_CASE("Store.memoryGenerationCountsEveryGrowth")
{
    // The generation moves by one per memory that grew, whichever it was, and stays where nothing grew:
    // refused grows, grows by nothing, and adding a memory.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    CHECK(store.memoryGeneration() == 0);
    auto const first = store.addMemory(oneToFourPages());
    REQUIRE(first.has_value());
    auto const second = store.addMemory(oneToFourPages());
    REQUIRE(second.has_value());
    CHECK(store.memoryGeneration() == 0);

    CHECK(store.growMemory(*first, 1) == 1U);
    CHECK(store.memoryGeneration() == 1);
    CHECK(store.growMemory(*second, 2) == 1U);
    CHECK(store.memoryGeneration() == 2);

    CHECK(store.growMemory(*first, 0) == 2U);
    CHECK(store.growMemory(*first, 3) == std::unexpected { Refusal::TypeMaximum });
    CHECK(store.memoryGeneration() == 2);

    auto const third = store.addMemory(oneToFourPages());
    REQUIRE(third.has_value());
    CHECK(store.memoryGeneration() == 2);
}

TEST_CASE("Store.memoryGenerationIgnoresDroppedMemories")
{
    // Dropping the memories of a failed instantiation (releaseTrailing()) changes no memory code read, so the
    // generation stays; its address is the same throughout, and holds what memoryGeneration() returns.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const* const counter = store.memoryGenerationAddress();
    auto const kept = store.addMemory(oneToFourPages());
    REQUIRE(kept.has_value());
    CHECK(store.growMemory(*kept, 1) == 1U);
    CHECK(*counter == 1);
    auto const firstDropped = static_cast<isoline::runtime::MemAddr>(store.memoryCount());
    auto const dropped = store.addMemory(oneToFourPages());
    REQUIRE(dropped.has_value());
    CHECK(store.growMemory(*dropped, 1) == 1U);
    CHECK(store.memoryGeneration() == 2);
    store.releaseTrailing(static_cast<isoline::runtime::TableAddr>(store.tableCount()), firstDropped);
    CHECK(store.memoryCount() == 1);
    CHECK(store.memoryGeneration() == 2);
    CHECK(store.memoryGenerationAddress() == counter);
    CHECK(*counter == store.memoryGeneration());
}

TEST_CASE("Store.memoryGenerationIgnoresARefusingEmbedder")
{
    // An embedder that declines the grow leaves the memory, and so the generation, as they were.
    auto allocator = ProvidedMemoryAllocator { 4 * PageSize, 0, [](uint64_t bytes) {
                                                  return bytes <= 2 * PageSize ? GrowDecision::Grant
                                                                               : GrowDecision::Refuse;
                                              } };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const memory = store.addMemory(oneToFourPages());
    REQUIRE(memory.has_value());
    CHECK(store.growMemory(*memory, 1) == 1U);
    CHECK(store.memoryGeneration() == 1);
    CHECK(store.growMemory(*memory, 1) == std::unexpected { Refusal::Embedder });
    CHECK(store.memoryGeneration() == 1);
}

TEST_CASE("Store.growthRecordIsPerKind")
{
    auto allocator = ProvidedMemoryAllocator { PageSize, 0, grantAll };
    auto store = Store { StoreLimits { .maxTableElements = 1 }, StoreServices { .allocator = allocator } };
    auto const table = store.addTable(TableType { .element = FuncRef, .limits = atLeast(1) }, NullFunction);
    REQUIRE(table.has_value());
    auto const memory = store.addMemory(oneToFourPages());
    REQUIRE(memory.has_value());

    CHECK(store.growTable(*table, 1, NullFunction) == std::unexpected { Refusal::Limit });
    CHECK(store.growthRecord() == GrowthRecord { .memory = std::nullopt, .table = Refusal::Limit });
    // A refused table.grow does not mask a later memory refusal: past its one-page buffer.
    CHECK(store.growMemory(*memory, 1) == std::unexpected { Refusal::Embedder });
    CHECK(store.growthRecord() == GrowthRecord { .memory = Refusal::Embedder, .table = Refusal::Limit });
}

TEST_CASE("Store.secondProvidedMemoryIsRefused")
{
    auto allocator = ProvidedMemoryAllocator { 4 * PageSize, 0, grantAll };
    auto limited = LimitedStore { StoreLimits {}, allocator };
    auto builder = ModuleBuilder {};
    builder.memory(oneToFourPages()).memory(oneToFourPages());
    auto const result = limited.instantiate(builder);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == InstantiationError {
              AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::Embedder } });
}

TEST_CASE("Store.failedInstantiationReleasesItsAllocations")
{
    auto allocator = ProvidedMemoryAllocator { 4 * PageSize, 0, grantAll };
    auto limited = LimitedStore { StoreLimits { .maxTableElements = 1'024 }, allocator };
    auto& store = limited.store();
    auto builder = moduleWithTables({ 600 });
    builder.memory(oneToFourPages()).memory(oneToFourPages());
    auto const result = limited.instantiate(builder);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == InstantiationError {
              AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::Embedder } });
    CHECK(store.moduleInstanceCount() == 0);
    CHECK(store.tableCount() == 0);
    CHECK(store.memoryCount() == 0);

    // The 600 elements are back in the budget: a table of all 1'024 fits.
    CHECK(limited.instantiate(moduleWithTables({ 1'024 })).has_value());
    CHECK(store.tableCount() == 1);
}

TEST_CASE("Store.releaseTrailingReturnsTheBudgets")
{
    auto limited = LimitedStore { StoreLimits { .maxMemoryBytes = 2 * PageSize, .maxTableElements = 10 } };
    auto& store = limited.store();
    REQUIRE(store.addMemory(MemoryType { .limits = atLeast(1) }).has_value());
    REQUIRE(store.addTable(TableType { .element = FuncRef, .limits = atLeast(4) }, NullFunction).has_value());
    REQUIRE(store.addMemory(MemoryType { .limits = atLeast(1) }).has_value());
    REQUIRE(store.addTable(TableType { .element = FuncRef, .limits = atLeast(6) }, NullFunction).has_value());

    store.releaseTrailing(1, 1);
    CHECK(store.tableCount() == 1);
    CHECK(store.memoryCount() == 1);
    // One page and six elements are free again, and no more.
    CHECK(store.addMemory(MemoryType { .limits = atLeast(1) }).has_value());
    CHECK(store.addTable(TableType { .element = FuncRef, .limits = atLeast(6) }, NullFunction).has_value());
    CHECK(store.addTable(TableType { .element = FuncRef, .limits = atLeast(1) }, NullFunction)
          == std::unexpected { AllocationError { LimitExceeded { Limit::TableElements } } });

    // Releasing nothing changes nothing.
    store.releaseTrailing(static_cast<uint32_t>(store.tableCount()),
                          static_cast<uint32_t>(store.memoryCount()));
    CHECK(store.tableCount() == 2);
}

TEST_CASE("Store.everyMemoryKeepsTheTailItsSizeNeeds")
{
    using isoline::runtime::MaxAccessWidth;
    using isoline::runtime::requiredTailBytesOf;
    auto const refused = std::unexpected { AllocationError {
        AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::Guard } } };
    auto const bytePages = [](uint64_t min, uint64_t max) {
        return MemoryType { .limits = between(min, max), .pageSizeLog2 = 0 };
    };
    auto const noPages = MemoryType { .limits = between(0, 1) };

    // The heap's memories: a masked access of MaxAccessWidth bytes at the base of a memory of no bytes, of
    // small pages or of large ones, stays within its storage.
    auto heap = HeapMemoryAllocator {};
    auto onTheHeap = Store { StoreLimits {}, StoreServices { .allocator = heap } };
    for (auto const& type: { bytePages(0, 4), noPages })
    {
        auto const address = onTheHeap.addMemory(type);
        REQUIRE(address.has_value());
        auto const& linear = std::as_const(onTheHeap).memory(*address).storage();
        CHECK(linear.bytes().empty());
        CHECK(linear.tailBytes() + linear.guardBytes() >= MaxAccessWidth);
    }

    // An embedder's memory of exactly its size: refused wherever it holds fewer than MaxAccessWidth bytes,
    // whatever its pages, taken once it holds that many.
    auto exact = MisreportingAllocator { std::nullopt, Refusal::Embedder };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = exact } };
    CHECK(store.addMemory(bytePages(4, 8)) == refused);
    CHECK(store.addMemory(bytePages(0, 8)) == refused);
    CHECK(store.addMemory(noPages) == refused);
    CHECK(store.memoryCount() == 0);
    CHECK(store.addMemory(bytePages(MaxAccessWidth, MaxAccessWidth)).has_value());
    CHECK(store.addMemory(oneToFourPages()).has_value());

    // One byte short, of readable slack and guard together, is refused; exactly enough is taken.
    for (auto const guard: { uint64_t { 0 }, uint64_t { 1 } })
    {
        INFO("guard " << guard);
        auto shortOfIt = ProvidedMemoryAllocator { MaxAccessWidth - 1 - guard, guard, grantAll };
        auto withLess = Store { StoreLimits {}, StoreServices { .allocator = shortOfIt } };
        CHECK(withLess.addMemory(bytePages(0, 1)) == refused);
        auto enough = ProvidedMemoryAllocator { MaxAccessWidth - guard, guard, grantAll };
        auto withEnough = Store { StoreLimits {}, StoreServices { .allocator = enough } };
        CHECK(withEnough.addMemory(bytePages(0, 1)).has_value());
    }

    // A provided buffer of 16 bytes, whose tail shrinks by what each grow adds, keeps the rule at every size.
    auto provided = ProvidedMemoryAllocator { 16, 0, grantAll };
    auto withProvided = Store { StoreLimits {}, StoreServices { .allocator = provided } };
    auto const address = withProvided.addMemory(bytePages(0, 16));
    REQUIRE(address.has_value());
    auto size = uint64_t { 0 };
    for (auto const target:
         { uint64_t { 0 }, uint64_t { 1 }, uint64_t { 8 }, uint64_t { 15 }, uint64_t { 16 } })
    {
        INFO("size " << target);
        if (target > size)
            REQUIRE(withProvided.growMemory(*address, target - size).has_value());
        size = target;
        auto const& linear = std::as_const(withProvided).memory(*address).storage();
        CHECK(linear.bytes().size() == size);
        CHECK(linear.tailBytes() + linear.guardBytes() >= requiredTailBytesOf(size));
        CHECK(isoline::runtime::keepsItsTail(linear));
    }
}

TEST_CASE("Store.growRefusesATailTheMemoryCannotKeep")
{
    // An embedder's memory that keeps 4 bytes past its end after a grow: a grow to a size that needs more is
    // refused with Guard, recorded, and leaves the memory, its generation and the store's budget as they
    // were; the request asks for exactly the new size's tail, so a size that needs 4 is taken.
    using isoline::runtime::MaxAccessWidth;
    auto allocator = ReallocatingMemoryAllocator { MaxAccessWidth, 4 };
    auto store =
        Store { StoreLimits { .maxMemoryBytes = MaxAccessWidth }, StoreServices { .allocator = allocator } };
    auto const address = store.addMemory(MemoryType { .limits = between(0, 64), .pageSizeLog2 = 0 });
    REQUIRE(address.has_value());
    auto const& linear = std::as_const(store).memory(*address).storage();
    auto const* const data = linear.bytes().data();
    auto const generation = store.memoryGeneration();

    for (auto const delta: { uint64_t { 1 }, MaxAccessWidth - 5 })
    {
        INFO("delta " << delta);
        CHECK(store.growMemory(*address, delta) == std::unexpected { Refusal::Guard });
        CHECK(store.growthRecord() == GrowthRecord { .memory = Refusal::Guard, .table = std::nullopt });
        CHECK(linear.bytes().empty());
        CHECK(linear.bytes().data() == data);
        CHECK(linear.tailBytes() == MaxAccessWidth);
        CHECK(store.memoryGeneration() == generation);
    }

    // Exactly the slack's worth of tail, then the whole budget: nothing was charged for the refusals.
    CHECK(store.growMemory(*address, MaxAccessWidth - 4) == 0U);
    CHECK(linear.bytes().size() == MaxAccessWidth - 4);
    CHECK(store.growMemory(*address, 4) == MaxAccessWidth - 4);
    CHECK(linear.bytes().size() == MaxAccessWidth);
    CHECK(store.memoryGeneration() == generation + 2);
}

TEST_CASE("Store.memoryRequestNamesItsBounds")
{
    /// Allocates on the heap, and keeps the last request.
    class RecordingAllocator final: public IMemoryAllocator
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<isoline::runtime::ILinearMemory>, Refusal> allocate(
            isoline::runtime::MemoryRequest const& request) noexcept override
        {
            last = request;
            return _heap.allocate(request);
        }

        std::optional<isoline::runtime::MemoryRequest> last; ///< The last request.

      private:
        HeapMemoryAllocator _heap;
    };
    auto allocator = RecordingAllocator {};
    auto store = Store { StoreLimits { .maxMemoryBytes = (3 * PageSize) + 100 },
                         StoreServices { .allocator = allocator, .requiredGuardBytes = 0 } };

    // Bounded by its type's maximum.
    REQUIRE(store.addMemory(MemoryType { .limits = between(1, 2) }).has_value());
    REQUIRE(allocator.last.has_value());
    CHECK(allocator.last->initialBytes == PageSize);
    CHECK(allocator.last->maximumBytes == 2 * PageSize);
    CHECK(allocator.last->requiredGuardBytes == 0);
    CHECK(allocator.last->requiredTailBytes == 0);

    // Bounded by the store's limit, rounded down to whole pages.
    REQUIRE(store.addMemory(MemoryType { .limits = atLeast(0) }).has_value());
    CHECK(allocator.last->initialBytes == 0);
    CHECK(allocator.last->maximumBytes == 3 * PageSize);
    // A memory of no bytes needs the whole width past its end, whatever its pages.
    CHECK(allocator.last->requiredTailBytes == isoline::runtime::MaxAccessWidth);
    REQUIRE(store.addMemory(MemoryType { .limits = atLeast(0), .pageSizeLog2 = 0 }).has_value());
    CHECK(allocator.last->maximumBytes == (3 * PageSize) + 100);
    CHECK(allocator.last->requiredTailBytes == isoline::runtime::MaxAccessWidth);
}

TEST_CASE("Store.refusalScopeRestoresTheOuterRecord")
{
    auto limited = LimitedStore { StoreLimits { .maxTableElements = 1 } };
    auto& store = limited.store();
    auto const table =
        store.addTable(TableType { .element = FuncRef, .limits = between(1, 2) }, NullFunction);
    REQUIRE(table.has_value());
    auto const outerRecord = GrowthRecord { .memory = std::nullopt, .table = Refusal::Limit };

    SECTION("the outer record survives a nested refusal")
    {
        CHECK(store.growTable(*table, 1, NullFunction) == std::unexpected { Refusal::Limit });
        {
            auto const inner = RefusalScope { store };
            CHECK(store.growthRecord() == GrowthRecord {});
            CHECK(store.growTable(*table, 5, NullFunction) == std::unexpected { Refusal::TypeMaximum });
            CHECK(inner.record() == GrowthRecord { .memory = std::nullopt, .table = Refusal::TypeMaximum });
        }
        CHECK(store.growthRecord() == outerRecord);
    }
    SECTION("a nested refusal does not leak into an outer scope that had none")
    {
        {
            auto const inner = RefusalScope { store };
            CHECK(store.growTable(*table, 1, NullFunction) == std::unexpected { Refusal::Limit });
            CHECK(inner.record() == outerRecord);
        }
        CHECK(store.growthRecord() == GrowthRecord {});
    }
}

TEST_CASE("Store.interruptBeatsStepLimitAtOnePoll")
{
    // A budget of 2 steps: two polls that charge one each spend it, and the third traps. Unwind charges
    // none, so it polls on. Once interrupted, every poll is Interrupted, the spent budget aside.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxSteps = 2 }, StoreServices { .allocator = allocator } };
    auto const invocation = InvocationScope { store };
    CHECK(store.poll(PollSite::FunctionEntry).has_value());
    CHECK(store.poll(PollSite::LoopBackEdge).has_value());
    CHECK(store.poll(PollSite::LoopBackEdge) == isoline::runtime::trap(TrapCode::StepLimitExceeded));
    CHECK(store.poll(PollSite::Unwind).has_value());
    CHECK(store.used(Limit::Steps) == 2);
    store.interrupter()->interrupt();
    CHECK(store.poll(PollSite::LoopBackEdge) == isoline::runtime::trap(TrapCode::Interrupted));
    CHECK(store.poll(PollSite::Unwind) == isoline::runtime::trap(TrapCode::Interrupted));
}

TEST_CASE("Store.everySiteChargesWhatItsRowSays")
{
    // One poll per site, in a budget of exactly the sites that charge a step: all of them pass, and
    // the count is what the table says.
    auto charging = uint64_t { 0 };
    for (auto const& row: isoline::runtime::PollSiteTable)
        charging += row.charge == isoline::runtime::StepCharge::OneStep ? 1U : 0U;
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxSteps = charging }, StoreServices { .allocator = allocator } };
    auto const invocation = InvocationScope { store };
    for (auto const& row: isoline::runtime::PollSiteTable)
        CHECK(store.poll(row.site).has_value());
    CHECK(store.used(Limit::Steps) == charging);
    CHECK(store.poll(PollSite::Unwind).has_value());
    CHECK(store.poll(PollSite::FunctionEntry) == isoline::runtime::trap(TrapCode::StepLimitExceeded));
}

TEST_CASE("Store.stepBudgetIsTheOutermostInvocations")
{
    // 3 steps per outermost invocation, which the one nested in it shares; 2 invocations at once.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxNestedInvocations = 2, .maxSteps = 3 },
                         StoreServices { .allocator = allocator } };
    {
        auto const outer = InvocationScope { store };
        CHECK(store.used(Limit::NestedInvocations) == 1);
        CHECK(store.poll(PollSite::FunctionEntry).has_value());
        CHECK(store.poll(PollSite::FunctionEntry).has_value());
        {
            auto const nested = InvocationScope { store };
            CHECK(store.invocationDepth() == 2);
            CHECK(store.checkInvocationNesting() == isoline::runtime::trap(TrapCode::CallStackExhausted));
            CHECK(store.poll(PollSite::FunctionEntry).has_value());
            CHECK(store.poll(PollSite::FunctionEntry) == isoline::runtime::trap(TrapCode::StepLimitExceeded));
        }
        CHECK(store.poll(PollSite::FunctionEntry) == isoline::runtime::trap(TrapCode::StepLimitExceeded));
    }
    // The steps taken stay readable after the invocation, until the next outermost one starts afresh.
    CHECK(store.used(Limit::NestedInvocations) == 0);
    CHECK(store.used(Limit::Steps) == 3);
    CHECK(store.checkInvocationNesting().has_value());
    auto const again = InvocationScope { store };
    CHECK(store.used(Limit::Steps) == 0);
    CHECK(store.poll(PollSite::FunctionEntry).has_value());
}

TEST_CASE("Store.frameChargesCapTheirSum")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxStackValues = 10 }, StoreServices { .allocator = allocator } };
    CHECK(store.chargeFrame(6).has_value());
    // A charge past the cap charges nothing.
    CHECK(store.chargeFrame(5) == isoline::runtime::trap(TrapCode::CallStackExhausted));
    CHECK(store.chargedStackValues() == 6);
    CHECK(store.chargeFrame(4).has_value());
    CHECK(store.used(Limit::StackValues) == 10);
    CHECK(store.chargeFrame(1) == isoline::runtime::trap(TrapCode::CallStackExhausted));
    CHECK(store.chargeFrame(std::numeric_limits<uint64_t>::max())
          == isoline::runtime::trap(TrapCode::CallStackExhausted));
    store.releaseFrame(4);
    store.releaseFrame(6);
    CHECK(store.chargedStackValues() == 0);
    CHECK(store.chargeFrame(10).has_value());
}

// The contract checks: each ends the process on purpose, so the unit run skips them and CTest runs
// each alone (tests/CMakeLists.txt).

TEST_CASE("Contract.Store.addGlobalOfItsOwnKind", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    std::ignore =
        store.addGlobal(GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Const },
                        isoline::runtime::I64 { .bits = 1 });
    FAIL("SURVIVED: an i32 global added with an i64 value");
}

TEST_CASE("Contract.Store.grownMemoryKeepsItsTail", "[.contract]")
{
    // An embedder's memory that drops its tail when it grows breaks ILinearMemory's contract, which the store
    // checks after the grow: a refusal could not undo it.
    isoline::runtime::testing::endAtContractViolation();
    class Shedding final: public isoline::runtime::ILinearMemory
    {
      public:
        [[nodiscard]] std::span<std::byte> bytes() noexcept override
        {
            return std::span { _storage }.first(static_cast<std::size_t>(_size));
        }
        [[nodiscard]] std::span<std::byte const> bytes() const noexcept override
        {
            return std::span { _storage }.first(static_cast<std::size_t>(_size));
        }
        [[nodiscard]] uint64_t guardBytes() const noexcept override { return 0; }
        [[nodiscard]] uint64_t tailBytes() const noexcept override
        {
            return _size == 0 ? _storage.size() : 0;
        }
        /// Grows whatever tail the request asks for: it lies.
        [[nodiscard]] std::expected<void, Refusal> grow(
            isoline::runtime::GrowRequest const& request) noexcept override
        {
            _size = request.newBytes;
            return {};
        }

      private:
        std::vector<std::byte> _storage = std::vector<std::byte>(64);
        uint64_t _size = 0;
    };
    class SheddingAllocator final: public IMemoryAllocator
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<isoline::runtime::ILinearMemory>, Refusal> allocate(
            isoline::runtime::MemoryRequest const& /*request*/) noexcept override
        {
            return std::make_unique<Shedding>();
        }
    };
    auto allocator = SheddingAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const address = store.addMemory(MemoryType { .limits = between(0, 4), .pageSizeLog2 = 0 });
    REQUIRE(address.has_value());
    std::ignore = store.growMemory(*address, 1);
    FAIL("SURVIVED: a memory grew past the tail its size needs");
}
