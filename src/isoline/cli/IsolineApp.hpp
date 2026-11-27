// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>
#include <core/cli/App.hpp>
#include <core/cli/CLI.hpp>

#include <iosfwd>
#include <span>
#include <string_view>

#include <isoline/cli/FileAccess.hpp>
#include <isoline/cli/Verbs.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::cli
{

/// The `isoline` command: one ::core::cli command per verb row, each taking its input as an operand
/// and the VerbOptions (`-o`/`--output`, required, and `--features all|wasm3`, default `all`), beside
/// core-cpp's `help`, `version` and `license`.
///
/// Options may come before or after the input (`isoline compile app.wat -o app.wasm`); see
/// parseVerbArguments(). Every exit status is an ExitCode: a command line that does not parse, or names
/// no verb, is a Refusal::CommandLine and prints the usage to the diagnostics, and an exception is
/// reported there too (failureOfCurrentException()).
class IsolineApp final: public ::core::cli::App
{
  public:
    /// The tool, reaching files through @p files and reporting to @p diagnostics.
    /// @param environment The process environment; it must outlive the app.
    /// @param files Where verbs read and write; it must outlive the app.
    /// @param diagnostics Where failures and the usage text go; it must outlive the app.
    /// @param limits The limits of text every verb runs with.
    /// @param verbs The verbs: Verbs in the tool. It must outlive the app.
    IsolineApp(::core::Environment const& environment,
               IFileAccess& files,
               std::ostream& diagnostics,
               text::TextLimits limits,
               std::span<VerbRow const> verbs);

    /// The command line syntax: the root `isoline`, its verbs and core-cpp's own commands.
    /// @return The syntax.
    [[nodiscard]] ::core::cli::Command parameterDefinition() const override;

    /// Parses the command line and runs the verb it names; core-cpp's own commands run in App::run.
    /// @param argc The argument count.
    /// @param argv The arguments, the program's name first.
    /// @return An ExitCode's value.
    int run(int argc, char const* argv[]) override;

  private:
    /// Runs a verb with the tokens after its name.
    /// @param verb The verb's row.
    /// @param tokens Its tokens.
    /// @return The ExitCode's value.
    [[nodiscard]] int runVerb(VerbRow const& verb, std::span<std::string_view const> tokens);

    /// Reports a wrong command line, with the usage text.
    /// @param message What is wrong.
    /// @return Refusal::CommandLine's exit code's value.
    [[nodiscard]] int refuseCommandLine(std::string_view message) const;

    IFileAccess& _files;             ///< Where verbs read and write.
    std::ostream& _diagnostics;      ///< Where failures go.
    text::TextLimits _limits;        ///< The limits of text.
    std::span<VerbRow const> _verbs; ///< The verbs.
};

} // namespace isoline::cli
