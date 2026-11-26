// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>

using isoline::runtime::LimitTable;
using isoline::runtime::PollSiteTable;
using isoline::runtime::StepCharge;
using isoline::runtime::StoreLimits;

namespace
{

/// The embedding guide, whose generated blocks the tables must match.
constexpr auto EmbeddingGuide = std::string_view { ISOLINE_SOURCE_DIR "/docs/embedding.md" };

/// The lines of @p text.
/// @return Each line, without its line feed, or its carriage return where a checkout gave it one.
std::vector<std::string> linesOf(std::string const& text)
{
    auto lines = std::vector<std::string> {};
    auto stream = std::istringstream { text };
    auto line = std::string {};
    while (std::getline(stream, line))
    {
        if (line.ends_with('\r'))
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

/// The lines between `<!-- name:begin -->` and `<!-- name:end -->` of @p lines.
/// @return Them; nothing unless both markers stand on lines of their own, once each, in that order.
std::optional<std::vector<std::string>> blockOf(std::vector<std::string> const& lines, std::string_view name)
{
    auto const begin = std::format("<!-- {}:begin -->", name);
    auto const end = std::format("<!-- {}:end -->", name);
    auto const first = std::ranges::find(lines, begin);
    auto const last = std::ranges::find(lines, end);
    if (first == lines.end() || last == lines.end() || last < first || std::ranges::count(lines, begin) != 1
        || std::ranges::count(lines, end) != 1)
        return std::nullopt;
    return std::vector<std::string>(std::next(first), last);
}

/// A limit's default as the guide reads it: the number and unit, or "unlimited" for the 2^64 - 1
/// that means no limit (StoreLimits::maxSteps).
std::string defaultOf(uint64_t value, std::string_view unit)
{
    if (value == std::numeric_limits<uint64_t>::max())
        return "unlimited";
    return std::format("{} {}", value, unit);
}

/// The limits block: one line per LimitTable row, with its default from StoreLimits {}.
std::vector<std::string> limitLines()
{
    auto lines = std::vector<std::string> {};
    auto const defaults = StoreLimits {};
    for (auto const& row: LimitTable)
        lines.push_back(
            std::format("- `{}` ({}): {}", row.field, row.name, defaultOf(row.value(defaults), row.unit)));
    return lines;
}

/// The poll-sites block: one line per PollSiteTable row, with what a poll there charges.
std::vector<std::string> pollSiteLines()
{
    auto lines = std::vector<std::string> {};
    for (auto const& row: PollSiteTable)
        lines.push_back(std::format("- **{}** ({}): {}.",
                                    row.name,
                                    row.charge == StepCharge::OneStep ? "one step" : "no step",
                                    row.rule));
    return lines;
}

/// The lines as one text, for a failure's message: what to paste into the guide.
std::string joined(std::vector<std::string> const& lines)
{
    auto text = std::string {};
    for (auto const& line: lines)
        text += line + '\n';
    return text;
}

} // namespace

TEST_CASE("PublishedLimits.docsMatchTables")
{
    auto file = std::ifstream { std::string { EmbeddingGuide } };
    REQUIRE(file.is_open());
    auto const lines = linesOf(std::string { std::istreambuf_iterator<char> { file }, {} });
    struct Block
    {
        std::string_view name;
        std::vector<std::string> expected;
    };
    for (auto const& [name, expected]:
         { Block { "limits", limitLines() }, Block { "poll-sites", pollSiteLines() } })
    {
        INFO("block " << name << " of " << EmbeddingGuide << " should read:\n" << joined(expected));
        auto const block = blockOf(lines, name);
        REQUIRE(block.has_value());
        CHECK(*block == expected);
    }
}
