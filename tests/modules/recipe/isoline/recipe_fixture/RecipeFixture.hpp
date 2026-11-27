// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Store.hpp>

/// The fixture of the recipe for a later public library and its module (docs/modules.md, "Adding a
/// module"): a library of its own, built on isoline_core, whose module isoline.recipe_fixture re-exports
/// module isoline. Built and installed by the testing build only, under its own install component.
namespace isoline::recipe_fixture
{

/// Doubles the result of the add module's export.
/// @param instance An instance that exports `add: (i32, i32) -> i32` (a precondition).
/// @param a The first argument of add.
/// @param b The second argument of add.
/// @return twice add(a, b), wrapping as i32 arithmetic does, or the call error.
[[nodiscard]] std::expected<std::int32_t, isoline::api::CallError> doubledSum(
    isoline::api::Instance const& instance, std::int32_t a, std::int32_t b);

} // namespace isoline::recipe_fixture
