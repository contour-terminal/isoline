// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <string_view>
#include <vector>

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

/// Refuses a `(component …)` node: component text needs the component model, which P11 adds by
/// replacing this function's TopLevelForms row.
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

/// Every kind of form a whole text may hold. Adding one is adding a row.
inline constexpr auto TopLevelForms = std::to_array<TopLevelForm>({
    { .keyword = "module", .read = &compileCoreModule },
    { .keyword = "component", .read = &refuseComponent },
});

/// Compiles a whole text to a binary.
///
/// When the text's first node (annotations aside) is a list whose keyword is a TopLevelForms row, it
/// must be the text's only node, and its row compiles it. Otherwise the text is a bare sequence of
/// module fields, an inline module (spec §6.6.13), which an empty text is too.
/// @param source The text.
/// @param limits The caps on reading text.
/// @return The binary, or the first error.
[[nodiscard]] std::expected<std::vector<std::byte>, TextError> compileText(std::string_view source,
                                                                           TextLimits const& limits);

} // namespace isoline::text
