// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Script.hpp"
#include <isoline/runtime/Value.hpp>

namespace isoline::spectest
{

/// Where a failure arose: an assertion expects one of these, and its message.
enum class Stage : uint8_t
{
    Harness,     ///< The script or its files are broken; never what an assertion expects.
    Decode,      ///< The decoder rejected the module.
    Validate,    ///< The validator rejected the module.
    Link,        ///< An import did not bind.
    Instantiate, ///< Instantiation trapped or could not allocate.
    Execute,     ///< An invocation trapped.
    Exception,   ///< An invocation threw an exception out of the function.
    Custom,      ///< A custom section was invalid; the module itself loaded.
    /// A module's text held a malformed custom annotation, as `assert_malformed_custom` expects: the
    /// reference interpreter tells these apart from every other malformed text (Decode).
    MalformedCustom,
};

/// The name of @p stage, for failure reports.
/// @param stage The stage.
/// @return Its lower-case name.
[[nodiscard]] std::string_view stageName(Stage stage);

/// A failure: where it arose, and the spec message for it (or, for Harness, what went wrong).
struct Failure
{
    Stage stage = Stage::Harness; ///< Where it arose.
    std::string message;          ///< Its message.
};

/// A check's verdict: nothing when it holds, else why not.
using CheckResult = std::expected<void, std::string>;

/// Whether @p actual is what @p expected allows.
///
/// Exact compares bits, so a NaN must match its payload; a reference compares by kind and address,
/// a null one by the heap it is the null of. CanonicalNan matches only the canonical NaN, and
/// ArithmeticNan any NaN with the quiet bit set, each of either sign and of its width only. Lanes
/// applies each lane's pattern to that lane of a v128, Either matches when one alternative does,
/// AnyNullRef matches the null reference of any heap, and AnyFunctionRef, AnyExternRef and
/// AnyExceptionRef any non-null reference of their kind.
/// @param expected The expected value or pattern.
/// @param actual The value produced.
/// @return Whether they match.
[[nodiscard]] bool matches(ExpectedValue const& expected, runtime::Value const& actual);

/// Checks an assert_return's results: as many as expected, each matching.
/// @param expected The expected values.
/// @param actual The results produced.
/// @return Nothing, or a description of the mismatch.
[[nodiscard]] CheckResult checkResults(std::span<ExpectedValue const> expected,
                                       std::span<runtime::Value const> actual);

/// How an assertion's expected message must appear in the actual one.
enum class MessageRule : uint8_t
{
    Prefix,    ///< It starts the actual message: the reference interpreter's rule, the core suites'.
    Substring, ///< It occurs anywhere in it: Wasmtime's rule, the component suites' (D12).
};

/// One way a suite words a failure that Isoline words otherwise: a failure at @p stage whose message the
/// pattern finds is also known to the suite as the wording, the pattern's groups substituted (`$1`, and
/// `$$` for a `$`). The suite's assertion may hold either text.
struct WordingRow
{
    Stage stage;              ///< The stage of the failures it words.
    std::string_view pattern; ///< An ECMAScript regular expression the message must hold a match of.
    std::string_view wording; ///< The suite's text, a std::regex format string.
};

/// The suite's texts for @p failure: the wording of every row of @p wordings its stage and message fit.
/// @param failure A failure.
/// @param wordings A suite's wordings.
/// @return The texts, in row order; empty when no row fits.
[[nodiscard]] std::vector<std::string> suiteWordingsOf(Failure const& failure,
                                                       std::span<WordingRow const> wordings);

/// Checks a failure assertion as checkFailure(Stage, std::string_view, std::optional<Failure> const&,
/// MessageRule) does, where the expected message may instead be in one of the suite's wordings of the
/// failure (suiteWordingsOf()).
/// @param stage The stage the failure must arise at.
/// @param expectedMessage The message the actual one, or one of its wordings, must hold.
/// @param actual The failure, or nullopt when the command succeeded.
/// @param rule Where in the actual message, or a wording, the expected one must be.
/// @param wordings The suite's wordings.
/// @return Nothing, or why the assertion does not hold; a Harness failure never satisfies it.
[[nodiscard]] CheckResult checkFailure(Stage stage,
                                       std::string_view expectedMessage,
                                       std::optional<Failure> const& actual,
                                       MessageRule rule,
                                       std::span<WordingRow const> wordings);

/// Checks a failure assertion: the command must have failed, at @p stage, with a message @p rule finds
/// @p expectedMessage in (an empty expected message matches any).
/// @param stage The stage the failure must arise at.
/// @param expectedMessage The message the actual one must hold.
/// @param actual The failure, or nullopt when the command succeeded.
/// @param rule Where in the actual message the expected one must be.
/// @return Nothing, or why the assertion does not hold; a Harness failure never satisfies it.
[[nodiscard]] CheckResult checkFailure(Stage stage,
                                       std::string_view expectedMessage,
                                       std::optional<Failure> const& actual,
                                       MessageRule rule);

/// Checks a failure assertion: the command must have failed, at @p stage, with a message that
/// starts with @p expectedMessage (the spec's rule, so an empty expected message matches any).
/// @param stage The stage the failure must arise at.
/// @param expectedMessage The message the actual one must start with.
/// @param actual The failure, or nullopt when the command succeeded.
/// @return Nothing, or why the assertion does not hold; a Harness failure never satisfies it.
[[nodiscard]] CheckResult checkFailure(Stage stage,
                                       std::string_view expectedMessage,
                                       std::optional<Failure> const& actual);

/// Spells @p value as the runner's diagnostics do, as `i32:3` or `ref.null:func`.
/// @param value The value.
/// @return Its token.
[[nodiscard]] std::string describe(runtime::Value const& value);

/// Spells @p values as describe() does, separated by spaces, or "(none)".
/// @param values The values.
/// @return Their tokens.
[[nodiscard]] std::string describeAll(std::span<runtime::Value const> values);

/// @copydoc describeAll(std::span<runtime::Value const>)
[[nodiscard]] std::string describeAll(std::span<ExpectedValue const> values);

/// What a skip reason's form takes between its fixed texts.
enum class SkipArgument : uint8_t
{
    None, ///< Nothing: the reason is the prefix alone.
    Gate, ///< A gated feature's emoji, a ComponentFeatureTable row's.
    Name, ///< A name: one word of no spaces.
    Line, ///< A line number: decimal digits.
};

/// What an oracle entry giving a reason records.
enum class ReasonUse : uint8_t
{
    Skip,   ///< A skip: the command is not run.
    Parity, ///< A parity divergence: the command runs, and its encoding differs from the oracle's there.
};

/// One form a component suite's oracle may give as the reason of a skip or of a parity divergence (D12):
/// its fixed text, what follows it, what closes it, and which entry gives it.
struct SkipReasonRow
{
    std::string_view prefix;  ///< The text it starts with.
    SkipArgument argument;    ///< What follows the prefix.
    std::string_view suffix;  ///< The text it ends with, after the argument.
    ReasonUse use;            ///< The entry that gives it.
    std::string_view meaning; ///< Why a command is skipped, or its encoding diverges, for it.
};

/// Every reason a component suite's oracle may skip a command for, or record its parity divergence for.
/// Anything else is refused: an entry without one of these reasons, of its use, fails its file. Adding a
/// reason is adding a row.
inline constexpr auto SkipReasons = std::to_array<SkipReasonRow>({
    { .prefix = "async (P12)",
      .argument = SkipArgument::None,
      .suffix = "",
      .use = ReasonUse::Skip,
      .meaning = "it runs async code, which P12 implements" },
    { .prefix = "gated ",
      .argument = SkipArgument::Gate,
      .suffix = "",
      .use = ReasonUse::Skip,
      .meaning = "it needs a gated feature Isoline refuses" },
    { .prefix = "upstream nyi.txt",
      .argument = SkipArgument::None,
      .suffix = "",
      .use = ReasonUse::Skip,
      .meaning = "the suite's own nyi.txt lists it as not yet implemented" },
    { .prefix = "core feature cut ",
      .argument = SkipArgument::Name,
      .suffix = "",
      .use = ReasonUse::Skip,
      .meaning = "it needs a core proposal Isoline does not implement" },
    { .prefix = "legacy text indices",
      .argument = SkipArgument::None,
      .suffix = "",
      .use = ReasonUse::Skip,
      .meaning = "its text uses index forms the pinned text format no longer reads" },
    { .prefix = "oracle unavailable since ",
      .argument = SkipArgument::Name,
      .suffix = "",
      .use = ReasonUse::Skip,
      .meaning =
          "the pinned wasm-tools fails the directive, so it is no oracle of it; the name is the suite's "
          "commit that introduced the expectation" },
    { .prefix = "wasmtime restriction (restrictions.wast:",
      .argument = SkipArgument::Line,
      .suffix = ")",
      .use = ReasonUse::Skip,
      .meaning =
          "Wasmtime refuses what the component model admits, as its restrictions.wast asserts at that line" },
    { .prefix = "diverges from CM pin (definitions.py:",
      .argument = SkipArgument::Line,
      .suffix = ")",
      .use = ReasonUse::Skip,
      .meaning = "the suite asserts what the pinned canonical ABI definitions do otherwise" },
    { .prefix = "diverges from CM pin (Binary.md:",
      .argument = SkipArgument::Line,
      .suffix = ")",
      .use = ReasonUse::Parity,
      .meaning = "the pinned wasm-tools encodes what the pinned Binary.md encodes otherwise" },
});

static_assert(std::ranges::all_of(SkipReasons,
                                  [](SkipReasonRow const& row) {
                                      return !row.prefix.empty() && !row.meaning.empty()
                                             && (row.argument != SkipArgument::None || row.suffix.empty());
                                  }),
              "every skip reason has a text and a meaning, and only a reason with an argument a suffix");

/// The row of SkipReasons a skip's or a parity divergence's reason is of.
/// @param reason The reason an oracle gives.
/// @param use The entry that gives it.
/// @return Its row, or nullptr when it is of none of @p use.
[[nodiscard]] SkipReasonRow const* reasonRowOf(std::string_view reason, ReasonUse use) noexcept;

} // namespace isoline::spectest
