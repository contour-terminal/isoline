// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <iostream>
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
#include <isoline/component/text/Forms.hpp>
#include <isoline/runtime/testing/ReportedContractChecks.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

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
using isoline::component::api::StoreEffectTexts;
using isoline::component::api::testing::ApiHarness;
using isoline::component::api::testing::callWith;
using isoline::component::api::testing::functionSpec;
using isoline::component::api::testing::hostFunction;
using isoline::component::api::testing::instanceOf;
using isoline::component::api::testing::lookup;
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
using isoline::runtime::testing::ReportedContractChecks;

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
                             || row.failure == ComponentCallFailure::HostFailure
                             || row.failure == ComponentCallFailure::ResourceExhausted;
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
    for (auto const& [kind, text]:
         { std::pair { isoline::api::HostFailureKind::ResultMismatch,
                       "host function failed: results not of the function's type 0" },
           std::pair { isoline::api::HostFailureKind::ArgumentMismatch,
                       "host function failed: arguments not of their types 0" } })
        CHECK(isoline::component::api::callError(
                  ComponentCallFailure::HostFailure,
                  isoline::api::HostFailure { .kind = kind, .code = 0, .mismatch = std::nullopt })
                  .message()
              == text);
    // Copies of a host resource type are one type; two constructed apart are two.
    auto const type = HostResourceType { "r", nullptr };
    auto copies = std::vector<HostResourceType> { type };
    CHECK(copies.front() == type);
    CHECK_FALSE(HostResourceType { "r", nullptr } == type);
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
        auto const keep = lookup(instance, "keep");
        REQUIRE(keep.has_value());
        auto lent = store.hostResource(type, 9, Ownership::Borrow);
        REQUIRE(lent.has_value());
        auto arguments = instance.argumentsFor(*keep);
        REQUIRE(arguments.has_value());
        arguments->resource(*lent);
        auto const built = arguments->finish();
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
    auto const id = lookup(instance, "id");
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
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
          (func (export "take") (param i32))
          (func (export "pair") (param i32 i32))))")),
                                              {});
    std::ignore = builder.exportItem(
        "take",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(core, "take", CoreSort::Func),
                          {},
                          functionType(builder, { TypeIndex { .index = own } }, std::nullopt)));
    auto const pair = builder.type(
        TypeDefinition { .type = DefinedValueType { TupleType {
                             .elements = { TypeIndex { .index = own }, PrimitiveValueType::U32 } } } });
    std::ignore = builder.exportItem(
        "pair",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(core, "pair", CoreSort::Func),
                          {},
                          functionType(builder, { TypeIndex { .index = pair } }, std::nullopt)));
    std::ignore = other;
    auto const component = harness.load(builder.build());
    auto const r = HostResourceType { "r", nullptr };
    auto const q = HostResourceType { "q", nullptr };
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineResource("r", r));
    require(linker.defineResource("q", q));
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, linker));
    auto const take = lookup(instance, "take");
    REQUIRE(take.has_value());
    auto const callWithHandle = [&](HostResourceType const& type) {
        auto handle = store.hostResource(type, 3, Ownership::Own);
        REQUIRE(handle.has_value());
        auto arguments = instance.argumentsFor(*take);
        REQUIRE(arguments.has_value());
        arguments->resource(*handle);
        auto built = arguments->finish();
        REQUIRE(built.has_value());
        return instance.call(*take, *built);
    };
    CHECK(failureOf(callWithHandle(q)).failure == ComponentCallFailure::ArgumentMismatch);
    CHECK_FALSE(store.isLockedDown());
    CHECK(callWithHandle(r).has_value());

    // A handle inside a compound is checked too.
    auto const paired = lookup(instance, "pair");
    REQUIRE(paired.has_value());
    auto const callWithPair = [&](HostResourceType const& type) {
        auto handle = store.hostResource(type, 4, Ownership::Own);
        REQUIRE(handle.has_value());
        auto arguments = instance.argumentsFor(*paired);
        REQUIRE(arguments.has_value());
        arguments->begin(2).resource(*handle).scalar(1).end();
        auto built = arguments->finish();
        REQUIRE(built.has_value());
        return instance.call(*paired, *built);
    };
    auto const mismatched = failureOf(callWithPair(q));
    CHECK(mismatched.failure == ComponentCallFailure::ArgumentMismatch);
    CHECK_FALSE(store.isLockedDown());
    CHECK(callWithPair(r).has_value());
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
    auto const nested = lookup(instance, "api#id");
    REQUIRE(nested.has_value());
    CHECK(nested->type()->params.size() == 1);
    CHECK(nested == lookup(instance, "id"));
    CHECK_FALSE(lookup(instance, "api").has_value());
    CHECK_FALSE(lookup(instance, "id#x").has_value());
    CHECK_FALSE(lookup(instance, "api#missing").has_value());
    CHECK_FALSE(lookup(instance, "missing").has_value());
}

