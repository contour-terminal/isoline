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

#include <isoline/binary/Opcode.hpp>
#include <isoline/fastinterpreter/LoweringTable.hpp>
#include <isoline/numerics/SimdShape.hpp>

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

    /// The name of the first instruction of OpcodeTable whose numerics kernel @p word is, for the generic
    /// vector operation @p operation: what a listing shows in its place, as an address differs from run to
    /// run. A relaxed operator the deterministic profile maps onto its plain twin shares the twin's kernel,
    /// and so its name.
    [[nodiscard]] std::string_view kernelNameOf(OperationId operation, Word word)
    {
        for (auto const& row: binary::OpcodeTable)
            if (simdKernelOperationOf(numerics::simdShape(row.opcode)) == operation
                && simdKernelWordOf(row.opcode) == word)
                return row.text;
        // Lowering writes only the kernels of OpcodeTable's rows.
        // uncovered-begin(unknown-kernel)
        core::unreachable();
    }
    // uncovered-end(unknown-kernel)
} // namespace

std::string describe(LoweredFunction const& function)
{
    auto listing = std::string {};
    auto const code = std::span { function.code };
    auto offset = std::size_t { 0 };
    while (offset < code.size())
    {
        auto const operation = operationOfHeader(code[offset]);
        auto const& row = operationRow(operation);
        auto const extent = operationExtent(operation, code.subspan(offset));
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
        // A generic vector operation's last word is a kernel, named by its instruction.
        auto const kernel = isSimdKernelOperation(operation);
        if (kernel)
            immediates = immediates.first(immediates.size() - 1);
        for (auto const immediate: immediates)
        {
            listing += std::format("{}#{}", separator, immediate);
            separator = ", ";
        }
        if (kernel)
            listing += std::format("{}{}", separator, kernelNameOf(operation, operands.back()));
        switch (row.form)
        {
            case OperationForm::Branch: listing += std::format(" → @{}", target & BranchTargetMask); break;
            case OperationForm::BranchMove:
                listing += std::format("{}#{} → @{}", separator, target >> 32U, target & BranchTargetMask);
                break;
            case OperationForm::BranchTable:
                describeEntries(listing, code.subspan(offset + row.words, extent - row.words));
                break;
            default: break;
        }
        listing += '\n';
        offset += extent;
    }
    return listing;
}

} // namespace isoline::fastinterpreter
