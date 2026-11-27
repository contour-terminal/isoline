// SPDX-License-Identifier: Apache-2.0
//
// Must not compile (tests/CMakeLists.txt, compile-fail-declaration-contract-on-free-function; where
// contracts are native): an ISOLINE_PRE on a noexcept free function. A free function's contract sees its
// namespace as the current scope, so its noexcept proof cannot name the function, and refuses it.

#include <isoline/Contracts.hpp>

namespace isoline::fixture
{

/// Doubles @p value, which must be positive.
/// @param value The value.
/// @return Twice @p value.
[[nodiscard]] int twice(int value) noexcept ISOLINE_PRE(value > 0);

} // namespace isoline::fixture

/// Calls the refused declaration, so that it is used.
int fixtureTwice()
{
    return isoline::fixture::twice(1);
}