namespace
{

/// A component importing the resource `r`, whose `take(p0: own<r>)` drops the own it is given.
[[nodiscard]] std::vector<std::byte> takesAndDrops()
{
    auto builder = ComponentBuilder {};
    auto const r =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const own = builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = r } } });
    auto const builtins = builder.coreInlineExports(
        { CoreInlineExport { .name = "drop",
                             .sort = CoreSort::Func,
                             .index = builder.canon(CanonDefinition { .builtin = CanonBuiltin::ResourceDrop,
                                                                      .immediates = { r },
                                                                      .options = {},
                                                                      .result = std::nullopt }) } });
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "r" "drop" (func $drop (param i32)))
      (func (export "take") (param i32) (call $drop (local.get 0)))
      (func (export "take2") (param i32 i32) (call $drop (local.get 0)) (call $drop (local.get 1)))
      (func (export "take-and-trap") (param i32) unreachable)))")),
                                              { { "r", builtins } });
    std::ignore = builder.exportItem(
        "take",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(core, "take", CoreSort::Func),
                          {},
                          functionType(builder, { TypeIndex { .index = own } }, std::nullopt)));
    std::ignore = builder.exportItem(
        "take-and-trap",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(core, "take-and-trap", CoreSort::Func),
                          {},
                          functionType(builder, { TypeIndex { .index = own } }, std::nullopt)));
    std::ignore = builder.exportItem(
        "take2",
        Sort::Func,
        builder.canonLift(
            builder.aliasCoreExport(core, "take2", CoreSort::Func),
            {},
            functionType(builder, { TypeIndex { .index = own }, TypeIndex { .index = own } }, std::nullopt)));
    return builder.build();
}

/// Calls @p function of @p instance with the single handle @p handle.
[[nodiscard]] CallResult callWithHandle(isoline::component::api::ComponentInstance& instance,
                                        std::string_view function,
                                        ResourceValue handle)
{
    auto const found = lookup(instance, function);
    REQUIRE(found.has_value());
    auto arguments = instance.argumentsFor(*found);
    REQUIRE(arguments.has_value());
    arguments->resource(handle);
    auto built = arguments->finish();
    REQUIRE(built.has_value());
    return instance.call(*found, *built);
}

/// The store @p store held, moved out of it: @p store is left empty.
[[nodiscard]] ComponentStore takeFrom(ComponentStore& store)
{
    return std::move(store);
}

/// The embedding guide, whose generated block the call failure table must match.
constexpr auto EmbeddingGuide = std::string_view { ISOLINE_SOURCE_DIR "/docs/embedding.md" };

} // namespace

TEST_CASE("ComponentStore.storeDeletedRefused")
{
    // An instance and a function outliving their store refuse instead of reaching it (ASan: no read).
    auto harness = ApiHarness {};
    auto const component = harness.load(liftedIdentity().build());
    auto store = std::make_unique<ComponentStore>(harness.store());
    auto instance = instanceOf(store->instantiate(component, Linker { ComponentConfig {} }));
    auto const id = lookup(instance, "id");
    REQUIRE(id.has_value());
    auto arguments = instance.argumentsFor(*id);
    REQUIRE(arguments.has_value());
    arguments->scalar(1);
    auto const built = arguments->finish();
    REQUIRE(built.has_value());
    REQUIRE(id->type() != nullptr);
    store.reset();
    CHECK(failureOf(instance.call(*id, *built)).failure == ComponentCallFailure::StoreDeleted);
    auto const found = instance.function("id");
    REQUIRE_FALSE(found.has_value());
    CHECK(found.error().failure == ComponentCallFailure::StoreDeleted);
    auto const builder = instance.argumentsFor(*id);
    REQUIRE_FALSE(builder.has_value());
    CHECK(builder.error().failure == ComponentCallFailure::StoreDeleted);
    CHECK(id->type() == nullptr);

    // A moved-from store refuses a drop, and is not busy.
    auto first = harness.store();
    auto const second = takeFrom(first);
    CHECK_FALSE(first.isBusy());
    auto const dropped = first.drop(ResourceValue {});
    REQUIRE_FALSE(dropped.has_value());
    CHECK(dropped.error().failure == ComponentCallFailure::StoreDeleted);
    CHECK_FALSE(second.isBusy());
}

TEST_CASE("HostResources.hostOwnsTrackedOnce")
{
    // The store tracks the owns the host holds: each is dropped or passed on once, and its resource is
    // destroyed once, by whichever took it.
    auto harness = ApiHarness {};
    auto const component = harness.load(takesAndDrops());
    auto const destructor = std::make_shared<RecordingDestructor>();
    auto const type = HostResourceType { "r", destructor };
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineResource("r", type));
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, linker));
    auto const own = store.hostResource(type, 5, Ownership::Own);
    REQUIRE(own.has_value());
    auto const refused = [&](std::expected<void, ComponentCallError> const& outcome) {
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().failure == ComponentCallFailure::ArgumentMismatch);
        CHECK_FALSE(store.isLockedDown());
    };

    SECTION("dropped twice")
    {
        CHECK(store.drop(*own).has_value());
        refused(store.drop(*own));
    }
    SECTION("passed to two calls")
    {
        CHECK(callWithHandle(instance, "take", *own).has_value());
        auto const again = callWithHandle(instance, "take", *own);
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().failure == ComponentCallFailure::ArgumentMismatch);
        CHECK_FALSE(store.isLockedDown());
    }
    SECTION("passed twice in one call")
    {
        auto const found = lookup(instance, "take2");
        REQUIRE(found.has_value());
        auto arguments = instance.argumentsFor(*found);
        REQUIRE(arguments.has_value());
        arguments->resource(*own).resource(*own);
        auto const built = arguments->finish();
        REQUIRE(built.has_value());
        auto const twice = instance.call(*found, *built);
        REQUIRE_FALSE(twice.has_value());
        CHECK(twice.error().failure == ComponentCallFailure::ArgumentMismatch);
        CHECK(destructor->destroyed.empty());
        CHECK(store.drop(*own).has_value());
    }
    SECTION("passed on, then dropped")
    {
        CHECK(callWithHandle(instance, "take", *own).has_value());
        refused(store.drop(*own));
    }
    SECTION("held twice, dropped twice")
    {
        auto const twin = store.hostResource(type, 5, Ownership::Own);
        REQUIRE(twin.has_value());
        CHECK(store.drop(*own).has_value());
        CHECK(store.drop(*twin).has_value());
        refused(store.drop(*own));
        CHECK(destructor->destroyed == std::vector<uint32_t> { 5, 5 });
        return;
    }
    CHECK(destructor->destroyed == std::vector<uint32_t> { 5 });
}

