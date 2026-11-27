// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>

#include <isoline/component/canonical/Values.hpp>

// The builder steps only lifting takes: growth it overwrites whole with guest memory, not zeroed first.
// Private to isoline_component; a host's in-place steps are ValueBuilder::encodedString() and
// packedElements(), whose growth is zeroed.

namespace isoline::component::canonical
{

/// ValueBuilder's overwrite-only steps, for liftString() and the lift of a list of scalars: each copies
/// guest memory over every byte it is given before the builder's next step, or abandons the builder. The
/// bytes are indeterminate until then, 0xFF in a debug build (ThisBuildPoisons), so a byte a lift skipped
/// shows as text that is not UTF-8 and as an element no type has.
class LiftSteps
{
  public:
    /// ValueBuilder::encodedString(), its growth not zeroed.
    /// @param builder The builder.
    /// @param encoding The string's encoding.
    /// @param taggedCodeUnits Its tagged code units.
    /// @return Its bytes, for the caller to overwrite every one; empty when refused or empty.
    /// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
    [[nodiscard]] static std::span<std::byte> encodedString(ValueBuilder& builder,
                                                            StringEncoding encoding,
                                                            uint32_t taggedCodeUnits)
    {
        return builder.stringSteps(encoding, taggedCodeUnits, GrowthFill::ForOverwrite);
    }

    /// ValueBuilder::packedElements(), its growth not zeroed.
    /// @param builder The builder.
    /// @return The elements' bytes, for the caller to overwrite every one; empty when refused or empty.
    [[nodiscard]] static std::span<std::byte> packedElements(ValueBuilder& builder)
    {
        return builder.packedSteps(GrowthFill::ForOverwrite);
    }
};

} // namespace isoline::component::canonical
