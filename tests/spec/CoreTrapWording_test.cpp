// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>

#include "CoreTrapWording.hpp"
#include <isoline/api/Types.hpp>
#include <isoline/runtime/Trap.hpp>

using isoline::spectest::coreTrapWording;
using isoline::spectest::CoreTrapWording;

TEST_CASE("CoreTrapWording.coversEveryTrapCode")
{
    // One worded row per trap the core raises, at its enumerator, whatever TrapTable adds next.
    REQUIRE(CoreTrapWording.size() == isoline::runtime::TrapTable.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, isoline::runtime::TrapTable.size()))
    {
        auto const code = isoline::runtime::TrapTable.at(index).code;
        INFO(isoline::api::specMessage(code));
        CHECK(CoreTrapWording.at(index).code == code);
        CHECK_FALSE(coreTrapWording(code).empty());
    }

    // The wordings the component suites assert, Wasmtime's.
    CHECK(coreTrapWording(isoline::api::TrapCode::Unreachable) == "wasm `unreachable` instruction executed");
    CHECK(coreTrapWording(isoline::api::TrapCode::OutOfBoundsMemoryAccess) == "out of bounds memory access");
    CHECK(coreTrapWording(isoline::api::TrapCode::CallStackExhausted) == "call stack exhausted");
    CHECK(coreTrapWording(isoline::api::TrapCode::IntegerDivideByZero) == "integer divide by zero");
}