TEST_CASE("HostResources.returnedOwnMustBeHeld")
{
    // A host function returning an own it does not hold (one it gave away already) fails, before the
    // guest's table takes it twice.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const r =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const own = builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = r } } });
    auto const make = builder.import(
        "make",
        ExternDesc { .sort = Sort::Func, .index = functionType(builder, {}, TypeIndex { .index = own }) });
    auto const lowered = builder.coreInlineExports({ CoreInlineExport {
        .name = "make", .sort = CoreSort::Func, .index = builder.canonLower(make, {}) } });
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "make" (func $make (result i32)))
      (func (export "run") (drop (call $make)))))")),
                                              { { "h", lowered } });
    std::ignore = builder.exportItem("run",
                                     Sort::Func,
                                     builder.canonLift(builder.aliasCoreExport(core, "run", CoreSort::Func),
                                                       {},
                                                       functionType(builder, {}, std::nullopt)));
    auto const component = harness.load(builder.build());
    auto const destructor = std::make_shared<RecordingDestructor>();
    auto const type = HostResourceType { "r", destructor };
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineResource("r", type));
    auto made = std::optional<ResourceValue> {};
    auto ownType = linker.ownType(type);
    REQUIRE(ownType.has_value());
    require(linker.defineFunction(
        "make",
        functionSpec({}, *ownType),
        hostFunction([&](HostComponentContext& context, Values const&, ValueBuilder& results) -> HostResult {
            if (!made)
            {
                auto fresh = context.newOwn(type, 3);
                if (!fresh)
                    return std::unexpected { embedderFailure() };
                made = *fresh;
            }
            results.resource(*made);
            return {};
        })));
    {
        auto store = harness.store();
        auto instance = instanceOf(store.instantiate(component, linker));
        CHECK(callWith(instance, "run").has_value());
        auto const error = failureOf(callWith(instance, "run"));
        CHECK(error.failure == ComponentCallFailure::HostFailure);
        auto const* host = std::get_if<isoline::api::HostFailure>(&error.detail);
        REQUIRE(host != nullptr);
        CHECK(host->kind == isoline::api::HostFailureKind::ArgumentMismatch);
        CHECK(store.isLockedDown());
    }
    // The one own the guest's table took is destroyed once, at the store's end.
    CHECK(destructor->destroyed == std::vector<uint32_t> { 3 });
}

TEST_CASE("HostResources.ownArgumentMovesToHost")
{
    // give(p0: own<r>) takes the own make() returned: the guest's handle is gone (dropping it traps
    // UnknownHandle), and the host's drop destroys it once; the store's end destroys nothing more.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const r =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const own = builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = r } } });
    auto const make = builder.import(
        "make",
        ExternDesc { .sort = Sort::Func, .index = functionType(builder, {}, TypeIndex { .index = own }) });
    auto const give = builder.import(
        "give",
        ExternDesc { .sort = Sort::Func,
                     .index = functionType(builder, { TypeIndex { .index = own } }, std::nullopt) });
    auto const lowered = builder.coreInlineExports({
        CoreInlineExport { .name = "make", .sort = CoreSort::Func, .index = builder.canonLower(make, {}) },
        CoreInlineExport { .name = "give", .sort = CoreSort::Func, .index = builder.canonLower(give, {}) },
        CoreInlineExport { .name = "drop",
                           .sort = CoreSort::Func,
                           .index = builder.canon(CanonDefinition { .builtin = CanonBuiltin::ResourceDrop,
                                                                    .immediates = { r },
                                                                    .options = {},
                                                                    .result = std::nullopt }) },
    });
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "make" (func $make (result i32)))
      (import "h" "give" (func $give (param i32)))
      (import "h" "drop" (func $drop (param i32)))
      (global $handle (mut i32) (i32.const 0))
      (func (export "run") (global.set $handle (call $make)) (call $give (global.get $handle)))
      (func (export "again") (call $drop (global.get $handle)))))")),
                                              { { "h", lowered } });
    for (auto const* name: { "run", "again" })
        std::ignore =
            builder.exportItem(name,
                               Sort::Func,
                               builder.canonLift(builder.aliasCoreExport(core, name, CoreSort::Func),
                                                 {},
                                                 functionType(builder, {}, std::nullopt)));
    auto const component = harness.load(builder.build());
    auto const destructor = std::make_shared<RecordingDestructor>();
    auto const type = HostResourceType { "r", destructor };
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineResource("r", type));
    auto ownType = linker.ownType(type);
    REQUIRE(ownType.has_value());
    require(linker.defineFunction(
        "make",
        functionSpec({}, *ownType),
        hostFunction([&](HostComponentContext& context, Values const&, ValueBuilder& results) -> HostResult {
            auto fresh = context.newOwn(type, 5);
            if (!fresh)
                return std::unexpected { embedderFailure() };
            results.resource(*fresh);
            return {};
        })));
    require(linker.defineFunction(
        "give",
        functionSpec({ *ownType }, std::nullopt),
        hostFunction([](HostComponentContext& context, Values const& args, ValueBuilder&) -> HostResult {
            if (!context.drop(args.root(0).resource()))
                return std::unexpected { embedderFailure() };
            return {};
        })));
    {
        auto store = harness.store();
        auto instance = instanceOf(store.instantiate(component, linker));
        CHECK(callWith(instance, "run").has_value());
        CHECK(destructor->destroyed == std::vector<uint32_t> { 5 });
        CHECK(trapOf(callWith(instance, "again")) == TrapCode::UnknownHandle);
    }
    CHECK(destructor->destroyed == std::vector<uint32_t> { 5 });
}

