// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

// The names Opcodes.def gives its instructions, the rule they follow, and the checks that hold them to
// it, read by Opcode.cpp's checks, its test and the reflection checks. Installed with isoline_core's
// headers, which every header of the module surface must be (scripts/check-module-exports.py), and C++23
// like each of them; its namespace is internal to module isoline.

namespace isoline::binary
{

/// Every Opcode enumerator's name as Opcodes.def spells it (#Name), in enumerator order.
inline constexpr auto OpcodeNames = std::to_array<std::string_view>({
#define ISOLINE_OPCODE(Name, Prefix, Code, Text, Immediate, Signature, Proposal) #Name,
#include <isoline/binary/Opcodes.def>
#undef ISOLINE_OPCODE
});

/// Every Opcode's text as Opcodes.def spells it (OpcodeInfo::text), in enumerator order.
inline constexpr auto OpcodeTexts = std::to_array<std::string_view>({
#define ISOLINE_OPCODE(Name, Prefix, Code, Text, Immediate, Signature, Proposal) Text,
#include <isoline/binary/Opcodes.def>
#undef ISOLINE_OPCODE
});

/// An Opcode whose name is not its mnemonic in PascalCase, and why.
struct OpcodeNameException
{
    std::string_view name;   ///< The Opcode enumerator's name.
    std::string_view text;   ///< Its text, as Opcodes.def spells it.
    std::string_view reason; ///< Why the name does not follow the text.
};

/// Every Opcode the naming rule (namedByRule) does not hold for, one row each.
inline constexpr auto OpcodeNameExceptions = std::to_array<OpcodeNameException>({
    { .name = "SelectTyped",
      .text = "select_t",
      .reason =
          "typed select shares select's text; Opcodes.def names it after the binary grammar's select t*" },
});

/// Whether @p name is @p text with each '.'- or '_'-separated part capitalized and the separators
/// dropped; no allocation. An empty part (a leading, trailing or doubled separator) follows no rule.
/// @param name An Opcode enumerator's name.
/// @param text Its text, as Opcodes.def spells it.
/// @return Whether the name is the text in PascalCase.
[[nodiscard]] consteval bool namedByRule(std::string_view name, std::string_view text) noexcept
{
    auto rest = name;
    auto partStarts = true;
    for (auto const character: text)
    {
        if (character == '.' || character == '_')
        {
            if (partStarts)
                return false;
            partStarts = true;
            continue;
        }
        auto const wanted = partStarts && character >= 'a' && character <= 'z'
                                ? static_cast<char>(character - 'a' + 'A')
                                : character;
        if (rest.empty() || rest.front() != wanted)
            return false;
        rest.remove_prefix(1);
        partStarts = false;
    }
    return !partStarts && rest.empty();
}

/// The number of rows of @p exceptions that name @p name with @p text.
/// @param exceptions The exception rows.
/// @param name An Opcode enumerator's name.
/// @param text Its text.
/// @return How many rows match both.
[[nodiscard]] consteval std::size_t exceptionsFor(std::span<OpcodeNameException const> exceptions,
                                                  std::string_view name,
                                                  std::string_view text) noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(
        exceptions, [&](OpcodeNameException const& row) { return row.name == name && row.text == text; }));
}

/// The first opcode whose name neither follows the rule (namedByRule) with no exception row, nor
/// fails it with exactly one.
/// @param names Every opcode's name, in enumerator order.
/// @param texts Every opcode's text, as many as @p names.
/// @param exceptions The exception rows.
/// @return The offender's index, or names.size() when there is none. C++23 cannot compute a
///         static_assert message, so a check gives the index alone.
[[nodiscard]] consteval std::size_t firstMisnamedOpcode(
    std::span<std::string_view const> names,
    std::span<std::string_view const> texts,
    std::span<OpcodeNameException const> exceptions) noexcept
{
    auto index = std::size_t { 0 };
    for (auto const name: names)
    {
        auto const text = texts[index];
        auto const rows = exceptionsFor(exceptions, name, text);
        if (namedByRule(name, text) ? rows != 0 : rows != 1)
            return index;
        ++index;
    }
    return names.size();
}

/// The first exception row that does not name exactly one opcode, by name and text, that the rule
/// does not hold for.
/// @param names Every opcode's name, in enumerator order.
/// @param texts Every opcode's text, as many as @p names.
/// @param exceptions The exception rows.
/// @return The offending row's index, or exceptions.size() when there is none.
[[nodiscard]] consteval std::size_t firstUnusedException(
    std::span<std::string_view const> names,
    std::span<std::string_view const> texts,
    std::span<OpcodeNameException const> exceptions) noexcept
{
    auto index = std::size_t { 0 };
    for (auto const& exception: exceptions)
    {
        auto uses = std::size_t { 0 };
        auto opcode = std::size_t { 0 };
        for (auto const name: names)
        {
            if (name == exception.name && texts[opcode] == exception.text)
                ++uses;
            ++opcode;
        }
        if (uses != 1 || namedByRule(exception.name, exception.text))
            return index;
        ++index;
    }
    return exceptions.size();
}

} // namespace isoline::binary
