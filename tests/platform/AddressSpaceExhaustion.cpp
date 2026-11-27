// SPDX-License-Identifier: Apache-2.0
//
// platform.addressSpaceExhaustionIsARefusal: run under an address-space cap (prlimit --as, 12 GiB, set by
// tests/CMakeLists.txt, so that no setrlimit leaks into another test), Wasmtime-parity memories are
// reserved until the system refuses one. At least two fit, and the next is a HostMemory refusal, never a
// crash. Then 64 capped memories of 1 MiB all fit in the same cap. Exits 0 when all of it holds.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <print>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/platform/VirtualMemory.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/MemoryLayout.hpp>
#include <isoline/runtime/ReservedMemoryAllocator.hpp>

namespace
{

constexpr auto MiB = uint64_t { 1 } << 20;
constexpr auto GiB = uint64_t { 1 } << 30;

/// More parity memories than a 12 GiB cap holds: reaching it means the cap is not in effect.
constexpr auto MostParityMemories = uint64_t { 16 };

/// The capped memories that must all fit.
constexpr auto CappedMemories = uint64_t { 64 };

/// What the store asks for a memory32 of 1 to @p maximumPages pages under @p layout.
[[nodiscard]] isoline::runtime::MemoryRequest requestOf(isoline::runtime::MemoryLayout layout,
                                                        uint64_t maximumPages)
{
    auto const type = isoline::binary::MemoryType { .limits = { .min = 1, .max = maximumPages } };
    auto const page = isoline::binary::pageSize(type);
    auto const requirements = isoline::runtime::memoryRequirementsFor(layout, type);
    return isoline::runtime::MemoryRequest { .type = type,
                                             .initialBytes = page,
                                             .maximumBytes = maximumPages * page,
                                             .requiredGuardBytes = requirements.guardBytes,
                                             .requiredReachBytes = requirements.reachBytes };
}

/// Reports a failed expectation.
/// @return The exit status of a failure.
[[nodiscard]] int failure(std::string_view what)
{
    std::println(stderr, "addressSpaceExhaustionIsARefusal: {}", what);
    return EXIT_FAILURE;
}

} // namespace

int main()
{
    auto system = isoline::platform::SystemVirtualMemory {};
    auto memories = std::vector<std::unique_ptr<isoline::runtime::ILinearMemory>> {};

    auto const parity =
        isoline::runtime::layoutOf(isoline::runtime::LayoutRow::WasmtimeParity, 4 * GiB, system.pageSize());
    auto parityAllocator = isoline::runtime::ReservedMemoryAllocator { system, parity };
    auto const parityRequest = requestOf(parity, 65'536);
    auto refused = false;
    for ([[maybe_unused]] auto const attempt: std::views::iota(uint64_t { 0 }, MostParityMemories))
    {
        auto memory = parityAllocator.allocate(parityRequest);
        if (!memory)
        {
            if (memory.error() != isoline::runtime::Refusal::HostMemory)
                return failure("a refused parity memory is not HostMemory");
            refused = true;
            break;
        }
        memories.push_back(std::move(*memory));
    }
    if (!refused)
        return failure("no parity memory was refused: the address-space cap is not in effect");
    if (memories.size() < 2)
        return failure("fewer than two parity memories fit in 12 GiB");
    std::println("{} parity memories fit, the next was HostMemory", memories.size());
    memories.clear();

    auto const capped =
        isoline::runtime::layoutOf(isoline::runtime::LayoutRow::Capped, 1 * MiB, system.pageSize());
    auto cappedAllocator = isoline::runtime::ReservedMemoryAllocator { system, capped };
    auto const cappedRequest =
        requestOf(capped, (1 * MiB) / isoline::binary::pageSize(isoline::binary::MemoryType {}));
    for ([[maybe_unused]] auto const index: std::views::iota(uint64_t { 0 }, CappedMemories))
    {
        auto memory = cappedAllocator.allocate(cappedRequest);
        if (!memory)
            return failure("a capped memory of 1 MiB was refused");
        memories.push_back(std::move(*memory));
    }
    std::println("{} capped memories of 1 MiB fit", memories.size());
    return EXIT_SUCCESS;
}
