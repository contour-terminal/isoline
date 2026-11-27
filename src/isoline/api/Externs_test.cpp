// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Extern.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/EngineRows.hpp>
#include <isoline/api/testing/RuntimeAccess.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

using isoline::api::Abrupt;
using isoline::api::CallError;
using isoline::api::CallFailure;
using isoline::api::Engine;
using isoline::api::ExportAccessError;
using isoline::api::ExternKind;
using isoline::api::ForeignHandle;
using isoline::api::FuncType;
using isoline::api::Global;
using isoline::api::GlobalAccessError;
using isoline::api::HeapMemoryAllocator;
using isoline::api::HostContext;
using isoline::api::HostFailure;
using isoline::api::HostFailureKind;
using isoline::api::I32;
using isoline::api::I64;
using isoline::api::Imports;
using isoline::api::InitValueMismatch;
using isoline::api::Instance;
using isoline::api::Limit;
using isoline::api::makeHostFunction;
using isoline::api::MemoryAccessError;
using isoline::api::Ref;
using isoline::api::RefKind;
using isoline::api::Refusal;
using isoline::api::Store;
using isoline::api::StoreDeleted;
using isoline::api::Table;
using isoline::api::TableAccessError;
using isoline::api::TableGrowthError;
using isoline::api::Trap;
using isoline::api::TrapCode;
using isoline::api::Value;
using isoline::api::testing::EngineRow;
using isoline::api::testing::engineRows;
using isoline::api::testing::runtimeOf;
using isoline::api::testing::contract::configOf;
using isoline::api::testing::contract::ContractLimits;
using isoline::binary::AbstractHeapType;
using isoline::binary::GlobalType;
using isoline::binary::Limits;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::binary::testing::code;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::ModuleBuilder;

namespace
{

constexpr auto TypeI32 = isoline::binary::I32;

/// A size of @p count elements (or pages), unbounded, of 32-bit addresses.
Limits atLeast(uint64_t count)
{
    return Limits { .min = count, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 };
}

/// One entity of every kind, exported out of ExternKind's order:
///
///     (module
///       (table $t 2 funcref) (memory $m 1) (tag $e (param i32))
///       (global $g (mut i32) (i32.const 5)) (global $c i32 (i32.const 7))
///       (func $f (result i32) (global.get $g)) (func $size (result i32) (table.size $t))
///       (export "g" (global $g)) (export "e" (tag $e)) (export "f" (func $f)) (export "m" (memory $m))
///       (export "t" (table $t)) (export "c" (global $c)) (export "size" (func $size)))
ModuleBuilder everyKind()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { TypeI32 } })
        .type(FuncType { .params = { TypeI32 }, .results = {} })
        .table(TableType { .element = isoline::binary::FuncRef, .limits = atLeast(2) })
        .memory(atLeast(1))
        .tag(1)
        .global(GlobalType { .type = TypeI32, .mutability = Mutability::Var }, i32Const(5))
        .global(GlobalType { .type = TypeI32, .mutability = Mutability::Const }, i32Const(7))
        .function(0, {}, code({ { 0x23, 0x00 } }))       // global.get 0
        .function(0, {}, code({ { 0xFC, 0x10, 0x00 } })) // table.size 0
        .exportEntity("g", ExternKind::Global, 0)
        .exportEntity("e", ExternKind::Tag, 0)
        .exportFunction("f", 0)
        .exportEntity("m", ExternKind::Memory, 0)
        .exportEntity("t", ExternKind::Table, 0)
        .exportEntity("c", ExternKind::Global, 1)
        .exportFunction("size", 1);
    return builder;
}

/// An engine of @p row under @p limits, an allocator, and a store of that engine, held on the heap so
/// that destroying it frees its memory (and a use after it is one AddressSanitizer reports).
struct Fixture
{
    /// A store of @p row's engine under @p limits.
    explicit Fixture(EngineRow const& row, ContractLimits limits = {}): engine { configOf(limits, row) } {}

    /// A store of an engine of @p config.
    explicit Fixture(isoline::api::EngineConfig const& config): engine { config } {}

    Engine engine;
    HeapMemoryAllocator allocator;
    std::unique_ptr<Store> store = std::make_unique<Store>(engine, allocator);

    /// Loads and instantiates @p builder's module (everyKind() by default) with @p imports, which must
    /// succeed.
    [[nodiscard]] Instance instantiate(ModuleBuilder const& builder = everyKind(),
                                       Imports const& imports = {}) const
    {
        auto const module = engine.load(builder.build());
        REQUIRE(module.has_value());
        auto instance = store->instantiate(*module, imports);
        REQUIRE(instance.has_value());
        return *instance;
    }
};

