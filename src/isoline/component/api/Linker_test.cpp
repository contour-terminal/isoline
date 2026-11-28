// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/api/testing/ApiFixtures.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/ComponentFixtures.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/testing/ResourceDoubles.hpp>
#include <isoline/component/runtime/testing/StoreFixtures.hpp>

using isoline::component::api::ComponentCallFailure;
using isoline::component::api::ComponentConfig;
using isoline::component::api::ForeignInstance;
using isoline::component::api::FunctionTypeSpec;
using isoline::component::api::HostComponentContext;
using isoline::component::api::HostResourceType;
using isoline::component::api::ImportOutcome;
using isoline::component::api::ImportResolution;
using isoline::component::api::ImportTypeMismatch;
using isoline::component::api::Linker;
using isoline::component::api::LinkerErrorCode;
using isoline::component::api::ParameterNames;
using isoline::component::api::Satisfied;
using isoline::component::api::SatisfiedByTypes;
using isoline::component::api::TypeMismatch;
using isoline::component::api::Unsatisfied;
using isoline::component::api::UnsatisfiedImport;
using isoline::component::api::testing::ApiHarness;
using isoline::component::api::testing::callWith;
using isoline::component::api::testing::constantFunction;
using isoline::component::api::testing::functionSpec;
using isoline::component::api::testing::hostFunction;
using isoline::component::api::testing::instanceOf;
using isoline::component::api::testing::lookup;
using isoline::component::api::testing::primitive;
using isoline::component::api::testing::refusalAs;
using isoline::component::api::testing::require;
using isoline::component::api::testing::u32Of;
using isoline::component::binary::Asyncness;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreSort;
using isoline::component::binary::DefinedValueType;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::FunctionTypeDefinition;
using isoline::component::binary::Implements;
using isoline::component::binary::ImportDefinition;
using isoline::component::binary::LabeledType;
using isoline::component::binary::NameAttributes;
using isoline::component::binary::OwnType;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::Sort;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::liftedIdentity;
using isoline::component::binary::testing::TypeScopeBuilder;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::ValueBuilder;
using isoline::component::canonical::Values;
using isoline::component::runtime::testing::coreModule;
using isoline::component::runtime::testing::functionType;
using isoline::component::runtime::testing::RecordingDestructor;
using isoline::component::types::ValueKind;

namespace
{

using HostResult = std::expected<void, isoline::api::HostFailure>;

/// Exports `run: func() -> u32`, which calls the component function @p function of that type.
void exportRunCalling(ComponentBuilder& builder, uint32_t function)
{
    auto const lowered = builder.coreInlineExports({ CoreInlineExport {
        .name = "f", .sort = CoreSort::Func, .index = builder.canonLower(function, {}) } });
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
}

/// A component importing the instance @p name, implementing @p implements if given, whose type exports
/// `f: func() -> u32`; its export `run` returns what `f` returns.
[[nodiscard]] std::vector<std::byte> instanceUser(std::string const& name,
                                                  std::optional<std::string> const& implements = {})
{
    auto builder = ComponentBuilder {};
    auto scope = TypeScopeBuilder {};
    std::ignore = scope.exportItem(
        "f", ExternDesc { .sort = Sort::Func, .index = functionType(scope, {}, PrimitiveValueType::U32) });
    auto const type = ExternDesc { .sort = Sort::Instance, .index = builder.instanceType(std::move(scope)) };
    auto attributes = NameAttributes { .name = name, .attributes = {} };
    if (implements)
        attributes.attributes.emplace_back(Implements { .interfaceName = *implements });
    auto const instance =
        builder.definition(ImportDefinition { .name = std::move(attributes), .type = type });
    exportRunCalling(builder, builder.aliasExport(instance, "f", Sort::Func));
    return builder.build();
}

/// The outcome of resolving a component's single import.
[[nodiscard]] ImportOutcome outcomeOf(Linker const& linker,
                                      isoline::component::api::Component const& component)
{
    auto const resolutions = linker.resolve(component);
    REQUIRE(resolutions.size() == 1);
    return resolutions.front().outcome;
}

/// A linker defining `<interface>@<version>#f` returning each value, for every (version, value).
[[nodiscard]] Linker versionedLinker(std::vector<std::pair<std::string, uint32_t>> const& versions)
{
    auto linker = Linker { ComponentConfig {} };
    for (auto const& [version, value]: versions)
        require(linker.defineFunction("ns:p/i@" + version + "#f",
                                      functionSpec({}, primitive(linker, ValueKind::U32)),
                                      constantFunction(value)));
    return linker;
}

/// The code of a refused definition (REQUIREd to be refused).
template <typename T>
[[nodiscard]] LinkerErrorCode refusalCode(
    std::expected<T, isoline::component::api::LinkerError> const& defined)
{
    REQUIRE_FALSE(defined.has_value());
    return defined.error().code;
}

} // namespace

