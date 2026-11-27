// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

#include <isoline/meta/EnumRows.hpp>

namespace
{

/// The enum the fixture tables are indexed by.
enum class Fixture : uint8_t
{
    A,
    B,
    C,
};

/// The last Fixture enumerator.
constexpr auto LastFixture = Fixture::C;

/// One row of a fixture table: its enumerator, and something it says about it.
struct FixtureRow
{
    Fixture code; ///< The enumerator; equal to the row's index where the table is in place.
    int payload;  ///< What the row says.
};

constexpr auto RowsInOrder = std::to_array<FixtureRow>({
    { .code = Fixture::A, .payload = 10 },
    { .code = Fixture::B, .payload = 11 },
    { .code = Fixture::C, .payload = 12 },
});

constexpr auto SwappedRows = std::to_array<FixtureRow>({
    { .code = Fixture::A, .payload = 10 },
    { .code = Fixture::C, .payload = 12 },
    { .code = Fixture::B, .payload = 11 },
});

constexpr auto MissingLastRow = std::to_array<FixtureRow>({
    { .code = Fixture::A, .payload = 10 },
    { .code = Fixture::B, .payload = 11 },
});

constexpr auto ExtraRow = std::to_array<FixtureRow>({
    { .code = Fixture::A, .payload = 10 },
    { .code = Fixture::B, .payload = 11 },
    { .code = Fixture::C, .payload = 12 },
    { .code = Fixture::A, .payload = 13 },
});

/// A table one row too long, whose extra row names a value past LastFixture, and so its own index.
constexpr auto ExtraRowPastLast = std::to_array<FixtureRow>({
    { .code = Fixture::A, .payload = 10 },
    { .code = Fixture::B, .payload = 11 },
    { .code = Fixture::C, .payload = 12 },
    { .code = static_cast<Fixture>(3), .payload = 13 },
});

} // namespace

using isoline::meta::firstMisplacedRow;
using isoline::meta::NoMisplacedRow;
using isoline::meta::rowsAtEnumeratorIndex;
using isoline::meta::sizedByEnum;

TEST_CASE("EnumRows.acceptsRowsInOrder")
{
    STATIC_REQUIRE(rowsAtEnumeratorIndex<LastFixture>(RowsInOrder, &FixtureRow::code));
    STATIC_REQUIRE(firstMisplacedRow<LastFixture>(RowsInOrder, &FixtureRow::code) == NoMisplacedRow);
}

TEST_CASE("EnumRows.refusesSwappedRows")
{
    STATIC_REQUIRE_FALSE(rowsAtEnumeratorIndex<LastFixture>(SwappedRows, &FixtureRow::code));
    STATIC_REQUIRE(firstMisplacedRow<LastFixture>(SwappedRows, &FixtureRow::code) == 1);
}

TEST_CASE("EnumRows.refusesAMissingLastRow")
{
    STATIC_REQUIRE_FALSE(rowsAtEnumeratorIndex<LastFixture>(MissingLastRow, &FixtureRow::code));
    STATIC_REQUIRE(firstMisplacedRow<LastFixture>(MissingLastRow, &FixtureRow::code) == 2);
}

TEST_CASE("EnumRows.refusesAnExtraRow")
{
    STATIC_REQUIRE_FALSE(rowsAtEnumeratorIndex<LastFixture>(ExtraRow, &FixtureRow::code));
    STATIC_REQUIRE(firstMisplacedRow<LastFixture>(ExtraRow, &FixtureRow::code) == 3);
    STATIC_REQUIRE_FALSE(rowsAtEnumeratorIndex<LastFixture>(ExtraRowPastLast, &FixtureRow::code));
    STATIC_REQUIRE(firstMisplacedRow<LastFixture>(ExtraRowPastLast, &FixtureRow::code) == 3);
}

TEST_CASE("EnumRows.sizedByEnum")
{
    STATIC_REQUIRE(sizedByEnum<LastFixture>(3));
    STATIC_REQUIRE_FALSE(sizedByEnum<LastFixture>(2));
    STATIC_REQUIRE_FALSE(sizedByEnum<LastFixture>(4));
    STATIC_REQUIRE(sizedByEnum<Fixture::A>(1));
}
