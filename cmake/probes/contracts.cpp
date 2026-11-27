// SPDX-License-Identifier: Apache-2.0
//
// The contracts probe (cmake/IsolineLanguageFeatures.cmake): a precondition, a postcondition naming the
// result, a body assertion and <contracts>, compiled under the row's semantic and linked, so that a
// missing contracts runtime (libstdc++exp on GCC) fails here and not in the build.

#include <contracts>

namespace probe
{
auto half(int const value) noexcept -> int pre(value % 2 == 0) post(result : result * 2 == value)
{
    contract_assert(value / 2 * 2 == value);
    return value / 2;
}
} // namespace probe

auto main(int argc, char**) -> int
{
    using Violation = std::contracts::contract_violation;
    static_assert(sizeof(Violation) > 0);
    return probe::half(argc * 2) == argc ? 0 : 1;
}
