// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/component/Limits.hpp>

using isoline::component::ComponentLimit;
using isoline::component::ComponentLimitCount;
using isoline::component::ComponentLimits;
using isoline::component::limitMessage;
using isoline::component::LimitTable;
using isoline::component::limitValue;

TEST_CASE("Limits.tableCoversEveryLimit")
{
    CHECK(LimitTable.size() == ComponentLimitCount);
    auto index = std::size_t { 0 };
    auto names = std::set<std::string_view> {};
    for (auto const& row: LimitTable)
    {
        CHECK(static_cast<std::size_t>(row.limit) == index);
        CHECK_FALSE(row.name.empty());
        CHECK_FALSE(row.rule.empty());
        CHECK_FALSE(row.message.empty());
        names.insert(row.name);
        ++index;
    }
    CHECK(names.size() == ComponentLimitCount);
}

TEST_CASE("Limits.valuesReadTheirField")
{
    // Each row reads its own field: a set of limits that differ in every field tells them apart.
    auto limits = ComponentLimits {};
    limits.maxComponentBytes = 1;
    limits.maxNesting = 2;
    limits.maxTypeDepth = 3;
    limits.maxTypeSize = 4;
    limits.maxTypes = 5;
    limits.maxDefinitions = 6;
    limits.maxDecodedItems = 7;
    limits.maxNameBytes = 8;
    limits.maxCoreInstances = 9;
    limits.maxComponentInstances = 10;
    limits.maxInstantiationSteps = 11;
    limits.maxResourceTypes = 12;
    limits.maxHandles = 13;
    limits.maxLiveValueBytes = 14;
    limits.maxSubtypingSteps = 15;
    limits.maxSubstitutedTypes = 16;
    limits.maxModules = 17;
    limits.maxComponents = 18;
    limits.maxInstances = 19;
    limits.maxFunctions = 20;
    limits.maxExports = 21;
    limits.maxCoreTables = 22;
    limits.maxCoreMemories = 23;
    limits.maxCoreGlobals = 24;
    limits.maxCoreTags = 25;
    limits.maxCoreMatchingSteps = 26;
    limits.maxNestedCalls = 27;
    limits.maxHostOwns = 28;
    for (auto const& row: LimitTable)
        CHECK(limitValue(limits, row.limit) == static_cast<uint64_t>(row.limit) + 1);

    CHECK(limitValue(ComponentLimits {}, ComponentLimit::ComponentBytes) == uint64_t { 256 } << 20);
    CHECK(limitValue(ComponentLimits {}, ComponentLimit::Nesting) == 100);
}

TEST_CASE("Limits.messages")
{
    // Every row's message is a format string that formats its limit's value (std::vformat throws on
    // one that does not).
    for (auto const& row: LimitTable)
    {
        INFO(row.name);
        auto const value = limitValue(ComponentLimits {}, row.limit);
        auto const message = limitMessage(row.limit, value);
        CHECK_FALSE(message.empty());
        if (row.message.contains("{}"))
            CHECK(message.contains(std::to_string(value)));
        else
            CHECK(message == row.message);
    }

    // wasm-tools' wording, at its own limit.
    CHECK(limitMessage(ComponentLimit::TypeSize, ComponentLimits {}.maxTypeSize)
          == "effective type size exceeds the limit of 1000000");
    CHECK(limitMessage(ComponentLimit::NameBytes, 100'000) == "string size out of bounds");
    CHECK(limitMessage(ComponentLimit::TypeDepth, 100) == "type nesting is too deep");
    // Isoline's own name the value they are given.
    CHECK(limitMessage(ComponentLimit::Nesting, 3) == "nesting exceeds the limit of 3 levels");
}

TEST_CASE("Limits.docsMatchTable")
{
    // The embedding guide lists every cap with its default, generated from the rows: it fails, printing
    // the block as it should read, when the two part.
    auto expected = std::vector<std::string> {};
    for (auto const& row: LimitTable)
        expected.push_back(
            std::format("- `{}` ({}): {}.", row.name, row.value(ComponentLimits {}), row.rule));
    auto lines = std::vector<std::string> {};
    auto file = std::ifstream { ISOLINE_SOURCE_DIR "/docs/embedding.md" };
    REQUIRE(file.is_open());
    auto line = std::string {};
    auto inside = false;
    while (std::getline(file, line))
    {
        if (line.ends_with('\r'))
            line.pop_back();
        if (line == "<!-- component-limits:end -->")
            break;
        if (inside)
            lines.push_back(line);
        inside = inside || line == "<!-- component-limits:begin -->";
    }
    auto joined = std::string {};
    for (auto const& entry: expected)
        joined += entry + "\n";
    INFO("the component-limits block of docs/embedding.md should read:\n" << joined);
    CHECK(lines == expected);
}
