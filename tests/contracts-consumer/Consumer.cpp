// SPDX-License-Identifier: Apache-2.0
//
// A consumer of isoline/Contracts.hpp, which fixes the package's declaration contracts when it is built
// (tests/CMakeLists.txt, the contracts-consumer rows): compiled alone, as a consumer's translation unit
// is, at the standard and with the flags of its row. Where the package's contracts are native, a unit
// compiled without contracts (C++23, or C++26 with -fno-contracts) stops at the header's #error; one compiled
// as the package's interface asks, and every unit on the fallback path, compiles.
// ISOLINE_EXPECTED_DECLARATION_CONTRACTS is the row's path, so the mode macro is checked against what CMake
// selected.

#include <isoline/Contracts.hpp>

static_assert(ISOLINE_DECLARATION_CONTRACTS == ISOLINE_EXPECTED_DECLARATION_CONTRACTS,
              "isoline/Contracts.hpp records the contracts path CMake selected");

namespace isoline::fixture
{

/// A class whose member function declares a precondition, as an installed header does.
class Positive
{
  public:
    /// Doubles @p value, which must be positive.
    /// @param value The value.
    /// @return Twice @p value.
    [[nodiscard]] int twice(int value) const noexcept ISOLINE_PRE(value > 0);
};

/// Names the declaration, so that it is parsed as a consumer's use would.
/// @return The address of the member function.
auto twiceAddress() noexcept -> int (Positive::*)(int) const noexcept;

auto twiceAddress() noexcept -> int (Positive::*)(int) const noexcept
{
    return &Positive::twice;
}

} // namespace isoline::fixture
