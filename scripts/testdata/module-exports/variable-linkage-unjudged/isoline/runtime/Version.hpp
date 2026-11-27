#pragma once

namespace isoline::runtime
{
// Two variables named `revision`: qualified by isoline::runtime, the name is ambiguous, so the compiler
// cannot say whether the outer one is const.
int const revision = 1;

inline namespace v1
{
    int revision = 2;
} // namespace v1
} // namespace isoline::runtime
