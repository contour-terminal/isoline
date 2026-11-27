// SPDX-License-Identifier: Apache-2.0
#include <isoline/cli/VerbArguments.hpp>

#include <core/cli/CLI.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace isoline::cli
{

namespace
{
    namespace corecli = ::core::cli;

    /// The two defaults a part is parsed with: an option whose value differs between the two parses
    /// was not given.
    constexpr auto FirstDefault = std::string_view {};
    constexpr auto SecondDefault = std::string_view { "\x01" };

    /// The token that ends the options, by ::core::cli's convention.
    constexpr auto EndOfOptions = std::string_view { "--" };

    /// One part of a verb's command line, parsed twice.
    struct ParsedPart
    {
        corecli::FlagStore first;  ///< Parsed with FirstDefault.
        corecli::FlagStore second; ///< Parsed with SecondDefault.
    };

    /// Parses @p tokens as the command @p verb, with an operand or without, once with each default.
    /// @param verb The verb's name.
    /// @param tokens The tokens after it.
    /// @param operand The operand the command takes, or nothing for options alone.
    /// @return Both parses, or ::core::cli's sentence for what is wrong.
    std::expected<ParsedPart, std::string> parsePart(std::string_view verb,
                                                     std::span<std::string_view const> tokens,
                                                     std::optional<corecli::Verbatim> const& operand)
    {
        auto arguments = corecli::StringViewList { verb };
        arguments.insert(arguments.end(), tokens.begin(), tokens.end());
        auto const parseWith = [&](std::string_view defaultValue) {
            return corecli::parse(corecli::Command { .name = verb,
                                                     .options = verbOptionList(defaultValue),
                                                     .verbatim = operand },
                                  arguments);
        };
        auto first = parseWith(FirstDefault);
        if (!first)
            return std::unexpected { std::move(first).error().message };
        // The second parse reads the same tokens by the same rules as the first, so it cannot fail where
        // the first did not; if it did, value() throws, and the tool reports an internal error.
        auto second = parseWith(SecondDefault).value();
        return ParsedPart { .first = std::move(*first), .second = std::move(second) };
    }

    /// Takes the options a part gives into @p arguments, over what they held.
    /// @param verb The verb's name, the prefix of the parsed keys.
    /// @param part The part.
    /// @param arguments The arguments so far.
    void takeOptions(std::string_view verb, ParsedPart const& part, VerbArguments& arguments)
    {
        for (auto const& row: VerbOptions)
        {
            auto const key = std::format("{}.{}", verb, row.name);
            if (auto const& value = part.first.str(key); value == part.second.str(key))
                arguments.options[static_cast<std::size_t>(row.option)] = value;
        }
    }
} // namespace

corecli::OptionList verbOptionList(std::string_view defaultValue)
{
    auto options = corecli::OptionList {};
    for (auto const& row: VerbOptions)
        options.push_back(corecli::Option {
            .name = row.shortName == '\0' ? corecli::OptionName { row.name }
                                          : corecli::OptionName { row.shortName, row.name },
            .v = corecli::Value { std::string { defaultValue } },
            .helpText = row.help,
            .placeholder = row.placeholder,
        });
    return options;
}

corecli::Verbatim verbOperand()
{
    auto names = std::string {};
    for (auto const& row: VerbOptions)
    {
        if (row.shortName != '\0')
            names += std::format("{}, ", row.shortName);
        names += std::format("{}, ", row.name);
    }
    names.resize(names.size() - 2);
    return corecli::Verbatim {
        .placeholder = "INPUT",
        .helpText = std::format(
            "The input; - is standard input. An input named like an option ({}) follows --.", names),
    };
}

std::expected<VerbArguments, std::string> parseVerbArguments(std::string_view verb,
                                                             std::span<std::string_view const> tokens)
{
    auto const head = parsePart(verb, tokens, verbOperand());
    if (!head)
        return std::unexpected { head.error() };
    auto arguments = VerbArguments {};
    takeOptions(verb, *head, arguments);
    auto const& operands = head->first.verbatim;
    if (operands.empty())
        return std::unexpected { std::string { "no input given" } };
    // The operands are the last tokens. A `--` before them ended the options, unless it was an option's
    // value: then the tokens before it do not parse as options alone.
    auto const firstOperand = tokens.size() - operands.size();
    auto const ended = firstOperand > 0 && tokens[firstOperand - 1] == EndOfOptions
                       && parsePart(verb, tokens.first(firstOperand - 1), std::nullopt).has_value();
    if (!ended && operands.size() > 1)
    {
        auto const tail = parsePart(verb, std::span { operands }.subspan(1), std::nullopt);
        if (!tail)
            return std::unexpected { tail.error() };
        takeOptions(verb, *tail, arguments);
    }
    else if (operands.size() > 1)
        return std::unexpected { std::format("Unexpected argument \"{}\" after the input.", operands[1]) };
    arguments.input = std::string { operands.front() };
    return arguments;
}

} // namespace isoline::cli
