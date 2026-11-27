// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <span>

#include <isoline/fastinterpreter/Backends.hpp>

// The tail-call dispatch's entry points where no probe row passed and the TailCall*.cpp family files are
// not compiled: no header words, and no code to run.

namespace isoline::fastinterpreter
{

std::span<Word const> tailCallHeaderWords() noexcept
{
    return {};
}

// uncovered-begin(tail-call-unavailable)
Exit runTailCall(State /*state*/) noexcept
{
    // The fast interpreter runs only an available dispatch (FastInterpreter's constructor).
    core::unreachable();
}
// uncovered-end(tail-call-unavailable)

} // namespace isoline::fastinterpreter
