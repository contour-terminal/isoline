// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/ComponentFixtures.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/ComponentStore.hpp>
#include <isoline/component/runtime/testing/ResourceDoubles.hpp>
#include <isoline/component/runtime/testing/StoreFixtures.hpp>

using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreSort;
using isoline::component::binary::DefinedValueType;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::OwnType;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::Sort;
using isoline::component::binary::SortIndex;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::doublingComponent;
using isoline::component::binary::testing::identityModule;
using isoline::component::binary::testing::liftedIdentity;
using isoline::component::binary::testing::TypeScopeBuilder;
using isoline::component::canonical::ResourceTypeIndex;
using isoline::component::canonical::ResourceValue;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::ValueBuilder;
using isoline::component::canonical::Values;
using isoline::component::runtime::ComponentStore;
using isoline::component::runtime::HostResourceIdentity;
using isoline::component::runtime::HostResourceImport;
using isoline::component::runtime::InstantiationFailure;
using isoline::component::runtime::InstantiationFailureKind;
using isoline::component::runtime::ResolvedImports;
using isoline::component::runtime::StoreState;
using isoline::component::runtime::testing::call;
using isoline::component::runtime::testing::coreModule;
using isoline::component::runtime::testing::Engines;
using isoline::component::runtime::testing::functionType;
using isoline::component::runtime::testing::Harness;
using isoline::component::runtime::testing::HarnessOptions;
using isoline::component::runtime::testing::hostFunction;
using isoline::component::runtime::testing::importsOf;
using isoline::component::runtime::testing::RecordingDestructor;
using isoline::component::runtime::testing::u32Result;

namespace
{

/// How long a thread of a test may take before it is called stuck.
constexpr auto ThreadDeadline = std::chrono::seconds { 60 };

/// The failure an instantiation ended in (REQUIREd to have failed).
[[nodiscard]] InstantiationFailure failureOf(std::expected<uint32_t, InstantiationFailure> const& outcome)
{
    REQUIRE_FALSE(outcome.has_value());
    return outcome.error();
}

/// A component instantiating identityModule() @p count times, exporting nothing.
[[nodiscard]] std::vector<std::byte> coreInstances(uint32_t count)
{
    auto builder = ComponentBuilder {};
    auto const module = builder.coreModule(identityModule());
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        std::ignore = builder.coreInstantiate(module, {});
    return builder.build();
}

/// A component whose core module's start function runs @p body.
[[nodiscard]] std::vector<std::byte> startComponent(std::string_view body)
{
    auto builder = ComponentBuilder {};
    auto const module = builder.coreModule(
        coreModule(std::string { "(module (func $start " } + std::string { body } + ") (start $start))"));
    std::ignore = builder.coreInstantiate(module, {});
    return builder.build();
}

} // namespace

TEST_CASE("ComponentStore.capsCheckedBeforeCreation")
{
    auto const engine = GENERATE(from_range(Engines));
    SECTION("core instances")
    {
        auto limits = ComponentLimits {};
        limits.maxCoreInstances = 2;
        auto harness = Harness { coreInstances(3), HarnessOptions { .engine = engine, .limits = limits } };
        auto const failure = failureOf(harness.instantiate());
        CHECK(failure.kind == InstantiationFailureKind::LimitExceeded);
        CHECK(std::get<ComponentLimit>(failure.detail) == ComponentLimit::CoreInstances);
        CHECK(harness.store->coreStore().used(isoline::api::Limit::Instances) == 2);
        CHECK(harness.store->state() == StoreState::LockedDown);
    }
    SECTION("component instances")
    {
        auto limits = ComponentLimits {};
        limits.maxComponentInstances = 3;
        auto harness =
            Harness { doublingComponent(20).build(), HarnessOptions { .engine = engine, .limits = limits } };
        auto const failure = failureOf(harness.instantiate());
        CHECK(failure.kind == InstantiationFailureKind::LimitExceeded);
        CHECK(std::get<ComponentLimit>(failure.detail) == ComponentLimit::ComponentInstances);
        CHECK(harness.store->resources().instances().size() == 3);
        CHECK(harness.store->state() == StoreState::LockedDown);
    }
    SECTION("instantiation steps")
    {
        auto limits = ComponentLimits {};
        limits.maxInstantiationSteps = 1000;
        auto harness =
            Harness { doublingComponent(20).build(), HarnessOptions { .engine = engine, .limits = limits } };
        auto const failure = failureOf(harness.instantiate());
        CHECK(failure.kind == InstantiationFailureKind::LimitExceeded);
        CHECK(std::get<ComponentLimit>(failure.detail) == ComponentLimit::InstantiationSteps);
        CHECK(harness.store->resources().instances().size() < 1000);
        CHECK(harness.store->state() == StoreState::LockedDown);
    }
    SECTION("the root's instance, before the first step")
    {
        auto limits = ComponentLimits {};
        limits.maxComponentInstances = 1;
        auto harness =
            Harness { liftedIdentity().build(), HarnessOptions { .engine = engine, .limits = limits } };
        std::ignore = harness.instance();
        auto const failure = failureOf(harness.instantiate());
        CHECK(failure.kind == InstantiationFailureKind::LimitExceeded);
        CHECK(harness.store->state() == StoreState::Live);
    }
}

