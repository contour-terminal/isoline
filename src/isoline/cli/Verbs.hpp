// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

#include <isoline/Features.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/cli/FileAccess.hpp>
#include <isoline/meta/EnumRows.hpp>
#include <isoline/text/TextLimits.hpp>

// The verbs of the isoline tool, as table rows: what each is called, what it does, and the function
// that does it. IsolineApp turns the rows into commands; adding a verb (P8's `aot` and `run`) is adding
// a row.
namespace isoline::cli
{

/// What a verb's run ends with: the process's exit status.
enum class ExitCode : uint8_t
{
    Success = 0,       ///< The verb did what it was asked.
    InputRejected = 1, ///< The input is malformed, invalid, or past a limit; nothing was written.
    UsageOrIo = 2,     ///< The command line is wrong, a file could not be read or written, or memory ran out.
    InternalError = 3, ///< The tool failed in a way it does not expect: an exception it has no row for.
};

/// Why a verb, or the command line naming it, failed: one row of Refusals each.
enum class Refusal : uint8_t
{
    CommandLine,         ///< The command line is wrong: an unknown verb or option, a missing operand.
    OutputIsInput,       ///< The output names the input, which writing it would destroy.
    BinaryToTerminal,    ///< A binary would be written to a terminal.
    Unreadable,          ///< The input could not be read.
    Unwritable,          ///< The output could not be written.
    OutOfMemory,         ///< The host ran out of memory (std::bad_alloc).
    InternalError,       ///< Any other exception.
    InputTooLarge,       ///< The input is larger than its limit.
    TextMalformed,       ///< The text does not read as a module, component text included.
    ModuleMalformed,     ///< The binary does not decode.
    ModuleInvalid,       ///< The module decodes but does not validate.
    ComponentBinary,     ///< The binary is a component, which needs the component model.
    TextTooLarge,        ///< The module's text would be larger than the text limit.
    PageSizeNotWritable, ///< A memory's page size is one the text format cannot say.
};

/// The last Refusal enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastRefusal = Refusal::PageSizeNotWritable;

/// One row of Refusals: a refusal, the exit code it ends the run with, and what it means.
struct RefusalRow
{
    Refusal refusal;          ///< The refusal, at its enumerator's index.
    ExitCode exitCode;        ///< What the run exits with.
    std::string_view meaning; ///< A sentence for the documentation.
};

/// Every refusal and its exit code. README.md's exit status table lists the same.
inline constexpr auto Refusals = std::to_array<RefusalRow>({
    { .refusal = Refusal::CommandLine,
      .exitCode = ExitCode::UsageOrIo,
      .meaning = "the command line is wrong" },
    { .refusal = Refusal::OutputIsInput,
      .exitCode = ExitCode::UsageOrIo,
      .meaning = "the output names the input" },
    { .refusal = Refusal::BinaryToTerminal,
      .exitCode = ExitCode::UsageOrIo,
      .meaning = "a binary would be written to a terminal" },
    { .refusal = Refusal::Unreadable,
      .exitCode = ExitCode::UsageOrIo,
      .meaning = "the input cannot be read" },
    { .refusal = Refusal::Unwritable,
      .exitCode = ExitCode::UsageOrIo,
      .meaning = "the output cannot be written" },
    { .refusal = Refusal::OutOfMemory,
      .exitCode = ExitCode::UsageOrIo,
      .meaning = "the host ran out of memory" },
    { .refusal = Refusal::InternalError,
      .exitCode = ExitCode::InternalError,
      .meaning = "the tool failed unexpectedly" },
    { .refusal = Refusal::InputTooLarge,
      .exitCode = ExitCode::InputRejected,
      .meaning = "the input is too large" },
    { .refusal = Refusal::TextMalformed,
      .exitCode = ExitCode::InputRejected,
      .meaning = "the text is malformed" },
    { .refusal = Refusal::ModuleMalformed,
      .exitCode = ExitCode::InputRejected,
      .meaning = "the binary is malformed" },
    { .refusal = Refusal::ModuleInvalid,
      .exitCode = ExitCode::InputRejected,
      .meaning = "the module is invalid" },
    { .refusal = Refusal::ComponentBinary,
      .exitCode = ExitCode::InputRejected,
      .meaning = "the binary is a component" },
    { .refusal = Refusal::TextTooLarge,
      .exitCode = ExitCode::InputRejected,
      .meaning = "the module's text would be too large" },
    { .refusal = Refusal::PageSizeNotWritable,
      .exitCode = ExitCode::InputRejected,
      .meaning = "a page size the text format cannot say" },
});

static_assert(meta::firstMisplacedRow<LastRefusal>(Refusals, &RefusalRow::refusal) == meta::NoMisplacedRow,
              "Refusals must hold one row per Refusal, at the index of its enumerator");

/// The exit code a refusal ends a run with.
/// @param refusal The refusal.
/// @return Its row's exit code.
[[nodiscard]] constexpr ExitCode exitCodeOf(Refusal refusal) noexcept
{
    return Refusals[static_cast<std::size_t>(refusal)].exitCode;
}

/// A verb that failed: why, and the diagnostic line that says so, in the form
/// `<file>[:<line>:<column>]: error[ at offset 0x<hex>]: <message>`.
struct Failure
{
    Refusal refusal;     ///< Why it failed.
    std::string message; ///< The diagnostic, without a line end.
};

/// The failure an exception in flight means: OutOfMemory for std::bad_alloc, else InternalError. Call
/// it in a catch handler only.
/// @param subject What the diagnostic names first: the input's path, or the program's name.
/// @return The failure.
[[nodiscard]] Failure failureOfCurrentException(std::string_view subject);

/// The failure an api::Engine refused a compiled module with. The binary has no place in the text,
/// so a validation error is placed by its function and instruction.
/// @param error Why it was refused.
/// @param input The text's path.
/// @return The failure.
[[nodiscard]] Failure loadFailure(api::LoadError const& error, std::string_view input);

/// The proposals a verb accepts, as `--features` names them.
enum class FeatureChoice : uint8_t
{
    All,   ///< Every proposal Isoline implements: FeatureSet::all().
    Wasm3, ///< WebAssembly 3.0: FeatureSet::wasm3().
};

/// The last FeatureChoice enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastFeatureChoice = FeatureChoice::Wasm3;

/// One row of FeatureChoices: a `--features` value and the proposals it names.
struct FeatureChoiceRow
{
    std::string_view name;                    ///< The value on the command line.
    FeatureChoice choice;                     ///< What it chooses, at its enumerator's index.
    FeatureSet features = FeatureSet::none(); ///< The proposals, closed over their prerequisites.
};

/// Every `--features` value. Adding a set is adding a row.
inline constexpr auto FeatureChoices = std::to_array<FeatureChoiceRow>({
    { .name = "all", .choice = FeatureChoice::All, .features = FeatureSet::all().withPrerequisites() },
    { .name = "wasm3", .choice = FeatureChoice::Wasm3, .features = FeatureSet::wasm3().withPrerequisites() },
});

static_assert(meta::firstMisplacedRow<LastFeatureChoice>(FeatureChoices, &FeatureChoiceRow::choice)
                  == meta::NoMisplacedRow,
              "FeatureChoices must hold one row per FeatureChoice, at the index of its enumerator");
static_assert(
    meta::firstRepeatedKey(FeatureChoices, &FeatureChoiceRow::name) == meta::NoRepeatedKey,
    "FeatureChoices must name each --features value once, as the parser takes the first row it names");

/// The proposals a FeatureChoice names.
/// @param choice The choice.
/// @return Its row's feature set, closed over its prerequisites.
[[nodiscard]] constexpr FeatureSet featuresOf(FeatureChoice choice) noexcept
{
    return FeatureChoices[static_cast<std::size_t>(choice)].features;
}

/// Everything a verb runs with but its operands.
struct VerbContext
{
    IFileAccess& files;                          ///< Where it reads and writes.
    std::ostream& diagnostics;                   ///< Where it reports a failure, one line each.
    FeatureChoice features = FeatureChoice::All; ///< The proposals the input may use.
    /// The limits of text: the largest source compile reads and the largest text disassemble writes
    /// (maxSourceBytes), and the largest module either reads or produces (maxModuleBytes).
    text::TextLimits limits;
};

/// Compiles a text module to a binary: reads it, compiles it, loads it into an api::Engine of the
/// chosen features (which decodes and validates it, spec §13's thin client), and writes it.
///
/// A text error is reported at its place, `<input>:<line>:<column>: error: <message>`. A decode or
/// validation error has no place in the text (the compiled binary carries no source map), so it is
/// reported as `<input>: error: function <N>, instruction <M>: <message>` within a body, else
/// `<input>: error: <message>`.
/// @param context The files, the diagnostics, the features and the limits.
/// @param input The text's path (StandardStreamPath: standard input); at most limits.maxSourceBytes.
/// @param output The binary's path (StandardStreamPath: standard output), written only on success.
/// @return Success, or the exit code of the refusal reported.
[[nodiscard]] ExitCode compileVerb(VerbContext const& context,
                                   std::string_view input,
                                   std::string_view output);

/// Disassembles a binary to text: reads it, picks its BinaryFormats row by its preamble, decodes it,
/// and writes its text.
///
/// The binary is decoded with binary::decodeModule, not loaded by an api::Engine: it is decoded and not
/// validated, as the writer prints any module that decodes, valid or not. A decode error is reported
/// as `<input>: error at offset 0x<hex>: <message>`.
/// @param context The files, the diagnostics, the features and the limits.
/// @param input The binary's path (StandardStreamPath: standard input); at most limits.maxModuleBytes.
/// @param output The text's path (StandardStreamPath: standard output), written only on success.
/// @return Success, or the exit code of the refusal reported.
[[nodiscard]] ExitCode disassembleVerb(VerbContext const& context,
                                       std::string_view input,
                                       std::string_view output);

/// What runs a verb: its context, its input path and its output path.
using VerbFunction = ExitCode (*)(VerbContext const&, std::string_view, std::string_view);

/// One verb of the tool.
struct VerbRow
{
    std::string_view name; ///< What the command line calls it.
    std::string_view help; ///< One line for the usage text.
    VerbFunction run;      ///< What runs it.
};

/// Every verb of the tool. Adding one is adding a row.
inline constexpr auto Verbs = std::to_array<VerbRow>({
    { .name = "compile", .help = "Compiles a text module (.wat) to a binary (.wasm).", .run = &compileVerb },
    { .name = "disassemble", .help = "Writes a binary (.wasm) as text (.wat).", .run = &disassembleVerb },
});

static_assert(meta::firstRepeatedKey(Verbs, &VerbRow::name) == meta::NoRepeatedKey,
              "Verbs must name each verb once, as the command line runs the first row it names");

/// The magic number and version a binary starts with.
using Preamble = std::array<std::byte, 8>;

/// What disassembles one kind of binary.
using Disassembler = std::expected<std::string, Failure> (*)(std::span<std::byte const> bytes,
                                                             VerbContext const& context,
                                                             std::string_view input);

/// One kind of binary disassemble reads: its preamble and what disassembles it.
struct BinaryFormat
{
    Preamble preamble;        ///< The first eight bytes of such a binary.
    Disassembler disassemble; ///< What disassembles it.
};

/// Disassembles a core module: decodes it with the chosen features and writes it with a ModuleWriter.
/// @param bytes The binary.
/// @param context The features and limits.
/// @param input The binary's path, for diagnostics.
/// @return The text, or the failure: a decode error, or a module the writer refuses.
[[nodiscard]] std::expected<std::string, Failure> disassembleCoreModule(std::span<std::byte const> bytes,
                                                                        VerbContext const& context,
                                                                        std::string_view input);

/// Refuses a component: its text needs the component model, which P11 adds by replacing this row.
/// @param bytes The binary; unused.
/// @param context Unused.
/// @param input The binary's path, for the diagnostic.
/// @return ComponentBinary.
[[nodiscard]] std::expected<std::string, Failure> refuseComponentBinary(std::span<std::byte const> bytes,
                                                                        VerbContext const& context,
                                                                        std::string_view input);

/// Every kind of binary, by preamble. A binary that starts with none of them is read as a core module,
/// whose decoder reports what is wrong with its preamble.
inline constexpr auto BinaryFormats = std::to_array<BinaryFormat>({
    { .preamble = { std::byte { 0x00 },
                    std::byte { 0x61 },
                    std::byte { 0x73 },
                    std::byte { 0x6d },
                    std::byte { 0x01 },
                    std::byte { 0x00 },
                    std::byte { 0x00 },
                    std::byte { 0x00 } },
      .disassemble = &disassembleCoreModule },
    { .preamble = { std::byte { 0x00 },
                    std::byte { 0x61 },
                    std::byte { 0x73 },
                    std::byte { 0x6d },
                    std::byte { 0x0d },
                    std::byte { 0x00 },
                    std::byte { 0x01 },
                    std::byte { 0x00 } },
      .disassemble = &refuseComponentBinary },
});

static_assert(meta::firstRepeatedKey(BinaryFormats, &BinaryFormat::preamble) == meta::NoRepeatedKey,
              "BinaryFormats must hold each preamble once, as disassemble takes the first row it matches");

} // namespace isoline::cli
