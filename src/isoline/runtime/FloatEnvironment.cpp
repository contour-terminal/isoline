// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/FloatEnvironment.hpp>

#include <cfenv>

namespace isoline::runtime
{

void establishFloatDefaults() noexcept
{
    if (std::fegetround() != FE_TONEAREST)
        std::fesetround(FE_TONEAREST);
    auto const control = readFloatControl();
    if (auto const defaults = ieeeDefaults(hostFloatControlLayout(), control); defaults != control)
        writeFloatControl(defaults);
}

// The rounding mode goes first: where the C library also sets it in the control register (glibc's
// x86-64 fesetround() writes MXCSR besides the x87 control word), the register's write then wins at
// entry, and the saved register's control bits at exit, even where the host had the two disagree.
// The flags the guest raised stay raised on either path.
FloatEnvironmentScope::FloatEnvironmentScope() noexcept:
    _hostControl { readFloatControl() }, _hostRounding { std::fegetround() }
{
    if (_hostRounding != FE_TONEAREST)
        std::fesetround(FE_TONEAREST);
    if (auto const defaults = ieeeDefaults(hostFloatControlLayout(), _hostControl); defaults != _hostControl)
        writeFloatControl(defaults);
}

FloatEnvironmentScope::~FloatEnvironmentScope()
{
    auto const roundedElsewise = _hostRounding != FE_TONEAREST;
    if (roundedElsewise)
        std::fesetround(_hostRounding);
    auto const& layout = hostFloatControlLayout();
    if (roundedElsewise || ieeeDefaults(layout, _hostControl) != _hostControl)
        writeFloatControl(withControlOf(layout, readFloatControl(), _hostControl));
}

} // namespace isoline::runtime