/// The error an access that must fail failed with.
template <typename T, typename E>
E errorOf(std::expected<T, E> const& outcome)
{
    REQUIRE_FALSE(outcome.has_value());
    return outcome.error();
}

/// The value an access that must succeed returned.
template <typename T, typename E>
T valueOf(std::expected<T, E> const& outcome)
{
    REQUIRE(outcome.has_value());
    return *outcome;
}

/// Instance @p instance's export @p name, of the handle type @p T, which must exist.
template <typename T>
T exported(std::optional<T> const& handle)
{
    REQUIRE(handle.has_value());
    return *handle;
}

/// The i32 @p instance's function @p name returns.
int32_t callI32(Instance const& instance, std::string_view name)
{
    auto results = std::array<Value, 1> {};
    REQUIRE(instance.call(exported(instance.function(name)), {}, results).has_value());
    REQUIRE(std::holds_alternative<I32>(results[0]));
    return static_cast<int32_t>(std::get<I32>(results[0]).bits);
}

/// A reference to the function at @p address.
Ref funcRef(uint32_t address)
{
    return Ref { .kind = RefKind::Func, .heap = { .kind = AbstractHeapType::Func }, .address = address };
}

/// A reference to host object @p address.
Ref externRef(uint32_t address)
{
    return Ref { .kind = RefKind::Extern, .heap = { .kind = AbstractHeapType::Extern }, .address = address };
}

/// The null funcref.
Ref nullFunc()
{
    return Ref { .kind = RefKind::Null, .heap = { .kind = AbstractHeapType::Func }, .address = 0 };
}

} // namespace

TEST_CASE("Externs.exportAtFollowsExportOrder", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto const instance = fixture.instantiate();

    // The export order, not ExternKind's: g, e, f, m, t, c, size.
    auto const expected = std::to_array<std::size_t>({ 3, 4, 0, 2, 1, 3, 0 });
    for (auto const [index, alternative]: core::views::enumerate(expected))
    {
        auto const entity = instance.exportAt(static_cast<uint32_t>(index));
        REQUIRE(entity.has_value());
        CHECK(entity->index() == alternative);
    }
    // Each is the very entity its name looks up.
    CHECK(std::get<Global>(valueOf(instance.exportAt(0))).address()
          == exported(instance.global("g")).address());
    CHECK(std::get<Global>(valueOf(instance.exportAt(5))).address()
          == exported(instance.global("c")).address());
    CHECK(std::get<isoline::api::Tag>(valueOf(instance.exportAt(1))).address()
          == exported(instance.tag("e")).address());
    CHECK(std::get<isoline::api::Function>(valueOf(instance.exportAt(2))).address()
          == exported(instance.function("f")).address());
    CHECK(std::get<isoline::api::Memory>(valueOf(instance.exportAt(3))).address()
          == exported(instance.memory("m")).address());
    CHECK(std::get<Table>(valueOf(instance.exportAt(4))).address()
          == exported(instance.table("t")).address());
    // A tag's type is its payload's.
    auto const tag = std::get<isoline::api::Tag>(valueOf(instance.exportAt(1)));
    CHECK(tag.type() == FuncType { .params = { TypeI32 }, .results = {} });
    CHECK(tag.isOf(fixture.store->id()));
    // A name of another kind finds nothing.
    CHECK_FALSE(instance.global("t").has_value());
    CHECK_FALSE(instance.table("g").has_value());
    CHECK_FALSE(instance.tag("f").has_value());
}

TEST_CASE("Externs.exportAtOutOfRange", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto const instance = fixture.instantiate();
    CHECK(instance.exportAt(6).has_value());
    CHECK(errorOf(instance.exportAt(7)) == ExportAccessError::OutOfRange);
    CHECK(errorOf(instance.exportAt(std::numeric_limits<uint32_t>::max())) == ExportAccessError::OutOfRange);
}

TEST_CASE("Externs.setImmutableGlobalRefused", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto const instance = fixture.instantiate();
    auto const constant = exported(instance.global("c"));
    CHECK(errorOf(fixture.store->setGlobal(constant, I32 { 9 })) == GlobalAccessError::Immutable);
    CHECK(valueOf(fixture.store->globalValue(constant)) == Value { I32 { 7 } });
    auto const type = valueOf(fixture.store->globalType(constant));
    CHECK(type.type == TypeI32);
    CHECK(type.mutability == Mutability::Const);
}

