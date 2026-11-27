// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>

using isoline::compiler::intermediate::guardFunctionAddress;
using isoline::compiler::intermediate::guardMemoryAccess;
using isoline::compiler::intermediate::guardTableIndex;
using isoline::compiler::intermediate::staticEndOverflows;
using isoline::compiler::intermediate::Type;

namespace
{

constexpr auto All = std::numeric_limits<uint64_t>::max();
constexpr auto Half = uint64_t { 1 } << 63U;
constexpr auto Page = uint64_t { 65'536 };
constexpr auto None = std::optional<uint64_t> {};

} // namespace

TEST_CASE("Guards.memoryAccessAtTheEdges")
{
    // What checkMemory's compare and select compute: the access's offset where it fits, nothing (a trap,
    // and 0 under misspeculation) elsewhere. The rows P4's fast interpreter once got wrong are here: an end
    // that wraps past 2^64, an offset at or past 2^63, a memory smaller than the access, and an I32 index
    // whose register carries high bits.
    struct Row
    {
        Type indexType;                 ///< The index's type: the memory's address type.
        uint64_t index;                 ///< The index register's bits.
        uint64_t offset;                ///< The static offset.
        uint64_t size;                  ///< The access's width.
        uint64_t bound;                 ///< The memory's bytes.
        std::optional<uint64_t> result; ///< What the guard leaves.
    };
    auto const rows = std::to_array<Row>({
        { Type::I32, 0, 0, 4, Page, 0 },
        { Type::I32, Page - 4, 0, 4, Page, Page - 4 },
        { Type::I32, Page - 3, 0, 4, Page, None },
        { Type::I32, 0, Page - 4, 4, Page, Page - 4 },
        { Type::I32, 1, Page - 4, 4, Page, None },
        { Type::I32, Page, 0, 1, Page, None },
        // An I32 index is its low 32 bits: high bits in the register neither help nor harm.
        { Type::I32, 0xFFFF'FFFF'0000'0004, 0, 4, Page, 4 },
        { Type::I32, 0xFFFF'FFFF'FFFF'FFFE, 0, 4, Page, None },
        { Type::I32, 0xFFFF'FFFF, 0xFFFF'FFFF, 8, Page, None },
        // A 64-bit memory: ends that wrap past 2^64, offsets at and past 2^63.
        { Type::I64, All, 0, 1, Page, None },
        { Type::I64, All - 3, 0, 4, Page, None },
        { Type::I64, All - 3, 0, 8, Page, None },
        { Type::I64, 4, All - 3, 4, Page, None },
        { Type::I64, Half, 0, 1, Page, None },
        { Type::I64, Half, Half, 1, Page, None },
        { Type::I64, Half + Page, 0, 4, Page, None },
        { Type::I64, 0, Half, 1, Page, None },
        { Type::I64, Page - 8, 0, 8, Page, Page - 8 },
        { Type::I64, Page - 8, 4, 8, Page, None },
        // A memory smaller than the access traps for every index, the smallest included.
        { Type::I32, 0, 0, 8, 4, None },
        { Type::I64, 0, 0, 8, 4, None },
        { Type::I64, 0, 0, 1, 0, None },
        { Type::I64, 0, 0, 8, 8, 0 },
        { Type::I64, 1, 0, 8, 8, None },
    });
    for (auto const& row: rows)
    {
        INFO("index " << row.index << ", offset " << row.offset << ", size " << row.size << ", bound "
                      << row.bound);
        CHECK(guardMemoryAccess(row.indexType, row.index, row.offset, row.size, row.bound) == row.result);
    }
}

TEST_CASE("Guards.tableIndexAtTheEdges")
{
    // checkTable: the element where the index is below the size, nothing elsewhere; a table32's index is its
    // low 32 bits.
    CHECK(guardTableIndex(Type::I32, 0, 1) == 0);
    CHECK(guardTableIndex(Type::I32, 9, 10) == 9);
    CHECK(guardTableIndex(Type::I32, 10, 10) == None);
    CHECK(guardTableIndex(Type::I32, 0, 0) == None);
    CHECK(guardTableIndex(Type::I32, 0xFFFF'FFFF'0000'0003, 10) == 3);
    CHECK(guardTableIndex(Type::I32, 0x0000'0001'FFFF'FFFF, 10) == None);
    CHECK(guardTableIndex(Type::I64, 0x0000'0001'0000'0003, 10) == None);
    CHECK(guardTableIndex(Type::I64, All, All) == None);
    CHECK(guardTableIndex(Type::I64, All - 1, All) == All - 1);
}

TEST_CASE("Guards.functionAddressAtTheEdges")
{
    // checkDispatch: a Ref's word holds its address in bits 32 to 63; the low bits (its kind and heap) never
    // move the row.
    auto const wordOf = [](uint64_t address, uint64_t low) {
        return (address << isoline::compiler::intermediate::RefWordAddressShift) | low;
    };
    CHECK(guardFunctionAddress(wordOf(0, 1), 1) == 0);
    CHECK(guardFunctionAddress(wordOf(4'095, 0xFFFF), 4'096) == 4'095);
    CHECK(guardFunctionAddress(wordOf(4'096, 0), 4'096) == None);
    CHECK(guardFunctionAddress(wordOf(0xFFFF'FFFF, 1), 4'096) == None);
    CHECK(guardFunctionAddress(wordOf(7, 1), 0) == None);
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
