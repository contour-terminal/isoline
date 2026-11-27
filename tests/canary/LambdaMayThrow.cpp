// SPDX-License-Identifier: Apache-2.0
//
// Must not compile where contracts are native: ISOLINE_INVARIANT in a lambda that is not noexcept,
// defined inside a try block of a noexcept function. A throwing violation handler's exception would
// leave the lambda and reach the enclosing handler, which would go on past the failed check; the macro's
// static_assert refuses the lambda instead (tests/CMakeLists.txt builds this file in a test of its own).
#include <cstdio>

#include <isoline/meta/Check.hpp>

namespace isoline::canary
{

/// Checks @p rounds in a lambda that may throw: refused at compile time.
/// @param rounds Positive: the check fails.
void invariantInThrowingLambdaInTry(unsigned rounds) noexcept
{
    try
    {
        auto const check = [rounds]() {
            ISOLINE_INVARIANT(rounds == 0);
        };
        check();
    }
    catch (...)
    {
        std::puts("lambda-may-throw: the enclosing try caught the check");
    }
}

} // namespace isoline::canary
