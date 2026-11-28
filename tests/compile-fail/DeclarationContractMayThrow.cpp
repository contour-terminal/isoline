// SPDX-License-Identifier: Apache-2.0
//
// Must not compile (tests/CMakeLists.txt, compile-fail-declaration-contract-may-throw; where contracts are
// native): an ISOLINE_PRE on a member function that may throw. A violation handler that throws would let
// its caller continue past the check, so the declaration contract's noexcept proof refuses it.

#include <isoline/Contracts.hpp>

namespace
{

/// A class whose one member function may throw, and carries a declaration contract.
class MayThrow
{
  public:
    /// Doubles @p value, which must be positive.
    /// @param value The value.
    /// @return Twice @p value.
    [[nodiscard]] int twice(int value) const ISOLINE_PRE(value > 0);
};

} // namespace

/// Calls the refused declaration, so that it is used.
int fixtureTwice()
{
    return MayThrow {}.twice(1);
}
