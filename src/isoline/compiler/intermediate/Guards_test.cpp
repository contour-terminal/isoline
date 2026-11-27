// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/intermediate/testing/GuardEdges.hpp>

using isoline::compiler::intermediate::guardFunctionAddress;
using isoline::compiler::intermediate::guardMemoryAccess;
using isoline::compiler::intermediate::guardTableIndex;
using isoline::compiler::intermediate::staticEndOverflows;
using isoline::compiler::intermediate::testing::DispatchGuardEdges;
using isoline::compiler::intermediate::testing::EdgeOutcome;
using isoline::compiler::intermediate::testing::MemoryGuardEdges;
using isoline::compiler::intermediate::testing::TableGuardEdges;

namespace
{

constexpr auto All = std::numeric_limits<uint64_t>::max();

/// What a guard should leave for @p row: its offset, element or address when it fits; none when it traps.
template <typename Edge>
std::optional<uint64_t> expected(Edge const& row)
{
    if (row.outcome == EdgeOutcome::Traps)
        return std::nullopt;
    return row.fits;
}

} // namespace

TEST_CASE("Guards.memoryAccessAtTheEdges")
{
    // What checkMemory's compare and select compute on every edge row: the access's offset where it fits,
    // nothing (a trap, and 0 under misspeculation) elsewhere.
    for (auto const& row: MemoryGuardEdges)
    {
        INFO("index " << row.index << ", offset " << row.offset << ", size " << row.size << ", bound "
                      << row.bound);
        CHECK(guardMemoryAccess(row.indexType, row.index, row.offset, row.size, row.bound) == expected(row));
    }
}

TEST_CASE("Guards.tableIndexAtTheEdges")
{
    // checkTable: the element where the index is below the size, nothing elsewhere; a table32's index is its
    // low 32 bits.
    for (auto const& row: TableGuardEdges)
    {
        INFO("index " << row.index << ", size " << row.size);
        CHECK(guardTableIndex(row.indexType, row.index, row.size) == expected(row));
    }
}

TEST_CASE("Guards.functionAddressAtTheEdges")
{
    // checkDispatch: a Ref's word holds its address in bits 32 to 63.
    for (auto const& row: DispatchGuardEdges)
    {
        INFO("reference " << row.reference << ", rows " << row.rows);
        CHECK(guardFunctionAddress(row.reference, row.rows) == expected(row));
    }
}

TEST_CASE("Guards.staticEndOverflowsPastTwoToThe64")
{
    CHECK_FALSE(staticEndOverflows(0, 8));
    CHECK_FALSE(staticEndOverflows(All - 8, 8));
    CHECK(staticEndOverflows(All - 7, 8));
    CHECK(staticEndOverflows(All - 3, 8));
    CHECK(staticEndOverflows(All, 1));
    CHECK(staticEndOverflows(All - 3, 4));
    CHECK_FALSE(staticEndOverflows(All - 4, 4));
}

TEST_CASE("Guards.everyGuardHasItsRow")
{
    // The rows by operation, at run time as the selector reads them: each guard's, and none for the rest.
    using isoline::compiler::intermediate::GuardBound;
    using isoline::compiler::intermediate::guardRowOf;
    using isoline::compiler::intermediate::Operation;
    REQUIRE(guardRowOf(Operation::CheckMemory).has_value());
    CHECK(guardRowOf(Operation::CheckMemory)->bound == GuardBound::MemoryBytes);
    REQUIRE(guardRowOf(Operation::CheckDispatch).has_value());
    CHECK(guardRowOf(Operation::CheckDispatch)->bound == GuardBound::DispatchRows);
    CHECK_FALSE(guardRowOf(Operation::Load).has_value());
    CHECK_FALSE(guardRowOf(Operation::I32Add).has_value());
}
