#pragma once

namespace isoline::runtime
{
// A braced linkage specification gives its declarations no `extern`: they keep the linkage they spell.
extern "C"
{
    static int ticks = 0;
    int const tickLimit = 1;
    static int tick()
    {
        return ticks;
    }
}
} // namespace isoline::runtime
