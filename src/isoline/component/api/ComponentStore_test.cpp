// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/api/CallError.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/api/testing/ApiFixtures.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/ComponentFixtures.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/testing/ResourceDoubles.hpp>
#include <isoline/component/runtime/testing/StoreFixtures.hpp>

using isoline::component::api::ComponentCallError;
using isoline::component::api::ComponentCallFailure;
using isoline::component::api::ComponentCallFailureTable;
using isoline::component::api::ComponentConfig;
using isoline::component::api::ComponentStore;
using isoline::component::api::FunctionTypeSpec;
using isoline::component::api::HostComponentContext;
using isoline::component::api::HostResourceType;
using isoline::component::api::Linker;
using isoline::component::api::StoreEffect;
using isoline::component::api::testing::ApiHarness;
using isoline::component::api::testing::callWith;
using isoline::component::api::testing::functionSpec;
using isoline::component::api::testing::hostFunction;
using isoline::component::api::testing::instanceOf;
using isoline::component::api::testing::primitive;
using isoline::component::api::testing::require;
using isoline::component::api::testing::u32Of;
using isoline::component::binary::Asyncness;
using isoline::component::binary::BorrowType;
using isoline::component::binary::CanonBuiltin;
using isoline::component::binary::CanonDefinition;
using isoline::component::binary::CanonOptionKind;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreSort;
using isoline::component::binary::DefinedValueType;
using isoline::component::binary::EnumType;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::FunctionTypeDefinition;
using isoline::component::binary::LabeledType;
using isoline::component::binary::ListType;
using isoline::component::binary::OwnType;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::RecordType;
using isoline::component::binary::Sort;
using isoline::component::binary::TupleType;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::liftedIdentity;
using isoline::component::canonical::Ownership;
using isoline::component::canonical::ResourceValue;
using isoline::component::canonical::Trap;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::ValueBuilder;
using isoline::component::canonical::ValueErrorCode;
using isoline::component::canonical::Values;
using isoline::component::runtime::testing::coreModule;
using isoline::component::runtime::testing::embedderFailure;
using isoline::component::runtime::testing::functionType;
using isoline::component::runtime::testing::option;
using isoline::component::runtime::testing::RecordingDestructor;
using isoline::component::types::DefinedType;
using isoline::component::types::ValueKind;

namespace
{

using HostResult = std::expected<void, isoline::api::HostFailure>;
using CallResult = std::expected<Values, ComponentCallError>;

/// The failure of a call (REQUIREd to have failed).
[[nodiscard]] ComponentCallError failureOf(CallResult const& called)
{
    REQUIRE_FALSE(called.has_value());
    return called.error();
}

/// The trap code a call failed with (REQUIREd to be a trap).
[[nodiscard]] TrapCode trapOf(CallResult const& called)
{
    auto const error = failureOf(called);
    auto const* trap = std::get_if<Trap>(&error.detail);
    REQUIRE(trap != nullptr);
    return trap->code;
}

/// A component importing `f: func() -> u32`, whose `run` returns what `f` returns.
[[nodiscard]] std::vector<std::byte> callsHostF()
{
    auto builder = ComponentBuilder {};
    auto const f = builder.import(
        "f", ExternDesc { .sort = Sort::Func, .index = functionType(builder, {}, PrimitiveValueType::U32) });
    auto const lowered = builder.coreInlineExports(
        { CoreInlineExport { .name = "f", .sort = CoreSort::Func, .index = builder.canonLower(f, {}) } });
    auto const instance = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "f" (func $f (result i32)))
      (func (export "run") (result i32) (call $f))))")),
                                                  { { "h", lowered } });
    std::ignore =
        builder.exportItem("run",
                           Sort::Func,
                           builder.canonLift(builder.aliasCoreExport(instance, "run", CoreSort::Func),
                                             {},
                                             functionType(builder, {}, PrimitiveValueType::U32)));
    return builder.build();
}

