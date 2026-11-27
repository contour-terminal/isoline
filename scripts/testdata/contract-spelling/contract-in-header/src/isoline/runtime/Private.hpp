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
    int a() & pre(true);
    int b() && post(r: r > 0);
    int c() const& noexcept pre(true);
    int d() override pre(true);
    int e() final post(r: r > 0);
};

/// A trailing return type.
auto h(int value) -> int pre(value > 0);
auto k(int value) -> std::pair<int, int>* post(r: r != nullptr);
} // namespace isoline::runtime