TEST_CASE("ComponentStore.dropWhileInstantiatingRefused")
{
    // A host function a start function calls cannot drop: the store is instantiating.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const f = builder.import(
        "f", ExternDesc { .sort = Sort::Func, .index = functionType(builder, {}, std::nullopt) });
    auto const lowered = builder.coreInlineExports(
        { CoreInlineExport { .name = "f", .sort = CoreSort::Func, .index = builder.canonLower(f, {}) } });
    std::ignore = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "f" (func $f))
      (func $start (call $f))
      (start $start)))")),
                                          { { "h", lowered } });
    auto failure = std::optional<ComponentCallFailure> {};
    auto busy = std::optional<bool> {};
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineFunction(
        "f",
        functionSpec({}, std::nullopt),
        hostFunction([&](HostComponentContext& context, Values const&, ValueBuilder&) -> HostResult {
            busy = context.store().isBusy();
            auto const dropped = context.drop(ResourceValue {});
            if (!dropped)
                failure = dropped.error().failure;
            return {};
        })));
    auto store = harness.store();
    CHECK(store.instantiate(harness.load(builder.build()), linker).has_value());
    CHECK(failure == ComponentCallFailure::Instantiating);
    CHECK(busy == true);
    CHECK_FALSE(store.isBusy());
}

TEST_CASE("ComponentStore.movedWhileBusyIsAContractViolation", "[.contract]")
{
    // Moving the store from a host function while its call runs ends the process (Require). Hidden:
    // CTest runs it alone and expects the report (isoline_component_contract_store_moved_while_busy).
    ReportedContractChecks const checks;
    auto harness = ApiHarness {};
    auto store = std::optional<ComponentStore> {};
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineFunction(
        "f",
        functionSpec({}, primitive(linker, ValueKind::U32)),
        hostFunction([&](HostComponentContext&, Values const&, ValueBuilder& results) -> HostResult {
            auto moved = std::move(*store);
            results.scalar(1);
            return {};
        })));
    store.emplace(harness.store());
    auto instance = instanceOf(store->instantiate(harness.load(callsHostF()), linker));
    std::ignore = callWith(instance, "run");
    // The sentinel isoline_add_contract_test() fails the run on, where the call went through.
    std::puts("contract-test: the check let the call through");
    FAIL("the store was moved mid-call without a contract violation");
}

TEST_CASE("CallError.docsMatchTable")
{
    // The embedding guide's block is generated from the rows: it fails, printing the block as it should
    // read, when the two part.
    auto expected = std::vector<std::string> {};
    for (auto const& row: ComponentCallFailureTable)
        expected.push_back(std::format("- `{}` (`{}`): {}; {}.",
                                       row.name,
                                       row.alternative,
                                       StoreEffectTexts[static_cast<std::size_t>(row.effect)],
                                       row.text));
    auto lines = std::vector<std::string> {};
    auto file = std::ifstream { std::string { EmbeddingGuide } };
    REQUIRE(file.is_open());
    auto line = std::string {};
    auto inside = false;
    while (std::getline(file, line))
    {
        if (line.ends_with('\r'))
            line.pop_back();
        if (line == "<!-- component-call-failures:end -->")
            break;
        if (inside)
            lines.push_back(line);
        inside = inside || line == "<!-- component-call-failures:begin -->";
    }
    auto joined = std::string {};
    for (auto const& entry: expected)
        joined += entry + "\n";
    INFO("the component-call-failures block of " << EmbeddingGuide << " should read:\n" << joined);
    CHECK(lines == expected);
}

namespace
{
/// Assigns @p from to @p to, which may be one store.
void assignFrom(ComponentStore& to, ComponentStore& from)
{
    to = std::move(from);
}
} // namespace

TEST_CASE("ComponentStore.selfAssignmentKeepsTheStore")
{
    auto harness = ApiHarness {};
    auto store = harness.store();
    auto instance =
        instanceOf(store.instantiate(harness.load(liftedIdentity().build()), Linker { ComponentConfig {} }));
    assignFrom(store, store);
    CHECK(instance.belongsTo(store));
    CHECK(u32Of(callWith(instance, "id", { 2 })) == 2);
}

TEST_CASE("ComponentStore.reexportedHostFunctionIsCallable")
{
    // An export of an imported host function is the host's callee: called from the host, it runs as a
    // guest's call of it does, its arguments and results checked and built in the exporting component's
    // types.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const f = builder.import(
        "f",
        ExternDesc { .sort = Sort::Func,
                     .index = functionType(builder, { PrimitiveValueType::U32 }, PrimitiveValueType::U32) });
    std::ignore = builder.exportItem("g", Sort::Func, f);
    auto const core = builder.coreInstantiate(
        builder.coreModule(coreModule(R"((module (func (export "h") (result i32) (i32.const 3))))")), {});
    std::ignore = builder.exportItem("h",
                                     Sort::Func,
                                     builder.canonLift(builder.aliasCoreExport(core, "h", CoreSort::Func),
                                                       {},
                                                       functionType(builder, {}, PrimitiveValueType::U32)));
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineFunction(
        "f",
        functionSpec({ primitive(linker, ValueKind::U32) }, primitive(linker, ValueKind::U32)),
        hostFunction([](HostComponentContext&, Values const& args, ValueBuilder& results) -> HostResult {
            results.scalar(args.root(0).scalar<uint32_t>() + 1);
            return {};
        })));
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(harness.load(builder.build()), linker));
    auto const g = lookup(instance, "g");
    REQUIRE(g.has_value());
    REQUIRE(g->type() != nullptr);
    CHECK(g->type()->params.size() == 1);
    CHECK(*g == *lookup(instance, "g"));
    CHECK_FALSE(*g == *lookup(instance, "h")); // a lifted function is another
    CHECK(u32Of(callWith(instance, "g", { 41 })) == 42);
    // A function of another store is refused.
    auto arguments = instance.argumentsFor(*g);
    REQUIRE(arguments.has_value());
    arguments->scalar(1);
    auto const one = arguments->finish();
    REQUIRE(one.has_value());
    auto otherStore = harness.store();
    auto other = instanceOf(otherStore.instantiate(harness.load(builder.build()), linker));
    CHECK(u32Of(instance.call(*g, *one)) == 2);
    CHECK(failureOf(other.call(*g, *one)).failure == ComponentCallFailure::ForeignFunction);
    CHECK_FALSE(store.isLockedDown());
}