/// A core module with a memory and a bump `realloc`.
constexpr auto MemoryModule = R"((module
  (memory (export "mem") 1)
  (global $next (mut i32) (i32.const 1024))
  (func (export "realloc") (param i32 i32 i32 i32) (result i32)
    (local $at i32)
    (local.set $at (i32.and (i32.add (global.get $next) (i32.sub (local.get 2) (i32.const 1)))
                            (i32.sub (i32.const 0) (local.get 2))))
    (global.set $next (i32.add (local.get $at) (local.get 3)))
    (local.get $at))
  (func (export "word") (param i32) (result i32) (i32.load (local.get 0))))
)";

} // namespace

TEST_CASE("CallError.tableNamesEveryFailure")
{
    // Every failure carries what its row names, and only a run that entered guest code locks down.
    for (auto const& row: ComponentCallFailureTable)
    {
        auto const entered = row.failure == ComponentCallFailure::Trap
                             || row.failure == ComponentCallFailure::Interrupted
                             || row.failure == ComponentCallFailure::HostFailure;
        CHECK((row.effect == StoreEffect::LocksDown) == entered);
        CHECK_FALSE(row.text.empty());
    }
    auto const trapped = isoline::component::api::callError(
        ComponentCallFailure::Trap, isoline::component::canonical::trapOf(TrapCode::UnknownHandle, 5));
    CHECK(trapped.message() == "component call trapped: unknown handle index 5");
    CHECK(isoline::component::api::callError(ComponentCallFailure::LockedDown).message()
          == "component store is locked down");
    CHECK(isoline::component::api::callError(ComponentCallFailure::HostFailure, embedderFailure(3)).message()
          == "host function failed: embedder failure 3");
}

