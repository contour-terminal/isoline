// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <isoline/Contracts.hpp>

TEST_CASE("Contracts.libraryModeIsTheHeaders")
{
    // The library's mode (src/isoline/Contracts.cpp) is the one the header it was built with records: a
    // translation unit that links it reads the same ISOLINE_DECLARATION_CONTRACTS. Through a volatile
    // pointer, so that the call runs rather than folds into a constant.
    auto* volatile modeOf = &isoline::meta::detail::declarationContractsMode;
    CHECK(modeOf() == ISOLINE_DECLARATION_CONTRACTS);
}
