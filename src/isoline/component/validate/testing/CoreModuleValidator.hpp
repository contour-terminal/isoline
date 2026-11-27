// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <variant>

#include <isoline/Features.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::component::validate::testing
{

/// The test double of ICoreModuleValidator: decodes and validates each module with
/// isoline::validate::ValidatedModule::load under its core features (every one by default), and counts
/// the calls.
class CoreModuleValidator final: public ICoreModuleValidator
{
  public:
    CoreModuleValidator() = default;

    /// A validator under @p features only.
    /// @param features The core features modules and declared core types are validated with.
    explicit CoreModuleValidator(FeatureSet features) noexcept: _features { features } {}

    [[nodiscard]] std::expected<CoreModuleShape, isoline::api::LoadError> validate(
        std::span<std::byte const> bytes) override
    {
        ++_calls;
        auto loaded = isoline::validate::ValidatedModule::load(bytes, _features, {});
        if (!loaded)
            return std::unexpected { std::visit(
                [](auto const& error) { return isoline::api::LoadError { error }; }, loaded.error()) };
        return CoreModuleShape { .module = std::move(*loaded) };
    }

    /// The modules validated so far.
    /// @return The number of validate() calls.
    [[nodiscard]] std::size_t calls() const noexcept { return _calls; }

    [[nodiscard]] FeatureSet coreFeatures() const noexcept override { return _features; }

  private:
    FeatureSet _features = FeatureSet::all();
    std::size_t _calls = 0;
};

} // namespace isoline::component::validate::testing