TEST_CASE("ComponentStore.failedInstantiationLocksDown")
{
    // The second core instantiation passes the core store's maxInstances: a step after the first.
    auto const engine = GENERATE(from_range(Engines));
    auto storeLimits = isoline::api::StoreLimits {};
    storeLimits.maxInstances = 1;
    auto harness =
        Harness { coreInstances(2), HarnessOptions { .engine = engine, .storeLimits = storeLimits } };
    auto const failure = failureOf(harness.instantiate());
    CHECK(failure.kind == InstantiationFailureKind::CoreInstantiation);
    auto const& error = std::get<isoline::api::InstantiateError>(failure.detail);
    CHECK(std::get<isoline::api::LimitExceeded>(error).limit == isoline::api::Limit::Instances);
    CHECK(harness.store->state() == StoreState::LockedDown);
    CHECK(failureOf(harness.instantiate()).kind == InstantiationFailureKind::LockedDown);
}

TEST_CASE("ComponentStore.startTrapLocksDown")
{
    auto const engine = GENERATE(from_range(Engines));
    auto harness = Harness { startComponent("unreachable"), HarnessOptions { .engine = engine } };
    auto const failure = failureOf(harness.instantiate());
    CHECK(failure.kind == InstantiationFailureKind::Trap);
    CHECK(std::get<isoline::component::canonical::Trap>(failure.detail).code == TrapCode::Core);
    CHECK(harness.store->state() == StoreState::LockedDown);
}

TEST_CASE("ComponentStore.refusalsBeforeTheFirstStepDoNotLockDown")
{
    auto const engine = GENERATE(from_range(Engines));
    SECTION("another engine's code")
    {
        auto harness = Harness { liftedIdentity().build(), HarnessOptions { .engine = engine } };
        auto other = isoline::api::Engine { isoline::api::EngineConfig {
            .engine = engine, .mode = isoline::api::ExecutionMode::Deterministic } };
        REQUIRE(other.config().identity() != harness.engine.config().identity());
        auto allocator = isoline::api::HeapMemoryAllocator {};
        auto store = ComponentStore { other, allocator, ComponentLimits {} };
        auto const failure = failureOf(store.instantiate(harness.component->code(), {}));
        CHECK(failure.kind == InstantiationFailureKind::EngineMismatch);
        CHECK(store.state() == StoreState::Live);
        CHECK(store.resources().instances().size() == 0);
    }
    SECTION("missing imports")
    {
        auto builder = ComponentBuilder {};
        auto const type = functionType(builder, {}, PrimitiveValueType::U32);
        std::ignore = builder.import("f", ExternDesc { .sort = Sort::Func, .index = type });
        auto harness = Harness { builder.build(), HarnessOptions { .engine = engine } };
        auto const failure = failureOf(harness.instantiate());
        CHECK(failure.kind == InstantiationFailureKind::ImportMismatch);
        CHECK(std::get<uint32_t>(failure.detail) == 0);
        auto wrongSort = ResolvedImports {};
        wrongSort.values.emplace_back(std::monostate {});
        wrongSort.imports.push_back(0);
        CHECK(failureOf(harness.instantiate(wrongSort)).kind == InstantiationFailureKind::ImportMismatch);
        auto outOfRange = ResolvedImports {};
        outOfRange.imports.push_back(3);
        CHECK(failureOf(harness.instantiate(outOfRange)).kind == InstantiationFailureKind::ImportMismatch);
        CHECK(harness.store->state() == StoreState::Live);
        CHECK(harness.store->resources().instances().size() == 0);
    }
}

TEST_CASE("ComponentStore.reentrantInstantiateIsRefused")
{
    auto const engine = GENERATE(from_range(Engines));
    auto failures = std::vector<InstantiationFailureKind> {};
    Harness* current = nullptr;
    auto host = hostFunction([&](ComponentStore& store,
                                 Values const&,
                                 ValueBuilder& results) -> std::expected<void, isoline::api::HostFailure> {
        auto const nested = store.instantiate(current->component->code(), {});
        failures.push_back(nested ? InstantiationFailureKind::HostMemoryExhausted : nested.error().kind);
        results.scalar(1);
        return {};
    });
    SECTION("from a call")
    {
        auto bytes = ComponentBuilder {};
        auto const type = functionType(bytes, {}, PrimitiveValueType::U32);
        auto const f = bytes.import("f", ExternDesc { .sort = Sort::Func, .index = type });
        auto const hostInstance = bytes.coreInlineExports(
            { CoreInlineExport { .name = "f", .sort = CoreSort::Func, .index = bytes.canonLower(f, {}) } });
        auto const module = bytes.coreModule(coreModule(R"((module
          (import "host" "f" (func $f (result i32)))
          (func (export "call") (result i32) (call $f))))"));
        auto const instance = bytes.coreInstantiate(module, { { "host", hostInstance } });
        std::ignore = bytes.exportItem(
            "call",
            Sort::Func,
            bytes.canonLift(bytes.aliasCoreExport(instance, "call", CoreSort::Func), {}, type));
        auto harness = Harness { bytes.build(), HarnessOptions { .engine = engine } };
        current = &harness;
        auto const root = harness.instance(importsOf({ host }));
        CHECK(u32Result(call(*harness.store, root, "call")) == 1);
        CHECK(harness.store->state() == StoreState::Live);
    }
    SECTION("from a start function")
    {
        auto bytes = ComponentBuilder {};
        auto const type = functionType(bytes, {}, PrimitiveValueType::U32);
        auto const f = bytes.import("f", ExternDesc { .sort = Sort::Func, .index = type });
        auto const hostInstance = bytes.coreInlineExports(
            { CoreInlineExport { .name = "f", .sort = CoreSort::Func, .index = bytes.canonLower(f, {}) } });
        auto const module = bytes.coreModule(coreModule(R"((module
          (import "host" "f" (func $f (result i32)))
          (func $start (drop (call $f)))
          (start $start)))"));
        std::ignore = bytes.coreInstantiate(module, { { "host", hostInstance } });
        auto harness = Harness { bytes.build(), HarnessOptions { .engine = engine } };
        current = &harness;
        std::ignore = harness.instance(importsOf({ host }));
        CHECK(harness.store->state() == StoreState::Live);
    }
    CHECK(failures == std::vector { InstantiationFailureKind::Reentrant });
}

