// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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
using isoline::api::AllocationFailure;
using isoline::api::CallError;
using isoline::api::CallFailure;
using isoline::api::DefineError;
using isoline::api::Engine;
using isoline::api::ExportAccessError;
using isoline::api::Extern;
using isoline::api::ExternKind;
using isoline::api::ForeignHandle;
using isoline::api::ForeignImport;
using isoline::api::Function;
using isoline::api::FunctionRefError;
using isoline::api::FuncType;
using isoline::api::Global;
using isoline::api::GlobalAccessError;
using isoline::api::HeapMemoryAllocator;
using isoline::api::HostContext;
using isoline::api::HostFailure;
using isoline::api::HostFailureKind;
using isoline::api::I32;
using isoline::api::I64;
using isoline::api::ImportCountMismatch;
using isoline::api::Imports;
using isoline::api::InitValueMismatch;
using isoline::api::Instance;
using isoline::api::InstantiateError;
using isoline::api::InvalidType;
using isoline::api::InvalidTypeReason;
using isoline::api::Limit;
using isoline::api::LimitExceeded;
using isoline::api::Linkage;
using isoline::api::LinkError;
using isoline::api::LinkErrorCode;
using isoline::api::makeHostFunction;
using isoline::api::MemoryAccessError;
using isoline::api::MemoryGrowthError;
using isoline::api::MemorySize;
using isoline::api::MemoryType;
using isoline::api::MismatchKind;
using isoline::api::Module;
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
using isoline::api::ValueMismatch;
using isoline::api::testing::EngineRow;
using isoline::api::testing::engineRows;
using isoline::api::testing::runtimeOf;
using isoline::api::testing::contract::configOf;
using isoline::api::testing::contract::ContractLimits;
using isoline::binary::AbstractHeapType;
using isoline::binary::AddressType;
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

/// A size of at least @p min and at most @p max elements (or pages), of @p addressType addresses.
Limits between(uint64_t min, uint64_t max, AddressType addressType = AddressType::I32)
{
    return Limits { .min = min, .max = max, .addressType = addressType };
}

/// The bytes of one page of the default page size.
constexpr auto PageBytes = uint64_t { 65'536 };

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
    // Calls and positional instantiation alike, the later store's own instance included.
    auto const foreignCall = CallError { .detail = ForeignHandle {}, .refusedGrowth = {} };
    CHECK(errorOf(later.call(function, {}, results)) == foreignCall);
    CHECK(errorOf(laterInstance.call(function, {}, results)) == foreignCall);
    auto importer = ModuleBuilder {};
    importer.importMemory("x", "m", atLeast(1));
    auto const importing = fixture.engine.load(importer.build());
    REQUIRE(importing.has_value());
    auto const deletedExterns = std::array<Extern, 1> { memory };
    CHECK(errorOf(later.instantiateByPosition(*importing, deletedExterns))
          == InstantiateError { ForeignImport { .index = 0 } });
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
///       (func (export "catch") (result exnref)
///         (block (result exnref) (try_table (catch_all_ref 0) (throw $e)) (unreachable)))
///       (export "g" (global $g)) (export "t" (table $t)))
ModuleBuilder stashModule()
{
    auto const exnRef = isoline::binary::ExnRef;
    auto builder = ModuleBuilder {};
    auto const caught = isoline::binary::testing::Bytes { 0x02, 0x69, 0x1F, 0x40, 0x01, 0x03,
                                                          0x00, 0x08, 0x00, 0x0B, 0x00, 0x0B };
    builder.type(FuncType {})
        .type(FuncType { .params = {}, .results = { exnRef } })
        .table(TableType { .element = exnRef, .limits = atLeast(1) })
        .tag(0)
        .global(GlobalType { .type = exnRef, .mutability = Mutability::Var }, { 0xD0, 0x69 })
        .function(0,
                  { exnRef },
                  code({ caught,
                         { 0x22, 0x00, 0x24, 0x00 }, // local.tee 0; global.set 0
                         i32Const(0),
                         { 0x20, 0x00, 0x26, 0x00 } })) // local.get 0; table.set 0
        .function(0, {}, code({ { 0xD0, 0x69, 0x24, 0x00 }, i32Const(0), { 0xD0, 0x69, 0x26, 0x00 } }))
        .function(0, {}, code({ { 0x08, 0x00 } }))
        .function(1, {}, code({ caught }))
        .exportFunction("stash", 0)
        .exportFunction("clear", 1)
        .exportFunction("throw", 2)
        .exportFunction("catch", 3)
        .exportEntity("g", ExternKind::Global, 0)
        .exportEntity("t", ExternKind::Table, 0);
    return builder;
}

/// Where the embedder reads the stashed exnref from.
enum class StashedIn : uint8_t
{
    Global,          ///< Store::globalValue().
    Table,           ///< Store::tableGet().
    InstanceResult,  ///< The result of the guest's catch, called by Instance::call().
    StoreCallResult, ///< The result of the guest's catch, called by Store::call().
};

} // namespace

