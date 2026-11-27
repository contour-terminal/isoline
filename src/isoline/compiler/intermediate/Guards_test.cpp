// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <tuple>
#include <vector>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/intermediate/MemoryStretches.hpp>
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

TEST_CASE("Guards.minimumBytesSaturates")
{
    // At run time, as memoryMinimumBytesOf() calls it per module: an exact product, and a saturated one for a
    // minimum past 2^64 bytes or a page size past 2^63.
    auto const pages = std::vector<uint64_t> { 1, uint64_t { 1 } << 48U, 3 };
    auto const sizes = std::vector<uint8_t> { 16, 16, 64 };
    CHECK(isoline::compiler::intermediate::minimumBytesOf(pages[0], sizes[0]) == 65'536);
    CHECK(isoline::compiler::intermediate::minimumBytesOf(pages[1], sizes[1]) == ~uint64_t { 0 });
    CHECK(isoline::compiler::intermediate::minimumBytesOf(pages[2], sizes[2]) == ~uint64_t { 0 });
}

TEST_CASE("Guards.stretchesOfAnEmptyFunction")
{
    // A function of no blocks has no stretches to number.
    std::pmr::monotonic_buffer_resource memory;
    auto const function = isoline::compiler::intermediate::Function { memory };
    auto const graph = isoline::compiler::intermediate::controlFlowGraphOf(function, memory);
    std::ignore = isoline::compiler::intermediate::MemoryStretches { function, graph, memory };
    CHECK(graph.blockCount() == 0);
}

TEST_CASE("Guards.rootGuardOfANonGuardIsItself")
{
    // At run time, as the verifier asks it of every pointer's producer: a guard names its root, anything else
    // itself.
    using isoline::compiler::intermediate::Operation;
    auto const operations = std::vector { Operation::I32Add, Operation::GuardAddress };
    CHECK(isoline::compiler::intermediate::rootGuardOf(operations[0]) == Operation::I32Add);
    CHECK(isoline::compiler::intermediate::rootGuardOf(operations[1]) != Operation::I32Add);
}