TEST_CASE("Linker.semverPicksHighestCompatible")
{
    auto harness = ApiHarness {};
    auto store = harness.store();
    auto const linker = versionedLinker({ { "0.1.0", 10 }, { "0.1.2", 12 }, { "0.2.0", 20 } });

    auto const compatible = harness.load(instanceUser("ns:p/i@0.1.1"));
    CHECK(linker.resolve(compatible)
          == std::vector { ImportResolution { .name = "ns:p/i@0.1.1",
                                              .outcome = Satisfied { .matched = "ns:p/i@0.1.2" } } });
    auto instance = instanceOf(store.instantiate(compatible, linker));
    CHECK(u32Of(callWith(instance, "run")) == 12);

    // No compatible version: 0.3.0 shares no major.minor with any; 0.0.N matches only itself.
    auto const newer = harness.load(instanceUser("ns:p/i@0.3.0"));
    CHECK(outcomeOf(linker, newer) == ImportOutcome { Unsatisfied {} });
    CHECK(refusalAs<UnsatisfiedImport>(store.instantiate(newer, linker)).name == "ns:p/i@0.3.0");
    CHECK_FALSE(store.isLockedDown());
    auto const patch = versionedLinker({ { "0.0.2", 2 } });
    CHECK(outcomeOf(patch, harness.load(instanceUser("ns:p/i@0.0.1"))) == ImportOutcome { Unsatisfied {} });
    CHECK(outcomeOf(patch, harness.load(instanceUser("ns:p/i@0.0.2")))
          == ImportOutcome { Satisfied { .matched = "ns:p/i@0.0.2" } });

    // The exact name wins over a higher compatible version; from 1.0.0 the major decides.
    auto const exact = versionedLinker({ { "0.1.1", 11 }, { "0.1.2", 12 } });
    CHECK(outcomeOf(exact, compatible) == ImportOutcome { Satisfied { .matched = "ns:p/i@0.1.1" } });
    auto const major = versionedLinker({ { "1.0.0", 100 }, { "1.4.2", 142 }, { "2.0.0", 200 } });
    CHECK(outcomeOf(major, harness.load(instanceUser("ns:p/i@1.2.0")))
          == ImportOutcome { Satisfied { .matched = "ns:p/i@1.4.2" } });
}

TEST_CASE("Linker.implementsFallback")
{
    auto harness = ApiHarness {};
    auto store = harness.store();
    auto const component = harness.load(instanceUser("primary", "ns:p/i@1.0.0"));

    // No definition of `primary`: the interface it implements, at the highest compatible version.
    auto const linker = versionedLinker({ { "1.2.0", 7 } });
    CHECK(outcomeOf(linker, component) == ImportOutcome { Satisfied { .matched = "ns:p/i@1.2.0" } });
    auto instance = instanceOf(store.instantiate(component, linker));
    CHECK(u32Of(callWith(instance, "run")) == 7);

    // Its own name first.
    auto named = versionedLinker({ { "1.2.0", 7 } });
    require(named.defineFunction(
        "primary#f", functionSpec({}, primitive(named, ValueKind::U32)), constantFunction(9)));
    CHECK(outcomeOf(named, component) == ImportOutcome { Satisfied { .matched = "primary" } });
    auto other = instanceOf(store.instantiate(component, named));
    CHECK(u32Of(callWith(other, "run")) == 9);

    // An import that implements nothing has no fallback.
    CHECK(outcomeOf(linker, harness.load(instanceUser("primary"))) == ImportOutcome { Unsatisfied {} });
}

TEST_CASE("Linker.typeOnlyInterfaceImport")
{
    auto harness = ApiHarness {};
    auto store = harness.store();
    auto builder = ComponentBuilder {};
    auto scope = TypeScopeBuilder {};
    std::ignore = scope.exportItem(
        "t",
        ExternDesc {
            .sort = Sort::Type,
            .index = scope.type(TypeDefinition { .type = DefinedValueType { PrimitiveValueType::U32 } }) });
    std::ignore = builder.import(
        "ns:p/types@0.1.0",
        ExternDesc { .sort = Sort::Instance, .index = builder.instanceType(std::move(scope)) });
    auto const component = harness.load(builder.build());
    auto const linker = Linker { ComponentConfig {} };
    CHECK(outcomeOf(linker, component) == ImportOutcome { SatisfiedByTypes {} });
    CHECK(store.instantiate(component, linker).has_value());

    // An instance that exports a function needs its definition.
    CHECK(outcomeOf(linker, harness.load(instanceUser("ns:p/types@0.1.0")))
          == ImportOutcome { Unsatisfied {} });
}

