// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include <isoline/Features.hpp>

namespace isoline::spectest
{

/// A feature set the spec runner's command line can name.
struct NamedFeatureSet
{
    std::string_view name; ///< What `--features` spells it as.
    FeatureSet features;   ///< The features a script runs with.
};

/// The feature sets `--features` can name: the two releases, and every feature Isoline knows.
constexpr auto FeatureSets = std::to_array<NamedFeatureSet>({
    { .name = "wasm2", .features = FeatureSet::wasm2() },
    { .name = "wasm3", .features = FeatureSet::wasm3() },
    { .name = "all", .features = FeatureSet::all() },
});

/// The feature set a command line names.
/// @param name The argument of `--features`.
/// @return Its features, or nullopt when no set has that name.
[[nodiscard]] constexpr std::optional<FeatureSet> featureSetNamed(std::string_view name)
{
    auto const found = std::ranges::find(FeatureSets, name, &NamedFeatureSet::name);
    return found == FeatureSets.end() ? std::nullopt : std::optional { found->features };
}

} // namespace isoline::spectest
