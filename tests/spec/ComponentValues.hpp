// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

#include "Script.hpp"
#include "SpecCheck.hpp"
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextLimits.hpp>

// The component value forms of a `.wast` script (wast 263.0.0's src/component/wast.rs), read into an
// untyped ComponentLiteral and converted to canonical::Values against the types a function declares.
namespace isoline::spectest
{

/// What a component value form holds after its keyword.
enum class LiteralSyntax : uint8_t
{
    Bool,          ///< `true` or `false`.
    Signed,        ///< A signed integer of the kind's width.
    Unsigned,      ///< An unsigned integer of the kind's width.
    Float,         ///< A float of the kind's width: `nan`, `inf` and hexadecimal floats included.
    Char,          ///< One string of one Unicode scalar value.
    String,        ///< One string.
    Values,        ///< Any number of values: a list's or a tuple's elements.
    Fields,        ///< `(field "label" <value>)` per field.
    Case,          ///< A case's label, then at most one value: its payload.
    Label,         ///< One label.
    Labels,        ///< Any number of labels: the flags set.
    Entries,       ///< `(entry <key> <value>)` per entry.
    NoValue,       ///< Nothing.
    OneValue,      ///< Exactly one value.
    OptionalValue, ///< At most one value.
};

/// One component value form: its keyword, the kind of value it builds, and what it holds.
struct ComponentValueForm
{
    std::string_view keyword;         ///< The list's keyword.
    component::types::ValueKind kind; ///< The kind it builds; a `list.const` also builds a fixed list.
    uint32_t caseIndex;               ///< For an option's or a result's case form, the case; else 0.
    LiteralSyntax syntax;             ///< What follows the keyword.
};

/// Every component value form a script may give (D12). Adding one is adding a row.
inline constexpr auto ComponentValueForms = std::to_array<ComponentValueForm>({
    // clang-format off
    { .keyword = "bool.const", .kind = component::types::ValueKind::Bool, .caseIndex = 0, .syntax = LiteralSyntax::Bool },
    { .keyword = "s8.const", .kind = component::types::ValueKind::S8, .caseIndex = 0, .syntax = LiteralSyntax::Signed },
    { .keyword = "u8.const", .kind = component::types::ValueKind::U8, .caseIndex = 0, .syntax = LiteralSyntax::Unsigned },
    { .keyword = "s16.const", .kind = component::types::ValueKind::S16, .caseIndex = 0, .syntax = LiteralSyntax::Signed },
    { .keyword = "u16.const", .kind = component::types::ValueKind::U16, .caseIndex = 0, .syntax = LiteralSyntax::Unsigned },
    { .keyword = "s32.const", .kind = component::types::ValueKind::S32, .caseIndex = 0, .syntax = LiteralSyntax::Signed },
    { .keyword = "u32.const", .kind = component::types::ValueKind::U32, .caseIndex = 0, .syntax = LiteralSyntax::Unsigned },
    { .keyword = "s64.const", .kind = component::types::ValueKind::S64, .caseIndex = 0, .syntax = LiteralSyntax::Signed },
    { .keyword = "u64.const", .kind = component::types::ValueKind::U64, .caseIndex = 0, .syntax = LiteralSyntax::Unsigned },
    { .keyword = "f32.const", .kind = component::types::ValueKind::F32, .caseIndex = 0, .syntax = LiteralSyntax::Float },
    { .keyword = "f64.const", .kind = component::types::ValueKind::F64, .caseIndex = 0, .syntax = LiteralSyntax::Float },
    { .keyword = "char.const", .kind = component::types::ValueKind::Char, .caseIndex = 0, .syntax = LiteralSyntax::Char },
    { .keyword = "str.const", .kind = component::types::ValueKind::String, .caseIndex = 0, .syntax = LiteralSyntax::String },
    { .keyword = "list.const", .kind = component::types::ValueKind::List, .caseIndex = 0, .syntax = LiteralSyntax::Values },
    { .keyword = "record.const", .kind = component::types::ValueKind::Record, .caseIndex = 0, .syntax = LiteralSyntax::Fields },
    { .keyword = "tuple.const", .kind = component::types::ValueKind::Tuple, .caseIndex = 0, .syntax = LiteralSyntax::Values },
    { .keyword = "variant.const", .kind = component::types::ValueKind::Variant, .caseIndex = 0, .syntax = LiteralSyntax::Case },
    { .keyword = "enum.const", .kind = component::types::ValueKind::Enum, .caseIndex = 0, .syntax = LiteralSyntax::Label },
    { .keyword = "option.none", .kind = component::types::ValueKind::Option, .caseIndex = 0, .syntax = LiteralSyntax::NoValue },
    { .keyword = "option.some", .kind = component::types::ValueKind::Option, .caseIndex = 1, .syntax = LiteralSyntax::OneValue },
    { .keyword = "result.ok", .kind = component::types::ValueKind::Result, .caseIndex = 0, .syntax = LiteralSyntax::OptionalValue },
    { .keyword = "result.err", .kind = component::types::ValueKind::Result, .caseIndex = 1, .syntax = LiteralSyntax::OptionalValue },
    { .keyword = "flags.const", .kind = component::types::ValueKind::Flags, .caseIndex = 0, .syntax = LiteralSyntax::Labels },
    { .keyword = "map.const", .kind = component::types::ValueKind::Map, .caseIndex = 0, .syntax = LiteralSyntax::Entries },
    // clang-format on
});

static_assert(ComponentValueForms.size() <= 256,
              "LiteralNode::form indexes ComponentValueForms in a uint8_t");

static_assert(
    [] {
        // Every form is the keyword canonical::toText() prints for its kind (or its case), so a value
        // printed in a diagnostic reads back as the same form.
        for (auto const& row: ComponentValueForms)
        {
            auto const& shape = component::canonical::valueShapeRow(row.kind);
            auto const caseForm =
                std::ranges::find_if(component::canonical::CaseForms, [&](auto const& candidate) {
                    return candidate.kind == row.kind && candidate.caseIndex == row.caseIndex;
                });
            auto printed = shape.form;
            if (printed.empty() && caseForm != component::canonical::CaseForms.end())
                printed = caseForm->form;
            if (printed != row.keyword)
                return false;
        }
        return true;
    }(),
    "every ComponentValueForms keyword is the one canonical::toText() prints for its kind");

/// The row of a literal node's form.
/// @param node A node.
/// @return Its ComponentValueForms row.
[[nodiscard]] constexpr ComponentValueForm const& componentValueForm(LiteralNode const& node) noexcept
{
    return ComponentValueForms[node.form];
}

/// Whether @p keyword is a component value form's.
/// @param keyword A list's keyword.
/// @return true when a ComponentValueForms row has it.
[[nodiscard]] constexpr bool isComponentValueKeyword(std::string_view keyword) noexcept
{
    return std::ranges::find(ComponentValueForms, keyword, &ComponentValueForm::keyword)
           != ComponentValueForms.end();
}

/// Reads a component value: the list @p list of @p tree, whose keyword is a ComponentValueForms row's.
/// Iterative, whatever the nesting.
/// @param tree The script's tree.
/// @param list The value's list.
/// @param limits The caps on reading text (a number's length).
/// @return The literal, its line @p list's; or the first error, at its line.
[[nodiscard]] std::expected<ComponentLiteral, ParseError> readComponentLiteral(
    text::SExpressionTree const& tree, text::NodeId list, text::TextLimits const& limits);

/// Builds one value per root type from @p literals, in order, through @p builder: each literal must be a
/// value of its root's type (its form that of the type's kind, its labels the type's, its numbers in
/// range). Iterative, whatever the nesting.
/// @param literals The literals, one per root.
/// @param arena The arena of @p roots.
/// @param roots The types, the builder's roots.
/// @param builder A builder of @p roots in @p arena, nothing built yet.
/// @return Nothing; or a Harness failure naming the literal's line and why it is no such value.
[[nodiscard]] std::expected<void, Failure> buildComponentValues(
    std::span<ComponentLiteral const> literals,
    component::types::TypeArena const& arena,
    std::span<component::types::TypeId const> roots,
    component::canonical::ValueBuilder& builder);

/// The values of @p literals, as buildComponentValues() builds them, charged to @p budget.
/// @param literals The literals, one per root.
/// @param arena The arena of @p roots.
/// @param roots The types.
/// @param budget What their storage is charged to.
/// @return The values; or a Harness failure naming the literal's line.
[[nodiscard]] std::expected<component::canonical::Values, Failure> componentValuesOf(
    std::span<ComponentLiteral const> literals,
    component::types::TypeArena const& arena,
    std::span<component::types::TypeId const> roots,
    std::shared_ptr<component::canonical::LiveBytesBudget> budget);

} // namespace isoline::spectest
