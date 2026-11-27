// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/ComponentFixtures.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>
#include <isoline/component/runtime/ComponentPlan.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/ValidatedComponent.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/component/validate/testing/CoreModuleValidator.hpp>

using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentLimits;
using isoline::component::binary::CanonBuiltin;
using isoline::component::binary::CanonDefinition;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreSort;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::FunctionTypeDefinition;
using isoline::component::binary::Sort;
using isoline::component::binary::SortIndex;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::doublingComponent;
using isoline::component::binary::testing::identityModule;
using isoline::component::binary::testing::taggedModule;
using isoline::component::runtime::AliasSlot;
using isoline::component::runtime::buildPlans;
using isoline::component::runtime::ComponentPlan;
using isoline::component::runtime::ComponentPlans;
using isoline::component::runtime::CoreInstanceExport;
using isoline::component::runtime::DefineBuiltin;
using isoline::component::runtime::DefineComponent;
using isoline::component::runtime::DefineCoreModule;
using isoline::component::runtime::DefineResourceType;
using isoline::component::runtime::ExportSlot;
using isoline::component::runtime::ImportSlot;
using isoline::component::runtime::InstantiateComponent;
using isoline::component::runtime::InstantiateCore;
using isoline::component::runtime::LiftFunction;
using isoline::component::runtime::LowerFunction;
using isoline::component::runtime::OuterSlot;
using isoline::component::runtime::PlanStep;
using isoline::component::runtime::SynthesizeCoreInstance;
using isoline::component::runtime::SynthesizeInstance;
using isoline::component::validate::validateComponent;
using isoline::component::validate::ValidatedComponent;
using isoline::component::validate::testing::CoreModuleValidator;

namespace
{

/// @p builder's component, validated (REQUIREd).
[[nodiscard]] std::shared_ptr<ValidatedComponent const> validatedOf(ComponentBuilder const& builder)
{
    auto coreModules = CoreModuleValidator {};
    auto validated =
        validateComponent(builder.pool(), ComponentFeatureSet::wasi03(), ComponentLimits {}, coreModules);
    if (!validated)
        FAIL("invalid: " << isoline::component::validate::specMessage(validated.error()));
    return *validated;
}

/// The steps of @p plan that are @p Step.
template <typename Step>
[[nodiscard]] std::vector<Step> stepsOf(ComponentPlan const& plan)
{
    auto found = std::vector<Step> {};
    for (auto const& step: plan.steps)
        if (auto const* typed = std::get_if<Step>(&step))
            found.push_back(*typed);
    return found;
}

/// The step at @p index of @p plan, as @p Step (REQUIREd).
template <typename Step>
[[nodiscard]] Step stepAt(ComponentPlan const& plan, std::size_t index)
{
    REQUIRE(index < plan.steps.size());
    auto const* typed = std::get_if<Step>(&plan.steps[index]);
    REQUIRE(typed != nullptr);
    return *typed;
}

/// Every plan's slots are the index spaces validation built, sort by sort.
void checkSlotsAreIndexSpaces(ValidatedComponent const& validated, ComponentPlans const& plans)
{
    REQUIRE(plans.plans.size() == validated.pool.components.size());
    for (auto const [scope, component]: core::views::enumerate(validated.pool.components))
    {
        if (component.kind != isoline::component::binary::ScopeKind::Component)
        {
            CHECK(plans.plans[scope].steps.empty());
            continue;
        }
        CAPTURE(scope);
        auto const& info = validated.scopes[scope];
        auto const& slots = plans.plans[scope].slots;
        CHECK(slots[Sort::CoreFunc] == info.coreFunctions.size());
        CHECK(slots[Sort::CoreTable] == info.coreTables.size());
        CHECK(slots[Sort::CoreMemory] == info.coreMemories.size());
        CHECK(slots[Sort::CoreGlobal] == info.coreGlobals.size());
        CHECK(slots[Sort::CoreTag] == info.coreTags.size());
        CHECK(slots[Sort::CoreType] == info.coreTypes.size());
        CHECK(slots[Sort::CoreModule] == info.coreModules.size());
        CHECK(slots[Sort::CoreInstance] == info.coreInstances.size());
        CHECK(slots[Sort::Func] == info.functions.size());
        CHECK(slots[Sort::Type] == info.types.size());
        CHECK(slots[Sort::Component] == info.components.size());
        CHECK(slots[Sort::Instance] == info.instances.size());
    }
}

/// A function type of no parameters and no result.
[[nodiscard]] TypeDefinition nullaryFunction()
{
    return TypeDefinition { .type = FunctionTypeDefinition { .params = {}, .result = std::nullopt } };
}

} // namespace

