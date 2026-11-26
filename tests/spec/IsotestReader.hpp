// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/runtime/Value.hpp>

namespace isoline::spectest
{

/// What one line of an .isotest script asks for.
enum class CommandKind : uint8_t
{
    Module,               ///< Instantiate a module and make it the current one.
    Register,             ///< Make a module's exports importable under a name.
    Invoke,               ///< Perform an action; its results are not checked.
    AssertReturn,         ///< An action must return the expected values.
    AssertTrap,           ///< An invocation must trap with the expected message.
    AssertTrapModule,     ///< Instantiating a module must trap with the expected message.
    AssertExhaustion,     ///< An invocation must exhaust the call stack.
    AssertInvalid,        ///< A module must decode and then fail validation.
    AssertMalformed,      ///< A module must fail to decode.
    AssertUnlinkable,     ///< A module must fail to link its imports.
    AssertUninstantiable, ///< A module must trap while it is instantiated.
    Skip,                 ///< A command the script cannot express (a text-only module); counted, not run.
};

/// What an action does with an export.
enum class ActionKind : uint8_t
{
    Invoke, ///< Call an exported function.
    Get,    ///< Read an exported global.
};

/// An invocation of an exported function, or a read of an exported global.
struct Action
{
    ActionKind kind = ActionKind::Invoke; ///< Invoke or get.
    std::string field;                    ///< The export's name.
    std::optional<std::string> module;    ///< The named module it belongs to; the current one if empty.
    std::vector<runtime::Value> args;     ///< Invoke: the arguments.
};

/// A value an assertion expects: exactly a value, or one of the patterns the spec suite allows.
struct ExpectedValue
{
    /// How the actual value is compared.
    enum class Pattern : uint8_t
    {
        Exact,           ///< Equal to value, bit for bit; a reference by kind, heap and address.
        CanonicalNan32,  ///< An f32 canonical NaN, of either sign.
        ArithmeticNan32, ///< An f32 NaN with the quiet bit set, of either sign.
        CanonicalNan64,  ///< An f64 canonical NaN, of either sign.
        ArithmeticNan64, ///< An f64 NaN with the quiet bit set, of either sign.
        AnyFunctionRef,  ///< Any non-null funcref.
    };

    Pattern pattern = Pattern::Exact; ///< How to compare.
    runtime::Value value;             ///< Exact: the value expected.

    bool operator==(ExpectedValue const&) const = default;
};

/// One command of an .isotest script.
struct Command
{
    CommandKind kind = CommandKind::Skip; ///< What it asks for.
    std::size_t line = 0;                 ///< Its 1-based line in the script.

    /// Module and the module assertions: the .wasm file, relative to the script's directory.
    std::string file;

    /// Module: its name, if named. Register: the module to register; the current one if empty.
    std::optional<std::string> name;

    /// Register: the name to register under. The assertions with a message: the message the actual
    /// one must start with. Skip: why it is skipped.
    std::string text;

    Action action;                       ///< Invoke and the action assertions: the action.
    std::vector<ExpectedValue> expected; ///< AssertReturn: the values expected.
};

/// Why a script could not be parsed.
struct ParseError
{
    std::size_t line;    ///< The 1-based line it is on.
    std::string message; ///< What is wrong with it.

    bool operator==(ParseError const&) const = default;
};

/// Parses an .isotest script: one command per line, tokens separated by exactly one space.
///
/// Names and messages are hex-encoded UTF-8; an empty name is the empty token, which two adjacent
/// spaces delimit. Values are `i32:`, `i64:`, `f32:` and `f64:` followed by their bits in hex, the
/// NaN patterns `f32:nan:canonical` and friends, `ref.null:func`, `ref.null:extern`,
/// `ref.extern:<n>`, `ref.func` (any non-null funcref, expected values only) and `ref.func:<n>`.
/// Empty lines are ignored.
/// @param script The script's text.
/// @return Its commands in order, or the first line that does not parse.
[[nodiscard]] std::expected<std::vector<Command>, ParseError> parseIsotest(std::string_view script);

} // namespace isoline::spectest
