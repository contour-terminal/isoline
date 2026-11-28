// SPDX-License-Identifier: Apache-2.0
// isoline_text_fuzzer: compiles each input as the text format and gives what compiles to the load target
// (FuzzTargets.hpp's runText()); traps when the lowering breaks one of its published bounds. A reader that is
// not linear in its source shows as a timeout or past the RSS limit.
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
    auto const verdict = isoline::fuzz::runText(std::as_bytes(std::span { data, size }));
    if (!verdict)
    {
        std::println(stderr, "bound broken: {}", verdict.error());
        __builtin_trap();
    }
    return 0;
}