TEST_CASE("HostResources.guestResourceWithoutDestructor")
{
    // The host's drop of an own of a guest resource without a destructor runs nothing, and succeeds once.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const r = builder.resource();
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
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "r" "new" (func $new (param i32) (result i32)))
      (func (export "make") (result i32) (call $new (i32.const 4)))))")),
                                              { { "r", builtins } });
    std::ignore =
        builder.exportItem("make",
                           Sort::Func,
                           builder.canonLift(builder.aliasCoreExport(core, "make", CoreSort::Func),
                                             {},
                                             functionType(builder, {}, TypeIndex { .index = own })));
    auto store = harness.store();
    auto instance =
        instanceOf(store.instantiate(harness.load(builder.build()), Linker { ComponentConfig {} }));
    auto const made = callWith(instance, "make");
    REQUIRE(made.has_value());
    auto const value = made->root(0).resource();
    CHECK(store.drop(value).has_value());
    auto const again = store.drop(value);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().failure == ComponentCallFailure::ArgumentMismatch);
}

namespace
{
/// A component importing `r` and `make: func() -> own<r>`, whose `run` drops what make returns.
[[nodiscard]] std::vector<std::byte> callsMake()
{
    auto builder = ComponentBuilder {};
    auto const r =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const own = builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = r } } });
    auto const make = builder.import(
        "make",
        ExternDesc { .sort = Sort::Func, .index = functionType(builder, {}, TypeIndex { .index = own }) });
    auto const lowered = builder.coreInlineExports({ CoreInlineExport {
        .name = "make", .sort = CoreSort::Func, .index = builder.canonLower(make, {}) } });
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "make" (func $make (result i32)))
      (func (export "run") (drop (call $make)))))")),
                                              { { "h", lowered } });
    std::ignore = builder.exportItem("run",
                                     Sort::Func,
                                     builder.canonLift(builder.aliasCoreExport(core, "run", CoreSort::Func),
                                                       {},
                                                       functionType(builder, {}, std::nullopt)));
    return builder.build();
}
} // namespace

