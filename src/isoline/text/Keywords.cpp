// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/Keywords.hpp>

#include <algorithm>
#include <string>
#include <string_view>

#include <isoline/text/Instructions.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>

namespace isoline::text
{

namespace
{
    /// Whether a Keyword token's text is keyword-shaped (the reference lexer's `keyword`: a lowercase
    /// letter first) and no number or memory argument the lexer reads before keywords: `inf`, `nan`,
    /// `nan:0x…`, `offset=…` or `align=…`.
    bool isKeywordShaped(std::string_view text) noexcept
    {
        return !text.empty() && text.front() >= 'a' && text.front() <= 'z' && text != "inf" && text != "nan"
               && !text.starts_with("nan:0x") && !text.contains('=');
    }
} // namespace

bool isKnownKeyword(std::string_view text) noexcept
{
    return std::ranges::binary_search(Keywords, text) || opcodeNamed(text).has_value();
}

TextError unexpectedKeyword(SExpressionTree const& tree, NodeId node)
{
    auto const& entry = tree.nodes()[node];
    auto const text = tree.textOf(node);
    auto const isUnknown =
        entry.kind == NodeKind::Atom
        && (entry.tokenKind == TokenKind::Reserved
            || (entry.tokenKind == TokenKind::Keyword && isKeywordShaped(text) && !isKnownKeyword(text)));
    if (isUnknown)
        return TextError { TextErrorCode::UnknownOperator, tree.positionOf(node), std::string { text } };
    return TextError { TextErrorCode::UnexpectedToken, tree.positionOf(node) };
}

} // namespace isoline::text
