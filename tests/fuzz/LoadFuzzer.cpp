// SPDX-License-Identifier: Apache-2.0
// isoline_load_fuzzer: decodes and validates each input as a module, and lowers it within a small compile
// budget (FuzzTargets.hpp's runLoad()); traps when the lowering breaks one of its published bounds. A
// lowering that is not linear in its input shows as a timeout or past the RSS limit, which -timeout and
// -rss_limit_mb set.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <print>
#include <span>

#include "FuzzTargets.hpp"

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, std::size_t size)
{
    auto const verdict =
        isoline::fuzz::runLoad(std::as_bytes(std::span { data, size }), isoline::fuzz::LoadCompileBudget);
    if (!verdict)
    {
        std::println(stderr, "bound broken: {}", verdict.error());
        __builtin_trap();
    }
    return 0;
}