TEST_CASE("HostFunctions.recordAndListArgs")
{
    // run() calls log(level: enum, message: string) with (warn, "hello"), then state() -> info, a
    // record {name: string, env: list<tuple<string, string>>}, which it returns.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const level = builder.import(
        "level",
        ExternDesc {
            .sort = Sort::Type,
            .index = builder.type(TypeDefinition {
                .type = DefinedValueType { EnumType { .labels = { "debug", "info", "warn" } } } }) });
    auto const tuple = builder.type(
        TypeDefinition { .type = DefinedValueType { TupleType {
                             .elements = { PrimitiveValueType::String, PrimitiveValueType::String } } } });
    auto const list = builder.type(
        TypeDefinition { .type = DefinedValueType { ListType { .element = TypeIndex { .index = tuple } } } });
    auto const info = builder.import(
        "info",
        ExternDesc { .sort = Sort::Type,
                     .index = builder.type(TypeDefinition {
                         .type = DefinedValueType { RecordType {
                             .fields = { LabeledType { .label = "name", .type = PrimitiveValueType::String },
                                         LabeledType { .label = "env",
                                                       .type = TypeIndex { .index = list } } } } } }) });
    auto const log = builder.import(
        "log",
        ExternDesc {
            .sort = Sort::Func,
            .index = builder.type(TypeDefinition {
                .type = FunctionTypeDefinition {
                    .asyncness = Asyncness::Sync,
                    .params = { LabeledType { .label = "level", .type = TypeIndex { .index = level } },
                                LabeledType { .label = "message", .type = PrimitiveValueType::String } },
                    .result = std::nullopt } }) });
    auto const state = builder.import(
        "state",
        ExternDesc { .sort = Sort::Func, .index = functionType(builder, {}, TypeIndex { .index = info }) });
    auto const memory = builder.coreInstantiate(builder.coreModule(coreModule(MemoryModule)), {});
    auto const options = std::vector {
        option(CanonOptionKind::Memory, builder.aliasCoreExport(memory, "mem", CoreSort::Memory)),
        option(CanonOptionKind::Realloc, builder.aliasCoreExport(memory, "realloc", CoreSort::Func)),
    };
    auto const lowered = builder.coreInlineExports({
        CoreInlineExport { .name = "log", .sort = CoreSort::Func, .index = builder.canonLower(log, options) },
        CoreInlineExport {
            .name = "state", .sort = CoreSort::Func, .index = builder.canonLower(state, options) },
    });
    auto const memoryExports = builder.coreInlineExports(
        { CoreInlineExport { .name = "mem",
                             .sort = CoreSort::Memory,
                             .index = builder.aliasCoreExport(memory, "mem", CoreSort::Memory) } });
    auto const guest = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "log" (func $log (param i32 i32 i32)))
      (import "h" "state" (func $state (param i32)))
      (import "m" "mem" (memory 1))
      (data (i32.const 16) "hello")
      (func (export "run") (result i32)
        (call $log (i32.const 2) (i32.const 16) (i32.const 5))
        (call $state (i32.const 256))
        (i32.const 256))))")),
                                               { { "h", lowered }, { "m", memoryExports } });
    std::ignore =
        builder.exportItem("run",
                           Sort::Func,
                           builder.canonLift(builder.aliasCoreExport(guest, "run", CoreSort::Func),
                                             { options.front() },
                                             functionType(builder, {}, TypeIndex { .index = info })));
    auto const component = harness.load(builder.build());

    auto linker = Linker { ComponentConfig {} };
    auto& types = linker.types();
    auto const string = primitive(linker, ValueKind::String);
    auto const interned = [&](DefinedType type) {
        auto id = types.intern(std::move(type));
        REQUIRE(id.has_value());
        return *id;
    };
    auto const levelType =
        interned(DefinedType { .kind = ValueKind::Enum, .labels = { "debug", "info", "warn" } });
    auto const tupleType = interned(DefinedType { .kind = ValueKind::Tuple, .children = { string, string } });
    auto const listType = interned(DefinedType { .kind = ValueKind::List, .children = { tupleType } });
    auto const infoType = interned(DefinedType {
        .kind = ValueKind::Record, .children = { string, listType }, .labels = { "name", "env" } });
    auto logged = std::vector<std::pair<uint32_t, std::string>> {};
    require(linker.defineFunction(
        "log",
        FunctionTypeSpec { .params = { { "level", levelType }, { "message", string } },
                           .result = std::nullopt },
        hostFunction([&](HostComponentContext&, Values const& args, ValueBuilder&) -> HostResult {
            auto message = args.root(1).string().utf8();
            if (!message)
                return std::unexpected { embedderFailure() };
            logged.emplace_back(args.root(0).caseIndex(), std::move(*message));
            return {};
        })));
    require(linker.defineFunction(
        "state",
        functionSpec({}, infoType),
        hostFunction([](HostComponentContext&, Values const&, ValueBuilder& results) -> HostResult {
            results.begin(2).string("ext").begin(2);
            results.begin(2).string("A").string("1").end();
            results.begin(2).string("B").string("2").end();
            results.end().end();
            return {};
        })));
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, linker));
    auto const called = callWith(instance, "run");
    REQUIRE(called.has_value());
    CHECK(logged == std::vector<std::pair<uint32_t, std::string>> { { 2, "hello" } });
    REQUIRE(called->rootCount() == 1);
    auto const record = called->root(0);
    CHECK(record.child(0).string().utf8() == "ext");
    auto const env = record.child(1);
    REQUIRE(env.childCount() == 2);
    auto entries = std::vector<std::pair<std::string, std::string>> {};
    for (auto const entry: env.children())
        entries.emplace_back(*entry.child(0).string().utf8(), *entry.child(1).string().utf8());
    CHECK(entries == std::vector<std::pair<std::string, std::string>> { { "A", "1" }, { "B", "2" } });
}

TEST_CASE("HostFunctions.wrongResultIsHostFailure")
{
    auto harness = ApiHarness {};
    auto const component = harness.load(callsHostF());
    auto const failsWith = [&](auto build) {
        auto linker = Linker { ComponentConfig {} };
        auto const function =
            hostFunction([build](HostComponentContext&, Values const&, ValueBuilder& results) -> HostResult {
                build(results);
                return {};
            });
        require(linker.defineFunction("f", functionSpec({}, primitive(linker, ValueKind::U32)), function));
        auto store = harness.store();
        auto instance = instanceOf(store.instantiate(component, linker));
        auto const error = failureOf(callWith(instance, "run"));
        CHECK(error.failure == ComponentCallFailure::HostFailure);
        auto const* host = std::get_if<isoline::api::HostFailure>(&error.detail);
        REQUIRE(host != nullptr);
        CHECK(host->kind == isoline::api::HostFailureKind::ResultMismatch);
        CHECK(store.isLockedDown());
        // Locked down: the next call enters nothing.
        CHECK(failureOf(callWith(instance, "run")).failure == ComponentCallFailure::LockedDown);
        CHECK(function->calls == 1);
    };
    SECTION("a u64 for a u32")
    {
        failsWith([](ValueBuilder& results) { results.scalar(uint64_t { 1 } << 40); });
    }
    SECTION("a string for a u32")
    {
        failsWith([](ValueBuilder& results) { results.string("7"); });
    }
    SECTION("no result")
    {
        failsWith([](ValueBuilder&) {});
    }
}