TEST_CASE("Externs.exnrefReadFromTheStoreEscapes", "[api]")
{
    // The embedder reads the exnref the guest stashed; the guest then drops its own references. The
    // embedder holds it, so the sweep the next throw makes in the full arena must not reclaim it.
    auto const row = GENERATE(from_range(engineRows()));
    auto const where =
        GENERATE(StashedIn::Global, StashedIn::Table, StashedIn::InstanceResult, StashedIn::StoreCallResult);
    auto config = configOf(ContractLimits {}, row);
    config.store.maxExceptions = 1;
    auto fixture = Fixture { config };
    auto const instance = fixture.instantiate(stashModule());
    auto& store = *fixture.store;
    REQUIRE(instance.call(exported(instance.function("stash")), {}, {}).has_value());

    auto const held = [&] {
        auto results = std::array<Value, 1> {};
        auto const catcher = exported(instance.function("catch"));
        // A result's exnref is a second exception: the stashed one is dropped first, so that the arena
        // of one has room for it.
        if (where == StashedIn::InstanceResult || where == StashedIn::StoreCallResult)
            REQUIRE(instance.call(exported(instance.function("clear")), {}, {}).has_value());
        switch (where)
        {
            case StashedIn::Global:
                results[0] = valueOf(store.globalValue(exported(instance.global("g"))));
                break;
            case StashedIn::Table:
                results[0] = valueOf(store.tableGet(exported(instance.table("t")), 0));
                break;
            case StashedIn::InstanceResult: REQUIRE(instance.call(catcher, {}, results).has_value()); break;
            case StashedIn::StoreCallResult: REQUIRE(store.call(catcher, {}, results).has_value()); break;
        }
        REQUIRE(std::holds_alternative<Ref>(results[0]));
        return std::get<Ref>(results[0]);
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

TEST_CASE("Externs.hostMemoryHasNoInstance", "[api]")
{
    // A memory the host defines is reached through the store alone: no instance holds it.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto const memory = valueOf(store.defineMemory(MemoryType { .limits = atLeast(1) }));
    CHECK(memory.isOf(store.id()));
    CHECK(valueOf(store.memorySize(memory)) == MemorySize { .pages = 1, .bytes = PageBytes });
    // Charged as a module's own memory is.
    CHECK(store.used(Limit::Memories) == 1);
    CHECK(store.used(Limit::MemoryBytes) == PageBytes);

    auto const whole = valueOf(store.memoryRange(memory, 0, PageBytes));
    CHECK(whole.size() == PageBytes);
    whole.back() = std::byte { 0x2A };
    CHECK(valueOf(store.memoryRange(memory, PageBytes - 1, 1)).front() == std::byte { 0x2A });
    // Past the end, an offset and a length whose sum overflows included, views nothing.
    CHECK(errorOf(store.memoryRange(memory, 0, PageBytes + 1)) == MemoryAccessError::OutOfBounds);
    CHECK(errorOf(store.memoryRange(memory, PageBytes, 1)) == MemoryAccessError::OutOfBounds);
    CHECK(errorOf(store.memoryRange(memory, std::numeric_limits<uint64_t>::max(), 2))
          == MemoryAccessError::OutOfBounds);
    CHECK(errorOf(store.memoryRange(memory, 1, std::numeric_limits<uint64_t>::max()))
          == MemoryAccessError::OutOfBounds);
    // A view of size 0 is valid at any address.
    CHECK(valueOf(store.memoryRange(memory, std::numeric_limits<uint64_t>::max(), 0)).empty());
}

TEST_CASE("Externs.growMemoryChargesStoreLimit", "[api][memory]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row, ContractLimits { .memoryBytes = 2 * PageBytes } };
    auto& store = *fixture.store;
    auto const memory = valueOf(store.defineMemory(MemoryType { .limits = atLeast(1) }));
    CHECK(valueOf(store.growMemory(memory, 1)) == 1);
    CHECK(store.used(Limit::MemoryBytes) == 2 * PageBytes);
    CHECK(errorOf(store.growMemory(memory, 1)) == MemoryGrowthError { Refusal::Limit });
    // A hostile delta is refused before anything is allocated.
    CHECK(errorOf(store.growMemory(memory, std::numeric_limits<uint64_t>::max()))
          == MemoryGrowthError { Refusal::TypeMaximum });
    // Growing by nothing succeeds at the limit, and changes nothing.
    CHECK(valueOf(store.growMemory(memory, 0)) == 2);
    CHECK(valueOf(store.memorySize(memory)) == MemorySize { .pages = 2, .bytes = 2 * PageBytes });
    CHECK(store.used(Limit::MemoryBytes) == 2 * PageBytes);
    // A memory's own maximum refuses as memory.grow's does.
    auto config = configOf(ContractLimits {}, row);
    auto bounded = Fixture { config };
    auto const small = valueOf(bounded.store->defineMemory(MemoryType { .limits = between(1, 2) }));
    CHECK(valueOf(bounded.store->growMemory(small, 1)) == 1);
    CHECK(errorOf(bounded.store->growMemory(small, 1)) == MemoryGrowthError { Refusal::TypeMaximum });
}

TEST_CASE("Externs.defineMemoryOverLimit", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto config = configOf(ContractLimits {}, row);
    config.store.maxMemories = 0;
    auto fixture = Fixture { config };
    auto& store = *fixture.store;
    CHECK(errorOf(store.defineMemory(MemoryType { .limits = atLeast(0) }))
          == DefineError { LimitExceeded { .limit = Limit::Memories } });
    CHECK(store.used(Limit::Memories) == 0);
}

TEST_CASE("Externs.defineMemoryRefusesHostileSizes", "[api][memory]")
{
    // Every hostile size is refused by the charge, before the allocator is asked for a byte.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row, ContractLimits { .memoryBytes = 2 * PageBytes } };
    auto& store = *fixture.store;
    auto const maximum = std::numeric_limits<uint64_t>::max();
    auto const pastTheType =
        DefineError { AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::TypeMaximum } };
    auto const pastTheStore = DefineError { LimitExceeded { .limit = Limit::MemoryBytes } };
    // Over the store's bytes.
    CHECK(errorOf(store.defineMemory(MemoryType { .limits = atLeast(3) })) == pastTheStore);
    // A minimum above its maximum, or past what its address type reaches (2^16 pages of i32, 2^48 of
    // i64), a minimum near 2^64 included.
    CHECK(errorOf(store.defineMemory(MemoryType { .limits = between(2, 1) })) == pastTheType);
    CHECK(errorOf(store.defineMemory(MemoryType { .limits = atLeast(65'537) })) == pastTheType);
    CHECK(errorOf(store.defineMemory(MemoryType { .limits = between(maximum, maximum, AddressType::I64) }))
          == pastTheType);
    // Byte pages of i64 addresses reach 2^64 - 1 pages: only the store's bytes refuse them.
    CHECK(errorOf(store.defineMemory(
              MemoryType { .limits = between(maximum, maximum, AddressType::I64), .pageSizeLog2 = 0 }))
          == pastTheStore);
    CHECK(store.used(Limit::Memories) == 0);
    CHECK(store.used(Limit::MemoryBytes) == 0);
    // A maximum near 2^64 is clamped to what the store allows, not allocated.
    auto const unbounded =
        valueOf(store.defineMemory(MemoryType { .limits = between(1, maximum, AddressType::I64) }));
    CHECK(valueOf(store.memorySize(unbounded)) == MemorySize { .pages = 1, .bytes = PageBytes });
    CHECK(errorOf(store.growMemory(unbounded, 2)) == MemoryGrowthError { Refusal::Limit });
}

TEST_CASE("Externs.hostTableHoldsItsInit", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto const function = valueOf(store.define(
        FuncType {}, makeHostFunction([](HostContext&, std::span<Value const>, std::span<Value>) noexcept {
            return std::expected<void, Abrupt> {};
        })));
    auto const type = TableType { .element = isoline::binary::FuncRef, .limits = atLeast(3) };
    auto const table = valueOf(store.defineTable(type, funcRef(function.address())));
    CHECK(table.isOf(store.id()));
    CHECK(valueOf(store.tableSize(table)) == 3);
    CHECK(valueOf(store.tableGet(table, 2)) == funcRef(function.address()));
    CHECK(valueOf(store.tableType(table)).limits.min == 3);
    CHECK(store.used(Limit::Tables) == 1);
    CHECK(store.used(Limit::TableElements) == 3);
    // An init not of the element type, or naming a function the store does not hold, adds nothing.
    CHECK(errorOf(store.defineTable(type, externRef(1)))
          == DefineError { ValueMismatch { .kind = MismatchKind::Heap, .index = 0 } });
    CHECK(errorOf(store.defineTable(type, funcRef(1'000'000)))
          == DefineError { ValueMismatch { .kind = MismatchKind::ForeignReference, .index = 0 } });
    CHECK(store.used(Limit::Tables) == 1);
    CHECK(store.used(Limit::TableElements) == 3);
}

TEST_CASE("Externs.defineTableRefusesHostileSizes", "[api][memory]")
{
    // Charged as a module's own table is: every hostile size is refused before an element is allocated.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row, ContractLimits { .tableElements = 4 } };
    auto& store = *fixture.store;
    auto const maximum = std::numeric_limits<uint64_t>::max();
    auto const tableOf = [](Limits limits) {
        return TableType { .element = isoline::binary::FuncRef, .limits = limits };
    };
    CHECK(errorOf(store.defineTable(tableOf(atLeast(5)), nullFunc()))
          == DefineError { LimitExceeded { .limit = Limit::TableElements } });
    // A lying size within the address type's reach is over the store's elements.
    CHECK(errorOf(store.defineTable(tableOf(atLeast(uint64_t { 1 } << 31)), nullFunc()))
          == DefineError { LimitExceeded { .limit = Limit::TableElements } });
    CHECK(errorOf(store.defineTable(tableOf(between(uint64_t { 1 } << 40, maximum, AddressType::I64)),
                                    nullFunc()))
          == DefineError { LimitExceeded { .limit = Limit::TableElements } });
    // A minimum above its maximum, or past what its elements' bytes can count (near 2^64), is past the type.
    auto const pastTheType =
        DefineError { AllocationFailure { .kind = ExternKind::Table, .reason = Refusal::TypeMaximum } };
    CHECK(errorOf(store.defineTable(tableOf(between(2, 1)), nullFunc())) == pastTheType);
    CHECK(errorOf(store.defineTable(tableOf(between(maximum, maximum, AddressType::I64)), nullFunc()))
          == pastTheType);
    CHECK(store.used(Limit::Tables) == 0);
    CHECK(store.used(Limit::TableElements) == 0);
    // A table's maximum near 2^64 is clamped to what its address type reaches, not allocated.
    auto const table = valueOf(store.defineTable(tableOf(between(4, maximum, AddressType::I64)), nullFunc()));
    CHECK(errorOf(store.growTable(table, 1, nullFunc())) == TableGrowthError { Refusal::Limit });

    auto config = configOf(ContractLimits {}, row);
    config.store.maxTables = 0;
    auto none = Fixture { config };
    CHECK(errorOf(none.store->defineTable(tableOf(atLeast(0)), nullFunc()))
          == DefineError { LimitExceeded { .limit = Limit::Tables } });
}

TEST_CASE("Externs.defineGlobalWrongValueKind", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto const type = GlobalType { .type = TypeI32, .mutability = Mutability::Var };
    CHECK(errorOf(store.defineGlobal(type, I64 { 1 }))
          == DefineError { ValueMismatch { .kind = MismatchKind::Kind, .index = 0 } });
    auto const funcType = GlobalType { .type = isoline::binary::FuncRef, .mutability = Mutability::Const };
    CHECK(errorOf(store.defineGlobal(funcType, Value { funcRef(1'000'000) }))
          == DefineError { ValueMismatch { .kind = MismatchKind::ForeignReference, .index = 0 } });
    // A value of its type defines it; the store reads and writes it as a module's own.
    auto const global = valueOf(store.defineGlobal(type, I32 { 3 }));
    CHECK(global.isOf(store.id()));
    CHECK(valueOf(store.globalValue(global)) == Value { I32 { 3 } });
    REQUIRE(store.setGlobal(global, I32 { 4 }).has_value());
    CHECK(valueOf(store.globalValue(global)) == Value { I32 { 4 } });
    CHECK(valueOf(store.globalType(global)).mutability == Mutability::Var);
    auto const constant = valueOf(store.defineGlobal(funcType, Value { nullFunc() }));
    CHECK(errorOf(store.setGlobal(constant, Value { nullFunc() })) == GlobalAccessError::Immutable);
}

TEST_CASE("Externs.foreignMemoryIsAnError", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto storeA = Fixture { row };
    auto storeB = Fixture { row };
    auto& store = *storeA.store;
    auto const own = valueOf(store.defineMemory(MemoryType { .limits = atLeast(1) }));
    // Store B's memory shares its address with store A's, which A must not reach for it.
    auto const memory = valueOf(storeB.store->defineMemory(MemoryType { .limits = atLeast(2) }));
    REQUIRE(memory.address() == own.address());
    CHECK_FALSE(memory.isOf(store.id()));
    CHECK(errorOf(store.memorySize(memory)) == MemoryAccessError::ForeignHandle);
    CHECK(errorOf(store.memoryType(memory)) == MemoryAccessError::ForeignHandle);
    CHECK(errorOf(store.memoryRange(memory, 0, 1)) == MemoryAccessError::ForeignHandle);
    CHECK(errorOf(store.memoryRange(memory, 0, 0)) == MemoryAccessError::ForeignHandle);
    CHECK(errorOf(store.growMemory(memory, 1)) == MemoryGrowthError { ForeignHandle {} });
    // Nothing of store A changed.
    CHECK(valueOf(store.memorySize(own)) == MemorySize { .pages = 1, .bytes = PageBytes });
    CHECK(store.used(Limit::MemoryBytes) == PageBytes);
    // An instance's exported memory is foreign to another store alike.
    auto const instanceB = storeB.instantiate();
    auto const exportedMemory = exported(instanceB.memory("m"));
    CHECK(errorOf(store.memorySize(exportedMemory)) == MemoryAccessError::ForeignHandle);
    // A store reaches an instance's exported memory as its own.
    CHECK(valueOf(storeB.store->memorySize(exportedMemory)) == MemorySize { .pages = 1, .bytes = PageBytes });
}

TEST_CASE("Externs.hostHandlesAfterStoreDeleteAreForeign", "[api]")
{
    // The handles a store made without an instance outlive it safely: a later store, which makes its own
    // at the same addresses, tells them apart and reads nothing for them.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto const memory = valueOf(fixture.store->defineMemory(MemoryType { .limits = atLeast(1) }));
    auto const table = valueOf(fixture.store->defineTable(
        TableType { .element = isoline::binary::FuncRef, .limits = atLeast(1) }, nullFunc()));
    auto const global = valueOf(fixture.store->defineGlobal(
        GlobalType { .type = TypeI32, .mutability = Mutability::Var }, I32 { 1 }));
    fixture.store.reset();

    fixture.store = std::make_unique<Store>(fixture.engine, fixture.allocator);
    auto& later = *fixture.store;
    auto const laterMemory = valueOf(later.defineMemory(MemoryType { .limits = atLeast(1) }));
    auto const laterTable = valueOf(later.defineTable(
        TableType { .element = isoline::binary::FuncRef, .limits = atLeast(1) }, nullFunc()));
    auto const laterGlobal =
        valueOf(later.defineGlobal(GlobalType { .type = TypeI32, .mutability = Mutability::Var }, I32 { 2 }));
    REQUIRE(laterMemory.address() == memory.address());
    REQUIRE(laterTable.address() == table.address());
    REQUIRE(laterGlobal.address() == global.address());
    CHECK_FALSE(memory.isOf(later.id()));
    CHECK_FALSE(table.isOf(later.id()));
    CHECK_FALSE(global.isOf(later.id()));
    CHECK(errorOf(later.memorySize(memory)) == MemoryAccessError::ForeignHandle);
    CHECK(errorOf(later.memoryType(memory)) == MemoryAccessError::ForeignHandle);
    CHECK(errorOf(later.memoryRange(memory, 0, 1)) == MemoryAccessError::ForeignHandle);
    CHECK(errorOf(later.growMemory(memory, 1)) == MemoryGrowthError { ForeignHandle {} });
    CHECK(errorOf(later.tableSize(table)) == TableAccessError::ForeignHandle);
    CHECK(errorOf(later.globalValue(global)) == GlobalAccessError::ForeignHandle);
    CHECK(errorOf(later.setGlobal(global, I32 { 9 })) == GlobalAccessError::ForeignHandle);
    // The later store's own are untouched.
    CHECK(valueOf(later.globalValue(laterGlobal)) == Value { I32 { 2 } });
    CHECK(valueOf(later.memorySize(laterMemory)) == MemorySize { .pages = 1, .bytes = PageBytes });
}

TEST_CASE("Externs.hostRefusedMemoryGrowthIsTheCallers", "[api][memory]")
{
    // A host function grows a memory the host defined past the store's limit while the guest's call
    // runs: the refusal is the running call's, as a refused memory.grow of the guest's would be.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row, ContractLimits { .memoryBytes = PageBytes } };
    auto const memory = valueOf(fixture.store->defineMemory(MemoryType { .limits = atLeast(1) }));
    auto refused = std::optional<Refusal> {};
    auto imports = Imports {};
    imports.define("host",
                   "grow",
                   FuncType {},
                   makeHostFunction([&](HostContext&,
                                        std::span<Value const>,
                                        std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                       auto const grown = fixture.store->growMemory(memory, 1);
                       if (!grown && std::holds_alternative<Refusal>(grown.error()))
                           refused = std::get<Refusal>(grown.error());
                       return std::unexpected { Abrupt { HostFailure {
                           .kind = HostFailureKind::Embedder, .code = 7, .mismatch = std::nullopt } } };
                   }));
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("host", "grow", 0)
        .function(0, {}, code({ { 0x10, 0x00 } })) // call $grow
        .exportFunction("run", 1);
    auto const instance = fixture.instantiate(builder, imports);

    auto const called = instance.call(exported(instance.function("run")), {}, {});
    CHECK(refused == Refusal::Limit);
    CHECK(errorOf(called).failure() == CallFailure::HostFailure);
    CHECK(errorOf(called).refusedGrowth.memory == Refusal::Limit);
    CHECK(errorOf(called).refusedGrowth.table == std::nullopt);
}

TEST_CASE("Externs.defineRefusesInvalidTypes", "[api]")
{
    // A type no entity the host defines may have is a value, and adds nothing to the store.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto const concrete =
        isoline::binary::ValueType { .kind = isoline::binary::ValueKind::Ref,
                                     .nullable = isoline::binary::Nullable::Yes,
                                     .heap = { .kind = AbstractHeapType::Concrete, .typeIndex = 0 } };
    auto const concreteIndex = DefineError { InvalidType { .reason = InvalidTypeReason::ConcreteTypeIndex } };
    auto const host = makeHostFunction([](HostContext&, std::span<Value const>, std::span<Value>) noexcept {
        return std::expected<void, Abrupt> {};
    });
    CHECK(errorOf(store.define(FuncType { .params = { concrete }, .results = {} }, host)) == concreteIndex);
    CHECK(errorOf(store.define(FuncType { .params = {}, .results = { concrete } }, host)) == concreteIndex);
    CHECK(errorOf(store.defineTable(TableType { .element = concrete, .limits = atLeast(0) }, nullFunc()))
          == concreteIndex);
    CHECK(errorOf(store.defineGlobal(GlobalType { .type = concrete, .mutability = Mutability::Const },
                                     Value { nullFunc() }))
          == concreteIndex);
    // The bot heap is the validator's own type: no module declares it, nor may the host.
    auto const bottom =
        isoline::binary::ValueType { .kind = isoline::binary::ValueKind::Ref,
                                     .nullable = isoline::binary::Nullable::Yes,
                                     .heap = { .kind = AbstractHeapType::Bot, .typeIndex = 0 } };
    auto const bottomHeap = DefineError { InvalidType { .reason = InvalidTypeReason::BottomHeap } };
    CHECK(errorOf(store.define(FuncType { .params = { TypeI32, bottom }, .results = {} }, host))
          == bottomHeap);
    CHECK(errorOf(store.define(FuncType { .params = {}, .results = { bottom } }, host)) == bottomHeap);
    CHECK(errorOf(store.defineTable(TableType { .element = bottom, .limits = atLeast(0) }, nullFunc()))
          == bottomHeap);
    CHECK(errorOf(store.defineGlobal(GlobalType { .type = bottom, .mutability = Mutability::Const },
                                     Value { nullFunc() }))
          == bottomHeap);
    // A table holds references: an element type of numbers is no table's, whatever init it is given.
    auto const nonReference =
        DefineError { InvalidType { .reason = InvalidTypeReason::NonReferenceElement } };
    CHECK(errorOf(store.defineTable(TableType { .element = TypeI32, .limits = atLeast(0) }, nullFunc()))
          == nonReference);
    // Only the page sizes custom-page-sizes admits, 1 byte and 64 KiB; a size past a u64 included.
    auto const pageSize = DefineError { InvalidType { .reason = InvalidTypeReason::PageSize } };
    for (auto const log2: { uint8_t { 1 },
                            uint8_t { 15 },
                            uint8_t { 17 },
                            isoline::binary::PageSizeLog2Limit,
                            std::numeric_limits<uint8_t>::max() })
    {
        CAPTURE(log2);
        CHECK(errorOf(store.defineMemory(MemoryType { .limits = atLeast(0), .pageSizeLog2 = log2 }))
              == pageSize);
    }
    CHECK(runtimeOf(store).functionCount() == 0);
    CHECK(store.used(Limit::Memories) == 0);
    CHECK(store.used(Limit::Tables) == 0);
    CHECK(
        valueOf(store.defineMemory(MemoryType { .limits = atLeast(1), .pageSizeLog2 = 0 })).isOf(store.id()));
    CHECK(store.used(Limit::MemoryBytes) == 1);
}

TEST_CASE("Externs.liveHostHandlesNameTheirStore", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto const function = valueOf(store.define(
        FuncType {}, makeHostFunction([](HostContext&, std::span<Value const>, std::span<Value>) noexcept {
            return std::expected<void, Abrupt> {};
        })));
    auto const memory = valueOf(store.defineMemory(MemoryType { .limits = atLeast(0) }));
    auto const table = valueOf(store.defineTable(
        TableType { .element = isoline::binary::FuncRef, .limits = atLeast(0) }, nullFunc()));
    auto const global = valueOf(
        store.defineGlobal(GlobalType { .type = TypeI32, .mutability = Mutability::Const }, I32 { 0 }));
    CHECK(&function.store() == &store.id());
    CHECK(&memory.store() == &store.id());
    CHECK(&table.store() == &store.id());
    CHECK(&global.store() == &store.id());
    CHECK(memory.store().serial() == store.id().serial());
    // An instance's handles alike, a tag's included.
    auto const instance = fixture.instantiate();
    CHECK(&exported(instance.tag("e")).store() == &store.id());
}

namespace
{

/// A host function returning @p answer as an i32, counting its calls in @p calls.
std::shared_ptr<isoline::api::IHostFunction> answering(uint32_t answer, std::size_t& calls)
{
    return makeHostFunction(
        [answer, &calls](HostContext&,
                         std::span<Value const>,
                         std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
            ++calls;
            results[0] = I32 { answer };
            return {};
        });
}

/// Imports a function and a global under one name, ("m", "x"), which Imports holds one definition of,
/// and exports their sum:
///
///     (module
///       (import "m" "x" (func $x (result i32))) (import "m" "x" (global $x i32))
///       (func (export "sum") (result i32) (i32.add (call $x) (global.get $x))))
ModuleBuilder sameNameTwoTypes()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { TypeI32 } })
        .importFunction("m", "x", 0)
        .importGlobal("m", "x", GlobalType { .type = TypeI32, .mutability = Mutability::Const })
        .function(0, {}, code({ { 0x10, 0x00 }, { 0x23, 0x00 }, { 0x6A } })) // call 0; global.get 0; i32.add
        .exportFunction("sum", 1);
    return builder;
}

/// Loads @p builder's module with @p fixture's engine, which must load.
Module loaded(Fixture const& fixture, ModuleBuilder const& builder)
{
    auto module = fixture.engine.load(builder.build());
    REQUIRE(module.has_value());
    return *std::move(module);
}

} // namespace

TEST_CASE("Externs.positionalSameNameTwoTypes", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto calls = std::size_t { 0 };
    auto const function =
        valueOf(store.define(FuncType { .params = {}, .results = { TypeI32 } }, answering(40, calls)));
    auto const global = valueOf(
        store.defineGlobal(GlobalType { .type = TypeI32, .mutability = Mutability::Const }, I32 { 2 }));
    auto const module = loaded(fixture, sameNameTwoTypes());

    // By name, one definition serves both imports, and fits only one of them.
    auto imports = Imports {};
    imports.define("m", "x", function);
    CHECK(imports.resolve(module) == std::vector { Linkage::Satisfied, Linkage::Unsatisfied });
    imports.define("m", "x", global);
    CHECK(imports.resolve(module) == std::vector { Linkage::Unsatisfied, Linkage::Satisfied });
    CHECK(errorOf(store.instantiate(module, imports))
          == InstantiateError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });

    // By position, each import has its own extern, so both link.
    auto const externs = std::array<Extern, 2> { function, global };
    auto const instance = valueOf(store.instantiateByPosition(module, externs));
    CHECK(callI32(instance, "sum") == 42);
    CHECK(calls == 1);

    // A positional extern of another kind than its import's is refused as a named definition is.
    auto const swapped = std::array<Extern, 2> { global, function };
    CHECK(errorOf(store.instantiateByPosition(module, swapped))
          == InstantiateError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
}

TEST_CASE("Externs.positionalCountMismatch", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto calls = std::size_t { 0 };
    auto const function =
        valueOf(store.define(FuncType { .params = {}, .results = { TypeI32 } }, answering(1, calls)));
    auto const global = valueOf(
        store.defineGlobal(GlobalType { .type = TypeI32, .mutability = Mutability::Const }, I32 { 2 }));
    auto const module = loaded(fixture, sameNameTwoTypes());
    auto const memories = store.used(Limit::Memories);

    auto const tooFew = std::array<Extern, 1> { function };
    CHECK(errorOf(store.instantiateByPosition(module, tooFew))
          == InstantiateError { ImportCountMismatch { .expected = 2, .given = 1 } });
    auto const tooMany = std::array<Extern, 3> { function, global, global };
    CHECK(errorOf(store.instantiateByPosition(module, tooMany))
          == InstantiateError { ImportCountMismatch { .expected = 2, .given = 3 } });
    CHECK(errorOf(store.instantiateByPosition(module, {}))
          == InstantiateError { ImportCountMismatch { .expected = 2, .given = 0 } });
    // Refused before anything ran or was added.
    CHECK(calls == 0);
    CHECK(runtimeOf(store).functionCount() == 1);
    CHECK(store.used(Limit::Memories) == memories);
    // A module of no imports takes no externs.
    CHECK(valueOf(store.instantiateByPosition(loaded(fixture, everyKind()), {})).exportAt(0).has_value());
}

TEST_CASE("Externs.positionalForeignImport", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto storeA = Fixture { row };
    auto storeB = Fixture { row };
    auto calls = std::size_t { 0 };
    auto const own =
        valueOf(storeA.store->define(FuncType { .params = {}, .results = { TypeI32 } }, answering(1, calls)));
    auto const foreign = valueOf(storeB.store->defineGlobal(
        GlobalType { .type = TypeI32, .mutability = Mutability::Const }, I32 { 2 }));
    auto const module = loaded(storeA, sameNameTwoTypes());
    auto const externs = std::array<Extern, 2> { own, foreign };
    CHECK(errorOf(storeA.store->instantiateByPosition(module, externs))
          == InstantiateError { ForeignImport { .index = 1 } });
    CHECK(calls == 0);
    // A module of another engine is refused first, whatever its externs.
    auto otherConfig = configOf(ContractLimits {}, row);
    otherConfig.features = isoline::api::FeatureSet::wasm2();
    auto const otherEngine = Engine { otherConfig };
    auto const otherModule = otherEngine.load(sameNameTwoTypes().build());
    REQUIRE(otherModule.has_value());
    CHECK(std::holds_alternative<isoline::api::EngineMismatch>(
        errorOf(storeA.store->instantiateByPosition(*otherModule, externs))));
}

TEST_CASE("Externs.importsDefineEveryKind", "[api]")
{
    // Imports::define() of an Extern of each kind snapshots its type: resolve() compares it without a
    // store, and the store that made it links it by name.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto const exporter = fixture.instantiate();
    auto const exporterModule = loaded(fixture, everyKind());
    auto imports = Imports {};
    for (auto const [index, description]: core::views::enumerate(exporterModule.exports()))
        imports.define(
            "x", std::string { description.name }, valueOf(exporter.exportAt(static_cast<uint32_t>(index))));
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { TypeI32 } })
        .type(FuncType { .params = { TypeI32 }, .results = {} })
        .importGlobal("x", "g", GlobalType { .type = TypeI32, .mutability = Mutability::Var })
        .importTag("x", "e", 1)
        .importFunction("x", "f", 0)
        .importMemory("x", "m", atLeast(1))
        .importTable("x", "t", TableType { .element = isoline::binary::FuncRef, .limits = atLeast(2) })
        .importGlobal("x", "c", GlobalType { .type = TypeI32, .mutability = Mutability::Const })
        .exportFunction("f", 0);
    auto const module = loaded(fixture, builder);
    CHECK(imports.resolve(module) == std::vector<Linkage>(6, Linkage::Satisfied));
    auto const importer = valueOf(fixture.store->instantiate(module, imports));
    CHECK(callI32(importer, "f") == 5);

    // A type the snapshot tells apart: a mutable global for a const import, a larger table.
    auto mismatched = ModuleBuilder {};
    mismatched.importGlobal("x", "g", GlobalType { .type = TypeI32, .mutability = Mutability::Const })
        .importTable("x", "t", TableType { .element = isoline::binary::FuncRef, .limits = between(0, 1) })
        .importMemory("x", "m", between(0, 1))
        .importGlobal("x", "c", GlobalType { .type = isoline::binary::I64, .mutability = Mutability::Const });
    CHECK(imports.resolve(loaded(fixture, mismatched)) == std::vector<Linkage>(4, Linkage::Unsatisfied));

    // A host-made memory's snapshot: its page size, which a memory of the default page size does not fit.
    imports.define(
        "x",
        "tiny",
        valueOf(fixture.store->defineMemory(MemoryType { .limits = atLeast(1), .pageSizeLog2 = 0 })));
    auto pageSized = ModuleBuilder {};
    pageSized.importMemory("x", "tiny", atLeast(1)).importMemory("x", "m", atLeast(1));
    CHECK(imports.resolve(loaded(fixture, pageSized))
          == std::vector { Linkage::Unsatisfied, Linkage::Satisfied });
}

TEST_CASE("Externs.storeCallHostFunction", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto calls = std::size_t { 0 };
    auto const function =
        valueOf(store.define(FuncType { .params = {}, .results = { TypeI32 } }, answering(17, calls)));
    // No instance exists: the store calls its own function.
    auto results = std::array<Value, 1> {};
    REQUIRE(store.call(function, {}, results).has_value());
    CHECK(results[0] == Value { I32 { 17 } });
    CHECK(calls == 1);
    // An exported function, without its instance.
    auto const instance = fixture.instantiate();
    auto const exportedFunction = exported(instance.function("f"));
    REQUIRE(store.call(exportedFunction, {}, results).has_value());
    CHECK(results[0] == Value { I32 { 5 } });
    // A guest's trap is the call's, as through an instance.
    auto trapping = ModuleBuilder {};
    trapping.type(FuncType {}).function(0, {}, code({ { 0x00 } })).exportFunction("trap", 0);
    auto const trapper = fixture.instantiate(trapping);
    auto const trapped = store.call(exported(trapper.function("trap")), {}, {});
    CHECK(errorOf(trapped).failure() == CallFailure::Trap);
    CHECK(std::get<Trap>(errorOf(trapped).detail).code == TrapCode::Unreachable);
}

TEST_CASE("Externs.storeCallArgumentMismatch", "[api]")
{
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto calls = std::size_t { 0 };
    auto const function = valueOf(
        store.define(FuncType { .params = { TypeI32 }, .results = { TypeI32 } }, answering(3, calls)));
    auto results = std::array<Value, 1> {};
    auto const wrongType = std::array<Value, 1> { I64 { 1 } };
    auto const wrongCount = std::array<Value, 2> { I32 { 1 }, I32 { 2 } };
    auto const right = std::array<Value, 1> { I32 { 1 } };
    CHECK(errorOf(store.call(function, wrongType, results)).failure() == CallFailure::ArgumentMismatch);
    CHECK(std::get<ValueMismatch>(errorOf(store.call(function, wrongType, results)).detail).kind
          == MismatchKind::Kind);
    CHECK(errorOf(store.call(function, wrongCount, results)).failure() == CallFailure::ArgumentMismatch);
    CHECK(errorOf(store.call(function, {}, results)).failure() == CallFailure::ArgumentMismatch);
    CHECK(errorOf(store.call(function, right, {})).failure() == CallFailure::ArgumentMismatch);
    // No call entered the host function.
    CHECK(calls == 0);
    REQUIRE(store.call(function, right, results).has_value());
    CHECK(calls == 1);
}

namespace
{

/// A host function of no params and no results that does nothing.
std::shared_ptr<isoline::api::IHostFunction> doNothing()
{
    return makeHostFunction([](HostContext&, std::span<Value const>, std::span<Value>) noexcept {
        return std::expected<void, Abrupt> {};
    });
}

/// The fields of @p type, compared with @p limits and @p pageSizeLog2.
void checkMemoryType(MemoryType const& type, Limits const& limits, uint8_t pageSizeLog2)
{
    CHECK(type.limits.min == limits.min);
    CHECK(type.limits.max == limits.max);
    CHECK(type.limits.addressType == limits.addressType);
    CHECK(type.pageSizeLog2 == pageSizeLog2);
}

} // namespace

TEST_CASE("Externs.functionFromReference", "[api]")
{
    // A funcref names a function to call and to ask the type of, an element segment's that no instance
    // exports included; anything else is refused as a value.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto const answerType = FuncType { .params = {}, .results = { TypeI32 } };
    auto builder = ModuleBuilder {};
    builder.type(answerType)
        .table(TableType { .element = isoline::binary::FuncRef, .limits = atLeast(2) })
        .function(0, {}, code({ i32Const(42) }))
        .exportEntity("t", ExternKind::Table, 0)
        .element({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00 }); // active, table 0 from 0: function 0
    auto const instance = fixture.instantiate(builder);
    auto const table = exported(instance.table("t"));

    auto const answer = valueOf(store.function(valueOf(store.tableGet(table, 0))));
    CHECK(answer.isOf(store.id()));
    CHECK(answer.type() == answerType);
    auto results = std::array<Value, 1> {};
    REQUIRE(store.call(answer, {}, results).has_value());
    CHECK(results[0] == Value { I32 { 42 } });
    // A host-defined function's reference names it back, at the last address the store holds.
    auto const host = valueOf(store.define(FuncType {}, doNothing()));
    REQUIRE(host.address() + 1 == runtimeOf(store).functionCount());
    auto const named = valueOf(store.function(funcRef(host.address())));
    CHECK(named.address() == host.address());
    CHECK(named.type() == FuncType {});
    CHECK(store.call(named, {}, {}).has_value());

    CHECK(errorOf(store.function(valueOf(store.tableGet(table, 1)))) == FunctionRefError::Null);
    CHECK(errorOf(store.function(externRef(0))) == FunctionRefError::NotFunction);
    auto const exception =
        Ref { .kind = RefKind::Exn, .heap = { .kind = AbstractHeapType::Exn }, .address = 0 };
    CHECK(errorOf(store.function(exception)) == FunctionRefError::NotFunction);
    auto const count = static_cast<uint32_t>(runtimeOf(store).functionCount());
    CHECK(errorOf(store.function(funcRef(count))) == FunctionRefError::UnknownAddress);
    CHECK(errorOf(store.function(funcRef(std::numeric_limits<uint32_t>::max())))
          == FunctionRefError::UnknownAddress);

    // Another store's reference, live or deleted, at an address this store holds no function at.
    auto other = Fixture { row };
    auto foreignAddress = uint32_t { 0 };
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count + 1))
        foreignAddress = valueOf(other.store->define(FuncType {}, doNothing())).address();
    REQUIRE(foreignAddress == count);
    auto const foreignRef = funcRef(foreignAddress);
    REQUIRE(other.store->function(foreignRef).has_value());
    CHECK(errorOf(store.function(foreignRef)) == FunctionRefError::UnknownAddress);
    other.store.reset();
    CHECK(errorOf(store.function(foreignRef)) == FunctionRefError::UnknownAddress);
}

