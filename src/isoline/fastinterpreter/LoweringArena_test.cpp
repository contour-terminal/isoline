// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <tuple>

#include <isoline/fastinterpreter/LoweringArena.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>

using isoline::fastinterpreter::BudgetExceeded;
using isoline::fastinterpreter::CountingArena;
using isoline::fastinterpreter::Refusal;
using isoline::fastinterpreter::RefusalReason;

namespace
{

/// A memory resource the host has no memory for: every allocation throws.
class ExhaustedResource final: public std::pmr::memory_resource
{
  private:
    void* do_allocate(std::size_t /*bytes*/, std::size_t /*alignment*/) override { throw std::bad_alloc(); }
    void do_deallocate(void* /*pointer*/, std::size_t /*bytes*/, std::size_t /*alignment*/) override {}
    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
    {
        return this == &other;
    }
};

} // namespace

TEST_CASE("LoweringArena.chargesAllocationsAndTracksThePeak")
{
    auto arena = CountingArena { 1'000, *std::pmr::new_delete_resource() };
    CHECK(arena.budget() == 1'000);
    auto* const first = arena.allocate(300, 8);
    auto* const second = arena.allocate(200, 8);
    CHECK(arena.charged() == 500);
    arena.deallocate(first, 300, 8);
    CHECK(arena.charged() == 200);
    CHECK(arena.peak() == 500);
    arena.deallocate(second, 200, 8);
    CHECK(arena.charged() == 0);
    CHECK(arena.peak() == 500);
    CHECK_FALSE(arena.lastRefusal().has_value());
}

TEST_CASE("LoweringArena.allocationsDrawOnTheReservation")
{
    auto arena = CountingArena { 1'000, *std::pmr::new_delete_resource() };
    REQUIRE(arena.reserve(600).has_value());
    CHECK(arena.charged() == 600);
    CHECK(arena.reserved() == 600);
    // Covered by the reservation: nothing new is charged.
    auto* const covered = arena.allocate(400, 8);
    CHECK(arena.charged() == 600);
    CHECK(arena.reserved() == 200);
    // Partly covered: the rest of the reservation, and 100 bytes more.
    auto* const partly = arena.allocate(300, 8);
    CHECK(arena.charged() == 700);
    CHECK(arena.reserved() == 0);
    CHECK(arena.peak() == 700);
    arena.deallocate(covered, 400, 8);
    arena.deallocate(partly, 300, 8);
    CHECK(arena.charged() == 0);
}

TEST_CASE("LoweringArena.releaseReturnsAReservation")
{
    auto arena = CountingArena { 1'000, *std::pmr::new_delete_resource() };
    REQUIRE(arena.reserve(600).has_value());
    arena.release(250);
    CHECK(arena.charged() == 350);
    CHECK(arena.reserved() == 350);
    arena.release(arena.reserved());
    CHECK(arena.charged() == 0);
    CHECK(arena.peak() == 600);
}

TEST_CASE("LoweringArena.refusesAReservationPastTheBudget")
{
    auto arena = CountingArena { 1'000, *std::pmr::new_delete_resource() };
    REQUIRE(arena.reserve(700).has_value());
    auto const refused = arena.reserve(301);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == BudgetExceeded { .requested = 301, .available = 300 });
    CHECK(arena.charged() == 700);
    REQUIRE(arena.reserve(300).has_value());
    CHECK(arena.charged() == 1'000);
}

TEST_CASE("LoweringArena.refusesAnAllocationPastTheBudget")
{
    auto arena = CountingArena { 1'000, *std::pmr::new_delete_resource() };
    REQUIRE(arena.reserve(300).has_value());
    // 300 of it drawn on the reservation, 600 charged afresh: 100 left of the budget.
    auto* const held = arena.allocate(900, 8);
    CHECK(arena.charged() == 900);
    CHECK_THROWS_AS(std::ignore = arena.allocate(101, 8), std::bad_alloc);
    REQUIRE(arena.lastRefusal().has_value());
    CHECK(*arena.lastRefusal() == Refusal { .reason = RefusalReason::Budget, .requested = 101 });
    CHECK(arena.charged() == 900);
    CHECK(arena.peak() == 900);
    auto* const fits = arena.allocate(100, 8);
    CHECK(arena.charged() == 1'000);
    arena.deallocate(fits, 100, 8);
    arena.deallocate(held, 900, 8);
}

TEST_CASE("LoweringArena.aRefusalLeavesTheReservation")
{
    auto arena = CountingArena { 1'000, *std::pmr::new_delete_resource() };
    REQUIRE(arena.reserve(600).has_value());
    // 600 reserved: an allocation of 1'001 needs 401 beside it, of the 400 left.
    CHECK_THROWS_AS(std::ignore = arena.allocate(1'001, 8), std::bad_alloc);
    CHECK(arena.reserved() == 600);
    CHECK(arena.charged() == 600);
    auto* const fits = arena.allocate(1'000, 8);
    CHECK(arena.reserved() == 0);
    CHECK(arena.charged() == 1'000);
    arena.deallocate(fits, 1'000, 8);
}

TEST_CASE("LoweringArena.recordsAHostRefusal")
{
    auto exhausted = ExhaustedResource {};
    auto arena = CountingArena { 1'000, exhausted };
    REQUIRE(arena.reserve(50).has_value());
    CHECK_THROWS_AS(std::ignore = arena.allocate(80, 8), std::bad_alloc);
    REQUIRE(arena.lastRefusal().has_value());
    CHECK(*arena.lastRefusal() == Refusal { .reason = RefusalReason::HostMemory, .requested = 80 });
    // Nothing was allocated, so nothing was charged or drawn.
    CHECK(arena.charged() == 50);
    CHECK(arena.reserved() == 50);
}

TEST_CASE("LoweringArena.equalsOnlyItself")
{
    auto arena = CountingArena { 1'000, *std::pmr::new_delete_resource() };
    auto other = CountingArena { 1'000, *std::pmr::new_delete_resource() };
    CHECK(arena.is_equal(arena));
    CHECK_FALSE(arena.is_equal(other));
}

// The contract tests below end the process at a failed Require, so they are hidden from the unit run:
// tests/CMakeLists.txt runs each in a process of its own and expects the Require's text
// (isoline::runtime::testing::endAtContractViolation()).

TEST_CASE("Contract.LoweringArena.releaseOnlyWhatIsReserved", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto arena = CountingArena { 1'000, *std::pmr::new_delete_resource() };
    REQUIRE(arena.reserve(100).has_value());
    arena.release(101);
    FAIL("SURVIVED: releasing more than was reserved");
}