TEST_CASE("HostFunctions.hostFailureLocksDown")
{
    // A host that fails ends the call as a trap does: HostFailure, its own failure, and lockdown.
    auto harness = ApiHarness {};
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineFunction(
        "f",
        functionSpec({}, primitive(linker, ValueKind::U32)),
        hostFunction([](HostComponentContext&, Values const&, ValueBuilder&) -> HostResult {
            return std::unexpected { embedderFailure(7) };
        })));
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(harness.load(callsHostF()), linker));
    auto const error = failureOf(callWith(instance, "run"));
    CHECK(error == isoline::component::api::callError(ComponentCallFailure::HostFailure, embedderFailure(7)));
    CHECK(store.isLockedDown());
}

TEST_CASE("HostResources.ownBorrowAndDrop")
{
    // run() makes a surface ([constructor]), writes to it ([method]surface.write, a borrow) and drops it;
    // keep(b: borrow<surface>) returns holding the borrow it was lent.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const surface = builder.import(
        "surface", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const own =
        builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = surface } } });
    auto const borrow =
        builder.type(TypeDefinition { .type = DefinedValueType { BorrowType { .resource = surface } } });
    auto const constructor = builder.import(
        "[constructor]surface",
        ExternDesc { .sort = Sort::Func, .index = functionType(builder, {}, TypeIndex { .index = own }) });
    auto const write = builder.import(
        "[method]surface.write",
        ExternDesc {
            .sort = Sort::Func,
            .index = builder.type(TypeDefinition {
                .type = FunctionTypeDefinition {
                    .asyncness = Asyncness::Sync,
                    .params = { LabeledType { .label = "self", .type = TypeIndex { .index = borrow } } },
                    .result = PrimitiveValueType::U32 } }) });
    auto const lowered = builder.coreInlineExports({
        CoreInlineExport {
            .name = "new", .sort = CoreSort::Func, .index = builder.canonLower(constructor, {}) },
        CoreInlineExport { .name = "write", .sort = CoreSort::Func, .index = builder.canonLower(write, {}) },
        CoreInlineExport { .name = "drop",
                           .sort = CoreSort::Func,
                           .index = builder.canon(CanonDefinition { .builtin = CanonBuiltin::ResourceDrop,
                                                                    .immediates = { surface },
                                                                    .options = {},
                                                                    .result = std::nullopt }) },
    });
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "new" (func $new (result i32)))
      (import "h" "write" (func $write (param i32) (result i32)))
      (import "h" "drop" (func $drop (param i32)))
      (func (export "run") (result i32) (local $h i32) (local $written i32)
        (local.set $h (call $new))
        (local.set $written (call $write (local.get $h)))
        (call $drop (local.get $h))
        (local.get $written))
      (func (export "keep") (param i32))))")),
                                              { { "h", lowered } });
    std::ignore = builder.exportItem("run",
                                     Sort::Func,
                                     builder.canonLift(builder.aliasCoreExport(core, "run", CoreSort::Func),
                                                       {},
                                                       functionType(builder, {}, PrimitiveValueType::U32)));
    std::ignore = builder.exportItem(
        "keep",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(core, "keep", CoreSort::Func),
                          {},
                          functionType(builder, { TypeIndex { .index = borrow } }, std::nullopt)));
    auto const component = harness.load(builder.build());

    auto const destructor = std::make_shared<RecordingDestructor>();
    auto const type = HostResourceType { "surface", destructor };
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineResource("surface", type));
    auto const ownType = linker.ownType(type);
    auto const borrowType = linker.borrowType(type);
    REQUIRE(ownType.has_value());
    REQUIRE(borrowType.has_value());
    auto next = uint32_t { 7 };
    auto written = std::vector<uint32_t> {};
    require(linker.defineFunction(
        "[constructor]surface",
        functionSpec({}, *ownType),
        hostFunction([&](HostComponentContext& context, Values const&, ValueBuilder& results) -> HostResult {
            auto made = context.newOwn(type, next++);
            if (!made)
                return std::unexpected { embedderFailure() };
            results.resource(*made);
            return {};
        })));
    require(linker.defineFunction(
        "[method]surface.write",
        FunctionTypeSpec { .params = { { "self", *borrowType } },
                           .result = primitive(linker, ValueKind::U32) },
        hostFunction([&](HostComponentContext&, Values const& args, ValueBuilder& results) -> HostResult {
            auto const self = args.root(0).resource();
            written.push_back(self.rep);
            results.scalar(self.ownership == Ownership::Borrow ? self.rep * 10 : 0);
            return {};
        })));

    {
        auto store = harness.store();
        auto instance = instanceOf(store.instantiate(component, linker));
        CHECK(u32Of(callWith(instance, "run")) == 70);
        CHECK(u32Of(callWith(instance, "run")) == 80);
        CHECK(written == std::vector<uint32_t> { 7, 8 });
        CHECK(destructor->destroyed == std::vector<uint32_t> { 7, 8 });

        // A borrow the callee still holds when it returns traps, and locks the store down.
        auto const keep = instance.function("keep");
        REQUIRE(keep.has_value());
        auto lent = store.hostResource(type, 9, Ownership::Borrow);
        REQUIRE(lent.has_value());
        auto arguments = instance.argumentsFor(*keep);
        arguments.resource(*lent);
        auto const built = arguments.finish();
        REQUIRE(built.has_value());
        CHECK(trapOf(instance.call(*keep, *built)) == TrapCode::BorrowOutstanding);
        CHECK(store.isLockedDown());

        // The host's own resources are host code only: dropped once, even locked down.
        auto const held = store.hostResource(type, 11, Ownership::Own);
        REQUIRE(held.has_value());
        CHECK(store.drop(*held).has_value());
        CHECK(destructor->destroyed == std::vector<uint32_t> { 7, 8, 11 });
        auto const borrowed = store.drop(*lent);
        REQUIRE_FALSE(borrowed.has_value());
        CHECK(borrowed.error().failure == ComponentCallFailure::ArgumentMismatch);
    }
    // No guest table held one: the store's end destroys nothing more.
    CHECK(destructor->destroyed == std::vector<uint32_t> { 7, 8, 11 });
    auto other = harness.store();
    auto const unbound = other.hostResource(type, 1, Ownership::Own);
    REQUIRE_FALSE(unbound.has_value());
    CHECK(unbound.error().code == TrapCode::WrongResource);
}