TEST_CASE("HostResources.failedLoweringKeepsTheOwn")
{
    // A lowering that traps passes no own: the host still holds each, its drop destroys it once, and the
    // store's end destroys nothing more.
    auto harness = ApiHarness {};
    auto const destructor = std::make_shared<RecordingDestructor>();
    auto const type = HostResourceType { "r", destructor };
    auto limits = isoline::component::ComponentLimits {};

    SECTION("an argument past the handle cap")
    {
        limits.maxHandles = 0;
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineResource("r", type));
        {
            auto store = harness.store(limits);
            auto instance = instanceOf(store.instantiate(harness.load(takesAndDrops()), linker));
            auto const own = store.hostResource(type, 5, Ownership::Own);
            REQUIRE(own.has_value());
            CHECK(trapOf(callWithHandle(instance, "take", *own)) == TrapCode::Handles);
            CHECK(store.isLockedDown());
            CHECK(store.drop(*own).has_value());
            CHECK(destructor->destroyed == std::vector<uint32_t> { 5 });
        }
        CHECK(destructor->destroyed == std::vector<uint32_t> { 5 });
    }
    SECTION("a callee that traps once it holds the own")
    {
        // The own was passed: the guest's table holds it, so the host does not, and the store's end
        // destroys it once.
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineResource("r", type));
        {
            auto store = harness.store(limits);
            auto instance = instanceOf(store.instantiate(harness.load(takesAndDrops()), linker));
            auto const own = store.hostResource(type, 9, Ownership::Own);
            REQUIRE(own.has_value());
            CHECK(trapOf(callWithHandle(instance, "take-and-trap", *own)) == TrapCode::Core);
            auto const dropped = store.drop(*own);
            REQUIRE_FALSE(dropped.has_value());
            CHECK(dropped.error().failure == ComponentCallFailure::ArgumentMismatch);
            CHECK(destructor->destroyed.empty());
        }
        CHECK(destructor->destroyed == std::vector<uint32_t> { 9 });
    }
    SECTION("the second of two arguments past the handle cap")
    {
        limits.maxHandles = 1;
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineResource("r", type));
        {
            auto store = harness.store(limits);
            auto instance = instanceOf(store.instantiate(harness.load(takesAndDrops()), linker));
            auto const first = store.hostResource(type, 6, Ownership::Own);
            auto const second = store.hostResource(type, 7, Ownership::Own);
            REQUIRE(first.has_value());
            REQUIRE(second.has_value());
            auto const found = lookup(instance, "take2");
            REQUIRE(found.has_value());
            auto arguments = instance.argumentsFor(*found);
            REQUIRE(arguments.has_value());
            arguments->resource(*first).resource(*second);
            auto const built = arguments->finish();
            REQUIRE(built.has_value());
            CHECK(trapOf(instance.call(*found, *built)) == TrapCode::Handles);
            CHECK(store.drop(*first).has_value());
            CHECK(store.drop(*second).has_value());
            CHECK(destructor->destroyed == std::vector<uint32_t> { 6, 7 });
        }
        CHECK(destructor->destroyed == std::vector<uint32_t> { 6, 7 });
    }
    SECTION("the second of two results past the handle cap")
    {
        limits.maxHandles = 1;
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineResource("r", type));
        auto ownType = linker.ownType(type);
        REQUIRE(ownType.has_value());
        auto pairType = linker.types().intern(isoline::component::types::DefinedType {
            .kind = ValueKind::Tuple, .children = { *ownType, *ownType } });
        REQUIRE(pairType.has_value());
        auto made = std::vector<ResourceValue> {};
        require(linker.defineFunction(
            "make2",
            functionSpec({}, *pairType),
            hostFunction(
                [&](HostComponentContext& context, Values const&, ValueBuilder& results) -> HostResult {
                    results.begin(2);
                    for (auto const rep: { 10U, 11U })
                    {
                        auto fresh = context.newOwn(type, rep);
                        if (!fresh)
                            return std::unexpected { embedderFailure() };
                        made.push_back(*fresh);
                        results.resource(*fresh);
                    }
                    results.end();
                    return {};
                })));
        auto builder = ComponentBuilder {};
        auto const r = builder.import(
            "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
        auto const own =
            builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = r } } });
        auto const pair = builder.type(TypeDefinition {
            .type = DefinedValueType {
                TupleType { .elements = { TypeIndex { .index = own }, TypeIndex { .index = own } } } } });
        auto const make2 =
            builder.import("make2",
                           ExternDesc { .sort = Sort::Func,
                                        .index = functionType(builder, {}, TypeIndex { .index = pair }) });
        auto const memory = builder.coreInstantiate(builder.coreModule(coreModule(MemoryModule)), {});
        auto const lowered = builder.coreInlineExports({ CoreInlineExport {
            .name = "make2",
            .sort = CoreSort::Func,
            .index =
                builder.canonLower(make2,
                                   { option(CanonOptionKind::Memory,
                                            builder.aliasCoreExport(memory, "mem", CoreSort::Memory)) }) } });
        auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
          (import "h" "make2" (func $make2 (param i32)))
          (func (export "run") (call $make2 (i32.const 64)))))")),
                                                  { { "h", lowered } });
        std::ignore =
            builder.exportItem("run",
                               Sort::Func,
                               builder.canonLift(builder.aliasCoreExport(core, "run", CoreSort::Func),
                                                 {},
                                                 functionType(builder, {}, std::nullopt)));
        {
            auto store = harness.store(limits);
            auto instance = instanceOf(store.instantiate(harness.load(builder.build()), linker));
            CHECK(trapOf(callWith(instance, "run")) == TrapCode::Handles);
            REQUIRE(made.size() == 2);
            for (auto const& held: made)
                CHECK(store.drop(held).has_value());
            CHECK(destructor->destroyed == std::vector<uint32_t> { 10, 11 });
        }
        CHECK(destructor->destroyed == std::vector<uint32_t> { 10, 11 });
    }
    SECTION("a host function's result past the handle cap")
    {
        limits.maxHandles = 0;
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineResource("r", type));
        auto ownType = linker.ownType(type);
        REQUIRE(ownType.has_value());
        auto made = std::optional<ResourceValue> {};
        require(linker.defineFunction(
            "make",
            functionSpec({}, *ownType),
            hostFunction(
                [&](HostComponentContext& context, Values const&, ValueBuilder& results) -> HostResult {
                    auto fresh = context.newOwn(type, 8);
                    if (!fresh)
                        return std::unexpected { embedderFailure() };
                    made = *fresh;
                    results.resource(*fresh);
                    return {};
                })));
        {
            auto store = harness.store(limits);
            auto instance = instanceOf(store.instantiate(harness.load(callsMake()), linker));
            CHECK(trapOf(callWith(instance, "run")) == TrapCode::Handles);
            REQUIRE(made.has_value());
            CHECK(store.drop(*made).has_value());
            CHECK(destructor->destroyed == std::vector<uint32_t> { 8 });
        }
        CHECK(destructor->destroyed == std::vector<uint32_t> { 8 });
    }
}

TEST_CASE("HostResources.guestToGuestOwnIsNoHosts")
{
    // An own B returns to A that cannot be lowered into A goes back to B, whose trap locked the store
    // down: the store's end destroys it, once, and the host, which never held it, cannot drop it.
    auto harness = ApiHarness {};
    auto const destructor = std::make_shared<RecordingDestructor>();
    auto const type = HostResourceType { "r", destructor };
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineResource("r", type));
    auto ownType = linker.ownType(type);
    REQUIRE(ownType.has_value());
    require(linker.defineFunction(
        "make",
        functionSpec({}, *ownType),
        hostFunction([&](HostComponentContext& context, Values const&, ValueBuilder& results) -> HostResult {
            auto fresh = context.newOwn(type, 20);
            if (!fresh)
                return std::unexpected { embedderFailure() };
            results.resource(*fresh);
            return {};
        })));
    auto limits = isoline::component::ComponentLimits {};
    limits.maxHandles = 1;
    {
        auto store = harness.store(limits);
        auto instance = instanceOf(
            store.instantiate(harness.load(isoline::component::runtime::testing::guestToGuest()), linker));
        CHECK(trapOf(callWith(instance, "run-give")) == TrapCode::Handles);
        auto const own = store.hostResource(type, 20, Ownership::Borrow);
        REQUIRE(own.has_value());
        auto const dropped =
            store.drop(ResourceValue { .type = own->type, .rep = 20, .ownership = Ownership::Own });
        REQUIRE_FALSE(dropped.has_value());
        CHECK(dropped.error().failure == ComponentCallFailure::ArgumentMismatch);
        CHECK(destructor->destroyed.empty());
    }
    CHECK(destructor->destroyed == std::vector<uint32_t> { 20 });
}

