// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <isoline/component/binary/Component.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/ValidatedComponent.hpp>

namespace isoline::component::runtime
{

// One plan per component definition (D1): the definitions of one entry of the pool, as linear steps,
// every reference resolved to a slot of that definition's own index spaces. A slot of sort s and index i
// holds what entry i of the index space of s holds at run time, so a step names its operands by the
// indices the component's own definitions use. Nothing is inlined across component definitions: a
// nested component is a slot holding its plan, and instantiating it runs that plan, so plans are
// linear in the component however often it instantiates its children.
//
// Slots of sort CoreType and Value are never filled: core types are static, and values are refused
// before validation (🪙). A type slot holds a runtime resource type when the entry is a resource, and
// nothing otherwise; only the definitions that can name a resource have a step.

/// The slots of a plan: per sort, the entries of that index space, equal to the counts validation
/// made (validate::ScopeInfo).
struct SlotCounts
{
    std::array<uint32_t, binary::SortCount> bySort {}; ///< Per binary::Sort, by its value.

    /// The slots of one sort.
    /// @param sort The sort.
    /// @return Their count.
    [[nodiscard]] uint32_t operator[](binary::Sort sort) const noexcept
    {
        return bySort[static_cast<std::size_t>(sort)];
    }

    /// Two counts are equal when every sort's is.
    bool operator==(SlotCounts const&) const = default;
};

/// An embedded core module: the slot holds the module of its ordinal (D10), loaded once per Component.
struct DefineCoreModule
{
    uint32_t slot = 0;    ///< The core module slot defined.
    uint32_t ordinal = 0; ///< The module's ordinal, pre-order over the pool.
};

/// A nested component: the slot holds its plan together with the slots of the plan run defining it,
/// which the nested plan's outer aliases read (OuterSlot).
struct DefineComponent
{
    uint32_t slot = 0; ///< The component slot defined.
    uint32_t plan = 0; ///< The plan's index in ComponentPlans, which is its pool index.
};

/// An import: the slot holds what instantiation supplies for import @ref import of the plan.
struct ImportSlot
{
    binary::Sort sort = binary::Sort::Func; ///< The sort of the slot defined.
    uint32_t slot = 0;                      ///< The slot defined.
    uint32_t import = 0;                    ///< The import's index in ComponentPlan::imports.
};

/// One argument of a core instantiation: an import module name and the core instance supplying it.
struct CoreArgument
{
    std::string name;          ///< The import module name.
    uint32_t coreInstance = 0; ///< The core instance slot supplying it.
};

/// `(core instance (instantiate m arg*))`: instantiates the core module in a slot.
struct InstantiateCore
{
    uint32_t slot = 0;                      ///< The core instance slot defined.
    uint32_t module = 0;                    ///< The core module slot instantiated.
    std::vector<CoreArgument> arguments {}; ///< The arguments, in encoded order.
};

/// One export of a synthesized core instance: a core definition of this plan, by name.
struct CoreExportSlot
{
    std::string name;                               ///< The export's name.
    binary::CoreSort sort = binary::CoreSort::Func; ///< The sort exported.
    uint32_t slot = 0;                              ///< The slot of that sort exported.
};

/// A core instance of inline exports: no module runs, the slots are gathered under names.
struct SynthesizeCoreInstance
{
    uint32_t slot = 0;                      ///< The core instance slot defined.
    std::vector<CoreExportSlot> exports {}; ///< The exports, in encoded order.
};

/// `canon lift`: a component function calling a core function through the canonical ABI.
struct LiftFunction
{
    uint32_t slot = 0;                           ///< The function slot defined.
    uint32_t coreFunction = 0;                   ///< The core function slot lifted.
    types::FunctionTypeId type {};               ///< Its component function type, in the arena.
    std::vector<binary::CanonOption> options {}; ///< Its options; their indices are slots of their sorts.
};

/// `canon lower`: a core function calling a component function through the canonical ABI.
struct LowerFunction
{
    uint32_t slot = 0;                           ///< The core function slot defined.
    uint32_t function = 0;                       ///< The function slot lowered.
    types::FunctionTypeId type {};               ///< That function's type, in the arena.
    std::vector<binary::CanonOption> options {}; ///< Its options; their indices are slots of their sorts.
};

/// Any other canonical built-in (`resource.new`, `resource.drop`, `resource.rep`): a core function.
struct DefineBuiltin
{
    uint32_t slot = 0;                                                ///< The core function slot defined.
    binary::CanonBuiltin builtin = binary::CanonBuiltin::ResourceNew; ///< The built-in.
    /// Its immediates, as CanonDefinition stores them; a type index is a type slot.
    std::vector<uint32_t> immediates {};
    std::vector<binary::CanonOption> options {}; ///< Its options; their indices are slots of their sorts.
};

/// A resource type definition: each run mints a fresh runtime resource type (D5).
struct DefineResourceType
{
    uint32_t slot = 0;                     ///< The type slot defined.
    types::ResourceTypeId resource {};     ///< The resource the validator gave the definition.
    std::optional<uint32_t> destructor {}; ///< Its destructor's core function slot, if any.
};

/// An alias source: an export of a component instance in a slot.
struct InstanceExport
{
    uint32_t instance = 0; ///< The instance slot.
    std::string name;      ///< The export's name.
};

/// An alias source: an export of a core instance in a slot.
struct CoreInstanceExport
{
    uint32_t coreInstance = 0; ///< The core instance slot.
    std::string name;          ///< The export's name.
};

/// An alias source: a slot of this plan or of an enclosing one (`alias outer n`), which the definition
/// reaches through the component values it was defined in: each DefineComponent's value keeps the slots
/// of the plan run that defined it, so an outer alias reads them wherever the component is instantiated.
struct OuterSlot
{
    uint32_t count = 0; ///< The enclosing definitions to count outward; 0 is this plan's own slots.
    uint32_t slot = 0;  ///< The slot there, of the alias's sort.
};

/// Where an alias takes its value from.
using AliasSource = std::variant<InstanceExport, CoreInstanceExport, OuterSlot>;

/// An alias: the slot holds the value of its source.
struct AliasSlot
{
    binary::Sort sort = binary::Sort::Func; ///< The sort of the slot defined.
    uint32_t slot = 0;                      ///< The slot defined.
    AliasSource source;                     ///< Where the value comes from.
};

/// A named slot: an argument of a component instantiation, or an export of a synthesized instance.
struct NamedSlot
{
    std::string name;       ///< The name.
    binary::SortIndex item; ///< The slot named.
};

/// `(instance (instantiate c arg*))`: runs the plan of the component in a slot with these arguments.
///
/// The component is a slot, not a plan, as it may be imported or aliased; a nested component's slot
/// holds its own plan.
struct InstantiateComponent
{
    uint32_t slot = 0;                   ///< The instance slot defined.
    uint32_t component = 0;              ///< The component slot instantiated.
    std::vector<NamedSlot> arguments {}; ///< The arguments, ordered by name (byte-wise), for lookups.
};

/// An instance of inline exports: no component runs, the slots are gathered under names.
struct SynthesizeInstance
{
    uint32_t slot = 0;                 ///< The instance slot defined.
    std::vector<NamedSlot> exports {}; ///< The exports, in encoded order.
};

/// An export: it adds an entry to its sort's index space, holding the value it exports.
struct ExportSlot
{
    binary::Sort sort = binary::Sort::Func; ///< The sort exported.
    uint32_t slot = 0;                      ///< The slot defined.
    uint32_t source = 0;                    ///< The slot exported.
    uint32_t exportIndex = 0;               ///< The export's index in ComponentPlan::exports.
};

/// One step of a plan, each defining one slot.
using PlanStep = std::variant<DefineCoreModule,
                              DefineComponent,
                              ImportSlot,
                              InstantiateCore,
                              SynthesizeCoreInstance,
                              LowerFunction,
                              LiftFunction,
                              DefineBuiltin,
                              DefineResourceType,
                              AliasSlot,
                              InstantiateComponent,
                              SynthesizeInstance,
                              ExportSlot>;

/// An import of a plan, which instantiation supplies by name.
struct ImportRequirement
{
    std::string name;                       ///< The import's name.
    binary::Sort sort = binary::Sort::Func; ///< Its sort.
    uint32_t slot = 0;                      ///< The slot it fills.
};

/// An export of a plan: a name and the slot it exports.
struct ExportBinding
{
    std::string name;       ///< The export's name.
    binary::SortIndex item; ///< The slot exported (the entry the export added).
};

/// The plan of one entry of the pool. A type scope's plan is empty: it defines no runtime values.
struct ComponentPlan
{
    std::vector<PlanStep> steps {};            ///< In definition order.
    SlotCounts slots {};                       ///< Its index spaces' sizes.
    std::vector<ImportRequirement> imports {}; ///< Its imports, in definition order.
    std::vector<ExportBinding> exports {};     ///< Its exports, in definition order.
};

/// Every plan of a validated component, by pool index: the root's is 0.
struct ComponentPlans
{
    std::vector<ComponentPlan> plans {}; ///< One per entry of the pool.
};

/// The plans of a validated component: one per entry of its pool, each definition a step or none, so
/// O(definitions) in time and memory whatever the component instantiates.
///
/// Infallible after validation: every index a definition names is in bounds, and every outer alias
/// reaches an enclosing component (validateComponent()).
/// @param validated The component.
/// @return Its plans.
/// @throws std::bad_alloc When they cannot be stored.
[[nodiscard]] ComponentPlans buildPlans(validate::ValidatedComponent const& validated);

} // namespace isoline::component::runtime
