// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <ranges>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/text/FunctionTypes.hpp>

using isoline::binary::concreteRef;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Nullable;
using isoline::text::FunctionTypes;

TEST_CASE("FunctionTypes.implicitTypesReuseTheFirst")
{
    auto types = std::vector<FuncType> {};
    auto index = FunctionTypes { types };
    CHECK(index.define(FuncType { .params = { I32 }, .results = {} }) == 0);
    CHECK(index.define(FuncType { .params = { I32 }, .results = {} }) == 1); // Explicit: its own.
    CHECK(index.use(FuncType { .params = { I32 }, .results = {} }) == 0);    // The first identical one.
    CHECK(index.use(FuncType { .params = {}, .results = { I64 } }) == 2);    // Appended.
    CHECK(index.use(FuncType { .params = {}, .results = { I64 } }) == 2);
    CHECK(types.size() == 3);
}

namespace
{

/// The comparisons @p uses lookups may cost in an ordered index of @p types types: two per level of a
/// balanced tree, and a few for the find before an insert; twice that, as MSVC's debug STL checks the
/// ordering of each comparison with a second call (4.83 million where the bound without it is 4.41 for
/// craftedTypesCostWhatOthersDo). A scan per use costs a thousand times more.
std::size_t logarithmicBound(std::size_t uses, std::size_t types)
{
    return uses * 4 * (static_cast<std::size_t>(std::bit_width(types)) + 2);
}

/// Defines each of @p types, then uses each again, and reports what the index compared.
std::size_t comparisonsOf(std::vector<FuncType> const& types)
{
    auto module = std::vector<FuncType> {};
    auto index = FunctionTypes { module };
    for (auto const& type: types)
        index.define(type);
    for (auto const [ordinal, type]: core::views::enumerate(types))
        CHECK(index.use(type) == ordinal);
    return index.comparisons();
}

/// The types (func (param (ref A) (ref B))) whose former hash (a boost-style mix of each value type's
/// kind and heap index) was one value for all of them: the reviewer's collide.py, ported. Of the
/// 65'536 candidates A, those whose B fits a type index fall into a few hash values; the largest group
/// is returned, 61'185 types, each pair distinct.
std::vector<FuncType> formerlyCollidingTypes()
{
    constexpr auto Mask32 = uint64_t { 0xFFFF'FFFF };
    constexpr auto Golden = uint64_t { 0x9E37'79B9'7F4A'7C15 };
    constexpr auto ReferenceKey = uint64_t { (5U << 16U) | 7U }; // Ref, not nullable, concrete
    constexpr auto LowWord = uint64_t { 0x8000'0000 };
    auto const mix = [](uint64_t hash, uint64_t value) {
        return hash ^ (value + Golden + (hash << 6U) + (hash >> 2U));
    };
    auto groups = std::map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> {};
    for (auto const first: std::views::iota(uint64_t { 0 }, uint64_t { 65'536 }))
    {
        auto const prefix = mix(mix(mix(2, ReferenceKey), first), ReferenceKey);
        auto const added = Golden + (prefix << 6U) + (prefix >> 2U);
        auto const borrow = (LowWord ^ (prefix & Mask32)) < (added & Mask32) ? 1U : 0U;
        auto const key = (((added >> 32U) + borrow) & Mask32) ^ (prefix >> 32U);
        auto const second = (((key << 32U) | LowWord) ^ prefix) - added;
        if (second <= Mask32)
            groups[key].emplace_back(static_cast<uint32_t>(first), static_cast<uint32_t>(second));
    }
    auto const largest =
        std::ranges::max_element(groups, {}, [](auto const& group) { return group.second.size(); });
    auto types = std::vector<FuncType> {};
    for (auto const& [first, second]: largest->second)
        types.push_back(
            FuncType { .params = { concreteRef(first, Nullable::No), concreteRef(second, Nullable::No) },
                       .results = {} });
    return types;
}

} // namespace

TEST_CASE("FunctionTypes.lookupsAreLogarithmicPerUse")
{
    // 100'000 distinct types, each used twice: a scan per use would compare billions of times; an
    // ordered lookup compares a use with one type per level of the tree.
    constexpr auto TypeCount = 100'000U;
    auto const typeOf = [](uint32_t ordinal) {
        constexpr auto ParamCount = 17U;
        auto type = FuncType {};
        for (auto const bit: std::views::iota(0U, ParamCount))
            type.params.push_back(((ordinal >> bit) & 1U) != 0 ? I64 : I32);
        return type;
    };
    auto types = std::vector<FuncType> {};
    auto index = FunctionTypes { types };
    for (auto const ordinal: std::views::iota(0U, TypeCount))
        CHECK(index.use(typeOf(ordinal)) == ordinal);
    for (auto const ordinal: std::views::iota(0U, TypeCount))
        CHECK(index.use(typeOf(ordinal)) == ordinal);
    CHECK(types.size() == TypeCount);
    CHECK(index.comparisons() <= logarithmicBound(std::size_t { 2 } * TypeCount, TypeCount));
}

TEST_CASE("FunctionTypes.craftedTypesCostWhatOthersDo", "[linear-time]")
{
    // Types chosen to share one hash cost what as many unrelated types do: within a quarter of a control
    // of the same count and shapes, and logarithmic. A hashed index made them a scan per use, 1.8 billion
    // comparisons for these, 75'000 such types in 3.7 MB of text taking 23 s to read.
    auto const crafted = formerlyCollidingTypes();
    REQUIRE(crafted.size() > 60'000);
    auto control = std::vector<FuncType> {};
    for (auto const [ordinal, type]: core::views::enumerate(crafted))
    {
        auto copy = type;
        copy.params[1].heap.typeIndex += static_cast<uint32_t>(ordinal) * 7'919U;
        control.push_back(std::move(copy));
    }
    auto const craftedComparisons = comparisonsOf(crafted);
    auto const controlComparisons = comparisonsOf(control);
    CAPTURE(crafted.size(), craftedComparisons, controlComparisons);
    CHECK(craftedComparisons * 4 <= controlComparisons * 5);
    CHECK(craftedComparisons <= logarithmicBound(std::size_t { 2 } * crafted.size(), crafted.size()));
}