TEST_CASE("Externs.setGlobalWrongKindRefused", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto const instance = fixture.instantiate();
    auto const global = exported(instance.global("g"));
    CHECK(errorOf(fixture.store->setGlobal(global, I64 { 9 })) == GlobalAccessError::ValueMismatch);
    CHECK(errorOf(fixture.store->setGlobal(global, Value { nullFunc() }))
          == GlobalAccessError::ValueMismatch);
    CHECK(valueOf(fixture.store->globalValue(global)) == Value { I32 { 5 } });
    CHECK(callI32(instance, "f") == 5);
    // A value of its type is assigned, and the guest reads it.
    REQUIRE(fixture.store->setGlobal(global, I32 { 9 }).has_value());
    CHECK(valueOf(fixture.store->globalValue(global)) == Value { I32 { 9 } });
    CHECK(callI32(instance, "f") == 9);
    CHECK(valueOf(fixture.store->globalType(global)).mutability == Mutability::Var);
}

TEST_CASE("Externs.tableGetOutOfBounds", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto const instance = fixture.instantiate();
    auto const table = exported(instance.table("t"));
    auto& store = *fixture.store;
    REQUIRE(valueOf(store.tableSize(table)) == 2);
    CHECK(valueOf(store.tableGet(table, 1)) == nullFunc());
    CHECK(errorOf(store.tableGet(table, 2)) == TableAccessError::OutOfBounds);
    CHECK(errorOf(store.tableGet(table, std::numeric_limits<uint64_t>::max()))
          == TableAccessError::OutOfBounds);
    auto const function = funcRef(exported(instance.function("f")).address());
    CHECK(errorOf(store.tableSet(table, 2, function)) == TableAccessError::OutOfBounds);
    // A reference not of the element type, or naming a function the store does not hold, is refused.
    CHECK(errorOf(store.tableSet(table, 0, externRef(1))) == TableAccessError::ValueMismatch);
    CHECK(errorOf(store.tableSet(table, 0, funcRef(1'000'000))) == TableAccessError::ValueMismatch);
    CHECK(valueOf(store.tableGet(table, 0)) == nullFunc());
    REQUIRE(store.tableSet(table, 0, function).has_value());
    CHECK(valueOf(store.tableGet(table, 0)) == function);
    auto const type = valueOf(store.tableType(table));
    CHECK(type.element == isoline::binary::FuncRef);
    CHECK(type.limits.min == 2);
}

TEST_CASE("Externs.growTableChargesStoreLimit", "[api][memory]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row, ContractLimits { .tableElements = 4 } };
    auto const instance = fixture.instantiate();
    auto const table = exported(instance.table("t"));
    auto& store = *fixture.store;
    CHECK(store.used(Limit::TableElements) == 2);
    // An init value not of the element type is refused before anything is charged.
    CHECK(errorOf(store.growTable(table, 1, externRef(1))) == TableGrowthError { InitValueMismatch {} });
    CHECK(valueOf(store.growTable(table, 2, nullFunc())) == 2);
    CHECK(store.used(Limit::TableElements) == 4);
    CHECK(errorOf(store.growTable(table, 1, nullFunc())) == TableGrowthError { Refusal::Limit });
    CHECK(errorOf(store.growTable(table, std::numeric_limits<uint64_t>::max(), nullFunc()))
          == TableGrowthError { Refusal::TypeMaximum });
    CHECK(valueOf(store.tableSize(table)) == 4);
    CHECK(valueOf(store.tableType(table)).limits.min == 4);
    CHECK(store.used(Limit::TableElements) == 4);
    // The guest sees the grown table.
    CHECK(callI32(instance, "size") == 4);
}

TEST_CASE("Externs.foreignHandleRefused", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto storeA = Fixture { row };
    auto storeB = Fixture { row };
    auto const instanceA = storeA.instantiate();
    auto const instanceB = storeB.instantiate();
    // Store B's global and table share their addresses with store A's, which A must not read for them.
    auto const global = exported(instanceB.global("g"));
    auto const table = exported(instanceB.table("t"));
    REQUIRE(global.address() == exported(instanceA.global("g")).address());
    REQUIRE(table.address() == exported(instanceA.table("t")).address());
    auto& store = *storeA.store;
    CHECK_FALSE(global.isOf(store.id()));
    CHECK(errorOf(store.globalValue(global)) == GlobalAccessError::ForeignHandle);
    CHECK(errorOf(store.setGlobal(global, I32 { 9 })) == GlobalAccessError::ForeignHandle);
    CHECK(errorOf(store.globalType(global)) == GlobalAccessError::ForeignHandle);
    CHECK(errorOf(store.tableSize(table)) == TableAccessError::ForeignHandle);
    CHECK(errorOf(store.tableGet(table, 0)) == TableAccessError::ForeignHandle);
    CHECK(errorOf(store.tableSet(table, 0, nullFunc())) == TableAccessError::ForeignHandle);
    CHECK(errorOf(store.growTable(table, 1, nullFunc())) == TableGrowthError { ForeignHandle {} });
    CHECK(errorOf(store.tableType(table)) == TableAccessError::ForeignHandle);
    // Nothing of store A changed.
    CHECK(callI32(instanceA, "f") == 5);
    CHECK(callI32(instanceA, "size") == 2);
}

