#pragma once

namespace isoline::c::detail
{
/// A handle of the C API.
class Handle
{
public:
    /// What it holds.
    [[nodiscard]] int value() const noexcept ISOLINE_PRE(isAlive());
    /// Whether it is alive.
    [[nodiscard]] bool isAlive() const noexcept;
};
} // namespace isoline::c::detail
