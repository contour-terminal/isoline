/* SPDX-License-Identifier: Apache-2.0 */
/* The ABI version a program is compiled against, and the rule that decides whether a library serves it. */

#include <stdint.h>

#include <isoline/c/isoline.h>

#define C_TEST_SUITE    "abi"
#define C_TEST_CASES(X) X(versionMatchesHeader) X(compatibilityRule)
#include "IsolineCTest.h"

/* The header's version, its major and its minor, as the unsigned values the functions take. */
static uint32_t const headerVersion = (uint32_t) ISOLINE_ABI_VERSION;
static uint32_t const headerMajor = (uint32_t) ISOLINE_ABI_VERSION_MAJOR;
static uint32_t const headerMinor = (uint32_t) ISOLINE_ABI_VERSION_MINOR;

static uint32_t abiVersionOf(uint32_t major, uint32_t minor)
{
    return (major << 16) | minor;
}

ISOLINE_TEST_CASE(versionMatchesHeader)
{
    CHECK(isoline_abi_version() == headerVersion);
    CHECK(abiVersionOf(headerMajor, headerMinor) == headerVersion);
}

ISOLINE_TEST_CASE(compatibilityRule)
{
    /* The header the library was built with. */
    CHECK(isoline_abi_compatible(headerVersion));
    /* A program built for a later major, or for a later minor, needs what this library lacks. */
    CHECK(!isoline_abi_compatible(abiVersionOf(headerMajor + 1, headerMinor)));
    CHECK(!isoline_abi_compatible(abiVersionOf(headerMajor, headerMinor + 1)));
    /* An earlier minor is served from 1.0 on; before 1.0 every minor may break the last. Every other
       combination is a row of AbiVersion_test.cpp. */
    if (headerMinor > 0)
        CHECK(isoline_abi_compatible(abiVersionOf(headerMajor, headerMinor - 1)) == (headerMajor > 0));
}

int main(int argc, char** argv)
{
    return c_test_run_all(argc, argv);
}