TEST_CASE("Externs.instanceAfterStoreDeleteIsAnError", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto const instance = fixture.instantiate();
    auto const function = exported(instance.function("f"));
    auto const memory = exported(instance.memory("m"));
    auto const global = exported(instance.global("g"));
    auto const table = exported(instance.table("t"));
    auto const tag = exported(instance.tag("e"));
    fixture.store.reset();

    // Every operation of the instance reports the deleted store, reading nothing of it.
    auto results = std::array<Value, 1> {};
    auto const called = instance.call(function, {}, results);
    CHECK(errorOf(called) == CallError { .detail = StoreDeleted {}, .refusedGrowth = {} });
    CHECK(errorOf(called).failure() == CallFailure::StoreDeleted);
    CHECK(errorOf(instance.memoryRange(memory, 0, 1)) == MemoryAccessError::StoreDeleted);
    CHECK(errorOf(instance.memorySize(memory)) == MemoryAccessError::StoreDeleted);
    CHECK(errorOf(instance.exportAt(0)) == ExportAccessError::StoreDeleted);
    // A deleted store names no export.
    CHECK_FALSE(instance.function("f").has_value());
    CHECK_FALSE(instance.memory("m").has_value());
    CHECK_FALSE(instance.global("g").has_value());
    CHECK_FALSE(instance.table("t").has_value());
    CHECK_FALSE(instance.tag("e").has_value());

    // Its handles are foreign to every live store, a later one too: they are told apart by serial number.
    fixture.store = std::make_unique<Store>(fixture.engine, fixture.allocator);
    auto& later = *fixture.store;
    auto const laterInstance = fixture.instantiate();
    CHECK_FALSE(function.isOf(later.id()));
    CHECK_FALSE(memory.isOf(later.id()));
    CHECK_FALSE(global.isOf(later.id()));
    CHECK_FALSE(table.isOf(later.id()));
    CHECK_FALSE(tag.isOf(later.id()));
    CHECK(errorOf(later.globalValue(global)) == GlobalAccessError::ForeignHandle);
    CHECK(errorOf(later.setGlobal(global, I32 { 1 })) == GlobalAccessError::ForeignHandle);
    CHECK(errorOf(later.tableGet(table, 0)) == TableAccessError::ForeignHandle);
    CHECK(errorOf(later.growTable(table, 1, nullFunc())) == TableGrowthError { ForeignHandle {} });
    CHECK(callI32(laterInstance, "f") == 5);
}

TEST_CASE("Externs.callFailureNamesEveryDetail", "[api]")
{
    CHECK(CallError { .detail = ForeignHandle {}, .refusedGrowth = {} }.failure()
          == CallFailure::ForeignHandle);
    CHECK(CallError { .detail = StoreDeleted {}, .refusedGrowth = {} }.failure()
          == CallFailure::StoreDeleted);
}

