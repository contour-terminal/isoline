// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <expected>
#include <string_view>
#include <utility>

#include <isoline/binary/Module.hpp>
#include <isoline/text/ModuleReader.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

namespace isoline::text::testing
{

/// Reads a whole text into the binary::Module the reader builds, for the tests that inspect it rather
/// than its encoding: as compileText() reads the text, its form dispatched through CoreForms. A
/// `(module …)` is read by ModuleReader::read(), a bare sequence of fields by readInline(), and the form of
/// any other row, which compiles to no core module, is refused as its row refuses it, or as unexpected.
/// @param source The text.
/// @param limits The caps on reading text.
/// @return The module, or the first error.
[[nodiscard]] inline std::expected<binary::Module, TextError> readModuleText(std::string_view source,
                                                                             TextLimits limits)
{
    auto const tree = parseSExpressions(source, limits);
    if (!tree)
        return std::unexpected { tree.error() };
    auto const reader = ModuleReader { limits };
    auto const form = firstTopLevelNode(*tree);
    auto const row = form == NoNode
                         ? CoreForms.end()
                         : std::ranges::find(CoreForms, headKeyword(*tree, form), &TopLevelForm::keyword);
    if (row == CoreForms.end())
        return reader.readInline(*tree);
    if (auto const sole = requireSoleTopLevelNode(*tree, form); !sole)
        return std::unexpected { sole.error() };
    if (row->read != &compileCoreModule)
    {
        auto const other = row->read(*tree, form, limits);
        return std::unexpected { other ? TextError { TextErrorCode::UnexpectedToken, tree->positionOf(form) }
                                       : other.error() };
    }
    return reader.read(*tree, form).and_then([&](binary::Module&& module) {
        return refuseAnnotationsOutside(*tree, form).transform([&] { return std::move(module); });
    });
}

} // namespace isoline::text::testing
