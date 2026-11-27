// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/TopLevel.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string_view>
#include <vector>

#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/text/ModuleReader.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

namespace
{
    /// Encodes @p module within TextLimits::maxModuleBytes.
    /// @param module The module read.
    /// @param limits The limits it was read under.
    /// @param position Where an encoding too large is reported: the module's start.
    /// @return Its bytes, or OutputTooLarge.
    std::expected<std::vector<std::byte>, TextError> encodeWithin(binary::Module const& module,
                                                                  TextLimits const& limits,
                                                                  SourcePosition position)
    {
        return binary::encodeModule(module, limits.maxModuleBytes)
            .transform_error([position](binary::EncodeError) {
                return TextError { TextErrorCode::OutputTooLarge, position };
            });
    }
} // namespace

std::expected<std::vector<std::byte>, TextError> compileCoreModule(SExpressionTree const& tree,
                                                                   NodeId form,
                                                                   TextLimits const& limits)
{
    return ModuleReader { limits }.read(tree, form).and_then([&](binary::Module const& module) {
        return encodeWithin(module, limits, tree.positionOf(form));
    });
}

std::expected<std::vector<std::byte>, TextError> refuseComponent(SExpressionTree const& tree,
                                                                 NodeId form,
                                                                 TextLimits const& /*limits*/)
{
    return std::unexpected { TextError { TextErrorCode::UnsupportedComponent, tree.positionOf(form) } };
}

std::expected<std::vector<std::byte>, TextError> compileText(std::string_view source,
                                                             TextLimits const& limits)
{
    auto const tree = parseSExpressions(source, limits);
    if (!tree)
        return std::unexpected { tree.error() };
    auto const form = firstTopLevelNode(*tree);
    auto const row = form == NoNode
                         ? TopLevelForms.end()
                         : std::ranges::find(TopLevelForms, headKeyword(*tree, form), &TopLevelForm::keyword);
    if (row == TopLevelForms.end())
        return ModuleReader { limits }.readInline(*tree).and_then(
            [&](binary::Module const& module) { return encodeWithin(module, limits, SourcePosition {}); });
    if (auto const sole = requireSoleTopLevelNode(*tree, form); !sole)
        return std::unexpected { sole.error() };
    return row->read(*tree, form, limits).and_then([&](std::vector<std::byte>&& bytes) {
        return refuseAnnotationsOutside(*tree, form).transform([&] { return std::move(bytes); });
    });
}

} // namespace isoline::text
