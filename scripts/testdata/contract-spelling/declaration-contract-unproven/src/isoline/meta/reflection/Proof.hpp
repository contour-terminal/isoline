#pragma once

/// A header of the proof's own seam, which it does not read.
struct Proven
{
    int at(int index) const noexcept ISOLINE_PRE(index >= 0);
};