TEST_CASE("ComponentPlan.onePlanPerDefinition")
{
    // parent → child → grandchild: one plan each, by pool index; each instantiates its child once, by
    // the slot its DefineComponent filled with that child's plan.
    auto grandchild = ComponentBuilder {};
    auto child = ComponentBuilder {};
    std::ignore = child.instantiate(child.nested(std::move(grandchild)), {});
    auto parent = ComponentBuilder {};
    std::ignore = parent.instantiate(parent.nested(std::move(child)), {});
    auto const validated = validatedOf(parent);
    auto const plans = buildPlans(*validated);

    REQUIRE(plans.plans.size() == 3);
    checkSlotsAreIndexSpaces(*validated, plans);
    for (auto const [index, plan]: core::views::enumerate(plans.plans))
    {
        CAPTURE(index);
        CHECK(stepsOf<InstantiateComponent>(plan).size() == (index < 2 ? 1U : 0U));
        CHECK(stepsOf<DefineComponent>(plan).size() == (index < 2 ? 1U : 0U));
    }
    CHECK(stepAt<DefineComponent>(plans.plans[0], 0).plan == 1);
    CHECK(stepAt<DefineComponent>(plans.plans[1], 0).plan == 2);
    auto const instantiated = stepAt<InstantiateComponent>(plans.plans[0], 1);
    CHECK(instantiated.slot == 0);
    CHECK(instantiated.component == 0);
    CHECK(instantiated.arguments.empty());
}

TEST_CASE("ComponentPlan.linearInInput")
{
    // n levels, each instantiating the previous twice: inlining would make 2^(n - 1) instances; the
    // plans hold three steps per level whatever n is, so 2n levels hold twice the steps of n.
    auto const stepsFor = [](uint32_t levels) {
        auto const builder = doublingComponent(levels);
        auto const validated = validatedOf(builder);
        auto const plans = buildPlans(*validated);
        REQUIRE(plans.plans.size() == levels);
        checkSlotsAreIndexSpaces(*validated, plans);
        auto steps = std::size_t { 0 };
        for (auto const& plan: plans.plans)
        {
            CHECK(stepsOf<InstantiateComponent>(plan).size() == (plan.steps.empty() ? 0U : 2U));
            steps += plan.steps.size();
        }
        return steps;
    };
    CHECK(doublingComponent(20).build().size() < 2048);
    CHECK(stepsFor(20) == std::size_t { 3 } * 19);
    CHECK(stepsFor(40) == std::size_t { 3 } * 39);
}

