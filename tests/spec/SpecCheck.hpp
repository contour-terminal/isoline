// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "IsotestReader.hpp"
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
/// applies each lane's pattern to that lane of a v128, Either matches when one alternative does, and
/// AnyNullRef matches the null reference of any heap.
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

/// Checks a failure assertion: the command must have failed, at @p stage, with a message that
/// starts with @p expectedMessage (the spec's rule, so an empty expected message matches any).
/// @param stage The stage the failure must arise at.
/// @param expectedMessage The message the actual one must start with.
/// @param actual The failure, or nullopt when the command succeeded.
/// @return Nothing, or why the assertion does not hold; a Harness failure never satisfies it.
[[nodiscard]] CheckResult checkFailure(Stage stage,
                                       std::string_view expectedMessage,
                                       std::optional<Failure> const& actual);

/// Spells @p value as an .isotest token does.
/// @param value The value.
/// @return Its token.
[[nodiscard]] std::string describe(runtime::Value const& value);

/// Spells @p values as .isotest tokens, separated by spaces, or "(none)".
/// @param values The values.
/// @return Their tokens.
[[nodiscard]] std::string describeAll(std::span<runtime::Value const> values);

/// @copydoc describeAll(std::span<runtime::Value const>)
[[nodiscard]] std::string describeAll(std::span<ExpectedValue const> values);

} // namespace isoline::spectest
