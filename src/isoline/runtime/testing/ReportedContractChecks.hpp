// SPDX-License-Identifier: Apache-2.0
#pragma once

// What a hidden [.contract] case installs, so that the check it violates is reported and the process ends
// without the crash CTest counts as a failure whatever the output: CTest runs the case alone
// (isoline_add_contract_test(), cmake/IsolineLanguageFeatures.cmake) and matches the report.

#include <core/Assert.hpp>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace isoline::runtime::testing
{

/// Ends the process normally on SIGABRT: a native contract violation terminates, which aborts, after its
/// report is written; std::_Exit() flushes no stream and runs no handler.
extern "C" inline void exitOnContractAbort(int /*signal*/)
{
    std::_Exit(EXIT_FAILURE);
}

/// While it lives: a failed check of the fallback path (core::setFailHandler()) is reported and ends the
/// process at once, and the abort a native contract violation ends with becomes an exit.
class ReportedContractChecks
{
  public:
    ReportedContractChecks(): _previousAbort { std::signal(SIGABRT, exitOnContractAbort) }
    {
        core::setFailHandler(
            [](std::string_view text, std::string_view message, std::string_view file, int line) {
                std::cerr << "contract check failed: [" << file << ':' << line << "] " << message << ' '
                          << text << '\n'
                          << std::flush;
                std::_Exit(0);
            });
    }

    ReportedContractChecks(ReportedContractChecks const&) = delete;
    ReportedContractChecks(ReportedContractChecks&&) = delete;
    ReportedContractChecks& operator=(ReportedContractChecks const&) = delete;
    ReportedContractChecks& operator=(ReportedContractChecks&&) = delete;

    /// Restores the default fail handler and the SIGABRT handler before it, so a case that ends without the
    /// violation leaves neither behind.
    ~ReportedContractChecks()
    {
        core::setFailHandler({});
        std::signal(SIGABRT, _previousAbort);
    }

  private:
    using SignalHandler = void (*)(int);

    SignalHandler _previousAbort; ///< The SIGABRT handler installed before.
};

} // namespace isoline::runtime::testing
