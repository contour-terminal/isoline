#pragma once

namespace isoline::api::testing
{
/// A testing header, which the proof reads: no finding.
struct Rows
{
    [[nodiscard]] int at(int index) const noexcept ISOLINE_PRE(index >= 0);
};
} // namespace isoline::api::testing