TEST_CASE("HostResources.guestResourceToHost")
{
    // make() -> own<r> hands the host an own of a guest resource; the host's drop runs r's destructor,
    // which records the rep and counts.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const first = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (memory (export "mem") 1)
      (func (export "dtor") (param $rep i32)
        (i32.store (i32.const 0) (i32.add (i32.load (i32.const 0)) (i32.const 1)))
        (i32.store (i32.const 4) (local.get $rep))
        (if (i32.eq (local.get $rep) (i32.const 13)) (then unreachable)))
      (func (export "count") (result i32) (i32.load (i32.const 0)))
      (func (export "last") (result i32) (i32.load (i32.const 4)))))")),
                                               {});
    auto const r = builder.resource(builder.aliasCoreExport(first, "dtor", CoreSort::Func));
    auto const exported = builder.exportItem("r", Sort::Type, r);
    auto const own =
        builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = exported } } });
    auto const builtins = builder.coreInlineExports(
        { CoreInlineExport { .name = "new",
                             .sort = CoreSort::Func,
                             .index = builder.canon(CanonDefinition { .builtin = CanonBuiltin::ResourceNew,
                                                                      .immediates = { r },
                                                                      .options = {},
                                                                      .result = std::nullopt }) } });
    auto const second = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "r" "new" (func $new (param i32) (result i32)))
      (func (export "make") (result i32) (call $new (i32.const 9)))
      (func (export "make-bad") (result i32) (call $new (i32.const 13)))))")),
                                                { { "r", builtins } });
    for (auto const* name: { "make", "make-bad" })
        std::ignore =
            builder.exportItem(name,
                               Sort::Func,
                               builder.canonLift(builder.aliasCoreExport(second, name, CoreSort::Func),
                                                 {},
                                                 functionType(builder, {}, TypeIndex { .index = own })));
    for (auto const* name: { "count", "last" })
        std::ignore =
            builder.exportItem(name,
                               Sort::Func,
                               builder.canonLift(builder.aliasCoreExport(first, name, CoreSort::Func),
                                                 {},
                                                 functionType(builder, {}, PrimitiveValueType::U32)));
    auto const component = harness.load(builder.build());

    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, Linker { ComponentConfig {} }));
    auto const made = callWith(instance, "make");
    REQUIRE(made.has_value());
    auto const value = made->root(0).resource();
    CHECK(value.ownership == Ownership::Own);
    CHECK(value.rep == 9);
    CHECK(u32Of(callWith(instance, "count")) == 0);
    REQUIRE(store.drop(value).has_value());
    CHECK(u32Of(callWith(instance, "count")) == 1);
    CHECK(u32Of(callWith(instance, "last")) == 9);

    // Refused, nothing run: a borrow, and a type the store does not have.
    auto const refused = [&](ResourceValue resource) {
        auto const dropped = store.drop(resource);
        REQUIRE_FALSE(dropped.has_value());
        CHECK(dropped.error().failure == ComponentCallFailure::ArgumentMismatch);
        CHECK_FALSE(store.isLockedDown());
    };
    refused(ResourceValue { .type = value.type, .rep = 9, .ownership = Ownership::Borrow });
    refused(ResourceValue { .type = { .value = 999 }, .rep = 9, .ownership = Ownership::Own });
    CHECK(u32Of(callWith(instance, "count")) == 1);

    SECTION("a destructor that traps locks the store down")
    {
        auto const bad = callWith(instance, "make-bad");
        REQUIRE(bad.has_value());
        auto const dropped = store.drop(bad->root(0).resource());
        REQUIRE_FALSE(dropped.has_value());
        CHECK(dropped.error().failure == ComponentCallFailure::Trap);
        CHECK(store.isLockedDown());
    }
    SECTION("a guest destructor does not run in a locked-down store")
    {
        store.runtime().lockDown();
        auto const locked = store.drop(value);
        REQUIRE_FALSE(locked.has_value());
        CHECK(locked.error().failure == ComponentCallFailure::LockedDown);
    }
}

