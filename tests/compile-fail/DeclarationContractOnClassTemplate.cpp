// SPDX-License-Identifier: Apache-2.0
//
// Must not compile (tests/CMakeLists.txt, compile-fail-declaration-contract-on-class-template; where
// contracts are native): an ISOLINE_PRE on a noexcept member function of a class template, instantiated.
// Its contract would be compiled with the template, in each consumer's translation unit, under the
// consumer's semantic, so the declaration contract's proof refuses it.

#include <isoline/Contracts.hpp>

namespace isoline::fixture
{

/// A class template whose member function carries a declaration contract.
template <typename T>
class Box
{
  public:
    /// @param value Must be positive.
    /// @return @p value.
    [[nodiscard]] T get(T value) const noexcept ISOLINE_PRE(value > 0);
};

template <typename T>
T Box<T>::get(T value) const noexcept
{
    return value;
}

/// Instantiates the member function, whose contract the proof then evaluates.
/// @return 1.
int fixtureGet() noexcept;

int fixtureGet() noexcept
{
    return Box<int> {}.get(1);
}

} // namespace isoline::fixture
