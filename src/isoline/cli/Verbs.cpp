// SPDX-License-Identifier: Apache-2.0
#include <isoline/cli/Verbs.hpp>

#include <core/Overloaded.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <new>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/cli/FileAccess.hpp>
#include <isoline/cli/VerbsTables.hpp>
#include <isoline/text/ModuleWriter.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>
#include <isoline/validate/ValidationError.hpp>

namespace isoline::cli
{

namespace
{
    /// A verb's failure, as the value of a std::expected.
    /// @param refusal Why it failed.
    /// @param message The diagnostic line.
    /// @return The unexpected value.
    std::unexpected<Failure> fail(Refusal refusal, std::string message)
    {
        return std::unexpected { Failure { .refusal = refusal, .message = std::move(message) } };
    }

    /// Reads a verb's input within @p maxBytes.
    /// @param context Where to read it.
    /// @param input Its path.
    /// @param maxBytes The most bytes it may hold.
    /// @return Its bytes, or Unreadable, or InputTooLarge.
    std::expected<std::vector<std::byte>, Failure> readInput(VerbContext const& context,
                                                             std::string_view input,
                                                             std::size_t maxBytes)
    {
        auto bytes = context.files.read(input, maxBytes);
        if (bytes)
            return std::move(*bytes);
        switch (bytes.error().kind)
        {
            case FileErrorKind::TooLarge:
                return fail(Refusal::InputTooLarge,
                            std::format("{}: error: larger than the limit of {} bytes", input, maxBytes));
            case FileErrorKind::Unavailable: break;
        }
        return fail(Refusal::Unreadable,
                    std::format("{}: error: cannot read: {}", input, bytes.error().reason));
    }

    /// Writes a verb's output.
    /// @param context Where to write it.
    /// @param output Its path.
    /// @param bytes What to write.
    /// @return Nothing, or Unwritable.
    std::expected<void, Failure> writeOutput(VerbContext const& context,
                                             std::string_view output,
                                             std::span<std::byte const> bytes)
    {
        return context.files.write(output, bytes).transform_error([output](FileError const& error) {
            return Failure { .refusal = Refusal::Unwritable,
                             .message = std::format("{}: error: cannot write: {}", output, error.reason) };
        });
    }

    /// Runs a verb's work, reporting its failure, a thrown exception included.
    /// @param context Where to report.
    /// @param input The input's path, which a host failure is reported at.
    /// @param work The work.
    /// @return Success, or the failure's exit code.
    template <typename Work>
    ExitCode report(VerbContext const& context, std::string_view input, Work const& work)
    {
        auto const outcome = [&]() -> std::expected<void, Failure> {
            try
            {
                return work();
            }
            catch (...)
            {
                return std::unexpected { failureOfCurrentException(input) };
            }
        }();
        if (outcome)
            return ExitCode::Success;
        context.diagnostics << outcome.error().message << '\n';
        return exitCodeOf(outcome.error().refusal);
    }

} // namespace

Failure loadFailure(api::LoadError const& error, std::string_view input)
{
    return std::visit(
        core::Overloaded {
            [input](api::ModuleTooLarge const& tooLarge) {
                // compileText refuses a module past TextLimits::maxModuleBytes, the engine's limit
                // too, before the engine sees it.
                return Failure { .refusal = Refusal::InputTooLarge,
                                 .message = std::format("{}: error: its module of {} bytes is larger "
                                                        "than the limit of {} bytes",
                                                        input,
                                                        tooLarge.bytes,
                                                        tooLarge.limit) };
            },
            [input](api::HostMemoryExhausted const&) {
                return Failure { .refusal = Refusal::OutOfMemory,
                                 .message = std::format("{}: error: out of memory", input) };
            },
            [input](binary::DecodeError const& decode) {
                return Failure { .refusal = Refusal::ModuleMalformed,
                                 .message =
                                     std::format("{}: error: {}", input, binary::specMessage(decode)) };
            },
            [input](validate::ValidationError const& invalid) {
                auto const message = validate::specMessage(invalid);
                auto const where = [&]() -> std::string {
                    if (invalid.function == validate::NoFunction)
                        return {};
                    if (invalid.instruction == validate::NoInstruction)
                        return std::format("function {}: ", invalid.function);
                    return std::format(
                        "function {}, instruction {}: ", invalid.function, invalid.instruction);
                }();
                return Failure { .refusal = Refusal::ModuleInvalid,
                                 .message = std::format("{}: error: {}{}", input, where, message) };
            },
        },
        error);
}

namespace
{

    /// Compiles a text, then loads it into an engine of the chosen features, which decodes and
    /// validates it.
    /// @param source The text.
    /// @param context The features and limits.
    /// @param input The text's path.
    /// @return The binary, or the failure.
    std::expected<std::vector<std::byte>, Failure> compileSource(std::string_view source,
                                                                 VerbContext const& context,
                                                                 std::string_view input)
    {
        auto bytes = text::compileText(source, context.limits);
        if (!bytes)
            return fail(Refusal::TextMalformed,
                        std::format("{}:{}:{}: error: {}",
                                    input,
                                    bytes.error().position.line,
                                    bytes.error().position.column,
                                    text::specMessage(bytes.error())));
        auto const engine = api::Engine { api::EngineConfig {
            .features = featuresOf(context.features),
            .load = api::LoadLimits { .maxModuleBytes = context.limits.maxModuleBytes } } };
        if (auto const loaded = engine.load(*bytes); !loaded)
            return std::unexpected { loadFailure(loaded.error(), input) };
        return std::move(*bytes);
    }

