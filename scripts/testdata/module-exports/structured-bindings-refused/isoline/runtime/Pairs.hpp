#pragma once

namespace isoline::runtime
{
struct Pair
{
    int first;
    int second;
};

// Structured bindings at namespace scope, in an internal namespace: each variable is TU-local, or of a
// linkage the lint cannot tell.
auto const [lowest, highest] = Pair { 1, 2 };
static auto [left, right] = Pair { 3, 4 };
auto [width, height] = Pair { 5, 6 };

// A kind the declaration-kind table does not know is a finding in an internal namespace too.
asm("");
} // namespace isoline::runtime
