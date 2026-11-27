// SPDX-License-Identifier: Apache-2.0
//
// A contract violation handler that throws, linked into isoline_require_canary_throwing_handler where
// contracts are native: the handler an embedder may install, which Isoline cannot forbid
// (docs/embedding.md). Isoline writes contracts only in noexcept functions, so the exception ends the
// process where it leaves the function, as a Require() would have; were a contract in a function that
// may throw, it would reach the canary's main(), which reports that the check let the call through.
#include <contracts>
#include <cstdio>
#include <stdexcept>

/// Replaces the violation handler of the contracts runtime (libstdc++exp on GCC).
/// @param violation The contract that failed.
void handle_contract_violation(std::contracts::contract_violation const& violation);

void handle_contract_violation(std::contracts::contract_violation const& violation)
{
    std::printf("throwing-handler: %s\n", violation.comment());
    // The termination that follows ends the process with std::_Exit(), which flushes no stream.
    std::fflush(stdout);
    throw std::logic_error { violation.comment() };
}
