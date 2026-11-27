#pragma once

namespace isoline::runtime
{
/// A check in a private header.
inline void check(int value) noexcept
{
    ISOLINE_INVARIANT(value > 0);
}

/// A contract specifier after a declarator.
int f(int value) noexcept pre(value > 0);

/// One after a const member declarator.
struct S
{
    int g() const post(r: r > 0);
};
} // namespace isoline::runtime
