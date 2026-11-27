// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/Instructions.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include <isoline/binary/Opcode.hpp>

namespace isoline::text
{

namespace
{
    using binary::Opcode;
    using binary::OpcodeTable;

    /// Whether @p opcode is a row of InternalTextNames, whose table text the text format does not spell.
    consteval bool hasInternalText(Opcode opcode) noexcept
    {
        for (auto const& row: InternalTextNames)
            if (row.opcode == opcode)
                return true;
        return false;
    }

    /// An instruction's OpcodeTable text.
    constexpr std::string_view tableText(Opcode opcode) noexcept
    {
        return OpcodeTable[static_cast<std::size_t>(opcode)].text;
    }

    /// The number of OpcodeTable rows the text format spells by their table text.
    consteval std::size_t spelledCount() noexcept
    {
        auto count = std::size_t { 0 };
        for (auto const& row: OpcodeTable)
            if (!hasInternalText(row.opcode))
                ++count;
        return count;
    }

    /// The opcodes the text format spells by their table text, sorted by that text.
    consteval std::array<Opcode, spelledCount()> spelledOpcodes() noexcept
    {
        auto opcodes = std::array<Opcode, spelledCount()> {};
        auto next = opcodes.begin();
        for (auto const& row: OpcodeTable)
            if (!hasInternalText(row.opcode))
                *next++ = row.opcode;
        std::ranges::sort(opcodes, {}, &tableText);
        return opcodes;
    }

    /// What opcodeNamed() searches: the opcodes the text format spells by their table text, sorted by it.
    constexpr auto OpcodesByText = spelledOpcodes();

    static_assert(std::ranges::adjacent_find(OpcodesByText, {}, &tableText) == OpcodesByText.end(),
                  "every instruction the text format spells by its OpcodeTable text has a text of its own");
} // namespace

std::optional<Opcode> opcodeNamed(std::string_view text) noexcept
{
    auto const found = std::ranges::lower_bound(OpcodesByText, text, {}, &tableText);
    if (found == OpcodesByText.end() || tableText(*found) != text)
        return std::nullopt;
    return *found;
}

IndexImmediate const* indexImmediateOf(binary::ImmediateKind kind) noexcept
{
    auto const found = std::ranges::find(IndexImmediates, kind, &IndexImmediate::kind);
    return found == IndexImmediates.end() ? nullptr : &*found;
}

std::string_view textName(Opcode opcode) noexcept
{
    auto const internal = std::ranges::find(InternalTextNames, opcode, &InternalTextName::opcode);
    if (internal != InternalTextNames.end())
        return internal->text;
    return binary::opcodeInfo(opcode).text;
}

} // namespace isoline::text