TEST_CASE("ComponentStore.badArgumentNotAborted")
{
    // Arguments not of the function are refused before anything runs, and the store stays live.
    auto harness = ApiHarness {};
    auto const component = harness.load(liftedIdentity().build());
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, Linker { ComponentConfig {} }));
    auto const id = instance.function("id");
    REQUIRE(id.has_value());
    auto const refusedWith = [&](Values const& args, ValueErrorCode code) {
        auto const error = failureOf(instance.call(*id, args));
        CHECK(error.failure == ComponentCallFailure::ArgumentMismatch);
        auto const* value = std::get_if<isoline::component::canonical::ValueError>(&error.detail);
        REQUIRE(value != nullptr);
        CHECK(value->code == code);
        CHECK_FALSE(store.isLockedDown());
    };
    auto const build = [&](isoline::component::types::TypeArena const& arena,
                           std::vector<isoline::component::types::TypeId> const& roots,
                           std::vector<uint64_t> const& values) {
        auto builder = ValueBuilder { arena, roots, store.runtime().liveBytes() };
        for (auto const value: values)
            builder.scalar(value);
        auto built = builder.finish();
        REQUIRE(built.has_value());
        return std::move(*built);
    };
    auto const& types = component.types();
    refusedWith(build(types, { types.primitive(ValueKind::U64) }, { 5 }), ValueErrorCode::TypeMismatch);
    refusedWith(build(types, {}, {}), ValueErrorCode::Unbalanced);
    auto linker = Linker { ComponentConfig {} };
    refusedWith(build(linker.types(), { primitive(linker, ValueKind::U32) }, { 5 }),
                ValueErrorCode::TypeMismatch);
    CHECK(u32Of(callWith(instance, "id", { 5 })) == 5);
}

