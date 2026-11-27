// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <isoline/Contracts.hpp>

namespace isoline::fixture::testing
{

/// A test helper whose member function may throw, and carries a declaration contract: the proof refuses it.
class Throwing
{
  public:
    /// @param index Must not be negative.
    /// @return @p index.
    [[nodiscard]] int at(int index) const ISOLINE_PRE(index >= 0);
};

} // namespace isoline::fixture::testing
