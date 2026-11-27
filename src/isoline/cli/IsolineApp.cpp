// SPDX-License-Identifier: Apache-2.0
#include <isoline/cli/IsolineApp.hpp>

#include <core/Environment.hpp>
#include <core/cli/App.hpp>
#include <core/cli/CLI.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/Version.hpp>
#include <isoline/cli/FileAccess.hpp>
#include <isoline/cli/VerbArguments.hpp>
#include <isoline/cli/Verbs.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::cli
{

namespace
{
    namespace corecli = ::core::cli;

    /// The program's name, the root of every parameter key.
    constexpr auto AppName = std::string_view { "isoline" };

    /// The width the usage text is laid out in.
    constexpr auto UsageWidth = 80U;

    /// The `--features` values, comma-separated, for a diagnostic.
    std::string featureChoiceNames()
    {
        auto names = std::string {};
        for (auto const& row: FeatureChoices)
            names += names.empty() ? std::string { row.name } : std::format(", {}", row.name);
        return names;
    }

    /// The FeatureChoices row named @p name.
    /// @param name A `--features` value.
    /// @return Its choice, or nothing for a name no row has.
    std::optional<FeatureChoice> featureChoiceNamed(std::string_view name)
    {
        auto const row = std::ranges::find(FeatureChoices, name, &FeatureChoiceRow::name);
        if (row == FeatureChoices.end())
            return std::nullopt;
        return row->choice;
    }

    /// The library's version, as `major.minor.patch`.
    std::string versionText()
    {
        auto const current = version();
        return std::format("{}.{}.{}", current.major, current.minor, current.patch);
    }
} // namespace

IsolineApp::IsolineApp(::core::Environment const& environment,
                       IFileAccess& files,
                       std::ostream& diagnostics,
                       text::TextLimits limits,
                       std::span<VerbRow const> verbs):
    App { environment, std::string { AppName }, "Isoline", versionText(), "Apache-2.0" },
    _files { files },
    _diagnostics { diagnostics },
    _limits { limits },
    _verbs { verbs }
{
}

corecli::Command IsolineApp::parameterDefinition() const
{
    auto root = corecli::Command {
        .name = AppName,
        .helpText = "Isoline, a WebAssembly runtime.",
        .children = {
            corecli::Command { .name = "help", .helpText = "Shows this help." },
            corecli::Command { .name = "version", .helpText = "Shows the version." },
            corecli::Command { .name = "license", .helpText = "Shows the licenses of Isoline and its parts." },
        },
    };
    for (auto const& verb: _verbs)
        root.children.push_back(corecli::Command {
            .name = verb.name,
            .helpText = verb.help,
            .options = verbOptionList({}),
            .verbatim = verbOperand(),
        });
    return root;
}

int IsolineApp::run(int argc, char const* argv[])
{
    // Every exception, out of memory included, ends the run with its ExitCode and a line in the
    // diagnostics, not with App::run's EXIT_FAILURE on std::cerr.
    try
    {
        // Parsed here first, so that a wrong command line exits with its ExitCode rather than with
        // App::run's EXIT_FAILURE, which is InputRejected's value.
        auto const syntax = parameterDefinition();
        auto const parsed = corecli::parse(syntax, argc, argv);
        if (!parsed)
            return refuseCommandLine(parsed.error().message);
        auto const chosen = std::ranges::find_if(syntax.children, [&](corecli::Command const& command) {
            return parsed->get<bool>(std::format("{}.{}", AppName, command.name));
        });
        if (chosen == syntax.children.end())
            return refuseCommandLine("no verb given");
        // A verb's command line is parsed by parseVerbArguments(), from the tokens after its name, which
        // is argv[1]: the root takes no options.
        auto const verb = std::ranges::find(_verbs, chosen->name, &VerbRow::name);
        if (verb == _verbs.end())
            return App::run(argc, argv);
        auto tokens = std::vector<std::string_view> {};
        for (auto const index: std::views::iota(2, argc))
            tokens.emplace_back(argv[index]);
        return runVerb(*verb, tokens);
    }
    catch (...)
    {
        auto const failure = failureOfCurrentException(AppName);
        _diagnostics << failure.message << '\n';
        return static_cast<int>(exitCodeOf(failure.refusal));
    }
}

int IsolineApp::runVerb(VerbRow const& verb, std::span<std::string_view const> tokens)
{
    // Not ::core::cli's own verb parse, which reads every token after the input as an operand: options
    // after the input are a workaround until contour-terminal/core-cpp#106 (see parseVerbArguments()).
    auto const arguments = parseVerbArguments(verb.name, tokens);
    if (!arguments)
        return refuseCommandLine(arguments.error());
    auto const& output = arguments->option(VerbOption::Output);
    if (!output)
        return refuseCommandLine("no output given (-o, --output)");
    if (output->empty())
        return refuseCommandLine("the output is empty (-o, --output)");
    auto const& features = arguments->option(VerbOption::Features);
    auto const choice = features ? featureChoiceNamed(*features) : std::optional { FeatureChoice::All };
    if (!choice)
        return refuseCommandLine(
            std::format("unknown feature set '{}' ({})", *features, featureChoiceNames()));
    auto const context =
        VerbContext { .files = _files, .diagnostics = _diagnostics, .features = *choice, .limits = _limits };
    return static_cast<int>(verb.run(context, arguments->input, *output));
}

int IsolineApp::refuseCommandLine(std::string_view message) const
{
    auto style = corecli::HelpDisplayStyle {};
    style.colors.reset();
    style.hyperlink = false;
    _diagnostics << std::format("{}: {}\n\n", AppName, message)
                 << corecli::usageText(parameterDefinition(), style, UsageWidth);
    return static_cast<int>(exitCodeOf(Refusal::CommandLine));
}

} // namespace isoline::cli