TEST_CASE("ComponentStore.outerAliasFollowsTheLexicalChain")
{
    // P defines m and C, whose `alias outer 1 m` names P's m; P passes C to its sibling Q, which defines
    // a module of its own at the same index and instantiates C. C must see P's m (identity, 7 → 7), never
    // Q's (which returns 99).
    auto const engine = GENERATE(from_range(Engines));
    auto parent = ComponentBuilder {};
    auto const m = parent.coreModule(identityModule());
    REQUIRE(m == 0);

    auto c = ComponentBuilder {};
    {
        auto const module = c.aliasOuter(Sort::CoreModule, 1, m);
        auto const instance = c.coreInstantiate(module, {});
        auto const core = c.aliasCoreExport(instance, "id", CoreSort::Func);
        auto const type = functionType(c, { PrimitiveValueType::U32 }, PrimitiveValueType::U32);
        std::ignore = c.exportItem("id", Sort::Func, c.canonLift(core, {}, type));
    }
    auto const cIndex = parent.nested(std::move(c));

    auto q = ComponentBuilder {};
    {
        auto const own = q.coreModule(coreModule(R"((module
          (func (export "id") (param i32) (result i32) (i32.const 99))))"));
        REQUIRE(own == 0);
        auto const idType = functionType(q, { PrimitiveValueType::U32 }, PrimitiveValueType::U32);
        auto declarations = TypeScopeBuilder {};
        auto const declared =
            declarations.type(TypeDefinition { .type = isoline::component::binary::FunctionTypeDefinition {
                                                   .asyncness = isoline::component::binary::Asyncness::Sync,
                                                   .params = { isoline::component::binary::LabeledType {
                                                       .label = "p0", .type = PrimitiveValueType::U32 } },
                                                   .result = PrimitiveValueType::U32 } });
        std::ignore = declarations.exportItem("id", ExternDesc { .sort = Sort::Func, .index = declared });
        auto const componentType = q.componentType(std::move(declarations));
        auto const imported = q.import("c", ExternDesc { .sort = Sort::Component, .index = componentType });
        auto const instance = q.instantiate(imported, {});
        std::ignore = idType;
        std::ignore = q.exportItem("id", Sort::Func, q.aliasExport(instance, "id", Sort::Func));
        std::ignore = q.coreInstantiate(own, {});
    }
    auto const qIndex = parent.nested(std::move(q));
    auto const qInstance =
        parent.instantiate(qIndex, { { "c", SortIndex { .sort = Sort::Component, .index = cIndex } } });
    std::ignore = parent.exportItem("id", Sort::Func, parent.aliasExport(qInstance, "id", Sort::Func));

    auto harness = Harness { parent.build(), HarnessOptions { .engine = engine } };
    auto const root = harness.instance();
    CHECK(u32Result(call(*harness.store, root, "id", [](ValueBuilder& args) { args.scalar(7); })) == 7);
}

TEST_CASE("ComponentStore.resourceIdentityPerInstantiation")
{
    // Each execution of a resource definition is a type of its own: two instances of one component,
    // and two instances of one child, never share one.
    auto const engine = GENERATE(from_range(Engines));
    auto child = ComponentBuilder {};
    std::ignore = child.exportItem("r", Sort::Type, child.resource());
    auto parent = ComponentBuilder {};
    auto const component = parent.nested(std::move(child));
    auto const first = parent.instantiate(component, {});
    auto const second = parent.instantiate(component, {});
    std::ignore = parent.exportItem("a", Sort::Instance, first);
    std::ignore = parent.exportItem("b", Sort::Instance, second);
    auto harness = Harness { parent.build(), HarnessOptions { .engine = engine } };
    auto& store = *harness.store;
    auto const typeOf = [&](uint32_t root, std::string_view instance) {
        auto const exports = store.exportsOf(root);
        auto const found =
            std::ranges::find(exports, instance, &isoline::component::runtime::NamedValue::name);
        REQUIRE(found != exports.end());
        auto const inner = store.exportsOf(std::get<isoline::component::runtime::InstanceRef>(found->value));
        REQUIRE(inner.size() == 1);
        return std::get<ResourceTypeIndex>(inner.front().value);
    };
    auto const once = harness.instance();
    auto const again = harness.instance();
    auto const types = std::vector {
        typeOf(once, "a"),
        typeOf(once, "b"),
        typeOf(again, "a"),
        typeOf(again, "b"),
    };
    for (auto const [index, type]: core::views::enumerate(types))
        for (auto const& other: std::span { types }.subspan(index + 1))
            CHECK(type != other);
}

