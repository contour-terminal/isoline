// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/FloatEnvironment.hpp>

#include <cfenv>

namespace isoline::runtime
{

namespace
{
    /// The C library's rounding mode, std::fegetround() and std::fesetround().
    class LibraryRoundingMode final: public IRoundingMode
    {
      public:
        [[nodiscard]] int get() const noexcept override { return std::fegetround(); }
        void set(int mode) noexcept override { std::fesetround(mode); }
    };
} // namespace

IRoundingMode& libraryRoundingMode() noexcept
{
    static auto instance = LibraryRoundingMode {};
    return instance;
}

// Every path reads the register before it switches the rounding mode, and writes it after the switch
// whenever one ran: a C library may implement fesetround() by rewriting the whole register, its sticky
// flags included (wine's ucrtbase clears them), and the write puts the flags read before back.

void establishFloatDefaults(IRoundingMode& rounding) noexcept
{
    auto const control = readFloatControl();
    auto const rounded = rounding.get() != FE_TONEAREST;
    if (rounded)
        rounding.set(FE_TONEAREST);
    if (auto const defaults = ieeeDefaults(hostFloatControlLayout(), control); rounded || defaults != control)
        writeFloatControl(defaults);
}

// The rounding mode goes first: where the C library also sets it in the control register (glibc's
// x86-64 fesetround() writes MXCSR besides the x87 control word), the register's write then wins at
// entry, and the saved register's control bits at exit, even where the host had the two disagree.
FloatEnvironmentScope::FloatEnvironmentScope(IRoundingMode& rounding) noexcept:
    _rounding { &rounding }, _hostControl { readFloatControl() }, _hostRounding { rounding.get() }
{
    auto const rounded = _hostRounding != FE_TONEAREST;
    if (rounded)
        _rounding->set(FE_TONEAREST);
    if (auto const defaults = ieeeDefaults(hostFloatControlLayout(), _hostControl);
        rounded || defaults != _hostControl)
        writeFloatControl(defaults);
}

FloatEnvironmentScope::~FloatEnvironmentScope()
{
    auto const current = readFloatControl();
    auto const roundedElsewise = _hostRounding != FE_TONEAREST;
    if (roundedElsewise)
        _rounding->set(_hostRounding);
    auto const& layout = hostFloatControlLayout();
    if (roundedElsewise || ieeeDefaults(layout, _hostControl) != _hostControl)
        writeFloatControl(withControlOf(layout, current, _hostControl));
}

} // namespace isoline::runtime
