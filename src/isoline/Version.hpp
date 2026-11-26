// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <isoline/VersionConfig.hpp>

namespace isoline
{

/// A semantic version: major.minor.patch.
struct Version
{
    int major; ///< Incremented on incompatible API changes.
    int minor; ///< Incremented on backwards-compatible additions.
    int patch; ///< Incremented on backwards-compatible fixes.
};

/// The version of the Isoline library.
/// @return The library's version, taken from the build's project version.
[[nodiscard]] constexpr Version version() noexcept
{
    return Version { .major = config::versionMajor,
                     .minor = config::versionMinor,
                     .patch = config::versionPatch };
}

} // namespace isoline