TEST_CASE("Externs.memoryTypeFollowsGrowth", "[api]")
{
    // A memory's type is its current size in pages, with the maximum, address type and page size it was
    // made with: for a host-made memory and for an instance's, reached through a re-exported import.
    auto const row = GENERATE(from_range(engineRows()));
    auto fixture = Fixture { row };
    auto& store = *fixture.store;
    auto const made = between(1, 4, AddressType::I64);
    auto const tiny = valueOf(store.defineMemory(MemoryType { .limits = made, .pageSizeLog2 = 0 }));
    checkMemoryType(valueOf(store.memoryType(tiny)), made, 0);
    REQUIRE(valueOf(store.growMemory(tiny, 2)) == 1);
    checkMemoryType(valueOf(store.memoryType(tiny)), between(3, 4, AddressType::I64), 0);

    auto const exporter = fixture.instantiate();
    auto imports = Imports {};
    imports.define("x", "m", exported(exporter.memory("m")));
    auto reexporting = ModuleBuilder {};
    reexporting.importMemory("x", "m", atLeast(1)).exportEntity("m", ExternKind::Memory, 0);
    auto const importer = fixture.instantiate(reexporting, imports);
    auto const reexported = exported(importer.memory("m"));
    REQUIRE(reexported.address() == exported(exporter.memory("m")).address());
    auto const defaultPage = isoline::binary::DefaultPageSizeLog2;
    checkMemoryType(valueOf(store.memoryType(reexported)), atLeast(1), defaultPage);
    REQUIRE(valueOf(store.growMemory(exported(exporter.memory("m")), 2)) == 1);
    checkMemoryType(valueOf(store.memoryType(reexported)), atLeast(3), defaultPage);
}
