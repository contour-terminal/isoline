// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <new>

#include <isoline/c/detail/Boundary.hpp>

using isoline::c::detail::boundary;

TEST_CASE("Boundary.returnsWhatTheBodyReturns", "[c-api]")
{
    auto bodyCalls = 0;
    auto exhaustedCalls = 0;
    auto const result = boundary(
        [&] {
            ++bodyCalls;
            return 42;
        },
        [&] {
            ++exhaustedCalls;
            return -1;
        });
    CHECK(result == 42);
    CHECK(bodyCalls == 1);
    CHECK(exhaustedCalls == 0);
}

TEST_CASE("Boundary.hostMemoryExhaustedGivesItsResult", "[c-api]")
{
    auto exhaustedCalls = 0;
    auto const* const fallback = "out of host memory";
    auto const* const result = boundary([]() -> char const* { throw std::bad_alloc {}; },
                                        [&] {
                                            ++exhaustedCalls;
                                            return fallback;
                                        });
    CHECK(result == fallback);
    CHECK(exhaustedCalls == 1);
}

TEST_CASE("Boundary.voidBodyReportsExhaustion", "[c-api]")
{
    auto exhaustedCalls = 0;
    boundary([] { throw std::bad_alloc {}; }, [&] { ++exhaustedCalls; });
    CHECK(exhaustedCalls == 1);
}

TEST_CASE("Boundary.isNoexcept", "[c-api]")
{
    STATIC_CHECK(noexcept(boundary([] { return 0; }, [] { return 1; })));
}
