// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <cstdlib>
#include <format>
#include <iostream>
#include <string_view>

namespace isoline::runtime::testing
{

/// The exit status of a process that a contract check ended on purpose (endAtContractViolation()).
/// Not zero, so a run that reaches a [.contract] case by a wildcard fails rather than ending early
/// and looking green.
inline constexpr auto ContractViolationExitCode = 3;

/// Makes the next failed Require() or Guarantee() end the process normally, after printing its text
/// as core does ("[file:line] Precondition failed. condition"), instead of by abort().
///
/// For the [.contract] test cases, each of which violates one contract on purpose: CTest runs each in
/// a process of its own and passes it on the check's text (tests/CMakeLists.txt). It judges a
/// process a signal ended as failed whatever it printed, hence the normal exit. The handler stays
/// installed for the rest of the process, which the violation ends.
inline void endAtContractViolation()
{
    core::setFailHandler(
        [](std::string_view text, std::string_view message, std::string_view file, int line) {
            std::cerr << std::format("[{}:{}] {} {}\n", file, line, message, text) << std::flush;
            std::_Exit(ContractViolationExitCode);
        });
}

} // namespace isoline::runtime::testing
