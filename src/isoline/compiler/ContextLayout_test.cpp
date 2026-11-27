// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

#include <isoline/compiler/ContextLayout.hpp>

using isoline::compiler::ContextLayoutRow;
using isoline::compiler::ExecutionState;
using isoline::compiler::ExecutionStateLayout;
using isoline::compiler::InstanceContext;
using isoline::compiler::InstanceContextLayout;
using isoline::compiler::layoutOf;
using isoline::compiler::memberCount;
using isoline::compiler::PendingAbrupt;
using isoline::compiler::PendingAbruptLayout;
using isoline::compiler::rowsCoverEveryMember;

namespace
{

/// The checked-in layout every CI host must reproduce.
constexpr auto Golden = std::string_view { ISOLINE_SOURCE_DIR "/tests/determinism/context-layout.golden" };

/// A record for the predicate's own test: three members, one gap.
struct Probe
{
    uint32_t first = 0;
    uint64_t second = 0;
    uint32_t third = 0;
};

/// Probe's fields.
enum class ProbeField : uint8_t
{
    First,
    Second,
    Third,
};

using ProbeRow = ContextLayoutRow<ProbeField>;

/// A record whose last member lies where padding would: only the member count tells it apart.
struct TailProbe
{
    uint64_t wide = 0;
    uint32_t narrow = 0;
    uint32_t hidden = 0;
};

constexpr auto FirstRow = ProbeRow {
    .field = ProbeField::First, .name = "first", .offset = offsetof(Probe, first), .size = 4, .alignment = 4
};
constexpr auto SecondRow = ProbeRow { .field = ProbeField::Second,
                                      .name = "second",
                                      .offset = offsetof(Probe, second),
                                      .size = 8,
                                      .alignment = 8 };
constexpr auto ThirdRow = ProbeRow {
    .field = ProbeField::Third, .name = "third", .offset = offsetof(Probe, third), .size = 4, .alignment = 4
};

/// Appends @p rows of @p record to @p text, one line per row: record, field, offset, size.
template <typename Field, std::size_t Count>
void render(std::string& text,
            std::string_view record,
            std::array<ContextLayoutRow<Field>, Count> const& rows)
{
    for (auto const& row: rows)
        std::format_to(std::back_inserter(text), "{} {} {} {}\n", record, row.name, row.offset, row.size);
}

} // namespace

TEST_CASE("ContextLayout.rowsCoverEveryMember")
{
    // The real tables (also static_asserted).
    CHECK(rowsCoverEveryMember<PendingAbrupt>(PendingAbruptLayout));
    CHECK(rowsCoverEveryMember<ExecutionState>(ExecutionStateLayout));
    CHECK(rowsCoverEveryMember<InstanceContext>(InstanceContextLayout));
    CHECK(memberCount<ExecutionState>() == ExecutionStateLayout.size());
    CHECK(memberCount<InstanceContext>() == InstanceContextLayout.size());
    CHECK(memberCount<PendingAbrupt>() == PendingAbruptLayout.size());
    // Each field's row is found by its enumerator.
    for (auto const& row: PendingAbruptLayout)
        CHECK(&layoutOf(row.field) == &row);
    for (auto const& row: ExecutionStateLayout)
        CHECK(&layoutOf(row.field) == &row);
    for (auto const& row: InstanceContextLayout)
        CHECK(&layoutOf(row.field) == &row);

    // The predicate fails each way a table can be wrong.
    STATIC_CHECK(memberCount<Probe>() == 3);
    CHECK(rowsCoverEveryMember<Probe>(std::array { FirstRow, SecondRow, ThirdRow }));
    // A member without a row, even one in the record's trailing padding.
    CHECK_FALSE(rowsCoverEveryMember<Probe>(std::array { FirstRow, SecondRow }));
    // A member where the record would otherwise have trailing padding.
    CHECK_FALSE(rowsCoverEveryMember<TailProbe>(std::array {
        ProbeRow { .field = ProbeField::First, .name = "wide", .offset = 0, .size = 8, .alignment = 8 },
        ProbeRow {
            .field = ProbeField::Second, .name = "narrow", .offset = 8, .size = 4, .alignment = 4 } }));
    // Rows out of order.
    CHECK_FALSE(rowsCoverEveryMember<Probe>(std::array { SecondRow, FirstRow, ThirdRow }));
    // A row too small, leaving a gap a member could hide in.
    constexpr auto ShortSecond = ProbeRow { .field = ProbeField::Second,
                                            .name = "second",
                                            .offset = SecondRow.offset + 4,
                                            .size = 4,
                                            .alignment = 8 };
    CHECK_FALSE(rowsCoverEveryMember<Probe>(std::array { FirstRow, ShortSecond, ThirdRow }));
    // A row too large, overlapping the next.
    constexpr auto WideFirst =
        ProbeRow { .field = ProbeField::First, .name = "first", .offset = 0, .size = 12, .alignment = 4 };
    CHECK_FALSE(rowsCoverEveryMember<Probe>(std::array { WideFirst, SecondRow, ThirdRow }));
    // A last row that ends short of the record.
    constexpr auto ShortThird = ProbeRow {
        .field = ProbeField::Third, .name = "third", .offset = ThirdRow.offset, .size = 0, .alignment = 1
    };
    CHECK_FALSE(rowsCoverEveryMember<Probe>(std::array { FirstRow, SecondRow, ShortThird }));
}

TEST_CASE("ContextLayout.matchesTheGolden")
{
    // Every host and standard library lays the records out alike, or generated code would differ (I9).
    auto rendered = std::string {};
    render(rendered, "PendingAbrupt", PendingAbruptLayout);
    render(rendered, "ExecutionState", ExecutionStateLayout);
    render(rendered, "InstanceContext", InstanceContextLayout);
    auto file = std::ifstream { std::string { Golden } };
    REQUIRE(file.is_open());
    auto golden = std::string {};
    auto line = std::string {};
    while (std::getline(file, line))
        if (!line.empty() && !line.starts_with('#'))
            golden += line + '\n';
    INFO("rendered:\n" << rendered);
    CHECK(rendered == golden);
}