TEST_CASE("HostResources.failedResultLiftKeepsTheOwn")
{
    // give() -> tuple<own<r>, string> returns make's own and a string out of bounds: the lift to the host
    // fails after it took the own, which goes back to the guest, whose trap locks the store down. The
    // host never held it, and the store's end destroys it, once.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const r =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const own = builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = r } } });
    auto const make = builder.import(
        "make",
        ExternDesc { .sort = Sort::Func, .index = functionType(builder, {}, TypeIndex { .index = own }) });
    auto const pair = builder.type(
        TypeDefinition { .type = DefinedValueType { TupleType {
                             .elements = { TypeIndex { .index = own }, PrimitiveValueType::String } } } });
    auto const memory = builder.coreInstantiate(builder.coreModule(coreModule(MemoryModule)), {});
    auto const lowered = builder.coreInlineExports({
        CoreInlineExport { .name = "make", .sort = CoreSort::Func, .index = builder.canonLower(make, {}) },
        CoreInlineExport { .name = "mem",
                           .sort = CoreSort::Memory,
                           .index = builder.aliasCoreExport(memory, "mem", CoreSort::Memory) },
    });
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "make" (func $make (result i32)))
      (import "h" "mem" (memory 1))
      (func (export "give") (result i32)
        (i32.store (i32.const 64) (call $make))
        (i32.store (i32.const 68) (i32.const 0xFFFF0000))
        (i32.store (i32.const 72) (i32.const 16))
        (i32.const 64))))")),
                                              { { "h", lowered } });
    std::ignore = builder.exportItem(
        "give",
        Sort::Func,
        builder.canonLift(
            builder.aliasCoreExport(core, "give", CoreSort::Func),
            { option(CanonOptionKind::Memory, builder.aliasCoreExport(memory, "mem", CoreSort::Memory)) },
            functionType(builder, {}, TypeIndex { .index = pair })));
    auto const destructor = std::make_shared<RecordingDestructor>();
    auto const type = HostResourceType { "r", destructor };
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineResource("r", type));
    auto ownType = linker.ownType(type);
    REQUIRE(ownType.has_value());
    require(linker.defineFunction(
        "make",
        functionSpec({}, *ownType),
        hostFunction([&](HostComponentContext& context, Values const&, ValueBuilder& results) -> HostResult {
            auto fresh = context.newOwn(type, 40);
            if (!fresh)
                return std::unexpected { embedderFailure() };
            results.resource(*fresh);
            return {};
        })));
    {
        auto store = harness.store();
        auto instance = instanceOf(store.instantiate(harness.load(builder.build()), linker));
        CHECK(trapOf(callWith(instance, "give")) == TrapCode::OutOfBounds);
        auto const held = store.hostResource(type, 40, Ownership::Borrow);
        REQUIRE(held.has_value());
        auto const dropped =
            store.drop(ResourceValue { .type = held->type, .rep = 40, .ownership = Ownership::Own });
        REQUIRE_FALSE(dropped.has_value());
        CHECK(dropped.error().failure == ComponentCallFailure::ArgumentMismatch);
        CHECK(destructor->destroyed.empty());
    }
    CHECK(destructor->destroyed == std::vector<uint32_t> { 40 });
}

namespace
{
/// The bytes of the component @p source gives as text; the text must compile.
[[nodiscard]] std::vector<std::byte> componentText(std::string_view source)
{
    auto bytes =
        isoline::text::compileText(source, isoline::text::TextLimits {}, isoline::component::text::Forms);
    if (!bytes)
        UNSCOPED_INFO(std::format("{}:{}", bytes.error().position.line, bytes.error().position.column));
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}
} // namespace

TEST_CASE("ComponentInstance.exportedModulesAndResources")
{
    // What Wasmtime's script runner reads of a named component (wast.rs:400-427): its exported core
    // modules, and its resource types, which another store's components import through a linker.
    auto harness = ApiHarness {};
    auto const linker = [] {
        auto made = Linker { ComponentConfig {} };
        require(made.defineResource("host-r", HostResourceType { "host-r", nullptr }));
        return made;
    }();
    auto const component = harness.load(componentText(R"((component
        (import "host-r" (type $h (sub resource)))
        (core module $m (func (export "f")))
        (type $r (resource (rep i32)))
        (export "m" (core module $m))
        (export "r" (type $r))
        (export "h" (type $h))
        (instance $i (export "inner" (core module $m)))
        (export "i" (instance $i))))"));
    auto store = harness.store();
    auto const first = instanceOf(store.instantiate(component, linker));
    auto const second = instanceOf(store.instantiate(component, linker));

    auto const module = first.module("m");
    REQUIRE(module.has_value());
    REQUIRE(module->has_value());
    CHECK((*module)->exports().size() == 1);
    auto const inner = first.module("i#inner");
    REQUIRE(inner.has_value());
    CHECK(inner->has_value());
    auto const notAModule = first.module("r");
    REQUIRE(notAModule.has_value());
    CHECK_FALSE(notAModule->has_value());
    auto const missing = first.module("nothing");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->has_value());

    // A guest's resource type has one identity per store and runtime type: asked twice, one type; of
    // another instance, another.
    auto const resource = first.resource("r");
    REQUIRE(resource.has_value());
    REQUIRE(resource->has_value());
    auto const again = first.resource("r");
    REQUIRE(again.has_value());
    REQUIRE(again->has_value());
    CHECK(**resource == **again);
    CHECK((*resource)->destructor() == nullptr);
    auto const other = second.resource("r");
    REQUIRE(other.has_value());
    REQUIRE(other->has_value());
    CHECK_FALSE(**resource == **other);
    // A host type it re-exports is the host's.
    auto const host = first.resource("h");
    REQUIRE(host.has_value());
    REQUIRE(host->has_value());
    CHECK((*host)->name() == "host-r");
    auto const notAResource = first.resource("m");
    REQUIRE(notAResource.has_value());
    CHECK_FALSE(notAResource->has_value());

    // Defined in a linker, the guest's type is what another store's import binds: one type is one, two
    // are two.
    auto named = Linker { ComponentConfig {} };
    require(named.defineResource("a#r", **resource));
    require(named.defineResource("b#r", **again));
    require(named.defineResource("c#r", **other));
    auto const importer = [&](std::string_view second) {
        return harness.load(componentText(std::format(R"((component
            (import "a" (instance $a (export "r" (type (sub resource)))))
            (alias export $a "r" (type $r))
            (import "{}" (instance (export "r" (type (eq $r)))))))",
                                                      second)));
    };
    auto otherStore = harness.store();
    CHECK(otherStore.instantiate(importer("b"), named).has_value());
    CHECK_FALSE(otherStore.instantiate(importer("c"), named).has_value());

    // Once the store is gone both refuse.
    auto const orphan = [&] {
        auto gone = harness.store();
        return instanceOf(gone.instantiate(component, linker));
    }();
    CHECK_FALSE(orphan.module("m").has_value());
    CHECK_FALSE(orphan.resource("r").has_value());
}