TEST_CASE("Linker.typeMismatchNamed")
{
    auto harness = ApiHarness {};
    auto store = harness.store();
    auto builder = ComponentBuilder {};
    std::ignore = builder.import(
        "f",
        ExternDesc { .sort = Sort::Func,
                     .index = functionType(builder, { PrimitiveValueType::U32 }, PrimitiveValueType::U32) });
    auto const component = harness.load(builder.build());
    auto const mismatchOf = [&](Linker const& linker) {
        auto const outcome = outcomeOf(linker, component);
        auto const* mismatch = std::get_if<TypeMismatch>(&outcome);
        REQUIRE(mismatch != nullptr);
        CHECK(mismatch->matched == "f");
        auto const refused = store.instantiate(component, linker);
        REQUIRE_FALSE(refused.has_value());
        auto const* named = std::get_if<ImportTypeMismatch>(&refused.error());
        REQUIRE(named != nullptr);
        CHECK(named->name == "f");
        CHECK(named->detail == mismatch->detail);
        CHECK_FALSE(store.isLockedDown());
        return mismatch->detail;
    };

    SECTION("a parameter of another type")
    {
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineFunction(
            "f",
            functionSpec({ primitive(linker, ValueKind::U64) }, primitive(linker, ValueKind::U32)),
            constantFunction(1)));
        CHECK(mismatchOf(linker)
              == "type mismatch in function parameter `p0`\nexpected primitive `u32` found primitive `u64`");
    }
    SECTION("a parameter of another name")
    {
        auto linker = Linker { ComponentConfig {} };
        require(
            linker.defineFunction("f",
                                  FunctionTypeSpec { .params = { { "x", primitive(linker, ValueKind::U32) } },
                                                     .result = primitive(linker, ValueKind::U32) },
                                  constantFunction(1)));
        CHECK(mismatchOf(linker) == "expected parameter named `p0`, found `x`");
    }
    SECTION("an unnamed definition of another type")
    {
        // Unnamed parameters match any name, never any type: the import's name gives the context.
        auto linker = Linker { ComponentConfig {} };
        require(
            linker.defineFunction("f",
                                  FunctionTypeSpec { .params = { { "x", primitive(linker, ValueKind::U64) } },
                                                     .result = primitive(linker, ValueKind::U32),
                                                     .names = ParameterNames::Unnamed },
                                  constantFunction(1)));
        CHECK(mismatchOf(linker)
              == "type mismatch in function parameter `p0`\nexpected primitive `u32` found primitive `u64`");
    }
    SECTION("an unnamed definition of another parameter count")
    {
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineFunction("f",
                                      FunctionTypeSpec { .params = {},
                                                         .result = primitive(linker, ValueKind::U32),
                                                         .names = ParameterNames::Unnamed },
                                      constantFunction(1)));
        CHECK(mismatchOf(linker) == "expected 1 parameters, found 0");
    }
    SECTION("an unnamed definition without the result")
    {
        auto linker = Linker { ComponentConfig {} };
        require(
            linker.defineFunction("f",
                                  FunctionTypeSpec { .params = { { "x", primitive(linker, ValueKind::U32) } },
                                                     .result = std::nullopt,
                                                     .names = ParameterNames::Unnamed },
                                  constantFunction(1)));
        CHECK(mismatchOf(linker) == "expected no result, found one");
    }
    SECTION("an unnamed definition of another result type")
    {
        auto linker = Linker { ComponentConfig {} };
        require(
            linker.defineFunction("f",
                                  FunctionTypeSpec { .params = { { "x", primitive(linker, ValueKind::U32) } },
                                                     .result = primitive(linker, ValueKind::S32),
                                                     .names = ParameterNames::Unnamed },
                                  constantFunction(1)));
        CHECK(mismatchOf(linker)
              == "type mismatch with result type\nexpected primitive `u32` found primitive `s32`");
    }
    SECTION("a definition of another sort")
    {
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineResource("f", HostResourceType { "f", nullptr }));
        CHECK(mismatchOf(linker) == "expected func, found resource");
    }
    SECTION("a linker instance")
    {
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineFunction("f#g", functionSpec({}, std::nullopt), constantFunction(1)));
        CHECK(mismatchOf(linker) == "expected func, found instance");
    }
    SECTION("no definition")
    {
        auto const linker = Linker { ComponentConfig {} };
        CHECK(refusalAs<UnsatisfiedImport>(store.instantiate(component, linker)).name == "f");
    }
}

