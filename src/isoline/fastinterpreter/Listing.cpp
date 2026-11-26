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

namespace isoline::fastinterpreter
{

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
        for (auto const immediate: operands.subspan((row.slots + 1) / 2))
        {
            listing += std::format("{}#{}", separator, immediate);
            separator = ", ";
        }
        listing += '\n';
        offset += row.words;
    }
    return listing;
}

} // namespace isoline::fastinterpreter
