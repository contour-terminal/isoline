#pragma once

#include <isoline/Contracts.hpp>

namespace isoline::api
{
/// Declaration contracts on functions a header defines or declares inline.
class Function
{
  public:
    explicit Function(int value) noexcept ISOLINE_PRE(value > 0): _value { value } {}
    [[nodiscard]] int type() const noexcept ISOLINE_PRE(_value > 0) { return _value; }
    [[nodiscard]] constexpr int offset() const noexcept ISOLINE_POST(result, result >= _value);
    [[nodiscard]] inline int both(int at) const noexcept ISOLINE_PRE(at > 0);
    Function(Function const&) noexcept ISOLINE_PRE(true) = default;
    [[nodiscard]] int fine(int at) const noexcept ISOLINE_PRE(at > 0);

  private:
    int _value;
};

template <typename T>
[[nodiscard]] T twice(T value) noexcept ISOLINE_PRE(value > 0);

[[nodiscard]] consteval int three() noexcept ISOLINE_POST(result, result == 3);

/// A class template, whose members' contracts its consumers would compile.
template <typename T>
class Box
{
  public:
    [[nodiscard]] T get(int at) const noexcept ISOLINE_PRE(at > 0);

    /// A class nested in it.
    struct Inner
    {
        [[nodiscard]] int size() const noexcept ISOLINE_POST(result, result >= 0);
    };
};

/// A class after it, whose contract is fine.
class After
{
  public:
    [[nodiscard]] int fine() const noexcept ISOLINE_POST(result, result >= 0);
};
} // namespace isoline::api
