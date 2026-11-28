// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory_resource>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/registerallocation/ParallelMoves.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>

using isoline::compiler::machine::Location;
using isoline::compiler::machine::LocationKind;
using isoline::compiler::machine::Move;
using isoline::compiler::machine::PhysicalRegister;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::registerallocation::sequentializeParallelMoves;

namespace
{

/// General register @p number.
Location general(uint8_t number)
{
    return Location::of(PhysicalRegister { .registerClass = RegisterClass::General, .number = number });
}

/// Float register @p number.
Location floating(uint8_t number)
{
    return Location::of(PhysicalRegister { .registerClass = RegisterClass::Float, .number = number });
}

/// The slot at @p offset.
Location slot(uint32_t offset)
{
    return Location::slot(RegisterClass::General, offset);
}

/// The scratch slot of these tests.
constexpr auto Scratch = Location::slot(RegisterClass::General, 100'000'000);

/// A fixed sequence of 64-bit words (splitmix64), the same on every host and run.
class SplitMix
{
  public:
    explicit SplitMix(uint64_t state) noexcept: _state { state } {}

    /// The next word below @p bound (non-zero).
    uint64_t below(uint64_t bound) noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15ULL;
        auto mix = _state;
        mix = (mix ^ (mix >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        mix = (mix ^ (mix >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return (mix ^ (mix >> 31U)) % bound;
    }

  private:
    uint64_t _state;
};

/// A place as the machine sees it: a slot whatever its class, a register of its class.
using Place = std::tuple<LocationKind, RegisterClass, uint32_t>;

Place placeOf(Location const& location)
{
    return { location.kind,
             location.kind == LocationKind::Slot ? RegisterClass::General : location.registerClass,
             location.index };
}

/// Every place's content: the place it started in, unless something was written.
using Contents = std::map<Place, Place>;

/// Runs @p moves one after the other over @p contents.
void runSequentially(std::span<Move const> moves, Contents& contents)
{
    for (auto const& move: moves)
    {
        auto const from = placeOf(move.from);
        auto const value = contents.contains(from) ? contents.at(from) : from;
        contents[placeOf(move.to)] = value;
    }
}

/// Whether @p sequential has @p parallel's effect: every destination holds its source's original value, and
/// nothing else but the scratch slot changed.
bool hasTheParallelEffect(std::span<Move const> parallel, std::span<Move const> sequential)
{
    auto contents = Contents {};
    runSequentially(sequential, contents);
    auto expected = Contents {};
    for (auto const& move: parallel)
        expected[placeOf(move.to)] = placeOf(move.from);
    for (auto const& [place, value]: contents)
    {
        if (place == placeOf(Scratch))
            continue;
        auto const wanted = expected.contains(place) ? expected.at(place) : place;
        if (value != wanted)
            return false;
    }
    return std::ranges::all_of(expected, [&](auto const& entry) {
        return contents.contains(entry.first) ? contents.at(entry.first) == entry.second
                                              : entry.first == entry.second;
    });
}

/// How many moves of @p moves touch the scratch slot.
std::size_t scratchMoves(std::span<Move const> moves)
{
    return static_cast<std::size_t>(std::ranges::count_if(
        moves, [](Move const& move) { return move.from.samePlace(Scratch) || move.to.samePlace(Scratch); }));
}

} // namespace

TEST_CASE("ParallelMoves.breaksCycles")
{
    auto memory = std::pmr::monotonic_buffer_resource {};

    SECTION("a swap goes through the scratch slot")
    {
        auto const moves = std::to_array(
            { Move { .from = general(0), .to = general(1) }, Move { .from = general(1), .to = general(0) } });
        auto const sequential = sequentializeParallelMoves(moves, Scratch, memory);
        CHECK(sequential.size() == 3);
        CHECK(scratchMoves(sequential) == 2);
        CHECK(hasTheParallelEffect(moves, sequential));
    }

    SECTION("a rotation of three registers and a slot")
    {
        auto const moves = std::to_array({ Move { .from = general(0), .to = general(1) },
                                           Move { .from = general(1), .to = slot(8) },
                                           Move { .from = slot(8), .to = general(2) },
                                           Move { .from = general(2), .to = general(0) } });
        auto const sequential = sequentializeParallelMoves(moves, Scratch, memory);
        CHECK(sequential.size() == 5);
        CHECK(hasTheParallelEffect(moves, sequential));
    }

    SECTION("a chain needs no scratch, and runs from its end")
    {
        auto const moves = std::to_array({ Move { .from = general(0), .to = general(1) },
                                           Move { .from = general(1), .to = general(2) },
                                           Move { .from = general(2), .to = slot(0) } });
        auto const sequential = sequentializeParallelMoves(moves, Scratch, memory);
        REQUIRE(sequential.size() == 3);
        CHECK(scratchMoves(sequential) == 0);
        CHECK(sequential[0] == moves[2]);
        CHECK(sequential[2] == moves[0]);
        CHECK(hasTheParallelEffect(moves, sequential));
    }

    SECTION("a cycle with a tree hanging off it, and a second cycle")
    {
        auto const moves = std::to_array({
            Move { .from = general(0), .to = general(1) },
            Move { .from = general(1), .to = general(0) },
            Move { .from = general(0), .to = general(3) }, // fans out of the cycle
            Move { .from = general(3), .to = slot(16) },   // and on
            Move { .from = floating(0), .to = floating(1) },
            Move { .from = floating(1), .to = floating(0) },
        });
        auto const sequential = sequentializeParallelMoves(moves, Scratch, memory);
        CHECK(hasTheParallelEffect(moves, sequential));
        CHECK(scratchMoves(sequential) == 4);
        // The scratch holds a float's bits as a float.
        CHECK(std::ranges::any_of(sequential, [](Move const& move) {
            return move.to.samePlace(Scratch) && move.to.registerClass == RegisterClass::Float;
        }));
    }

    SECTION("a move onto itself is dropped, and registers of two classes are two places")
    {
        auto const moves = std::to_array({ Move { .from = general(1), .to = general(1) },
                                           Move { .from = general(0), .to = floating(0) },
                                           Move { .from = floating(0), .to = general(0) } });
        auto const sequential = sequentializeParallelMoves(moves, Scratch, memory);
        CHECK(sequential.size() == 3);
        CHECK(hasTheParallelEffect(moves, sequential));
    }

    SECTION("nothing to do")
    {
        CHECK(sequentializeParallelMoves({}, Scratch, memory).empty());
    }

    SECTION("a rotation of each of x86-64's allocatable register files")
    {
        // Every allocatable General register takes the next one's value, and every Float register too: two
        // cycles of 12 and 16, each broken once through the scratch slot.
        auto moves = std::vector<Move> {};
        for (auto const file:
             { std::span<PhysicalRegister const> { isoline::compiler::x86_64::AllocatableGeneral },
               std::span<PhysicalRegister const> { isoline::compiler::x86_64::AllocatableFloat } })
            for (auto const index: std::views::iota(std::size_t { 0 }, file.size()))
                moves.push_back(Move { .from = Location::of(file[(index + 1) % file.size()]),
                                       .to = Location::of(file[index]) });
        auto const sequential = sequentializeParallelMoves(moves, Scratch, memory);
        CHECK(hasTheParallelEffect(moves, sequential));
        CHECK(sequential.size() == moves.size() + 2);
        CHECK(scratchMoves(sequential) == 4);
    }
}

TEST_CASE("ParallelMoves.matchTheParallelEffectOnRandomSets")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto random = SplitMix { 20'261'127 };
    auto places = std::vector<Location> {};
    for (auto const number: std::views::iota(uint8_t { 0 }, uint8_t { 4 }))
    {
        places.push_back(general(number));
        places.push_back(floating(number));
        places.push_back(slot(uint32_t { number } * 8));
    }
    for ([[maybe_unused]] auto const round: std::views::iota(0, 2'000))
    {
        auto destinations = places;
        for (auto const index: std::views::iota(std::size_t { 1 }, destinations.size()) | std::views::reverse)
            std::swap(destinations[index], destinations[random.below(index + 1)]);
        auto const count = random.below(places.size() + 1);
        auto moves = std::vector<Move> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
            moves.push_back(Move { .from = places[random.below(places.size())], .to = destinations[index] });
        auto const sequential = sequentializeParallelMoves(moves, Scratch, memory);
        REQUIRE(hasTheParallelEffect(moves, sequential));
        CHECK(sequential.size() <= moves.size() + (moves.size() / 2));
    }
}

TEST_CASE("ParallelMoves.longCyclesAreLinear")
{
    // One rotation of 200'000 slots: a single cycle, one scratch save and one restore.
    auto memory = std::pmr::monotonic_buffer_resource {};
    constexpr auto Count = uint32_t { 200'000 };
    auto moves = std::vector<Move> {};
    moves.reserve(Count);
    for (auto const index: std::views::iota(uint32_t { 0 }, Count))
        moves.push_back(Move { .from = slot(index * 8), .to = slot(((index + 1) % Count) * 8) });
    auto const sequential = sequentializeParallelMoves(moves, Scratch, memory);
    CHECK(sequential.size() == Count + 1);
    CHECK(scratchMoves(sequential) == 2);
    CHECK(sequential.front().from.samePlace(slot(8)));
}
