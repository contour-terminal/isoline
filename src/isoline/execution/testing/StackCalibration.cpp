// SPDX-License-Identifier: Apache-2.0
#include <isoline/execution/testing/StackCalibration.hpp>

#include <cstdint>

namespace isoline::execution::testing
{

std::uint32_t stackCalibrationLevel(StackCalibration const& calibration, std::uint32_t levels)
{
    if (levels == 0)
        return 0;
    return calibration.level(calibration, levels - 1) ^ levels;
}

} // namespace isoline::execution::testing
