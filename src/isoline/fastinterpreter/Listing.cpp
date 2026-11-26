// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/Listing.hpp>

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace isoline::fastinterpreter
{

namespace
{
    /// Appends the records of a br_table to @p listing: one `@<offset> s<destination>` per label.
    void describeEntries(std::string& listing, std::span<Word const> entries)
    {
        auto separator = std::string_view { " → " };
        for (auto const label: std::views::iota(std::size_t { 0 }, entries.size() / BranchTableEntryWords))
        {
            auto const record = entries.subspan(label * BranchTableEntryWords, BranchTableEntryWords);
            Require(record[1] <= std::to_underlying(LoopPoll::Yes));
            auto const poll = static_cast<LoopPoll>(record[1]) == LoopPoll::Yes ? std::string_view { ".loop" }
                                                                                : std::string_view {};
            listing +=
                std::format("{}@{}{} s{}", separator, record[0] & BranchTargetMask, poll, record[0] >> 32U);
            separator = ", ";
        }
    }
} // namespace

std::string describe(LoweredFunction const& function)
{
    auto listing = std::string {};
    auto const code = std::span { function.code };
    auto offset = std::size_t { 0 };
    while (offset < code.size())
    {
        Require(code[offset] < OperationCount);
        auto const& row = operationRow(static_cast<OperationId>(code[offset]));
        Require(offset + row.words <= code.size());
        auto const operands = code.subspan(offset + 1, row.words - 1);
        listing += row.text;
        auto separator = std::string_view { " " };
        for (auto const index: std::views::iota(uint32_t { 0 }, row.slots))
        {
            auto const word = operands[index / 2];
            auto const slot = static_cast<uint32_t>(index % 2 == 0 ? word : word >> 32U);
            listing += std::format("{}s{}", separator, slot);
            separator = ", ";
        }
        auto immediates = operands.subspan((row.slots + 1) / 2);
        auto const branches = row.form == OperationForm::Branch || row.form == OperationForm::BranchMove;
        auto const target = branches ? immediates.back() : Word { 0 };
        if (branches)
            immediates = immediates.first(immediates.size() - 1);
        for (auto const immediate: immediates)
        {
            listing += std::format("{}#{}", separator, immediate);
            separator = ", ";
        }
        auto extent = std::size_t { row.words };
        switch (row.form)
        {
            case OperationForm::Branch: listing += std::format(" → @{}", target & BranchTargetMask); break;
            case OperationForm::BranchMove:
                listing += std::format("{}#{} → @{}", separator, target >> 32U, target & BranchTargetMask);
                break;
            case OperationForm::BranchTable: {
                // The immediates are the values' slot count, then the labels before the default.
                auto const records = (immediates[1] + 1) * BranchTableEntryWords;
                Require(records <= code.size() - offset - row.words);
                describeEntries(listing, code.subspan(offset + row.words, records));
                extent += records;
                break;
            }
            default: break;
        }
        listing += '\n';
        offset += extent;
    }
    return listing;
}

} // namespace isoline::fastinterpreter