TEST_CASE("Linker.instanceExportsAreChecked")
{
    // An instance's exports are matched one by one: a missing one, and one of another type, name the
    // export they were found at, outermost first.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto inner = TypeScopeBuilder {};
    std::ignore = inner.exportItem(
        "f", ExternDesc { .sort = Sort::Func, .index = functionType(inner, {}, PrimitiveValueType::U32) });
    // A type the linker need not define.
    std::ignore = inner.exportItem(
        "t",
        ExternDesc {
            .sort = Sort::Type,
            .index = inner.type(TypeDefinition { .type = DefinedValueType { PrimitiveValueType::U32 } }) });
    auto outer = TypeScopeBuilder {};
    std::ignore = outer.exportItem(
        "nested", ExternDesc { .sort = Sort::Instance, .index = outer.instanceType(std::move(inner)) });
    auto const host = builder.import(
        "host", ExternDesc { .sort = Sort::Instance, .index = builder.instanceType(std::move(outer)) });
    exportRunCalling(
        builder, builder.aliasExport(builder.aliasExport(host, "nested", Sort::Instance), "f", Sort::Func));
    auto const component = harness.load(builder.build());

    auto linker = Linker { ComponentConfig {} };
    require(linker.defineFunction(
        "host#nested#f", functionSpec({}, primitive(linker, ValueKind::U32)), constantFunction(4)));
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, linker));
    CHECK(u32Of(callWith(instance, "run")) == 4);

    auto missing = Linker { ComponentConfig {} };
    require(missing.defineFunction(
        "host#nested#g", functionSpec({}, primitive(missing, ValueKind::U32)), constantFunction(4)));
    CHECK(outcomeOf(missing, component)
          == ImportOutcome { TypeMismatch { .matched = "host",
                                            .detail = "type mismatch in instance export `nested`\n"
                                                      "missing expected export `f`" } });
    auto mistyped = Linker { ComponentConfig {} };
    require(mistyped.defineFunction(
        "host#nested#f", functionSpec({}, primitive(mistyped, ValueKind::S32)), constantFunction(4)));
    CHECK(outcomeOf(mistyped, component)
          == ImportOutcome { TypeMismatch { .matched = "host",
                                            .detail = "type mismatch in instance export `nested`\n"
                                                      "type mismatch in instance export `f`\n"
                                                      "type mismatch with result type\n"
                                                      "expected primitive `u32` found primitive `s32`" } });
}

TEST_CASE("Linker.foreignInstanceRefused")
{
    auto harness = ApiHarness {};
    auto provider = harness.load(liftedIdentity().build());
    auto storeA = harness.store();
    auto storeB = harness.store();
    auto const provided = instanceOf(storeA.instantiate(provider, Linker { ComponentConfig {} }));

    // A component importing `prov` whose `id: func(x: u32) -> u32` its `run(p0: u32)` calls.
    auto builder = ComponentBuilder {};
    auto scope = TypeScopeBuilder {};
    std::ignore = scope.exportItem(
        "id",
        ExternDesc { .sort = Sort::Func,
                     .index = scope.type(TypeDefinition {
                         .type = FunctionTypeDefinition {
                             .asyncness = Asyncness::Sync,
                             .params = { LabeledType { .label = "x", .type = PrimitiveValueType::U32 } },
                             .result = PrimitiveValueType::U32 } }) });
    auto const prov = builder.import(
        "prov", ExternDesc { .sort = Sort::Instance, .index = builder.instanceType(std::move(scope)) });
    auto const lowered = builder.coreInlineExports({ CoreInlineExport {
        .name = "id",
        .sort = CoreSort::Func,
        .index = builder.canonLower(builder.aliasExport(prov, "id", Sort::Func), {}) } });
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "p" "id" (func $id (param i32) (result i32)))
      (func (export "run") (param i32) (result i32) (call $id (local.get 0)))))")),
                                              { { "p", lowered } });
    std::ignore = builder.exportItem(
        "run",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(core, "run", CoreSort::Func),
                          {},
                          functionType(builder, { PrimitiveValueType::U32 }, PrimitiveValueType::U32)));
    auto const consumer = harness.load(builder.build());

    auto linker = Linker { ComponentConfig {} };
    require(linker.defineInstance("prov", provided));
    CHECK(outcomeOf(linker, consumer) == ImportOutcome { Satisfied { .matched = "prov" } });
    CHECK(provided.belongsTo(storeA));
    CHECK_FALSE(provided.belongsTo(storeB));
    CHECK(refusalAs<ForeignInstance>(storeB.instantiate(consumer, linker)).name == "prov");
    CHECK_FALSE(storeB.isLockedDown());

    auto instance = instanceOf(storeA.instantiate(consumer, linker));
    CHECK(u32Of(callWith(instance, "run", { 5 })) == 5);
    // A function of another store's instance is refused by the call, before anything runs.
    auto const foreign = lookup(instance, "run");
    REQUIRE(foreign.has_value());
    auto arguments = instance.argumentsFor(*foreign);
    REQUIRE(arguments.has_value());
    arguments->scalar(1);
    auto const built = arguments->finish();
    REQUIRE(built.has_value());
    auto other = instanceOf(storeB.instantiate(provider, Linker { ComponentConfig {} }));
    auto const foreignBuilder = other.argumentsFor(*foreign);
    REQUIRE_FALSE(foreignBuilder.has_value());
    CHECK(foreignBuilder.error().failure == ComponentCallFailure::ForeignFunction);
    auto const called = other.call(*foreign, *built);
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().failure == ComponentCallFailure::ForeignFunction);
    CHECK_FALSE(storeB.isLockedDown());
}

