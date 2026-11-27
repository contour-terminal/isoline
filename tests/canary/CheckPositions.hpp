// SPDX-License-Identifier: Apache-2.0
//
// The positions a contract-spelled check must be evaluated in, each a noexcept function whose check
// fails when given a positive count (CheckPositions.cpp). GCC 16.2.1 drops a contract_assert from some
// statement positions (docs/language-features.md); the canary runs each of these, so that a macro
// expansion which loses one fails a test instead of checking nothing.
#pragma once

namespace isoline::canary
{

/// ISOLINE_PRECONDITION as the function's first statement.
/// @param entryRounds Positive: the check fails.
void preconditionFirst(unsigned entryRounds) noexcept;

/// ISOLINE_PRECONDITION as the body of a braceless `if`.
/// @param ifRounds Positive: the check fails.
void preconditionInBracelessIf(unsigned ifRounds) noexcept;

/// ISOLINE_INVARIANT as the body of a braceless `else`.
/// @param elseRounds Positive: the check fails.
void invariantInBracelessElse(unsigned elseRounds) noexcept;

/// ISOLINE_INVARIANT as the first statement of a `for` body.
/// @param forRounds Positive: the check fails.
void invariantInForBody(unsigned forRounds) noexcept;

/// ISOLINE_INVARIANT as the first statement of a `while` body.
/// @param whileRounds Positive: the check fails.
void invariantInWhileBody(unsigned whileRounds) noexcept;

/// ISOLINE_INVARIANT as the first statement of a `do` body.
/// @param doRounds Positive: the check fails.
void invariantInDoBody(unsigned doRounds) noexcept;

/// ISOLINE_INVARIANT as the first statement of a range-based `for` body.
/// @param rangeRounds Positive: the check fails.
void invariantInRangeForBody(unsigned rangeRounds) noexcept;

/// ISOLINE_INVARIANT in a noexcept lambda defined inside a try block of a noexcept function: the
/// lambda's own noexcept boundary ends the process, so the enclosing try never catches the check.
/// @param lambdaRounds Positive: the check fails.
void invariantInNoexceptLambdaInTry(unsigned lambdaRounds) noexcept;

} // namespace isoline::canary
