// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::component::validate
{

/// What validation keeps of one embedded core module: the module, decoded and validated, never
/// compiled (D10).
struct CoreModuleShape
{
    std::shared_ptr<isoline::validate::ValidatedModule const> module; ///< The validated module.
    /// Its type, in the arena of the ValidatedComponent: its imports and exports in canonical core ids.
    types::CoreModuleTypeId type {};
};

/// An entry of a core type index space: a core function type, or a core module type.
using CoreTypeEntry = std::variant<types::CoreFunctionTypeId, types::CoreModuleTypeId>;

/// The index spaces of one scope of the pool, every entry resolved to its type in the arena, built
/// incrementally in definition order as Binary.md has it (an export, import or alias adds an entry to
/// its sort's space). Core tables, memories and globals are held by their types, concrete references
/// in the arena's canonical core ids.
struct ScopeInfo
{
    std::vector<types::TypeIdentity> types {};                ///< Sort type.
    std::vector<CoreTypeEntry> coreTypes {};                  ///< Sort core type.
    std::vector<types::CoreFunctionTypeId> coreFunctions {};  ///< Sort core func.
    std::vector<isoline::binary::TableType> coreTables {};    ///< Sort core table.
    std::vector<isoline::binary::MemoryType> coreMemories {}; ///< Sort core memory.
    std::vector<isoline::binary::GlobalType> coreGlobals {};  ///< Sort core global.
    std::vector<types::CoreFunctionTypeId> coreTags {};       ///< Sort core tag, by its function type.
    std::vector<types::CoreModuleTypeId> coreModules {};      ///< Sort core module.
    std::vector<types::CoreInstanceTypeId> coreInstances {};  ///< Sort core instance.
    std::vector<types::FunctionTypeId> functions {};          ///< Sort func.
    std::vector<types::InstanceTypeId> instances {};          ///< Sort instance.
    std::vector<types::ComponentTypeId> components {};        ///< Sort component.
};

/// What the bounded walks of one validation cost: the work counts tests pin the validator's time
/// bounds by, as ratios between inputs of size n and 2n, never by wall time.
struct ValidationWork
{
    uint64_t coreMatchingSteps =
        0; ///< Charged to maxCoreMatchingSteps: core imports matched, exports measured.
    uint64_t resourceWalkSteps = 0; ///< Types the free-resource walks took up, the ones skipped included.
    uint64_t exportLookupSteps =
        0;                       ///< Export alias lookups, plus the entries their name indices were built of.
    uint64_t subtypingSteps = 0; ///< The validation's TypeRelation steps.
};

/// A component that passed validation: immutable, shared by every Component, plan and store built
/// from it. Only validateComponent() makes one.
struct ValidatedComponent
{
    binary::ComponentPool pool;               ///< The component as decoded.
    types::TypeArena types;                   ///< Every type of every scope of the pool.
    std::vector<ScopeInfo> scopes;            ///< Per entry of the pool, by its index: its index spaces.
    types::ComponentTypeInfo type;            ///< The root's imports and exports, typed.
    std::vector<CoreModuleShape> coreModules; ///< The embedded core modules, by ordinal (D10).
    ValidationWork work {};                   ///< What validating it cost.
};

} // namespace isoline::component::validate
