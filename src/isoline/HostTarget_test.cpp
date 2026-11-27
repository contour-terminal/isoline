// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include <isoline/HostTarget.hpp>

using isoline::compiler::machine::OperatingSystemAbi;

TEST_CASE("HostTarget.describesThisBuild")
{
    // The ABI the compiler's predefined macros chose agrees with CMake's name for the system the tests
    // run on; every build the tests run in targets one architecture Isoline describes.
    constexpr auto SystemName = std::string_view { ISOLINE_TEST_SYSTEM_NAME };
    auto expected = OperatingSystemAbi::SystemV;
    if (SystemName == "Windows")
        expected = OperatingSystemAbi::Windows;
    else if (SystemName == "Darwin" || SystemName == "iOS")
        expected = OperatingSystemAbi::Apple;
    INFO("system " << SystemName);
    CHECK(isoline::HostOperatingSystemAbi == expected);
    CHECK(isoline::HostArchitecture.has_value());
}
