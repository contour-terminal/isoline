// SPDX-License-Identifier: Apache-2.0
//
// An api precondition, checked as a process of its own: a Require() that fails ends the process, so no
// Catch2 case can observe it in-process. CTest judges it by the check's own text on stderr
// (tests/CMakeLists.txt), which only that Require() prints; the line below it, printed only when the
// check let the call through, fails it.
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Types.hpp>

namespace
{

/// Ends the process normally on SIGABRT: CTest fails a test that dies of a signal whatever its output,
/// so the abort a failed Require() ends with becomes an exit, and the output decides.
extern "C" void exitOnAbort(int /*signal*/)
{
    std::_Exit(EXIT_FAILURE);
}

} // namespace

int main()
{
    std::signal(SIGABRT, exitOnAbort);
    using isoline::api::Feature;
    using isoline::api::FeatureSet;
    // Relaxed SIMD without SIMD: a feature set not closed over its prerequisites, which Engine's
    // constructor refuses (Engine.cpp).
    auto const open = FeatureSet::wasm2().without(Feature::Simd).with(Feature::RelaxedSimd);
    auto const engine = isoline::api::Engine { isoline::api::EngineConfig { .features = open } };
    std::puts("require-canary: an engine of an open feature set was constructed");
    return engine.config().features == open ? 0 : 1;
}
