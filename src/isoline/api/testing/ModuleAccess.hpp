// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

#include <isoline/api/Module.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::api::testing
{

/// Test-only access to what an api::Module keeps from the embedder: the validated module its
/// descriptions point into. Not part of the embedding API.
struct ModuleAccess
{
    /// The validated module @p module describes.
    /// @param module The module.
    /// @return Its validated module, shared with every copy of @p module.
    [[nodiscard]] static std::shared_ptr<validate::ValidatedModule const> const& validatedOf(
        Module const& module) noexcept
    {
        return module.validated();
    }
};

} // namespace isoline::api::testing