TEST_CASE("ComponentStore.hostOwnsDestroyedAtTeardown")
{
    // keep() takes an own<r> from the host's make and keeps it: the store's destruction destroys it,
    // once, through the host type's destructor.
    auto const engine = GENERATE(from_range(Engines));
    auto builder = ComponentBuilder {};
    auto const resource =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const own =
        builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = resource } } });
    auto const makeType = functionType(builder, {}, TypeIndex { .index = own });
    auto const make = builder.import("make", ExternDesc { .sort = Sort::Func, .index = makeType });
    auto const host = builder.coreInlineExports({ CoreInlineExport {
        .name = "make", .sort = CoreSort::Func, .index = builder.canonLower(make, {}) } });
    auto const module = builder.coreModule(coreModule(R"((module
      (import "host" "make" (func $make (result i32)))
      (func (export "keep") (drop (call $make)))))"));
    auto const instance = builder.coreInstantiate(module, { { "host", host } });
    std::ignore = builder.exportItem(
        "keep",
        Sort::Func,
        builder.canonLift(
            builder.aliasCoreExport(instance, "keep", CoreSort::Func), {}, functionType(builder, {}, {})));

    auto harness = Harness { builder.build(), HarnessOptions { .engine = engine } };
    auto const identity = std::make_shared<HostResourceIdentity const>(HostResourceIdentity { .name = "r" });
    auto const destructor = std::make_shared<RecordingDestructor>();
    auto next = uint32_t { 5 };
    auto makeFunction = hostFunction([&](ComponentStore& store, Values const&, ValueBuilder& results)
                                         -> std::expected<void, isoline::api::HostFailure> {
        auto const type = store.hostResourceTypeOf(*identity);
        if (!type)
            return std::unexpected { isoline::component::runtime::testing::embedderFailure() };
        results.resource(ResourceValue { .type = *type, .rep = next++ });
        return {};
    });
    auto imports = ResolvedImports {};
    imports.values.emplace_back(HostResourceImport { .identity = identity, .destructor = destructor });
    imports.values.emplace_back(makeFunction);
    imports.imports = { 0, 1 };
    auto const root = harness.instance(imports);
    REQUIRE(call(*harness.store, root, "keep").result.has_value());
    REQUIRE(call(*harness.store, root, "keep").result.has_value());
    CHECK(destructor->destroyed.empty());
    harness.store.reset();
    CHECK(destructor->destroyed == std::vector<uint32_t> { 5, 6 });
}

TEST_CASE("ComponentStore.sharedComponentAcrossThreads", "[threading]")
{
    // One loaded component, a store per thread: each instantiates and calls it at once.
    auto const engine = GENERATE(from_range(Engines));
    auto harness = Harness { liftedIdentity().build(), HarnessOptions { .engine = engine } };
    auto const code = harness.component->code();
    auto const run = [&](uint32_t seed) {
        auto allocator = isoline::api::HeapMemoryAllocator {};
        auto store = ComponentStore { harness.engine, allocator, ComponentLimits {} };
        auto sum = uint64_t { 0 };
        for (auto const round: std::views::iota(uint32_t { 0 }, uint32_t { 50 }))
        {
            auto const instance = store.instantiate(code, {});
            if (!instance)
                return uint64_t { 0 };
            auto const& lifted = store.lifted(
                std::get<isoline::component::runtime::LiftedRef>(*store.function(*instance, "id")));
            auto const outcome = isoline::component::runtime::testing::callWith(
                store,
                lifted,
                [&](ValueBuilder& args) { args.scalar(seed + round); },
                isoline::component::runtime::testing::ArgumentOwnership::Owned,
                [](Values const&) {});
            if (!outcome.result || !outcome.values)
                return uint64_t { 0 };
            sum += outcome.values->root(0).scalar<uint32_t>();
        }
        return sum;
    };
    auto first = std::async(std::launch::async, run, uint32_t { 1000 });
    auto second = std::async(std::launch::async, run, uint32_t { 2000 });
    REQUIRE(first.wait_for(ThreadDeadline) == std::future_status::ready);
    REQUIRE(second.wait_for(ThreadDeadline) == std::future_status::ready);
    // Σ (seed + round) over 50 rounds = 50 seed + 1225.
    CHECK(first.get() == (50 * 1000) + 1225);
    CHECK(second.get() == (50 * 2000) + 1225);
}

