// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cfenv>

#include <isoline/runtime/FloatEnvironment.hpp>

namespace isoline::runtime::testing
{

/// A test's hold on the calling thread's floating-point environment: what the embedder would change,
/// and given back at the end of the test whatever the test did to it.
class HostFloatEnvironment
{
  public:
    /// Saves the thread's environment.
    HostFloatEnvironment() noexcept: _control { readFloatControl() } { std::fegetenv(&_environment); }

    /// Restores the environment saved at construction.
    ~HostFloatEnvironment()
    {
        std::fesetenv(&_environment);
        writeFloatControl(_control);
    }

    HostFloatEnvironment(HostFloatEnvironment const&) = delete;
    HostFloatEnvironment(HostFloatEnvironment&&) = delete;
    HostFloatEnvironment& operator=(HostFloatEnvironment const&) = delete;
    HostFloatEnvironment& operator=(HostFloatEnvironment&&) = delete;

  private:
    FloatControlWord _control;
    std::fenv_t _environment {};
};

/// Departs from the IEEE defaults on the calling thread as an embedder might: rounds upward, and
/// flushes subnormals to zero where the target can (FloatControlLayout::flushes). Hold a
/// HostFloatEnvironment first, to have the environment back.
inline void departFromIeeeDefaults() noexcept
{
    std::fesetround(FE_UPWARD);
    writeFloatControl(readFloatControl() | hostFloatControlLayout().flushes);
}

} // namespace isoline::runtime::testing