TEST_CASE("Linker.hostResourceIdentityShared")
{
    // make() -> own<resource1>, take(p0: own<resource1-again>) -> u32; run() passes make's handle to
    // take. One HostResourceType defines both: one resource type, so the own passes.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const subResource = ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource };
    auto const first = builder.import("resource1", subResource);
    auto const again = builder.import("resource1-again", subResource);
    auto const ownFirst =
        builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = first } } });
    auto const ownAgain =
        builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = again } } });
    auto const make =
        builder.import("make",
                       ExternDesc { .sort = Sort::Func,
                                    .index = functionType(builder, {}, TypeIndex { .index = ownFirst }) });
    auto const take = builder.import(
        "take",
        ExternDesc {
            .sort = Sort::Func,
            .index = functionType(builder, { TypeIndex { .index = ownAgain } }, PrimitiveValueType::U32) });
    auto const lowered = builder.coreInlineExports({
        CoreInlineExport { .name = "make", .sort = CoreSort::Func, .index = builder.canonLower(make, {}) },
        CoreInlineExport { .name = "take", .sort = CoreSort::Func, .index = builder.canonLower(take, {}) },
    });
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "make" (func $make (result i32)))
      (import "h" "take" (func $take (param i32) (result i32)))
      (func (export "run") (result i32) (call $take (call $make)))))")),
                                              { { "h", lowered } });
    std::ignore = builder.exportItem("run",
                                     Sort::Func,
                                     builder.canonLift(builder.aliasCoreExport(core, "run", CoreSort::Func),
                                                       {},
                                                       functionType(builder, {}, PrimitiveValueType::U32)));
    auto const component = harness.load(builder.build());

    auto const destructor = std::make_shared<RecordingDestructor>();
    auto const resource = HostResourceType { "resource1", destructor };
    auto const linkWith = [&](HostResourceType const& againType) {
        auto linker = Linker { ComponentConfig {} };
        require(linker.defineResource("resource1", resource));
        require(linker.defineResource("resource1-again", againType));
        auto const ownOf = [&](HostResourceType const& type) {
            auto own = linker.ownType(type);
            REQUIRE(own.has_value());
            return *own;
        };
        require(linker.defineFunction(
            "make",
            functionSpec({}, ownOf(resource)),
            hostFunction(
                [&](HostComponentContext& context, Values const&, ValueBuilder& results) -> HostResult {
                    auto own = context.newOwn(resource, 42);
                    if (!own)
                        return std::unexpected { isoline::component::runtime::testing::embedderFailure() };
                    results.resource(*own);
                    return {};
                })));
        require(linker.defineFunction(
            "take",
            functionSpec({ ownOf(againType) }, primitive(linker, ValueKind::U32)),
            hostFunction(
                [](HostComponentContext& context, Values const& args, ValueBuilder& results) -> HostResult {
                    // The own is the host's now: it drops it.
                    auto const own = args.root(0).resource();
                    results.scalar(own.rep);
                    if (!context.store().drop(own))
                        return std::unexpected { isoline::component::runtime::testing::embedderFailure() };
                    return {};
                })));
        return linker;
    };

    SECTION("copies of one type")
    {
        auto const linker = linkWith(resource);
        auto store = harness.store();
        auto instance = instanceOf(store.instantiate(component, linker));
        CHECK(u32Of(callWith(instance, "run")) == 42);
        CHECK(destructor->destroyed == std::vector<uint32_t> { 42 });
    }
    SECTION("two types of one name")
    {
        auto const linker = linkWith(HostResourceType { "resource1", destructor });
        auto store = harness.store();
        auto instance = instanceOf(store.instantiate(component, linker));
        auto const called = callWith(instance, "run");
        REQUIRE_FALSE(called.has_value());
        CHECK(called.error().failure == ComponentCallFailure::Trap);
        CHECK(std::get<isoline::component::canonical::Trap>(called.error().detail).code
              == TrapCode::WrongHandleType);
        CHECK(store.isLockedDown());
    }
}

