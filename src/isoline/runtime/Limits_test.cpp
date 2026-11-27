// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <ranges>
#include <string_view>

#include <isoline/runtime/Limits.hpp>

using isoline::runtime::describe;
using isoline::runtime::Limit;
using isoline::runtime::LimitExceeded;
using isoline::runtime::LimitTable;
using isoline::runtime::limitValue;
using isoline::runtime::Refusal;
using isoline::runtime::RefusalTable;
using isoline::runtime::StoreLimits;

TEST_CASE("Limits.tablesAreComplete")
{
    // Every Limit has a row at its index, every Refusal a text.
    for (auto const index: std::views::iota(std::size_t { 0 }, LimitTable.size()))
    {
        CHECK(static_cast<std::size_t>(LimitTable[index].limit) == index);
        CHECK_FALSE(LimitTable[index].name.empty());
        CHECK_FALSE(LimitTable[index].unit.empty());
    }
    for (auto const index: std::views::iota(std::size_t { 0 }, RefusalTable.size()))
    {
        CHECK(static_cast<std::size_t>(RefusalTable[index].refusal) == index);
        CHECK_FALSE(RefusalTable[index].text.empty());
    }
    CHECK(describe(LimitExceeded { Limit::MemoryBytes }) == "memory bytes limit exceeded");
    CHECK(describe(LimitExceeded { Limit::Types }) == "types limit exceeded");
    CHECK(describe(LimitExceeded { Limit::Steps }) == "steps limit exceeded");
    CHECK(LimitTable.back().limit == Limit::Steps);
    CHECK(describe(Refusal::Limit) == "above the store's limit");
    CHECK(describe(Refusal::Embedder) == "refused by the embedder");
}

TEST_CASE("Limits.limitValueReadsEveryField")
{
    // Distinct values in every field, so that a row reading another row's field shows.
    auto const limits = StoreLimits { .maxCallDepth = 1,
                                      .maxStackValues = 2,
                                      .maxNestedInvocations = 3,
                                      .maxMemoryBytes = 4,
                                      .maxTableElements = 5,
                                      .maxMemories = 6,
                                      .maxTables = 7,
                                      .maxInstances = 8,
                                      .maxExceptions = 9,
                                      .maxExceptionValues = 10,
                                      .maxTypes = 11,
                                      .maxTags = 12,
                                      .maxSteps = 13 };
    for (auto const& row: LimitTable)
        CHECK(limitValue(limits, row.limit) == static_cast<uint64_t>(row.limit) + 1);
}

TEST_CASE("Limits.defaults")
{
    auto const limits = StoreLimits {};
    CHECK(limits.maxCallDepth == 10'000);
    CHECK(limits.maxStackValues == uint64_t { 1 } << 22);
    CHECK(limits.maxNestedInvocations == 100);
    CHECK(limits.maxMemoryBytes == uint64_t { 1 } << 32);
    CHECK(limits.maxTableElements == 10'000'000);
    CHECK(limits.maxMemories == 10'000);
    CHECK(limits.maxTables == 100'000);
    CHECK(limits.maxInstances == 10'000);
    CHECK(limits.maxExceptions == 65'536);
    CHECK(limits.maxExceptionValues == uint64_t { 1 } << 20);
    CHECK(limits.maxTypes == 1'000'000);
    CHECK(limits.maxTags == 1'000'000);
    // No step limit: the budget is the whole range, and the docs say "unlimited".
    CHECK(limits.maxSteps == std::numeric_limits<uint64_t>::max());
}

TEST_CASE("Limits.eachRowNamesTheFieldItReads")
{
    // A store of a different value per field: each row's value() must read the field its field names.
    auto const distinct = StoreLimits { .maxCallDepth = 1,
                                        .maxStackValues = 2,
                                        .maxNestedInvocations = 3,
                                        .maxMemoryBytes = 4,
                                        .maxTableElements = 5,
                                        .maxMemories = 6,
                                        .maxTables = 7,
                                        .maxInstances = 8,
                                        .maxExceptions = 9,
                                        .maxExceptionValues = 10,
                                        .maxTypes = 11,
                                        .maxTags = 12,
                                        .maxSteps = 13 };
    auto const fields = std::map<std::string_view, uint64_t> {
        { "maxCallDepth", 1 },        { "maxStackValues", 2 },   { "maxNestedInvocations", 3 },
        { "maxMemoryBytes", 4 },      { "maxTableElements", 5 }, { "maxMemories", 6 },
        { "maxTables", 7 },           { "maxInstances", 8 },     { "maxExceptions", 9 },
        { "maxExceptionValues", 10 }, { "maxTypes", 11 },        { "maxTags", 12 },
        { "maxSteps", 13 },
    };
    CHECK(fields.size() == LimitTable.size());
    for (auto const& row: LimitTable)
    {
        INFO(row.field);
        REQUIRE(fields.contains(row.field));
        CHECK(row.value(distinct) == fields.at(row.field));
    }
}
