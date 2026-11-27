// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <isoline/binary/BodyUnits.hpp>
#include <isoline/binary/Module.hpp>

using isoline::binary::bodyUnits;

TEST_CASE("BodyUnits.countEveryDecodedPart")
{
    // A decoded body unit is an instruction, a br_table label, a catch clause, a typed select's type
    // or a local run: each took at least one byte of the body, so a bound per unit is one per byte.
    auto function = isoline::binary::Function {};
    CHECK(bodyUnits(function) == 0);
    function.body.resize(5);
    function.brTableLabels.resize(7);
    function.catchClauses.resize(2);
    function.selectTypes.resize(3);
    function.locals.resize(11);
    CHECK(bodyUnits(function) == 5 + 7 + 2 + 3 + 11);
}