namespace
{

/// `func(p0: u32) -> u32` as a type definition.
[[nodiscard]] TypeDefinition idFunctionType()
{
    return TypeDefinition { .type = isoline::component::binary::FunctionTypeDefinition {
                                .asyncness = isoline::component::binary::Asyncness::Sync,
                                .params = { isoline::component::binary::LabeledType {
                                    .label = "p0", .type = PrimitiveValueType::U32 } },
                                .result = PrimitiveValueType::U32 } };
}

/// `func() -> u32` as a type definition.
[[nodiscard]] TypeDefinition constantFunctionType()
{
    return TypeDefinition { .type = isoline::component::binary::FunctionTypeDefinition {
                                .asyncness = isoline::component::binary::Asyncness::Sync,
                                .params = {},
                                .result = PrimitiveValueType::U32 } };
}

/// A sub-resource bound.
constexpr auto SubResource = ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource };

/// A component importing one value of every sort a root plan takes: a core module `m`, a component `c`,
/// a host instance `host` (a function `f`, a resource `r`, a type `n` equal to u32, and an instance `inner`
/// of a resource `r2`), an
/// instance `types` of types only, an instance `prior` of this store, and a type `t` equal to u32. It
/// exports what it makes of each, and an instance `bundle` it synthesizes.
[[nodiscard]] std::vector<std::byte> everySortComponent()
{
    using isoline::component::binary::CoreExportDeclaration;
    using isoline::component::binary::CoreModuleTypeDefinition;
    using isoline::component::binary::CoreTypeDeclaration;
    using isoline::component::binary::CoreTypeDefinition;
    auto builder = ComponentBuilder {};
    auto const idType = builder.type(idFunctionType());
    auto const constantType = builder.type(constantFunctionType());
    auto const moduleType = builder.coreType(CoreTypeDefinition {
        .type = CoreModuleTypeDefinition {
            .declarations = {
                CoreTypeDeclaration { .type =
                                          isoline::binary::FuncType { .params = { isoline::binary::I32 },
                                                                      .results = { isoline::binary::I32 } } },
                CoreExportDeclaration {
                    .name = "id", .type = { .kind = isoline::binary::ExternKind::Func, .typeIndex = 0 } },
            } } });
    auto componentScope = TypeScopeBuilder {};
    std::ignore = componentScope.exportItem(
        "id", ExternDesc { .sort = Sort::Func, .index = componentScope.type(idFunctionType()) });
    auto const componentType = builder.componentType(std::move(componentScope));
    auto innerScope = TypeScopeBuilder {};
    std::ignore = innerScope.exportItem("r2", SubResource);
    auto hostScope = TypeScopeBuilder {};
    std::ignore = hostScope.exportItem(
        "f", ExternDesc { .sort = Sort::Func, .index = hostScope.type(constantFunctionType()) });
    std::ignore = hostScope.exportItem("r", SubResource);
    std::ignore =
        hostScope.exportItem("n",
                             ExternDesc { .sort = Sort::Type,
                                          .index = hostScope.type(TypeDefinition {
                                              .type = DefinedValueType { PrimitiveValueType::U32 } }) });
    std::ignore = hostScope.exportItem(
        "inner",
        ExternDesc { .sort = Sort::Instance, .index = hostScope.instanceType(std::move(innerScope)) });
    auto const hostType = builder.instanceType(std::move(hostScope));
    auto typesScope = TypeScopeBuilder {};
    std::ignore =
        typesScope.exportItem("t",
                              ExternDesc { .sort = Sort::Type,
                                           .index = typesScope.type(TypeDefinition {
                                               .type = DefinedValueType { PrimitiveValueType::U32 } }) });
    auto const typesType = builder.instanceType(std::move(typesScope));
    auto priorScope = TypeScopeBuilder {};
    std::ignore = priorScope.exportItem(
        "id", ExternDesc { .sort = Sort::Func, .index = priorScope.type(idFunctionType()) });
    auto const priorType = builder.instanceType(std::move(priorScope));
    auto const u32 = builder.type(TypeDefinition { .type = DefinedValueType { PrimitiveValueType::U32 } });

    auto const m = builder.import("m", ExternDesc { .sort = Sort::CoreModule, .index = moduleType });
    auto const c = builder.import("c", ExternDesc { .sort = Sort::Component, .index = componentType });
    auto const host = builder.import("host", ExternDesc { .sort = Sort::Instance, .index = hostType });
    auto const types = builder.import("types", ExternDesc { .sort = Sort::Instance, .index = typesType });
    auto const prior = builder.import("prior", ExternDesc { .sort = Sort::Instance, .index = priorType });
    std::ignore = builder.import("t", ExternDesc { .sort = Sort::Type, .index = u32 });

    auto const moduleInstance = builder.coreInstantiate(m, {});
    auto const fromModule =
        builder.canonLift(builder.aliasCoreExport(moduleInstance, "id", CoreSort::Func), {}, idType);
    std::ignore = builder.exportItem("from-module", Sort::Func, fromModule);
    auto const componentInstance = builder.instantiate(c, {});
    std::ignore = builder.exportItem(
        "from-component", Sort::Func, builder.aliasExport(componentInstance, "id", Sort::Func));
    auto const f = builder.aliasExport(host, "f", Sort::Func);
    std::ignore = builder.aliasExport(host, "inner", Sort::Instance);
    std::ignore = builder.aliasExport(types, "t", Sort::Type);
    auto const lowered = builder.coreInlineExports(
        { CoreInlineExport { .name = "f", .sort = CoreSort::Func, .index = builder.canonLower(f, {}) } });
    auto const caller = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "f" (func $f (result i32)))
      (func (export "call") (result i32) (call $f))))")),
                                                { { "h", lowered } });
    std::ignore = builder.exportItem(
        "from-host",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(caller, "call", CoreSort::Func), {}, constantType));
    std::ignore = builder.exportItem("from-prior", Sort::Func, builder.aliasExport(prior, "id", Sort::Func));
    auto const bundle =
        builder.inlineInstance({ { "id", SortIndex { .sort = Sort::Func, .index = fromModule } } });
    std::ignore = builder.exportItem("bundle", Sort::Instance, bundle);
    return builder.build();
}