TEST_CASE("HostResources.reexportedHostResourceFunctions")
{
    // A host instance re-exported whole: its resource's constructor returns an own the host then holds, its
    // method borrows it, and its consumer takes it back; the owns move as between the host and a guest.
    auto harness = ApiHarness {};
    auto const component = harness.load(componentText(R"((component
        (import "host" (instance $h
            (export "r" (type (sub resource)))
            (export "[constructor]r" (func (param "rep" u32) (result (own 0))))
            (export "[method]r.get" (func (param "self" (borrow 0)) (result u32)))
            (export "consume" (func (param "x" (own 0)) (result u32)))))
        (export "h" (instance $h))))"));
    auto const destructor = std::make_shared<RecordingDestructor>();
    auto const type = HostResourceType { "r", destructor };
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineResource("host#r", type));
    auto const ownType = linker.ownType(type);
    auto const borrowType = linker.borrowType(type);
    REQUIRE(ownType.has_value());
    REQUIRE(borrowType.has_value());
    auto const u32 = primitive(linker, ValueKind::U32);
    require(linker.defineFunction(
        "host#[constructor]r",
        FunctionTypeSpec { .params = { { "rep", u32 } }, .result = *ownType },
        hostFunction(
            [&](HostComponentContext& context, Values const& args, ValueBuilder& results) -> HostResult {
                auto fresh = context.newOwn(type, args.root(0).scalar<uint32_t>());
                if (!fresh)
                    return std::unexpected { embedderFailure() };
                results.resource(*fresh);
                return {};
            })));
    require(linker.defineFunction(
        "host#[method]r.get",
        FunctionTypeSpec { .params = { { "self", *borrowType } }, .result = u32 },
        hostFunction([](HostComponentContext&, Values const& args, ValueBuilder& results) -> HostResult {
            auto const self = args.root(0).resource();
            results.scalar(self.ownership == Ownership::Borrow ? self.rep * 10 : 0);
            return {};
        })));
    require(linker.defineFunction(
        "host#consume",
        FunctionTypeSpec { .params = { { "x", *ownType } }, .result = u32 },
        hostFunction(
            [](HostComponentContext& context, Values const& args, ValueBuilder& results) -> HostResult {
                auto const own = args.root(0).resource();
                if (!context.drop(own))
                    return std::unexpected { embedderFailure() };
                results.scalar(own.rep);
                return {};
            })));
    {
        auto store = harness.store();
        auto instance = instanceOf(store.instantiate(component, linker));
        auto const made = callWith(instance, "h#[constructor]r", { 7 });
        REQUIRE(made.has_value());
        auto const own = made->root(0).resource();
        CHECK(own.rep == 7);
        CHECK(instance.hostHolds(own) == 1);

        auto const get = lookup(instance, "h#[method]r.get");
        REQUIRE(get.has_value());
        auto lent = instance.argumentsFor(*get);
        REQUIRE(lent.has_value());
        lent->resource(ResourceValue { .type = own.type, .rep = own.rep, .ownership = Ownership::Borrow });
        auto const borrowed = lent->finish();
        REQUIRE(borrowed.has_value());
        CHECK(u32Of(instance.call(*get, *borrowed)) == 70);
        CHECK(instance.hostHolds(own) == 1);

        auto const consume = lookup(instance, "h#consume");
        REQUIRE(consume.has_value());
        auto given = instance.argumentsFor(*consume);
        REQUIRE(given.has_value());
        given->resource(own);
        auto const passed = given->finish();
        REQUIRE(passed.has_value());
        CHECK(u32Of(instance.call(*consume, *passed)) == 7);
        CHECK(instance.hostHolds(own) == 0);
        CHECK(destructor->destroyed == std::vector<uint32_t> { 7 });
        // An own the host no longer holds is refused before the callee runs.
        CHECK(failureOf(instance.call(*consume, *passed)).failure == ComponentCallFailure::ArgumentMismatch);
        CHECK(destructor->destroyed == std::vector<uint32_t> { 7 });
        CHECK_FALSE(store.isLockedDown());
    }
    CHECK(destructor->destroyed == std::vector<uint32_t> { 7 });
}
