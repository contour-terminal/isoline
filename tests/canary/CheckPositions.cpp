// SPDX-License-Identifier: Apache-2.0
//
// The check positions of CheckPositions.hpp, compiled as Isoline's libraries are, through the contracts
// seam: contracts where they are native, Require() elsewhere. The `for`, `while` and `do` statements are
// what is tested here, so these loops are not range-based.
#include "CheckPositions.hpp"

#include <cstdio>
#include <ranges>

#include <isoline/meta/Check.hpp>

namespace isoline::canary
{

void preconditionFirst(unsigned entryRounds) noexcept
{
    ISOLINE_PRECONDITION(entryRounds == 0);
}

void preconditionInBracelessIf(unsigned ifRounds) noexcept
{
    if (ifRounds > 0)
        ISOLINE_PRECONDITION(ifRounds == 0);
    else
        std::puts("check-positions: no rounds");
}

void invariantInBracelessElse(unsigned elseRounds) noexcept
{
    if (elseRounds == 0)
        std::puts("check-positions: no rounds");
    else
        ISOLINE_INVARIANT(elseRounds == 0);
}

void invariantInForBody(unsigned forRounds) noexcept
{
    for (unsigned round = 0; round < forRounds; ++round)
    {
        ISOLINE_INVARIANT(forRounds == 0);
    }
}

void invariantInWhileBody(unsigned whileRounds) noexcept
{
    auto rounds = whileRounds;
    while (rounds-- > 0)
    {
        ISOLINE_INVARIANT(whileRounds == 0);
    }
}

void invariantInDoBody(unsigned doRounds) noexcept
{
    auto rounds = doRounds;
    do
    {
        ISOLINE_INVARIANT(doRounds == 0);
    } while (rounds-- > 1);
}

void invariantInRangeForBody(unsigned rangeRounds) noexcept
{
    for ([[maybe_unused]] auto const round: std::views::iota(0U, rangeRounds))
    {
        ISOLINE_INVARIANT(rangeRounds == 0);
    }
}

} // namespace isoline::canary