/// What everySortComponent() imports, in a store: values 0–9, the root's imports {0, 1, 6, 7, 8, 9}.
struct EverySortImports
{
    std::shared_ptr<HostResourceIdentity const> r =
        std::make_shared<HostResourceIdentity const>(HostResourceIdentity { .name = "r" });
    std::shared_ptr<HostResourceIdentity const> r2 =
        std::make_shared<HostResourceIdentity const>(HostResourceIdentity { .name = "r2" });

    [[nodiscard]] ResolvedImports of(Harness& harness, Harness& other, uint32_t priorInstance) const
    {
        auto module = harness.engine.load(identityModule());
        REQUIRE(module.has_value());
        auto imports = ResolvedImports {};
        imports.values.emplace_back(std::move(*module));
        imports.values.emplace_back(
            isoline::component::runtime::ComponentImport { .code = other.component->code() });
        imports.values.emplace_back(hostFunction([](ComponentStore&, Values const&, ValueBuilder& results)
                                                     -> std::expected<void, isoline::api::HostFailure> {
            results.scalar(5);
            return {};
        }));
        imports.values.emplace_back(HostResourceImport { .identity = r });
        imports.values.emplace_back(HostResourceImport { .identity = r2 });
        imports.values.emplace_back(
            isoline::component::runtime::HostInstanceImport { .exports = { { "r2", 4 } } });
        imports.values.emplace_back(isoline::component::runtime::HostInstanceImport {
            .exports = { { "f", 2 }, { "r", 3 }, { "inner", 5 } } });
        imports.values.emplace_back(std::monostate {});
        imports.values.emplace_back(harness.store->instanceValueOf(priorInstance));
        imports.values.emplace_back(std::monostate {});
        imports.imports = { 0, 1, 6, 7, 8, 9 };
        return imports;
    }
};

} // namespace

TEST_CASE("ComponentStore.importsOfEverySort")
{
    auto const engine = GENERATE(from_range(Engines));
    auto harness = Harness { everySortComponent(), HarnessOptions { .engine = engine } };
    auto other = Harness { liftedIdentity().build(), HarnessOptions { .engine = engine } };
    auto& store = *harness.store;
    // `prior`: an instance of this store, from code the store has not seen before.
    auto const prior = store.instantiate(other.component->code(), {});
    REQUIRE(prior.has_value());
    auto const imports = EverySortImports {};
    auto const resolved = imports.of(harness, other, *prior);

    SECTION("instantiated and called")
    {
        auto const root = harness.instance(resolved);
        auto const id = [&](std::string_view name, uint32_t value) {
            return u32Result(call(store, root, name, [&](ValueBuilder& args) { args.scalar(value); }));
        };
        CHECK(id("from-module", 7) == 7);
        CHECK(id("from-component", 8) == 8);
        CHECK(id("from-prior", 9) == 9);
        CHECK(u32Result(call(store, root, "from-host")) == 5);
        CHECK(store.hostResourceTypeOf(*imports.r).has_value());
        CHECK(store.hostResourceTypeOf(*imports.r2).has_value());
        CHECK(store.hostResourceTypeOf(*imports.r) != store.hostResourceTypeOf(*imports.r2));
        CHECK_FALSE(store.hostResourceTypeOf(HostResourceIdentity { .name = "r" }).has_value());
        // The synthesized instance exports the lifted function; a name it lacks, or one of another sort,
        // is no function.
        auto const exports = store.exportsOf(root);
        auto const bundle = std::ranges::find(
            exports, std::string_view { "bundle" }, &isoline::component::runtime::NamedValue::name);
        REQUIRE(bundle != exports.end());
        auto const bundled =
            store.exportsOf(std::get<isoline::component::runtime::InstanceRef>(bundle->value));
        REQUIRE(bundled.size() == 1);
        CHECK(bundled.front().name == "id");
        CHECK_FALSE(store.function(root, "missing").has_value());
        CHECK_FALSE(store.function(root, "bundle").has_value());
    }
    SECTION("a host instance lacking what its type exports")
    {
        // Found only as the plan binds the instance's resources: after the first step, so locked down.
        auto const lacking = [&](uint32_t value, std::string_view name) {
            auto imports = resolved;
            auto& exports =
                std::get<isoline::component::runtime::HostInstanceImport>(imports.values[value]).exports;
            std::erase_if(exports, [&](auto const& entry) { return entry.first == name; });
            return imports;
        };
        SECTION("a resource")
        {
            CHECK(failureOf(harness.instantiate(lacking(6, "r"))).kind
                  == InstantiationFailureKind::ImportMismatch);
        }
        SECTION("an instance")
        {
            CHECK(failureOf(harness.instantiate(lacking(6, "inner"))).kind
                  == InstantiationFailureKind::ImportMismatch);
        }
        CHECK(store.state() == StoreState::LockedDown);
    }
    SECTION("refused before the first step")
    {
        auto const refusedWith = [&](ResolvedImports const& imports) {
            auto const failure = failureOf(harness.instantiate(imports));
            CHECK(failure.kind == InstantiationFailureKind::ImportMismatch);
            CHECK(store.state() == StoreState::Live);
        };
        auto forward = resolved; // an instance naming a value after it
        std::get<isoline::component::runtime::HostInstanceImport>(forward.values[6]).exports.front().second =
            7;
        refusedWith(forward);
        auto stale = resolved; // an instance the store does not have
        stale.values[8] = isoline::component::runtime::InstanceRef { .index = 1000 };
        refusedWith(stale);
        auto foreign = resolved; // a component of another engine
        auto otherEngine = isoline::api::Engine { isoline::api::EngineConfig {
            .engine = engine, .mode = isoline::api::ExecutionMode::Deterministic } };
        auto loader = isoline::component::api::EngineCoreModuleLoader { otherEngine };
        auto validator = isoline::component::api::EngineCoreModuleValidator { otherEngine };
        auto loaded = isoline::component::api::Component::load(
            otherEngine, liftedIdentity().build(), {}, loader, validator);
        REQUIRE(loaded.has_value());
        foreign.values[1] = isoline::component::runtime::ComponentImport { .code = loaded->code() };
        refusedWith(foreign);
        auto wrongModule = resolved; // a component where a module goes
        wrongModule.values[0] = std::monostate {};
        refusedWith(wrongModule);
    }
}

