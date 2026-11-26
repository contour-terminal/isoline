// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <isoline/Version.hpp>

TEST_CASE("Version.isSemantic")
{
    // A constant expression, and a function a run-time caller may call as well.
    static_assert(isoline::version().major == 0);
    // Through a volatile pointer, so that the call runs rather than folds into a constant.
    auto* volatile versionOf = &isoline::version;
    auto const v = versionOf();
    CHECK(v.major == 0);
    CHECK(v.minor == 1);
    CHECK(v.patch == 0);
}