TEST_CASE("Linker.definitionsAreChecked")
{
    // Every misuse is a named refusal, and the linker is as it was.
    auto linker = Linker { ComponentConfig {} };
    auto const u32 = primitive(linker, ValueKind::U32);
    require(linker.defineFunction("a", functionSpec({}, u32), constantFunction(1)));
    require(linker.defineFunction("i#f", functionSpec({}, u32), constantFunction(1)));
    auto const refusedWith = [&](std::expected<void, isoline::component::api::LinkerError> const& defined) {
        REQUIRE_FALSE(defined.has_value());
        CHECK_FALSE(defined.error().message().empty());
        return defined.error().code;
    };
    auto const define = [&](std::string path) {
        return linker.defineFunction(std::move(path), functionSpec({}, u32), constantFunction(2));
    };
    CHECK(refusedWith(define("a")) == LinkerErrorCode::Redefinition);
    CHECK(refusedWith(define("a#b")) == LinkerErrorCode::PathConflict);
    CHECK(refusedWith(define("i")) == LinkerErrorCode::PathConflict);
    CHECK(refusedWith(linker.defineResource("i", HostResourceType { "i", nullptr }))
          == LinkerErrorCode::PathConflict);
    CHECK(refusedWith(define("")) == LinkerErrorCode::InvalidPath);
    CHECK(refusedWith(define("i##f")) == LinkerErrorCode::InvalidPath);
    CHECK(refusedWith(define("i#")) == LinkerErrorCode::InvalidPath);
    CHECK(refusedWith(define("Not-Kebab")) == LinkerErrorCode::InvalidPath);
    CHECK(refusedWith(define("ns:p/i@1.x")) == LinkerErrorCode::InvalidPath);
    CHECK(refusedWith(define(std::string { "\xff\xfe" })) == LinkerErrorCode::InvalidPath);
    CHECK(refusedWith(linker.defineFunction("n", functionSpec({}, u32), nullptr))
          == LinkerErrorCode::NullDefinition);
    CHECK(refusedWith(linker.defineFunction(
              "p", FunctionTypeSpec { .params = { { "Bad", u32 } }, .result = u32 }, constantFunction(1)))
          == LinkerErrorCode::InvalidParameterName);
    CHECK(refusedWith(linker.defineFunction(
              "p",
              FunctionTypeSpec { .params = { { "x", u32 }, { "x", u32 } }, .result = u32 },
              constantFunction(1)))
          == LinkerErrorCode::InvalidParameterName);
    auto const foreignType = isoline::component::types::TypeId { .value = 1'000'000 };
    CHECK(refusedWith(linker.defineFunction("p", functionSpec({ foreignType }, u32), constantFunction(1)))
          == LinkerErrorCode::InvalidType);
    CHECK(refusedWith(linker.defineFunction("p", functionSpec({}, foreignType), constantFunction(1)))
          == LinkerErrorCode::InvalidType);
    CHECK(refusedWith(linker.defineResource("r", HostResourceType { std::string { "\xc0\x80" }, nullptr }))
          == LinkerErrorCode::InvalidResourceName);
    // The refused paths stayed free.
    require(linker.defineFunction("p", functionSpec({}, u32), constantFunction(3)));
    require(linker.defineResource("r", HostResourceType { "r", nullptr }));
}

TEST_CASE("Linker.unnamedParametersMatchAnyName")
{
    // A host function of ParameterNames::Unnamed, as Wasmtime's func_wrap defines one, satisfies imports
    // whatever their parameters are called: two components naming f's parameter `a` and `b` both link it.
    auto harness = ApiHarness {};
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineFunction(
        "f",
        FunctionTypeSpec { .params = { { "host-label", primitive(linker, ValueKind::U32) } },
                           .result = primitive(linker, ValueKind::U32),
                           .names = ParameterNames::Unnamed },
        hostFunction([](HostComponentContext&,
                        isoline::component::canonical::Values const& args,
                        isoline::component::canonical::ValueBuilder& results)
                         -> std::expected<void, isoline::api::HostFailure> {
            results.scalar(args.root(0).scalar<uint32_t>() + 1);
            return {};
        })));
    for (auto const* label: { "a", "b" })
    {
        INFO(label);
        auto builder = ComponentBuilder {};
        auto const imported = builder.import(
            "f",
            ExternDesc {
                .sort = Sort::Func,
                .index = builder.type(TypeDefinition {
                    .type = FunctionTypeDefinition {
                        .asyncness = Asyncness::Sync,
                        .params = { LabeledType { .label = label, .type = PrimitiveValueType::U32 } },
                        .result = PrimitiveValueType::U32 } }) });
        std::ignore = imported;
        auto const component = harness.load(builder.build());
        CHECK(outcomeOf(linker, component) == ImportOutcome { Satisfied { .matched = "f" } });
        auto store = harness.store();
        CHECK(store.instantiate(component, linker).has_value());
    }
}