TEST_CASE("ComponentStore.handlesCheckedBeforeEntering")
{
    // A handle of another resource type than its parameter's is refused before lowering would trap.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const resource =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const other =
        builder.import("q", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const own =
        builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = resource } } });
    auto const core = builder.coreInstantiate(
        builder.coreModule(coreModule(R"((module (func (export "take") (param i32))))")), {});
    std::ignore = builder.exportItem(
        "take",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(core, "take", CoreSort::Func),
                          {},
                          functionType(builder, { TypeIndex { .index = own } }, std::nullopt)));
    std::ignore = other;
    auto const component = harness.load(builder.build());
    auto const r = HostResourceType { "r", nullptr };
    auto const q = HostResourceType { "q", nullptr };
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineResource("r", r));
    require(linker.defineResource("q", q));
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, linker));
    auto const take = instance.function("take");
    REQUIRE(take.has_value());
    auto const callWithHandle = [&](HostResourceType const& type) {
        auto handle = store.hostResource(type, 3, Ownership::Own);
        REQUIRE(handle.has_value());
        auto arguments = instance.argumentsFor(*take);
        arguments.resource(*handle);
        auto built = arguments.finish();
        REQUIRE(built.has_value());
        return instance.call(*take, *built);
    };
    CHECK(failureOf(callWithHandle(q)).failure == ComponentCallFailure::ArgumentMismatch);
    CHECK_FALSE(store.isLockedDown());
    CHECK(callWithHandle(r).has_value());
}

TEST_CASE("ComponentStore.lockedDownRefusesInstantiate")
{
    auto harness = ApiHarness {};
    auto const component = harness.load(liftedIdentity().build());
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, Linker { ComponentConfig {} }));
    store.interrupter()->interrupt();
    auto const interrupted = failureOf(callWith(instance, "id", { 1 }));
    CHECK(interrupted.failure == ComponentCallFailure::Interrupted);
    CHECK(store.isLockedDown());
    auto const refused = store.instantiate(component, Linker { ComponentConfig {} });
    REQUIRE_FALSE(refused.has_value());
    auto const* failure = std::get_if<isoline::component::runtime::InstantiationFailure>(&refused.error());
    REQUIRE(failure != nullptr);
    CHECK(failure->kind == isoline::component::runtime::InstantiationFailureKind::LockedDown);
    CHECK(failureOf(callWith(instance, "id", { 1 })).failure == ComponentCallFailure::LockedDown);
}

TEST_CASE("ComponentStore.engineMismatchRefused")
{
    auto harness = ApiHarness {};
    auto otherEngine = isoline::api::Engine { isoline::api::EngineConfig {
        .mode = isoline::api::ExecutionMode::Deterministic } };
    auto loader = isoline::component::api::EngineCoreModuleLoader { otherEngine };
    auto validator = isoline::component::api::EngineCoreModuleValidator { otherEngine };
    auto foreign = isoline::component::api::Component::load(
        otherEngine, liftedIdentity().build(), {}, loader, validator);
    REQUIRE(foreign.has_value());
    auto store = harness.store();
    auto const refused = store.instantiate(*foreign, Linker { ComponentConfig {} });
    REQUIRE_FALSE(refused.has_value());
    CHECK(std::holds_alternative<isoline::api::EngineMismatch>(refused.error()));
    // A module of another engine, defined for a module import, is refused before anything runs too.
    auto const module = otherEngine.load(isoline::component::binary::testing::identityModule());
    REQUIRE(module.has_value());
    auto builder = ComponentBuilder {};
    std::ignore = builder.import(
        "m",
        ExternDesc {
            .sort = Sort::CoreModule,
            .index = builder.coreType(isoline::component::binary::CoreTypeDefinition {
                .type = isoline::component::binary::CoreModuleTypeDefinition { .declarations = {} } }) });
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineModule("m", *module));
    auto const user = harness.load(builder.build());
    auto const mismatched = store.instantiate(user, linker);
    REQUIRE_FALSE(mismatched.has_value());
    CHECK(std::holds_alternative<isoline::api::EngineMismatch>(mismatched.error()));
    CHECK_FALSE(store.isLockedDown());
    auto ownLinker = Linker { ComponentConfig {} };
    auto const ownModule = harness.engine.load(isoline::component::binary::testing::identityModule());
    REQUIRE(ownModule.has_value());
    require(ownLinker.defineModule("m", *ownModule));
    CHECK(store.instantiate(user, ownLinker).has_value());
}