TEST_CASE("ComponentStore.hostResourceTypesByIdentity")
{
    // Two imports of one host type are one runtime type (D8); the cap on resource types counts host
    // types and guest ones alike, before either is created.
    auto const engine = GENERATE(from_range(Engines));
    auto builder = ComponentBuilder {};
    auto const a = builder.import("a", SubResource);
    auto const b = builder.import("b", SubResource);
    std::ignore = builder.exportItem("ea", Sort::Type, a);
    std::ignore = builder.exportItem("eb", Sort::Type, b);
    auto const bytes = builder.build();
    auto const one = std::make_shared<HostResourceIdentity const>(HostResourceIdentity { .name = "one" });
    auto const two = std::make_shared<HostResourceIdentity const>(HostResourceIdentity { .name = "two" });
    auto const importing = [](std::shared_ptr<HostResourceIdentity const> const& first,
                              std::shared_ptr<HostResourceIdentity const> const& second) {
        auto imports = ResolvedImports {};
        imports.values.emplace_back(HostResourceImport { .identity = first });
        imports.values.emplace_back(HostResourceImport { .identity = second });
        imports.imports = { 0, 1 };
        return imports;
    };
    SECTION("one identity, one type")
    {
        auto harness = Harness { bytes, HarnessOptions { .engine = engine } };
        auto const root = harness.instance(importing(one, one));
        auto const exports = harness.store->exportsOf(root);
        REQUIRE(exports.size() == 2);
        CHECK(std::get<ResourceTypeIndex>(exports[0].value) == std::get<ResourceTypeIndex>(exports[1].value));
    }
    SECTION("the cap, for host types")
    {
        auto limits = ComponentLimits {};
        limits.maxResourceTypes = 1;
        auto harness = Harness { bytes, HarnessOptions { .engine = engine, .limits = limits } };
        auto const failure = failureOf(harness.instantiate(importing(one, two)));
        CHECK(failure.kind == InstantiationFailureKind::Trap);
        CHECK(std::get<isoline::component::canonical::Trap>(failure.detail).code == TrapCode::ResourceTypes);
        CHECK(harness.store->state() == StoreState::LockedDown);
    }
    SECTION("the cap, for guest types")
    {
        auto guest = ComponentBuilder {};
        std::ignore = guest.resource();
        std::ignore = guest.resource();
        auto limits = ComponentLimits {};
        limits.maxResourceTypes = 1;
        auto harness = Harness { guest.build(), HarnessOptions { .engine = engine, .limits = limits } };
        auto const failure = failureOf(harness.instantiate());
        CHECK(failure.kind == InstantiationFailureKind::Trap);
        CHECK(std::get<isoline::component::canonical::Trap>(failure.detail).code == TrapCode::ResourceTypes);
        CHECK(harness.store->state() == StoreState::LockedDown);
    }
}

