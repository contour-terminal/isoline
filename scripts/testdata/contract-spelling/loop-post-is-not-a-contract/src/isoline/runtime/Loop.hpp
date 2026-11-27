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
    // A comment that reads ) pre(x) is no code.
}
} // namespace isoline::runtime
