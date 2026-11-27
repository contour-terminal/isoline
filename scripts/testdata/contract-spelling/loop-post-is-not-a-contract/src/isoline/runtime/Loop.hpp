#pragma once

namespace isoline::runtime
{
/// A loop whose post() and pre() are member functions, not contract specifiers.
template <typename Loop>
void drive(Loop& loop)
{
    loop.post(1);
    loop.pre (2);
    loop.get().post(3);
    loop.get()->post(4);
    auto const prepared = loop.prepare(5) & loop.prepare(6);
    loop.postpone(prepared);
    // A comment that reads ) pre(x) is no code.
}
} // namespace isoline::runtime
