// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/testing/ProvidedMemory.hpp>

using isoline::binary::AddressType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::ILinearMemory;
using isoline::runtime::MemoryRequest;
using isoline::runtime::Refusal;
using isoline::runtime::testing::GrowDecision;
using isoline::runtime::testing::ProvidedMemoryAllocator;

namespace
{

/// The default page size, 64 KiB.
constexpr auto PageSize = isoline::binary::pageSize(MemoryType {});

/// A request for a memory of @p pages pages that may grow to 4 GiB, with no guard.
MemoryRequest requestOf(uint64_t pages)
{
    return MemoryRequest {
        .type =
            MemoryType { .limits =
                             Limits { .min = pages, .max = std::nullopt, .addressType = AddressType::I32 } },
        .initialBytes = pages * PageSize,
        .maximumBytes = uint64_t { 1 } << 32,
        .requiredGuardBytes = 0,
        .requiredTailBytes = 0,
    };
}

/// Whether every byte of @p bytes is zero.
bool allZero(std::span<std::byte const> bytes)
{
    return std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte {}; });
}

} // namespace

TEST_CASE("HeapMemoryAllocator.growsZeroed")
{
    auto allocator = HeapMemoryAllocator {};
    auto memory = allocator.allocate(requestOf(1));
    REQUIRE(memory.has_value());
    auto& linear = **memory;
    REQUIRE(linear.bytes().size() == PageSize);
    CHECK(allZero(linear.bytes()));
    CHECK(linear.guardBytes() == 0);

    std::ranges::fill(linear.bytes(), std::byte { 0xFF });
    REQUIRE(linear.grow(2 * PageSize).has_value());
    REQUIRE(linear.bytes().size() == 2 * PageSize);
    CHECK(std::ranges::all_of(linear.bytes().first(PageSize),
                              [](std::byte value) { return value == std::byte { 0xFF }; }));
    CHECK(allZero(linear.bytes().subspan(PageSize)));
}

TEST_CASE("HeapMemoryAllocator.keepsATailPastEveryEnd")
{
    // Every memory keeps MaxAccessWidth readable bytes past its end, as its storage reports them, at every
    // size: a masked access of MaxAccessWidth bytes at the base of a memory of no bytes stays in its storage.
    auto allocator = HeapMemoryAllocator {};
    auto request = requestOf(0);
    request.type.pageSizeLog2 = 0;
    auto memory = allocator.allocate(request);
    REQUIRE(memory.has_value());
    auto& linear = **memory;
    CHECK(linear.bytes().empty());
    CHECK(linear.tailBytes() >= isoline::runtime::MaxAccessWidth);
    CHECK(isoline::runtime::keepsItsTail(linear));
    for (auto const size: { uint64_t { 1 }, uint64_t { 9 }, uint64_t { 64 } })
    {
        INFO("size " << size);
        REQUIRE(linear.grow(size).has_value());
        CHECK(linear.bytes().size() == size);
        CHECK(allZero(linear.bytes()));
        CHECK(linear.tailBytes() >= isoline::runtime::MaxAccessWidth);
        CHECK(isoline::runtime::keepsItsTail(linear));
    }
}

TEST_CASE("LinearMemory.requiredTailFollowsTheSize")
{
    // What an access of MaxAccessWidth bytes at the base reads past a memory of each size; nothing of a
    // memory of MaxAccessWidth bytes or more.
    using isoline::runtime::MaxAccessWidth;
    using isoline::runtime::requiredTailBytesOf;
    CHECK(requiredTailBytesOf(0) == MaxAccessWidth);
    CHECK(requiredTailBytesOf(1) == MaxAccessWidth - 1);
    CHECK(requiredTailBytesOf(MaxAccessWidth - 1) == 1);
    CHECK(requiredTailBytesOf(MaxAccessWidth) == 0);
    CHECK(requiredTailBytesOf(PageSize) == 0);
}

TEST_CASE("HeapMemoryAllocator.refusesWhatNoVectorHolds", "[memory]")
{
    auto allocator = HeapMemoryAllocator {};
    auto request = requestOf(0);
    request.initialBytes = std::numeric_limits<uint64_t>::max();
    CHECK(allocator.allocate(request).error() == Refusal::HostMemory);

    auto memory = allocator.allocate(requestOf(1));
    REQUIRE(memory.has_value());
    CHECK((*memory)->grow(std::numeric_limits<uint64_t>::max()) == std::unexpected { Refusal::HostMemory });
    CHECK(std::as_const(**memory).bytes().size() == PageSize);
}

TEST_CASE("ProvidedMemoryAllocator.providesOneStableMemory")
{
    auto decisions = std::vector<uint64_t> {};
    auto allocator = ProvidedMemoryAllocator { 2 * PageSize, 4'096, [&](uint64_t bytes) {
                                                  decisions.push_back(bytes);
                                                  return GrowDecision::Grant;
                                              } };
    // Larger than the buffer: refused, and the one memory is still to be had.
    CHECK(allocator.allocate(requestOf(3)).error() == Refusal::Embedder);

    auto memory = allocator.allocate(requestOf(1));
    REQUIRE(memory.has_value());
    auto& linear = **memory;
    CHECK(linear.guardBytes() == 4'096);
    CHECK(allZero(linear.bytes()));
    auto const* const data = linear.bytes().data();
    linear.bytes()[0] = std::byte { 0x2A };

    REQUIRE(linear.grow(2 * PageSize).has_value());
    CHECK(linear.bytes().data() == data);
    CHECK(linear.bytes()[0] == std::byte { 0x2A });
    CHECK(allZero(linear.bytes().subspan(PageSize)));
    // Past the buffer, refused even when granted.
    CHECK(linear.grow(3 * PageSize) == std::unexpected { Refusal::Embedder });
    CHECK(decisions == std::vector { 2 * PageSize, 3 * PageSize });

    // It hands out one memory only.
    CHECK(allocator.allocate(requestOf(1)).error() == Refusal::Embedder);
}
