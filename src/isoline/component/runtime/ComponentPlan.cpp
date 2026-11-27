// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/runtime/ComponentPlan.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/binary/Decoder.hpp>

namespace isoline::component::runtime
{

namespace
{

    /// The plan of one component scope of the pool, built definition by definition.
    class PlanBuilder
    {
      public:
        /// @param validated The component.
        /// @param scope The scope planned: a component.
        /// @param ordinals The ordinals of its core modules, in definition order.
        PlanBuilder(validate::ValidatedComponent const& validated,
                    uint32_t scope,
                    std::vector<uint32_t> const& ordinals) noexcept:
            _info { validated.scopes[scope] }, _ordinals { ordinals }
        {
        }

        /// Plans every definition of @p component.
        /// @param component The scope's definitions.
        /// @return The plan.
        [[nodiscard]] ComponentPlan build(binary::Component const& component)
        {
            _plan.steps.reserve(component.definitions.size());
            for (auto const& definition: component.definitions)
            {
                auto const sort = binary::introducedSort(definition);
                if (!sort)
                    continue;
                auto& count = _plan.slots.bySort[static_cast<std::size_t>(*sort)];
                auto const slot = count;
                ++count;
                std::visit([&](auto const& alternative) { plan(alternative, *sort, slot); }, definition);
            }
            return std::move(_plan);
        }

      private:
        void plan(binary::CoreModuleDefinition const&, binary::Sort, uint32_t slot)
        {
            Require(_nextModule < _ordinals.size());
            _plan.steps.emplace_back(DefineCoreModule { .slot = slot, .ordinal = _ordinals[_nextModule] });
            ++_nextModule;
        }

        void plan(binary::CoreInstanceDefinition const& definition, binary::Sort, uint32_t slot)
        {
            if (auto const* instantiate = std::get_if<binary::CoreInstantiate>(&definition.expression))
            {
                auto step = InstantiateCore { .slot = slot, .module = instantiate->module, .arguments = {} };
                step.arguments.reserve(instantiate->arguments.size());
                for (auto const& argument: instantiate->arguments)
                    step.arguments.push_back(
                        CoreArgument { .name = argument.name, .coreInstance = argument.instance });
                _plan.steps.emplace_back(std::move(step));
                return;
            }
            auto const& inlineExports = std::get<binary::CoreInlineExports>(definition.expression);
            auto step = SynthesizeCoreInstance { .slot = slot, .exports = {} };
            step.exports.reserve(inlineExports.exports.size());
            for (auto const& entry: inlineExports.exports)
                step.exports.push_back(
                    CoreExportSlot { .name = entry.name, .sort = entry.sort, .slot = entry.index });
            _plan.steps.emplace_back(std::move(step));
        }

        void plan(binary::NestedComponent const& nested, binary::Sort, uint32_t slot)
        {
            _plan.steps.emplace_back(DefineComponent { .slot = slot, .plan = nested.index });
        }

        void plan(binary::InstanceDefinition const& definition, binary::Sort, uint32_t slot)
        {
            if (auto const* instantiate = std::get_if<binary::Instantiate>(&definition.expression))
            {
                auto step = InstantiateComponent { .slot = slot,
                                                   .component = instantiate->component,
                                                   .arguments = {} };
                step.arguments.reserve(instantiate->arguments.size());
                for (auto const& argument: instantiate->arguments)
                    step.arguments.push_back(NamedSlot { .name = argument.name, .item = argument.item });
                // By name, so that instantiation finds an import's argument by a binary search.
                std::ranges::sort(step.arguments, std::less<> {}, &NamedSlot::name);
                _plan.steps.emplace_back(std::move(step));
                return;
            }
            auto const& inlineExports = std::get<binary::InlineExports>(definition.expression);
            auto step = SynthesizeInstance { .slot = slot, .exports = {} };
            step.exports.reserve(inlineExports.exports.size());
            for (auto const& entry: inlineExports.exports)
                step.exports.push_back(NamedSlot { .name = entry.name.name, .item = entry.item });
            _plan.steps.emplace_back(std::move(step));
        }

        void plan(binary::AliasDefinition const& alias, binary::Sort sort, uint32_t slot)
        {
            switch (alias.target)
            {
                case binary::AliasTarget::Export:
                    _plan.steps.emplace_back(AliasSlot {
                        .sort = sort,
                        .slot = slot,
                        .source = InstanceExport { .instance = alias.instance, .name = alias.name } });
                    return;
                case binary::AliasTarget::CoreExport:
                    _plan.steps.emplace_back(AliasSlot {
                        .sort = sort,
                        .slot = slot,
                        .source =
                            CoreInstanceExport { .coreInstance = alias.instance, .name = alias.name } });
                    return;
                case binary::AliasTarget::Outer:
                    // A core type is static, and so is a type aliased from outside: validation refuses an
                    // outer alias naming a resource not defined in the component itself, so it names no
                    // runtime resource type. Modules and components keep their values, and an outer 0 type
                    // alias may name the component's own resource.
                    if (sort == binary::Sort::CoreType
                        || (sort == binary::Sort::Type && alias.outerCount > 0))
                        return;
                    _plan.steps.emplace_back(AliasSlot {
                        .sort = sort,
                        .slot = slot,
                        .source = OuterSlot { .count = alias.outerCount, .slot = alias.outerIndex } });
                    return;
            }
        }

