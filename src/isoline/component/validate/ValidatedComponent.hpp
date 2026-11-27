// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <vector>

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
};

/// A component that passed validation: immutable, shared by every Component, plan and store built
/// from it. Only validateComponent() makes one.
///
/// Task 7 adds each scope's resolved index spaces and the root's exports.
struct ValidatedComponent
{
    binary::ComponentPool pool;               ///< The component as decoded.
    types::TypeArena types;                   ///< Every type of every scope of the pool.
    types::ComponentTypeInfo type;            ///< The root's imports, typed.
    std::vector<CoreModuleShape> coreModules; ///< The embedded core modules, by ordinal (D10).
};

} // namespace isoline::component::validate
