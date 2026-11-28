#pragma once

#include <isoline/Contracts.hpp>

namespace isoline::api
{
/// Declaration contracts on declarations, each defined in a .cpp.
class Function
{
  public:
    explicit Function(int value) noexcept ISOLINE_PRE(value > 0);
    [[nodiscard]] int type() const noexcept ISOLINE_PRE(_value > 0);
    [[nodiscard]] int offset() const noexcept ISOLINE_POST(result, result >= _value);
    [[nodiscard]] int both(int at) const noexcept ISOLINE_PRE(at > 0) ISOLINE_POST(result, result > at);
    [[nodiscard]] int nested(int at) const noexcept ISOLINE_PRE((at > 0) && (f(at) > 0));

  private:
    int _value; ///< ISOLINE_PRE(x) { in a comment is no contract
};
} // namespace isoline::api
