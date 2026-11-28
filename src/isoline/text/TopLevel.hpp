// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <isoline/meta/EnumRows.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

/// Compiles a `(module …)` node to a binary module: ModuleReader::read, then binary::encodeModule.
/// @param tree The tree; its source must outlive the call.
/// @param form The `(module …)` list.
/// @param limits The caps on reading text.
/// @return The module's bytes, or the first error.
[[nodiscard]] std::expected<std::vector<std::byte>, TextError> compileCoreModule(SExpressionTree const& tree,
                                                                                 NodeId form,
                                                                                 TextLimits const& limits);

/// Refuses a `(component …)` node: component text needs the component model, which isoline_text does not
/// link; component::text::Forms reads it instead, a table of its own (D13).
/// @param tree The tree.
/// @param form The `(component …)` list.
/// @param limits The caps on reading text; unused.
/// @return UnsupportedComponent, at @p form.
[[nodiscard]] std::expected<std::vector<std::byte>, TextError> refuseComponent(SExpressionTree const& tree,
                                                                               NodeId form,
                                                                               TextLimits const& limits);

/// What compiles one kind of top-level form.
using TopLevelReader = std::expected<std::vector<std::byte>, TextError> (*)(SExpressionTree const&,
                                                                            NodeId,
                                                                            TextLimits const&);

/// One kind of form a whole text may hold: its keyword and what compiles it.
struct TopLevelForm
{
    std::string_view keyword; ///< The keyword the form's list starts with.
    TopLevelReader read;      ///< What compiles it.
};

/// The forms a core-only text may hold: a module, and a component refused. Adding a form is adding a
/// row; component::text::Forms is this table with the component row read.
inline constexpr auto CoreForms = std::to_array<TopLevelForm>({
    { .keyword = "module", .read = &compileCoreModule },
    { .keyword = "component", .read = &refuseComponent },
});

static_assert(meta::firstRepeatedKey(CoreForms, &TopLevelForm::keyword) == meta::NoRepeatedKey,
              "CoreForms must hold each keyword once, as compileText() takes the first row it names");

/// Compiles a whole text to a binary.
///
/// When the text's first node (annotations aside) is a list whose keyword is a row of @p forms, it must
/// be the text's only node, and its row compiles it. Otherwise the text is a bare sequence of module
/// fields, an inline module (spec §6.6.13), which an empty text is too.
/// @param source The text.
/// @param limits The caps on reading text.
/// @param forms The forms the text may hold: CoreForms by default, so a core-only consumer names no
///              table; component::text::Forms for components (D13).
/// @return The binary, or the first error.
[[nodiscard]] std::expected<std::vector<std::byte>, TextError> compileText(
    std::string_view source, TextLimits const& limits, std::span<TopLevelForm const> forms = CoreForms);

} // namespace isoline::text
