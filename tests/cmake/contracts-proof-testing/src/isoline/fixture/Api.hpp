// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <isoline/Contracts.hpp>

namespace isoline::fixture
{

/// A public class whose declaration contract is proven noexcept.
class Api
{
  public:
    /// @param index Must not be negative.
    /// @return @p index.
    [[nodiscard]] int at(int index) const noexcept ISOLINE_PRE(index >= 0);
};

} // namespace isoline::fixture
