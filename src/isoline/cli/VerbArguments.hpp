// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/cli/CLI.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <isoline/meta/EnumRows.hpp>

// A verb's command line: its options, as table rows, and its one operand, the input.
namespace isoline::cli
{

/// An option every verb takes: one row of VerbOptions each.
enum class VerbOption : uint8_t
{
    Output,   ///< `-o`/`--output`: where the result goes.
    Features, ///< `--features`: the proposals the input may use.
};

/// The last VerbOption enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastVerbOption = VerbOption::Features;

/// One option of every verb.
struct VerbOptionRow
{
    VerbOption option;            ///< The option, at its enumerator's index.
    char shortName;               ///< Its one-letter spelling, or '\0' for none.
    std::string_view name;        ///< Its long spelling, and its key in the parsed options.
    std::string_view placeholder; ///< What its value is called in the usage text.
    std::string_view help;        ///< Its line in the usage text.
};

/// Every option every verb takes. Adding one is adding a row.
inline constexpr auto VerbOptions = std::to_array<VerbOptionRow>({
    { .option = VerbOption::Output,
      .shortName = 'o',
      .name = "output",
      .placeholder = "FILE",
      .help = "Where to write the result; - is standard output. Required." },
    { .option = VerbOption::Features,
      .shortName = '\0',
      .name = "features",
      .placeholder = "SET",
      .help = "The proposals the input may use: all (the default) or wasm3." },
});

static_assert(meta::firstMisplacedRow<LastVerbOption>(VerbOptions, &VerbOptionRow::option)
                  == meta::NoMisplacedRow,
              "VerbOptions must hold one row per VerbOption, at the index of its enumerator");
static_assert(meta::firstRepeatedKey(VerbOptions, &VerbOptionRow::name) == meta::NoRepeatedKey,
              "VerbOptions must spell each long option once: the command line tells the options apart by it");
static_assert(
    meta::firstRepeatedKey(VerbOptions, &VerbOptionRow::shortName, '\0') == meta::NoRepeatedKey,
    "VerbOptions must spell each short option once: the command line tells the options apart by it");

/// A verb's command line, parsed.
struct VerbArguments
{
    std::string input; ///< The one operand.
    /// Each VerbOptions row's value, by index; empty when the command line does not give it. An option
    /// given an empty value (`--output=`) holds the empty string.
    std::array<std::optional<std::string>, VerbOptions.size()> options {};

    /// The value of @p option.
    /// @param option The option.
    /// @return Its value, or nothing when it was not given.
    [[nodiscard]] std::optional<std::string> const& option(VerbOption option) const
    {
        return options[static_cast<std::size_t>(option)];
    }
};

/// The ::core::cli options of a verb, from VerbOptions.
/// @param defaultValue Every option's default: what parsing reports for one not given.
/// @return The options.
[[nodiscard]] ::core::cli::OptionList verbOptionList(std::string_view defaultValue);

/// The ::core::cli description of a verb's operand.
/// @return The `INPUT` operand, whose help says that a name like an option's follows `--`.
[[nodiscard]] ::core::cli::Verbatim verbOperand();

/// Parses a verb's command line: its options, before and after its input, and the input.
///
/// ::core::cli takes every token from a command's first operand on as an operand
/// (contour-terminal/core-cpp#106), so the options after the input are parsed again here, as a command
/// line of their own; every token is still read by ::core::cli::parse. An option given both before and
/// after the input takes the later value. A `--` before the input ends the options: what follows it is
/// the input, and nothing may follow that. Each part is parsed twice, with two defaults, so that an option
/// not given (both parses report their default) is told from one given (both report its value).
///
/// ::core::cli also reads a bare option name as the option (its Natural style), so an input named `o`,
/// `output` or `features` must follow `--`.
/// @param verb The verb's name.
/// @param tokens The tokens after the verb's name.
/// @return The arguments, or a sentence saying what is wrong.
[[nodiscard]] std::expected<VerbArguments, std::string> parseVerbArguments(
    std::string_view verb, std::span<std::string_view const> tokens);

} // namespace isoline::cli