TEST_CASE("Linker.storeInstanceExportsAreChecked")
{
    // An instance of a store is matched export by export against the instance type imported.
    auto harness = ApiHarness {};
    auto store = harness.store();
    auto const provided =
        instanceOf(store.instantiate(harness.load(liftedIdentity().build()), Linker { ComponentConfig {} }));
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineInstance("prov", provided));
    auto const importing = [&](std::string const& name,
                               PrimitiveValueType param,
                               std::string_view label = "x") {
        auto builder = ComponentBuilder {};
        auto scope = TypeScopeBuilder {};
        std::ignore = scope.exportItem(
            name,
            ExternDesc { .sort = Sort::Func,
                         .index = scope.type(TypeDefinition {
                             .type = FunctionTypeDefinition {
                                 .asyncness = Asyncness::Sync,
                                 .params = { LabeledType { .label = std::string { label }, .type = param } },
                                 .result = PrimitiveValueType::U32 } }) });
        std::ignore = builder.import(
            "prov", ExternDesc { .sort = Sort::Instance, .index = builder.instanceType(std::move(scope)) });
        return harness.load(builder.build());
    };
    CHECK(outcomeOf(linker, importing("id", PrimitiveValueType::U32))
          == ImportOutcome { Satisfied { .matched = "prov" } });
    CHECK(outcomeOf(linker, importing("id", PrimitiveValueType::S32))
          == ImportOutcome { TypeMismatch { .matched = "prov",
                                            .detail = "type mismatch in instance export `id`\n"
                                                      "type mismatch in function parameter `x`\n"
                                                      "expected primitive `s32` found primitive `u32`" } });
    // Between two guests, parameter names are compared, as a functype's are (Explainer.md).
    CHECK(outcomeOf(linker, importing("id", PrimitiveValueType::U32, "y"))
          == ImportOutcome { TypeMismatch { .matched = "prov",
                                            .detail = "type mismatch in instance export `id`\n"
                                                      "expected parameter named `y`, found `x`" } });
    CHECK(outcomeOf(linker, importing("missing", PrimitiveValueType::U32))
          == ImportOutcome {
              TypeMismatch { .matched = "prov", .detail = "missing expected export `missing`" } });
}

TEST_CASE("Linker.resourceAliasMustMatch")
{
    // `r-alias` is `(eq r)`: one resource, so the linker must define both with one resource type.
    auto harness = ApiHarness {};
    auto builder = ComponentBuilder {};
    auto const r =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    std::ignore =
        builder.import("r-alias", ExternDesc { .sort = Sort::Type, .index = r, .bound = TypeBound::Equal });
    auto const component = harness.load(builder.build());
    auto const type = HostResourceType { "r", nullptr };
    auto same = Linker { ComponentConfig {} };
    require(same.defineResource("r", type));
    require(same.defineResource("r-alias", type));
    CHECK(same.resolve(component)
          == std::vector {
              ImportResolution { .name = "r", .outcome = Satisfied { .matched = "r" } },
              ImportResolution { .name = "r-alias", .outcome = Satisfied { .matched = "r-alias" } } });
    auto other = Linker { ComponentConfig {} };
    require(other.defineResource("r", type));
    require(other.defineResource("r-alias", HostResourceType { "r", nullptr }));
    CHECK(other.resolve(component).back().outcome
          == ImportOutcome {
              TypeMismatch { .matched = "r-alias", .detail = "resource types are not the same" } });
}

TEST_CASE("Linker.storeInstanceResourcesBound")
{
    // An instance of a store exporting a resource satisfies an import of `(sub resource)` by name.
    auto harness = ApiHarness {};
    auto store = harness.store();
    auto provider = ComponentBuilder {};
    std::ignore = provider.exportItem("r", Sort::Type, provider.resource());
    auto const provided =
        instanceOf(store.instantiate(harness.load(provider.build()), Linker { ComponentConfig {} }));
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineInstance("prov", provided));
    auto builder = ComponentBuilder {};
    auto scope = TypeScopeBuilder {};
    std::ignore =
        scope.exportItem("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    std::ignore = builder.import(
        "prov", ExternDesc { .sort = Sort::Instance, .index = builder.instanceType(std::move(scope)) });
    auto const consumer = harness.load(builder.build());
    CHECK(outcomeOf(linker, consumer) == ImportOutcome { Satisfied { .matched = "prov" } });
    CHECK(store.instantiate(consumer, linker).has_value());
}

