// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/runtime/Value.hpp>
#include <isoline/text/TextError.hpp>

namespace isoline::spectest
{

/// What one command of a script asks for.
enum class CommandKind : uint8_t
{
    Module,               ///< Instantiate a module and make it the current one.
    ModuleDefinition,     ///< Decode and validate a module, and name it for ModuleInstance.
    ModuleInstance,       ///< Instantiate a named definition and make it the current module.
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
    AssertException,      ///< An invocation must throw an exception out of the function.
    AssertInvalidCustom,  ///< A module must load, and one of its custom sections be invalid.
    /// A module's text must fail to compile with a malformed custom annotation (a `.wast` script's
    /// `assert_malformed_custom`).
    AssertMalformedCustom,
    /// A command its oracle names a skip for, with its reason: a module the GC cut removes, or a command
    /// using a module, definition, instance or registration a skip poisoned. Counted against the pinned
    /// count, not run.
    Skip,
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

/// How a v128 of Pattern::Lanes is split into the lanes its patterns apply to.
enum class LaneShape : uint8_t
{
    F32x4, ///< Four f32 lanes, lane 0 the lowest 32 bits.
    F64x2, ///< Two f64 lanes, lane 0 the lowest 64 bits.
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
        AnyExternRef,    ///< Any non-null externref.
        AnyExceptionRef, ///< Any non-null exnref.
        AnyNullRef,      ///< The null reference of any heap.
        Lanes,           ///< A v128 whose float lanes each match their own pattern of lanes.
        Either,          ///< Any one of alternatives.
    };

    Pattern pattern = Pattern::Exact; ///< How to compare.
    /// Exact: the value expected. Lanes: a V128 holding the bits of each Exact lane, zero elsewhere.
    runtime::Value value;
    /// Lanes: each lane's pattern, Exact or a NaN pattern of the shape's width (F64x2 uses two).
    std::array<Pattern, 4> lanes {};
    LaneShape laneShape = LaneShape::F32x4;     ///< Lanes: how the v128 is split.
    std::vector<ExpectedValue> alternatives {}; ///< Either: the values any one of which matches.

    /// Member-wise equality. Defaulted out of line: inlined, its recursion through alternatives
    /// reads to GCC's optimizer as a potential null dereference of an empty vector's data.
    /// @param other The value to compare with.
    /// @return Whether every member is equal.
    [[nodiscard]] bool operator==(ExpectedValue const& other) const;
};

/// The facts of one LaneShape: how its lane pattern token is spelled and what its lanes are.
struct LaneFormat
{
    std::string_view prefix;           ///< The token's prefix, `v128:<shape>:`.
    LaneShape shape;                   ///< The shape it describes.
    std::size_t count;                 ///< How many lanes the token lists.
    unsigned width;                    ///< The bits of each lane.
    ExpectedValue::Pattern canonical;  ///< What `nan:canonical` means in a lane.
    ExpectedValue::Pattern arithmetic; ///< What `nan:arithmetic` means in a lane.
};

/// Every LaneShape's facts, indexed by the shape: the reader parses lane tokens by them, and the
/// checker splits and spells the lanes by them.
constexpr auto LaneFormats = std::to_array<LaneFormat>({
    { .prefix = "v128:f32x4:",
      .shape = LaneShape::F32x4,
      .count = 4,
      .width = 32,
      .canonical = ExpectedValue::Pattern::CanonicalNan32,
      .arithmetic = ExpectedValue::Pattern::ArithmeticNan32 },
    { .prefix = "v128:f64x2:",
      .shape = LaneShape::F64x2,
      .count = 2,
      .width = 64,
      .canonical = ExpectedValue::Pattern::CanonicalNan64,
      .arithmetic = ExpectedValue::Pattern::ArithmeticNan64 },
});

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, LaneFormats.size()),
                                  [](std::size_t index) {
                                      return static_cast<std::size_t>(LaneFormats.at(index).shape) == index;
                                  }),
              "LaneFormats is indexed by LaneShape");

/// The facts of @p shape.
/// @param shape A lane shape.
/// @return Its row of LaneFormats.
[[nodiscard]] constexpr LaneFormat const& laneFormat(LaneShape shape)
{
    return LaneFormats.at(static_cast<std::size_t>(shape));
}

/// How a command gives its module.
enum class ModuleForm : uint8_t
{
    Text,   ///< A `.wast` script's `(module …)`, in the text format.
    Binary, ///< A `.wast` script's `(module binary "…")`: the bytes of its strings.
    Quote,  ///< A `.wast` script's `(module quote "…")`: the text of its strings.
};

/// The module a command loads, in the form the script gives it.
struct ModuleSource
{
    ModuleForm form = ModuleForm::Text; ///< How the module is given.
    /// Text: the verbatim `(module …)` source. Binary: the module's bytes. Quote: its strings,
    /// concatenated.
    std::string text;
    /// Text and Quote: where the module's `(` is in the script. A text error's position in the module's
    /// text is shifted by it for Text, whose text is the script's own; Binary: unused.
    text::SourcePosition position {};
};

/// One command of a script.
struct Command
{
    CommandKind kind = CommandKind::Skip; ///< What it asks for.
    /// Its 1-based line in the script, that of its `(`. For diagnostics only.
    std::size_t line = 0;

    /// Module, ModuleDefinition and the module assertions: the module.
    ModuleSource source;

    /// Module, ModuleDefinition and ModuleInstance: the name it is given, if any. Register: the
    /// module to register; the current one if empty.
    std::optional<std::string> name;

    /// ModuleInstance: the name of the ModuleDefinition to instantiate.
    std::string definition;

    /// Register: the name to register under. The assertions with a message: the message the actual
    /// one must start with. Skip: why it is skipped.
    std::string text;

    Action action;                       ///< Invoke, AssertException and the action assertions: the action.
    std::vector<ExpectedValue> expected; ///< AssertReturn: the values expected.
};

/// Why a script could not be parsed.
struct ParseError
{
    std::size_t line;    ///< The 1-based line it is on.
    std::string message; ///< What is wrong with it.

    bool operator==(ParseError const&) const = default;
};

} // namespace isoline::spectest