TEST_CASE("ComponentPlan.stepsResolveToSlots")
{
    // Every kind of definition with a runtime value is one step naming the slots of its own operands,
    // which are their indices in the component's own index spaces.
    auto builder = ComponentBuilder {};
    auto const nullary = builder.type(nullaryFunction());
    auto const imported = builder.import("f", ExternDesc { .sort = Sort::Func, .index = nullary });
    auto const module = builder.coreModule(identityModule());
    auto const instance = builder.coreInstantiate(module, {});
    auto const core = builder.aliasCoreExport(instance, "id", CoreSort::Func);
    auto const signature = builder.type(TypeDefinition {
        .type = FunctionTypeDefinition {
            .params = { isoline::component::binary::LabeledType {
                .label = "x", .type = isoline::component::binary::PrimitiveValueType::U32 } },
            .result = isoline::component::binary::PrimitiveValueType::U32 } });
    auto const lifted = builder.canonLift(core, {}, signature);
    auto const lowered = builder.canonLower(imported, {});
    auto const gathered = builder.coreInlineExports(
        { CoreInlineExport { .name = "g", .sort = CoreSort::Func, .index = lowered } });
    auto const resource = builder.resource();
    auto const made = builder.canon(CanonDefinition { .builtin = CanonBuiltin::ResourceNew,
                                                      .immediates = { resource },
                                                      .options = {},
                                                      .result = std::nullopt });
    auto const bundle =
        builder.inlineInstance({ { "id", SortIndex { .sort = Sort::Func, .index = lifted } } });
    auto const exported = builder.exportItem("run", Sort::Func, lifted);
    auto const validated = validatedOf(builder);
    auto const plans = buildPlans(*validated);
    REQUIRE(plans.plans.size() == 1);
    checkSlotsAreIndexSpaces(*validated, plans);
    auto const& plan = plans.plans[0];
    auto const& info = validated->scopes[0];

    // The two function types are static: no step.
    REQUIRE(plan.steps.size() == 11);
    auto const import = stepAt<ImportSlot>(plan, 0);
    CHECK(import.sort == Sort::Func);
    CHECK(import.slot == imported);
    REQUIRE(plan.imports.size() == 1);
    CHECK(plan.imports[import.import].name == "f");
    CHECK(plan.imports[import.import].slot == imported);
    CHECK(stepAt<DefineCoreModule>(plan, 1).ordinal == 0);
    auto const instantiated = stepAt<InstantiateCore>(plan, 2);
    CHECK(instantiated.slot == instance);
    CHECK(instantiated.module == module);
    auto const alias = stepAt<AliasSlot>(plan, 3);
    CHECK(alias.sort == Sort::CoreFunc);
    CHECK(alias.slot == core);
    REQUIRE(std::holds_alternative<CoreInstanceExport>(alias.source));
    CHECK(std::get<CoreInstanceExport>(alias.source).coreInstance == instance);
    CHECK(std::get<CoreInstanceExport>(alias.source).name == "id");
    auto const lift = stepAt<LiftFunction>(plan, 4);
    CHECK(lift.slot == lifted);
    CHECK(lift.coreFunction == core);
    CHECK(lift.type == info.functions[lifted]);
    auto const lower = stepAt<LowerFunction>(plan, 5);
    CHECK(lower.slot == lowered);
    CHECK(lower.function == imported);
    CHECK(lower.type == info.functions[imported]);
    auto const synthesized = stepAt<SynthesizeCoreInstance>(plan, 6);
    CHECK(synthesized.slot == gathered);
    REQUIRE(synthesized.exports.size() == 1);
    CHECK(synthesized.exports[0].slot == lowered);
    auto const defined = stepAt<DefineResourceType>(plan, 7);
    CHECK(defined.slot == resource);
    CHECK(info.types[resource].type == isoline::component::types::AnyTypeId { defined.resource });
    CHECK_FALSE(defined.destructor.has_value());
    auto const builtin = stepAt<DefineBuiltin>(plan, 8);
    CHECK(builtin.slot == made);
    CHECK(builtin.builtin == CanonBuiltin::ResourceNew);
    CHECK(builtin.immediates == std::vector<uint32_t> { resource });
    auto const instanceOfExports = stepAt<SynthesizeInstance>(plan, 9);
    CHECK(instanceOfExports.slot == bundle);
    REQUIRE(instanceOfExports.exports.size() == 1);
    CHECK(instanceOfExports.exports[0].item == SortIndex { .sort = Sort::Func, .index = lifted });
    auto const exportStep = stepAt<ExportSlot>(plan, 10);
    CHECK(exportStep.slot == exported);
    CHECK(exportStep.source == lifted);
    REQUIRE(plan.exports.size() == 1);
    CHECK(plan.exports[exportStep.exportIndex].name == "run");
    CHECK(plan.exports[exportStep.exportIndex].item == SortIndex { .sort = Sort::Func, .index = exported });
}