TEST_CASE("ComponentStore.stepsChargedForSlotsAndWalks")
{
    auto const engine = GENERATE(from_range(Engines));
    SECTION("the root's slots, charged before its frame")
    {
        auto limits = ComponentLimits {};
        limits.maxInstantiationSteps = 1;
        auto harness =
            Harness { liftedIdentity().build(), HarnessOptions { .engine = engine, .limits = limits } };
        auto const failure = failureOf(harness.instantiate());
        CHECK(failure.kind == InstantiationFailureKind::LimitExceeded);
        CHECK(std::get<ComponentLimit>(failure.detail) == ComponentLimit::InstantiationSteps);
        CHECK(harness.store->resources().instances().size() == 0);
    }
    SECTION("an imported instance's resource exports, each walked")
    {
        constexpr auto Resources = uint32_t { 40 };
        auto scope = TypeScopeBuilder {};
        for (auto const index: std::views::iota(uint32_t { 0 }, Resources))
            std::ignore = scope.exportItem("r" + std::to_string(index), SubResource);
        auto builder = ComponentBuilder {};
        std::ignore = builder.import(
            "host", ExternDesc { .sort = Sort::Instance, .index = builder.instanceType(std::move(scope)) });
        auto limits = ComponentLimits {};
        limits.maxInstantiationSteps = 20;
        auto harness = Harness { builder.build(), HarnessOptions { .engine = engine, .limits = limits } };
        auto imports = ResolvedImports {};
        auto instance = isoline::component::runtime::HostInstanceImport {};
        for (auto const index: std::views::iota(uint32_t { 0 }, Resources))
        {
            imports.values.emplace_back(HostResourceImport {
                .identity =
                    std::make_shared<HostResourceIdentity const>(HostResourceIdentity { .name = "r" }) });
            instance.exports.emplace_back("r" + std::to_string(index), index);
        }
        imports.values.emplace_back(std::move(instance));
        imports.imports = { Resources };
        auto const failure = failureOf(harness.instantiate(imports));
        CHECK(failure.kind == InstantiationFailureKind::LimitExceeded);
        CHECK(std::get<ComponentLimit>(failure.detail) == ComponentLimit::InstantiationSteps);
        CHECK(harness.store->state() == StoreState::LockedDown);
    }
}

TEST_CASE("ComponentStore.startFunctionHostFailure")
{
    // A start function's lowered host function fails: the instantiation fails as that failure.
    auto const engine = GENERATE(from_range(Engines));
    auto builder = ComponentBuilder {};
    auto const type = functionType(builder, {}, PrimitiveValueType::U32);
    auto const f = builder.import("f", ExternDesc { .sort = Sort::Func, .index = type });
    auto const host = builder.coreInlineExports(
        { CoreInlineExport { .name = "f", .sort = CoreSort::Func, .index = builder.canonLower(f, {}) } });
    std::ignore = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "host" "f" (func $f (result i32)))
      (func $start (drop (call $f)))
      (start $start)))")),
                                          { { "host", host } });
    auto harness = Harness { builder.build(), HarnessOptions { .engine = engine } };
    auto failing = hostFunction(
        [](ComponentStore&, Values const&, ValueBuilder&) -> std::expected<void, isoline::api::HostFailure> {
            return std::unexpected { isoline::component::runtime::testing::embedderFailure(4) };
        });
    auto const failure = failureOf(harness.instantiate(importsOf({ failing })));
    CHECK(failure.kind == InstantiationFailureKind::HostFailure);
    CHECK(std::get<isoline::api::HostFailure>(failure.detail)
          == isoline::component::runtime::testing::embedderFailure(4));
    CHECK(harness.store->state() == StoreState::LockedDown);
}

TEST_CASE("ComponentStore.coreDefinitionRefused")
{
    // Lowering a function defines a host function in the core store, which its type cap may refuse.
    auto const engine = GENERATE(from_range(Engines));
    auto builder = ComponentBuilder {};
    auto lowered = std::vector<CoreInlineExport> {};
    for (auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 4 }))
    {
        auto params = std::vector<isoline::component::binary::ValueTypeRef>(index, PrimitiveValueType::U32);
        auto const type = functionType(builder, params, PrimitiveValueType::U32);
        auto const f =
            builder.import("f" + std::to_string(index), ExternDesc { .sort = Sort::Func, .index = type });
        lowered.push_back(CoreInlineExport { .name = "f" + std::to_string(index),
                                             .sort = CoreSort::Func,
                                             .index = builder.canonLower(f, {}) });
    }
    std::ignore = builder.coreInlineExports(std::move(lowered));
    auto storeLimits = isoline::api::StoreLimits {};
    storeLimits.maxTypes = 4;
    auto harness =
        Harness { builder.build(), HarnessOptions { .engine = engine, .storeLimits = storeLimits } };
    auto host = hostFunction([](ComponentStore&,
                                Values const&,
                                ValueBuilder& results) -> std::expected<void, isoline::api::HostFailure> {
        results.scalar(0);
        return {};
    });
    auto const failure = failureOf(harness.instantiate(importsOf({ host, host, host, host })));
    CHECK(failure.kind == InstantiationFailureKind::CoreDefinition);
    CHECK(harness.store->state() == StoreState::LockedDown);

    // A built-in too.
    auto builtins = ComponentBuilder {};
    auto const resource = builtins.resource();
    std::ignore = builtins.canon(isoline::component::binary::CanonDefinition {
        .builtin = isoline::component::binary::CanonBuiltin::ResourceNew,
        .immediates = { resource },
        .options = {},
        .result = std::nullopt });
    storeLimits.maxTypes = 1;
    auto tight =
        Harness { builtins.build(), HarnessOptions { .engine = engine, .storeLimits = storeLimits } };
    CHECK(failureOf(tight.instantiate()).kind == InstantiationFailureKind::CoreDefinition);
    CHECK(tight.store->state() == StoreState::LockedDown);
}
