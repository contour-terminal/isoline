#pragma once

/// A test header's class, which the proof does not read.
struct Positions
{
    int at(int index) const noexcept ISOLINE_PRE(index >= 0);
};
