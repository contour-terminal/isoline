// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <vector>

#include <isoline/binary/TypeCanon.hpp>
#include <isoline/binary/Types.hpp>

using isoline::binary::canonicalTypeIds;
using isoline::binary::concreteRef;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::jointCanonicalTypeIds;
using isoline::binary::Nullable;

TEST_CASE("TypeCanon.structuralEquality")
{
    // (func), (func), (func (param (ref 0))), (func (param (ref 1))): 0 and 1 are equal, so are the
    // references to them, and so the types that take them.
    auto const types = std::vector<FuncType> {
        FuncType {},
        FuncType {},
        FuncType { .params = { concreteRef(0, Nullable::No) }, .results = {} },
        FuncType { .params = { concreteRef(1, Nullable::No) }, .results = {} },
    };
    CHECK(canonicalTypeIds(types) == std::vector<uint32_t> { 0, 0, 2, 2 });
}

TEST_CASE("TypeCanon.distinguishesWhatDiffers")
{
    auto const types = std::vector<FuncType> {
        FuncType { .params = { I32 }, .results = {} },
        FuncType { .params = {}, .results = { I32 } },                           // a result, not a param
        FuncType { .params = { concreteRef(0, Nullable::No) }, .results = {} },  // non-null
        FuncType { .params = { concreteRef(0, Nullable::Yes) }, .results = {} }, // nullable
        FuncType { .params = { concreteRef(1, Nullable::No) }, .results = {} },  // another type
        FuncType { .params = { FuncRef }, .results = {} },                       // an abstract heap
        FuncType { .params = { I32 }, .results = {} },                           // equal to 0
    };
    CHECK(canonicalTypeIds(types) == std::vector<uint32_t> { 0, 1, 2, 3, 4, 5, 0 });
    CHECK(canonicalTypeIds({}).empty());
}

TEST_CASE("TypeCanon.canonicalizeDeepTypeChain")
{
    // Type i takes (ref i-1): 100 000 types, each distinct, each naming the one before. Canonical
    // ids are found in one bottom-up pass: no recursion follows the chain.
    constexpr auto Count = uint32_t { 100'000 };
    auto types = std::vector<FuncType> {};
    types.reserve(Count);
    types.push_back(FuncType {});
    for (auto const index: std::views::iota(uint32_t { 1 }, Count))
        types.push_back(FuncType { .params = { concreteRef(index - 1, Nullable::No) }, .results = {} });
    auto const ids = canonicalTypeIds(types);
    REQUIRE(ids.size() == Count);
    auto expected = std::vector<uint32_t>(Count);
    for (auto const index: std::views::iota(uint32_t { 0 }, Count))
        expected[index] = index;
    CHECK(ids == expected);

    // The same chain twice over: the second copy's types are equal to the first's, link by link.
    auto doubled = types;
    doubled.push_back(FuncType {});
    for (auto const index: std::views::iota(uint32_t { 1 }, Count))
        doubled.push_back(
            FuncType { .params = { concreteRef(Count + index - 1, Nullable::No) }, .results = {} });
    auto const doubledIds = canonicalTypeIds(doubled);
    REQUIRE(doubledIds.size() == std::size_t { 2 } * Count);
    CHECK(std::ranges::equal(doubledIds | std::views::drop(Count), expected));
}

TEST_CASE("TypeCanon.jointSections")
{
    // Two sections in one space: the second's indices count from its own start, and its types share
    // the first's ids when equal.
    auto const first = std::vector<FuncType> {
        FuncType { .params = { I32 }, .results = {} },
        FuncType { .params = { concreteRef(0, Nullable::Yes) }, .results = {} },
    };
    auto const second = std::vector<FuncType> {
        FuncType {},
        FuncType { .params = { I32 }, .results = {} },
        FuncType { .params = { concreteRef(1, Nullable::Yes) }, .results = {} },
        FuncType { .params = { concreteRef(0, Nullable::Yes) }, .results = {} },
    };
    auto const sections = std::vector<std::span<FuncType const>> { first, second };
    CHECK(jointCanonicalTypeIds(sections) == std::vector<uint32_t> { 0, 1, 2, 0, 1, 5 });
}