namespace
{

/// A store of one exception, whose guest stashes a caught exnref in a global and a table and clears both
/// again:
///
///     (module
///       (tag $e) (global $g (mut exnref) (ref.null exn)) (table $t 1 exnref)
///       (func (export "stash") (local exnref)
///         (block (result exnref) (try_table (catch_all_ref 0) (throw $e)) (unreachable))
///         (local.tee 0) (global.set $g) (table.set $t (i32.const 0) (local.get 0)))
///       (func (export "clear") (global.set $g (ref.null exn)) (table.set $t (i32.const 0) (ref.null exn)))
///       (func (export "throw") (throw $e))
///       (export "g" (global $g)) (export "t" (table $t)))
ModuleBuilder stashModule()
{
    auto const exnRef = isoline::binary::ExnRef;
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .table(TableType { .element = exnRef, .limits = atLeast(1) })
        .tag(0)
        .global(GlobalType { .type = exnRef, .mutability = Mutability::Var }, { 0xD0, 0x69 })
        .function(0,
                  { exnRef },
                  code({ { 0x02, 0x69, 0x1F, 0x40, 0x01, 0x03, 0x00, 0x08, 0x00, 0x0B, 0x00, 0x0B }, // catch
                         { 0x22, 0x00, 0x24, 0x00 }, // local.tee 0; global.set 0
                         i32Const(0),
                         { 0x20, 0x00, 0x26, 0x00 } })) // local.get 0; table.set 0
        .function(0, {}, code({ { 0xD0, 0x69, 0x24, 0x00 }, i32Const(0), { 0xD0, 0x69, 0x26, 0x00 } }))
        .function(0, {}, code({ { 0x08, 0x00 } }))
        .exportFunction("stash", 0)
        .exportFunction("clear", 1)
        .exportFunction("throw", 2)
        .exportEntity("g", ExternKind::Global, 0)
        .exportEntity("t", ExternKind::Table, 0);
    return builder;
}

/// Where the embedder reads the stashed exnref from.
enum class StashedIn : uint8_t
{
    Global, ///< Store::globalValue().
    Table,  ///< Store::tableGet().
};

} // namespace

TEST_CASE("Externs.exnrefReadFromTheStoreEscapes", "[api]")
{
    // The embedder reads the exnref the guest stashed; the guest then drops its own references. The
    // embedder holds it, so the sweep the next throw makes in the full arena must not reclaim it.
    auto const row = GENERATE(from_range(engineRows()));
    auto const where = GENERATE(StashedIn::Global, StashedIn::Table);
    auto config = configOf(ContractLimits {}, row);
    config.store.maxExceptions = 1;
    auto fixture = Fixture { config };
    auto const instance = fixture.instantiate(stashModule());
    auto& store = *fixture.store;
    REQUIRE(instance.call(exported(instance.function("stash")), {}, {}).has_value());

    auto const held = [&] {
        if (where == StashedIn::Global)
        {
            auto const value = valueOf(store.globalValue(exported(instance.global("g"))));
            REQUIRE(std::holds_alternative<Ref>(value));
            return std::get<Ref>(value);
        }
        return valueOf(store.tableGet(exported(instance.table("t")), 0));
    }();
    REQUIRE(held.kind == RefKind::Exn);
    REQUIRE(instance.call(exported(instance.function("clear")), {}, {}).has_value());

    auto const thrown = instance.call(exported(instance.function("throw")), {}, {});
    CHECK(errorOf(thrown).detail == CallError::Detail { Trap { TrapCode::ExceptionLimitExceeded } });
    CHECK(runtimeOf(store).liveExceptions() == 1);
    CHECK(runtimeOf(store).isLiveException(held.address));
}

TEST_CASE("Externs.hostRefusedTableGrowthIsTheCallers", "[api][memory]")
{
    // A host function grows the store's table past its limit while the guest's call runs, then fails
    // the call: the refusal is the running call's, as a refused table.grow of the guest's would be.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row, ContractLimits { .tableElements = 2 } };
    auto table = std::optional<Table> {};
    auto refused = std::optional<isoline::api::Refusal> {};
    auto imports = Imports {};
    imports.define("host",
                   "grow",
                   FuncType {},
                   makeHostFunction([&](HostContext&,
                                        std::span<Value const>,
                                        std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                       auto const grown = fixture.store->growTable(*table, 1, nullFunc());
                       if (!grown && std::holds_alternative<Refusal>(grown.error()))
                           refused = std::get<Refusal>(grown.error());
                       return std::unexpected { Abrupt { HostFailure {
                           .kind = HostFailureKind::Embedder, .code = 7, .mismatch = std::nullopt } } };
                   }));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("host", "grow", 0)
        .table(TableType { .element = isoline::binary::FuncRef, .limits = atLeast(2) })
        .function(0, {}, code({ { 0x10, 0x00 } })) // call $grow
        .exportFunction("run", 1)
        .exportEntity("t", ExternKind::Table, 0);
    auto const instance = fixture.instantiate(builder, imports);
    table = exported(instance.table("t"));

    auto const called = instance.call(exported(instance.function("run")), {}, {});
    CHECK(refused == Refusal::Limit);
    CHECK(errorOf(called).failure() == CallFailure::HostFailure);
    CHECK(errorOf(called).refusedGrowth.table == Refusal::Limit);
    CHECK(errorOf(called).refusedGrowth.memory == std::nullopt);
}