    /// The format of a binary, by its preamble: the core module's when none matches.
    /// @param bytes The binary.
    /// @return Its BinaryFormats row.
    BinaryFormat const& formatOf(std::span<std::byte const> bytes)
    {
        auto const format = std::ranges::find_if(BinaryFormats, [bytes](BinaryFormat const& row) {
            return bytes.size() >= row.preamble.size()
                   && std::ranges::equal(bytes.first(row.preamble.size()), row.preamble);
        });
        return format == BinaryFormats.end() ? BinaryFormats.front() : *format;
    }

    /// What a verb writes.
    enum class OutputContent : uint8_t
    {
        Text,   ///< Text, which a terminal shows.
        Binary, ///< A binary, which a terminal would show as noise.
    };

    /// Refuses an output that would destroy the input, or show a binary on a terminal, before the input
    /// is read.
    /// @param context Where the files are.
    /// @param input The input's path.
    /// @param output The output's path.
    /// @param content What the verb writes.
    /// @return Nothing, or OutputIsInput, or BinaryToTerminal.
    std::expected<void, Failure> checkOutput(VerbContext const& context,
                                             std::string_view input,
                                             std::string_view output,
                                             OutputContent content)
    {
        if (context.files.isSameFile(input, output))
            return fail(
                Refusal::OutputIsInput,
                std::format("{}: error: the output names the input, which it would overwrite", output));
        if (content == OutputContent::Binary && context.files.isTerminal(output))
            return fail(
                Refusal::BinaryToTerminal,
                std::format("{}: error: a binary is not written to a terminal; redirect it, or name a "
                            "file with -o",
                            output));
        return {};
    }
} // namespace

ExitCode compileVerb(VerbContext const& context, std::string_view input, std::string_view output)
{
    return report(context, input, [&] {
        return checkOutput(context, input, output, OutputContent::Binary)
            .and_then([&] { return readInput(context, input, context.limits.maxSourceBytes); })
            .and_then([&](std::vector<std::byte> const& source) {
                auto const text =
                    std::string_view { reinterpret_cast<char const*>(source.data()), source.size() };
                return compileSource(text, context, input);
            })
            .and_then(
                [&](std::vector<std::byte> const& binary) { return writeOutput(context, output, binary); });
    });
}

ExitCode disassembleVerb(VerbContext const& context, std::string_view input, std::string_view output)
{
    return report(context, input, [&] {
        return checkOutput(context, input, output, OutputContent::Text)
            .and_then([&] { return readInput(context, input, context.limits.maxModuleBytes); })
            .and_then([&](std::vector<std::byte> const& binary) {
                return formatOf(binary).disassemble(binary, context, input);
            })
            .and_then([&](std::string const& text) {
                return writeOutput(context, output, std::as_bytes(std::span { text }));
            });
    });
}

std::expected<std::string, Failure> disassembleCoreModule(std::span<std::byte const> bytes,
                                                          VerbContext const& context,
                                                          std::string_view input)
{
    auto const module = binary::decodeModule(bytes, featuresOf(context.features));
    if (!module)
        return fail(Refusal::ModuleMalformed,
                    std::format("{}: error at offset {:#x}: {}",
                                input,
                                module.error().offset,
                                binary::specMessage(module.error())));
    // The text it writes must read back: no larger than the reader's source limit.
    auto text =
        text::ModuleWriter { text::WriterOptions { .maxTextBytes = context.limits.maxSourceBytes } }.write(
            *module);
    if (text)
        return std::move(*text);
    auto const& row = WriteRefusals[static_cast<std::size_t>(text.error())];
    return fail(row.refusal,
                std::format("{}: error: {}",
                            input,
                            std::vformat(row.message, std::make_format_args(context.limits.maxSourceBytes))));
}

std::expected<std::string, Failure> refuseComponentBinary(std::span<std::byte const> /*bytes*/,
                                                          VerbContext const& /*context*/,
                                                          std::string_view input)
{
    // The message is the text format's own for component text, so that both verbs say it alike.
    auto const message = text::specMessage(text::TextError { text::TextErrorCode::UnsupportedComponent, {} });
    return fail(Refusal::ComponentBinary, std::format("{}: error: {}", input, message));
}

Failure failureOfCurrentException(std::string_view subject)
{
    try
    {
        throw;
    }
    catch (std::bad_alloc const&)
    {
        return Failure { .refusal = Refusal::OutOfMemory,
                         .message = std::format("{}: error: out of memory", subject) };
    }
    catch (std::exception const& exception)
    {
        return Failure { .refusal = Refusal::InternalError,
                         .message = std::format("{}: error: internal error: {}", subject, exception.what()) };
    }
    catch (...)
    {
        return Failure { .refusal = Refusal::InternalError,
                         .message = std::format("{}: error: internal error: an unknown exception", subject) };
    }
}

} // namespace isoline::cli
