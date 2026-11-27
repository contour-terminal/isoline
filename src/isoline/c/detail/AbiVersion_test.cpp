// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>

#include <isoline/c/detail/AbiVersion.hpp>

using isoline::c::detail::AbiVersion;
using isoline::c::detail::compatible;

namespace
{

/// One case of the compatibility rule: a library, the header a program was built with, and the verdict.
struct CompatibilityRow
{
    AbiVersion library;
    AbiVersion header;
    bool compatible;
};

} // namespace

TEST_CASE("AbiVersion.compatibilityRows", "[c-api]")
{
    auto const row = GENERATE(values<CompatibilityRow>({
        // Before 1.0 the minors must be equal.
        { { 0, 1 }, { 0, 1 }, true },
        { { 0, 1 }, { 0, 0 }, false },
        { { 0, 1 }, { 0, 2 }, false },
        { { 0, 1 }, { 1, 1 }, false },
        { { 0, 2 }, { 0, 2 }, true },
        // From 1.0 a library serves every minor up to its own, of its own major.
        { { 1, 3 }, { 1, 3 }, true },
        { { 1, 3 }, { 1, 0 }, true },
        { { 1, 3 }, { 1, 4 }, false },
        { { 1, 3 }, { 2, 3 }, false },
        { { 1, 3 }, { 0, 3 }, false },
        { { 2, 0 }, { 1, 0 }, false },
    }));
    CAPTURE(row.library.major, row.library.minor, row.header.major, row.header.minor);
    CHECK(compatible(row.library, row.header) == row.compatible);
}

TEST_CASE("AbiVersion.unpackSplitsMajorAndMinor", "[c-api]")
{
    STATIC_CHECK(AbiVersion::unpack(0x0003'0002U).major == 3);
    STATIC_CHECK(AbiVersion::unpack(0x0003'0002U).minor == 2);
    STATIC_CHECK(AbiVersion::unpack(0xFFFF'FFFFU).major == 0xFFFFU);
}
