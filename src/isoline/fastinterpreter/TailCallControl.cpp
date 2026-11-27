// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <ranges>
#include <span>

#include <isoline/fastinterpreter/Backends.hpp>
#include <isoline/fastinterpreter/TailCall.hpp>

// The control family's handlers, and what joins the family files into one dispatch. This is the file the
// configure-time probe compiles with each candidate tail-call spelling (fastinterpreter/CMakeLists.txt).

namespace isoline::fastinterpreter
{

namespace
{
    /// The handler table of every family file, which together hold one handler per operation.
    using FamilyHandlers = std::array<Handler, OperationCount> const& (*) () noexcept;

    /// The family files: adding one is a row here and one in fastinterpreter/CMakeLists.txt.
    inline constexpr auto FamilyFiles = std::to_array<FamilyHandlers>({
        &controlHandlers,
        &numericHandlers,
        &memoryHandlers,
        &simdHandlers,
        &exceptionHandlers,
    });
} // namespace

std::array<Handler, OperationCount> const& controlHandlers() noexcept
{
    static constexpr auto Handlers = familyHandlers<OperationFamily::Control>();
    return Handlers;
}

std::span<Word const> tailCallHeaderWords() noexcept
{
    static auto const words = [] {
        auto joined = std::array<Word, OperationCount> {};
        for (auto const familyFile: FamilyFiles)
        {
            auto const& handlers = familyFile();
            for (auto const index: std::views::iota(std::size_t { 0 }, OperationCount))
            {
                if (handlers[index] == nullptr)
                    continue;
                // Each operation's family is in one file only.
                Require(joined[index] == 0);
                joined[index] = std::bit_cast<Word>(handlers[index]);
            }
        }
        // Every operation has a handler, at an address no OperationId is, so a header word names its
        // operation under either dispatch (operationOfHeader()).
        Require(std::ranges::all_of(joined, [](Word word) { return word >= OperationCount; }));
        return joined;
    }();
    return words;
}

Exit runTailCall(State state) noexcept
{
    return std::bit_cast<Handler>(*state.pc)(
        state.pc, state.fp, state.context, state.memory0, state.memory0Size, state.stepsLeft);
}

} // namespace isoline::fastinterpreter