        void plan(binary::TypeDefinition const& definition, binary::Sort, uint32_t slot)
        {
            // Only a resource type has a runtime value: each run mints it afresh.
            auto const* resource = std::get_if<binary::ResourceTypeDefinition>(&definition.type);
            if (resource == nullptr)
                return;
            Require(slot < _info.types.size());
            auto const* defined = std::get_if<types::ResourceTypeId>(&_info.types[slot].type);
            Require(defined != nullptr);
            _plan.steps.emplace_back(DefineResourceType {
                .slot = slot, .resource = *defined, .destructor = resource->destructor });
        }

        void plan(binary::CanonDefinition const& canon, binary::Sort, uint32_t slot)
        {
            switch (canon.builtin)
            {
                case binary::CanonBuiltin::Lift:
                    Require(slot < _info.functions.size() && !canon.immediates.empty());
                    _plan.steps.emplace_back(LiftFunction { .slot = slot,
                                                            .coreFunction = canon.immediates[0],
                                                            .type = _info.functions[slot],
                                                            .options = canon.options });
                    return;
                case binary::CanonBuiltin::Lower:
                    Require(!canon.immediates.empty() && canon.immediates[0] < _info.functions.size());
                    _plan.steps.emplace_back(LowerFunction { .slot = slot,
                                                             .function = canon.immediates[0],
                                                             .type = _info.functions[canon.immediates[0]],
                                                             .options = canon.options });
                    return;
                default:
                    _plan.steps.emplace_back(DefineBuiltin { .slot = slot,
                                                             .builtin = canon.builtin,
                                                             .immediates = canon.immediates,
                                                             .options = canon.options });
                    return;
            }
        }

        void plan(binary::ImportDefinition const& import, binary::Sort sort, uint32_t slot)
        {
            auto const index = static_cast<uint32_t>(_plan.imports.size());
            _plan.imports.push_back(
                ImportRequirement { .name = import.name.name, .sort = sort, .slot = slot });
            _plan.steps.emplace_back(ImportSlot { .sort = sort, .slot = slot, .import = index });
        }

        void plan(binary::ExportDefinition const& definition, binary::Sort sort, uint32_t slot)
        {
            auto const index = static_cast<uint32_t>(_plan.exports.size());
            _plan.exports.push_back(ExportBinding {
                .name = definition.name.name, .item = binary::SortIndex { .sort = sort, .index = slot } });
            _plan.steps.emplace_back(ExportSlot {
                .sort = sort, .slot = slot, .source = definition.item.index, .exportIndex = index });
        }

        // A core type is static; a custom section adds no index (introducedSort()), and an export
        // declaration belongs to a type scope, which has no plan.
        void plan(binary::CoreTypeDefinition const&, binary::Sort, uint32_t) noexcept {}
        void plan(binary::CustomDefinition const&, binary::Sort, uint32_t) noexcept {}  // uncovered(no-sort)
        void plan(binary::ExportDeclaration const&, binary::Sort, uint32_t) noexcept {} // uncovered(no-sort)

        validate::ScopeInfo const& _info;
        std::vector<uint32_t> const& _ordinals;
        std::size_t _nextModule = 0;
        ComponentPlan _plan {};
    };

} // namespace

ComponentPlans buildPlans(validate::ValidatedComponent const& validated)
{
    auto const& pool = validated.pool;
    Require(validated.scopes.size() == pool.components.size());

    // The ordinals (D10) are pre-order over nested components, not pool order: each scope's modules,
    // in definition order, get theirs from where the walk met them.
    auto ordinals = std::vector<std::vector<uint32_t>>(pool.components.size());
    for (auto const [ordinal, position]: core::views::enumerate(binary::coreModulePositions(pool)))
        ordinals[position.scope].push_back(static_cast<uint32_t>(ordinal));

    auto plans = ComponentPlans {};
    plans.plans.reserve(pool.components.size());
    for (auto const [scope, component]: core::views::enumerate(pool.components))
    {
        if (component.kind != binary::ScopeKind::Component)
        {
            plans.plans.emplace_back();
            continue;
        }
        auto const index = static_cast<uint32_t>(scope);
        plans.plans.push_back(PlanBuilder { validated, index, ordinals[scope] }.build(component));
    }
    return plans;
}

} // namespace isoline::component::runtime