TEST_CASE("Linker.capsAndMoves")
{
    // The linker's arena keeps to its caps: a refusal names the cap, and the linker is as it was.
    auto limits = isoline::component::ComponentLimits {};
    limits.maxResourceTypes = 1;
    auto linker = Linker { ComponentConfig { .limits = limits } };
    require(linker.defineResource("a", HostResourceType { "a", nullptr }));
    auto const second = linker.defineResource("b", HostResourceType { "b", nullptr });
    REQUIRE_FALSE(second.has_value());
    CHECK(refusalCode(second) == LinkerErrorCode::TypeLimit);
    auto const own = linker.ownType(HostResourceType { "c", nullptr });
    REQUIRE_FALSE(own.has_value());
    CHECK(refusalCode(own) == LinkerErrorCode::TypeLimit);

    auto small = isoline::component::ComponentLimits {};
    small.maxTypeSize = 3;
    auto sized = Linker { ComponentConfig { .limits = small } };
    auto const u32 = primitive(sized, ValueKind::U32);
    auto const wide = sized.defineFunction("f", functionSpec({ u32, u32, u32 }, u32), constantFunction(1));
    REQUIRE_FALSE(wide.has_value());
    CHECK(refusalCode(wide) == LinkerErrorCode::TypeLimit);

    // The paths of the other definitions are checked as a function's are.
    auto harness = ApiHarness {};
    auto store = harness.store();
    auto const component = harness.load(liftedIdentity().build());
    auto const instance = instanceOf(store.instantiate(component, Linker { ComponentConfig {} }));
    auto const module = harness.engine.load(isoline::component::binary::testing::identityModule());
    REQUIRE(module.has_value());
    CHECK(refusalCode(sized.defineInstance("Bad", instance)) == LinkerErrorCode::InvalidPath);
    CHECK(refusalCode(sized.defineModule("Bad", *module)) == LinkerErrorCode::InvalidPath);
    CHECK(refusalCode(sized.defineComponent("Bad", component)) == LinkerErrorCode::InvalidPath);

    // A moved linker keeps its definitions.
    auto moved = std::move(linker);
    auto const& view = moved;
    CHECK(view.types().valueTypeCount() > 0);
    auto assigned = Linker { ComponentConfig {} };
    assigned = std::move(moved);
    CHECK(
        refusalAs<UnsatisfiedImport>(store.instantiate(harness.load(instanceUser("ns:p/i@1.0.0")), assigned))
            .name
        == "ns:p/i@1.0.0");
    CHECK(refusalCode(assigned.defineResource("a", HostResourceType { "a", nullptr }))
          == LinkerErrorCode::Redefinition);
}

TEST_CASE("Linker.definitionKindsAreChecked")
{
    // An instance import defined as a function, and a type import defined anyway, which needs nothing.
    auto harness = ApiHarness {};
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineFunction(
        "host", functionSpec({}, primitive(linker, ValueKind::U32)), constantFunction(1)));
    require(linker.defineResource("t", HostResourceType { "t", nullptr }));
    CHECK(outcomeOf(linker, harness.load(instanceUser("host")))
          == ImportOutcome { TypeMismatch { .matched = "host", .detail = "expected instance, found func" } });
    auto builder = ComponentBuilder {};
    std::ignore = builder.import("t",
                                 ExternDesc { .sort = Sort::Type,
                                              .index = builder.type(TypeDefinition {
                                                  .type = DefinedValueType { PrimitiveValueType::U32 } }) });
    CHECK(outcomeOf(linker, harness.load(builder.build())) == ImportOutcome { Satisfied { .matched = "t" } });
}

TEST_CASE("Linker.componentOfAnotherEngineRefused")
{
    // A component defined for a component-typed import must be of the store's engine.
    auto harness = ApiHarness {};
    auto otherEngine = isoline::api::Engine { isoline::api::EngineConfig {
        .mode = isoline::api::ExecutionMode::Deterministic } };
    auto loader = isoline::component::api::EngineCoreModuleLoader { otherEngine };
    auto validator = isoline::component::api::EngineCoreModuleValidator { otherEngine };
    auto foreign = isoline::component::api::Component::load(
        otherEngine, liftedIdentity().build(), {}, loader, validator);
    REQUIRE(foreign.has_value());
    auto builder = ComponentBuilder {};
    auto scope = TypeScopeBuilder {};
    std::ignore = scope.exportItem(
        "id",
        ExternDesc { .sort = Sort::Func,
                     .index = scope.type(TypeDefinition {
                         .type = FunctionTypeDefinition {
                             .asyncness = Asyncness::Sync,
                             .params = { LabeledType { .label = "x", .type = PrimitiveValueType::U32 } },
                             .result = PrimitiveValueType::U32 } }) });
    std::ignore = builder.import(
        "c", ExternDesc { .sort = Sort::Component, .index = builder.componentType(std::move(scope)) });
    auto linker = Linker { ComponentConfig {} };
    require(linker.defineComponent("c", *foreign));
    auto store = harness.store();
    CHECK(refusalAs<isoline::api::EngineMismatch>(store.instantiate(harness.load(builder.build()), linker))
          == isoline::api::EngineMismatch {});
    CHECK_FALSE(store.isLockedDown());
}