TEST_CASE("ComponentPlan.ordinalsArePreOrder")
{
    // A, then a nested component holding B, then C: the ordinals are pre-order over nesting (D10),
    // not the order of the pool's entries.
    auto builder = ComponentBuilder {};
    std::ignore = builder.coreModule(taggedModule("A"));
    auto child = ComponentBuilder {};
    std::ignore = child.coreModule(taggedModule("B"));
    std::ignore = builder.nested(std::move(child));
    std::ignore = builder.coreModule(taggedModule("C"));
    auto const plans = buildPlans(*validatedOf(builder));
    REQUIRE(plans.plans.size() == 2);
    auto const ordinals = [](ComponentPlan const& plan) {
        auto found = std::vector<uint32_t> {};
        for (auto const& step: stepsOf<DefineCoreModule>(plan))
            found.push_back(step.ordinal);
        return found;
    };
    CHECK(ordinals(plans.plans[0]) == std::vector<uint32_t> { 0, 2 });
    CHECK(ordinals(plans.plans[1]) == std::vector<uint32_t> { 1 });
}

TEST_CASE("ComponentPlan.outerAliasReadsEnclosingSlots")
{
    // A nested component instantiating its parent's module through `alias outer 1`: the alias names the
    // parent's slot, never a copy of the module, so the child's plan stays its own.
    auto child = ComponentBuilder {};
    auto const aliased = child.aliasOuter(Sort::CoreModule, 1, 0);
    std::ignore = child.coreInstantiate(aliased, {});
    // A type from outside is static: a slot, no step.
    auto const aliasedType = child.aliasOuter(Sort::Type, 1, 0);
    auto parent = ComponentBuilder {};
    std::ignore = parent.type(nullaryFunction());
    std::ignore = parent.coreModule(identityModule());
    std::ignore = parent.instantiate(parent.nested(std::move(child)), {});
    auto const validated = validatedOf(parent);
    auto const plans = buildPlans(*validated);
    REQUIRE(plans.plans.size() == 2);
    checkSlotsAreIndexSpaces(*validated, plans);
    auto const alias = stepAt<AliasSlot>(plans.plans[1], 0);
    CHECK(alias.sort == Sort::CoreModule);
    REQUIRE(std::holds_alternative<OuterSlot>(alias.source));
    CHECK(std::get<OuterSlot>(alias.source).count == 1);
    CHECK(std::get<OuterSlot>(alias.source).slot == 0);
    CHECK(stepAt<InstantiateCore>(plans.plans[1], 1).module == aliased);
    CHECK(stepsOf<DefineCoreModule>(plans.plans[1]).empty());
    CHECK(plans.plans[1].steps.size() == 2);
    CHECK(plans.plans[1].slots[Sort::Type] == aliasedType + 1);
}

TEST_CASE("ComponentPlan.argumentsOrderedByName")
{
    // Instantiation finds each import's argument by a binary search over names, so the arguments are
    // sorted by name; the child's imports keep their definition order.
    auto child = ComponentBuilder {};
    auto const childType = child.type(nullaryFunction());
    std::ignore = child.import("b", ExternDesc { .sort = Sort::Func, .index = childType });
    std::ignore = child.import("a", ExternDesc { .sort = Sort::Func, .index = childType });
    auto parent = ComponentBuilder {};
    auto const type = parent.type(nullaryFunction());
    auto const function = parent.import("f", ExternDesc { .sort = Sort::Func, .index = type });
    auto const nested = parent.nested(std::move(child));
    std::ignore = parent.instantiate(nested,
                                     { { "b", SortIndex { .sort = Sort::Func, .index = function } },
                                       { "a", SortIndex { .sort = Sort::Func, .index = function } } });
    auto const plans = buildPlans(*validatedOf(parent));
    REQUIRE(plans.plans.size() == 2);
    auto const instantiate = stepsOf<InstantiateComponent>(plans.plans[0]);
    REQUIRE(instantiate.size() == 1);
    REQUIRE(instantiate[0].arguments.size() == 2);
    CHECK(instantiate[0].arguments[0].name == "a");
    CHECK(instantiate[0].arguments[1].name == "b");
    REQUIRE(plans.plans[1].imports.size() == 2);
    CHECK(plans.plans[1].imports[0].name == "b");
    CHECK(plans.plans[1].imports[1].name == "a");
}