TEST_CASE("ComponentStore.componentImportIsTyped")
{
    // A component defined for a component-typed import is matched by subtyping, then instantiated.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto scope = isoline::component::binary::testing::TypeScopeBuilder {};
    std::ignore = scope.exportItem(
        "id",
        ExternDesc { .sort = Sort::Func,
                     .index = scope.type(TypeDefinition {
                         .type = FunctionTypeDefinition {
                             .asyncness = Asyncness::Sync,
                             .params = { LabeledType { .label = "x", .type = PrimitiveValueType::U32 } },
                             .result = PrimitiveValueType::U32 } }) });
    auto const c = builder.import(
        "c", ExternDesc { .sort = Sort::Component, .index = builder.componentType(std::move(scope)) });
    std::ignore = builder.exportItem(
        "id", Sort::Func, builder.aliasExport(builder.instantiate(c, {}), "id", Sort::Func));
    auto const user = harness.load(builder.build());
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineComponent("c", harness.load(liftedIdentity().build())));
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(user, linker));
    CHECK(u32Of(callWith(instance, "id", { 6 })) == 6);

    auto mistyped = Linker { ComponentConfig {} };
    require(mistyped.defineComponent(
        "c", harness.load(isoline::component::binary::testing::ComponentBuilder {}.build())));
    auto const refused = store.instantiate(user, mistyped);
    REQUIRE_FALSE(refused.has_value());
    CHECK(std::holds_alternative<isoline::component::api::ImportTypeMismatch>(refused.error()));
}

TEST_CASE("ComponentStore.movedStoreKeepsItsInstances")
{
    // Moving the store re-points its state: instances made before still call, and a host function's
    // context names the store where it is now.
    auto harness = ApiHarness {};
    auto seen = static_cast<ComponentStore const*>(nullptr);
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineFunction(
        "f",
        functionSpec({}, primitive(linker, ValueKind::U32)),
        hostFunction([&](HostComponentContext& context, Values const&, ValueBuilder& results) -> HostResult {
            seen = &context.store();
            results.scalar(3);
            return {};
        })));
    auto first = harness.store();
    auto instance = instanceOf(first.instantiate(harness.load(callsHostF()), linker));
    auto moved = std::move(first);
    CHECK(u32Of(callWith(instance, "run")) == 3);
    CHECK(seen == &moved);
    CHECK(instance.belongsTo(moved));
    auto assigned = harness.store();
    assigned = std::move(moved);
    CHECK(u32Of(callWith(instance, "run")) == 3);
    CHECK(seen == &assigned);
}

TEST_CASE("ComponentStore.functionPaths")
{
    // A path names an export of a nested instance; anything but a lifted function is none.
    auto harness = ApiHarness {};
    auto builder = liftedIdentity();
    auto const bundle = builder.inlineInstance(
        { { "id", isoline::component::binary::SortIndex { .sort = Sort::Func, .index = 0 } } });
    std::ignore = builder.exportItem("api", Sort::Instance, bundle);
    auto const component = harness.load(builder.build());
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, Linker { ComponentConfig {} }));
    auto const nested = instance.function("api#id");
    REQUIRE(nested.has_value());
    CHECK(nested->type().params.size() == 1);
    CHECK(nested == instance.function("id"));
    CHECK_FALSE(instance.function("api").has_value());
    CHECK_FALSE(instance.function("id#x").has_value());
    CHECK_FALSE(instance.function("api#missing").has_value());
    CHECK_FALSE(instance.function("missing").has_value());
}
