// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/Keywords.hpp>

#include <algorithm>
#include <array>
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
    /// Every keyword of the component model's text format (Explainer.md, as wast reads it) that is no
    /// core keyword or instruction, sorted: component text is read by isoline_component_text, which
    /// isoline_text does not link, but a misplaced component keyword is an unexpected token, not an
    /// unknown operator, in either form. P11's CanonBuiltinTable names are rows too
    /// (ComponentReader.everyCanonRowHasText checks them).
    constexpr auto ComponentKeywords = std::to_array<std::string_view>({
        "alias",
        "async",
        "backpressure.dec",
        "backpressure.inc",
        "bool",
        "borrow",
        "callback",
        "canon",
        "case",
        "char",
        "component",
        "context.get",
        "context.set",
        "core",
        "core-type",
        "dtor",
        "enum",
        "eq",
        "error",
        "error-context",
        "error-context.debug-message",
        "error-context.drop",
        "error-context.new",
        "external-id",
        "field",
        "flags",
        "float32",
        "float64",
        "future",
        "future.cancel-read",
        "future.cancel-write",
        "future.drop-readable",
        "future.drop-writable",
        "future.forward",
        "future.new",
        "future.read",
        "future.write",
        "gc",
        "implements",
        "instantiate",
        "interface",
        "lift",
        "list",
        "lower",
        "map",
        "option",
        "outer",
        "own",
        "post-return",
        "realloc",
        "record",
        "rep",
        "resource",
        "resource.drop",
        "resource.new",
        "resource.rep",
        "s16",
        "s32",
        "s64",
        "s8",
        "shared",
        "stream",
        "stream.cancel-read",
        "stream.cancel-write",
        "stream.drop-readable",
        "stream.drop-writable",
        "stream.forward",
        "stream.new",
        "stream.read",
        "stream.write",
        "string",
        "sub",
        "subtask.cancel",
        "subtask.drop",
        "task.cancel",
        "task.return",
        "thread.available-parallelism",
        "thread.index",
        "thread.new-indirect",
        "thread.resume-later",
        "thread.spawn-indirect",
        "thread.spawn-ref",
        "thread.suspend",
        "thread.suspend-then-promote",
        "thread.suspend-then-resume",
        "thread.yield",
        "thread.yield-then-promote",
        "thread.yield-then-resume",
        "tuple",
        "u16",
        "u32",
        "u64",
        "u8",
        "value",
        "variant",
        "version-suffix",
        "waitable-set.drop",
        "waitable-set.new",
        "waitable-set.poll",
        "waitable-set.wait",
        "waitable.join",
        "with",
    });

    static_assert(
        std::ranges::is_sorted(ComponentKeywords)
            && std::ranges::adjacent_find(ComponentKeywords) == ComponentKeywords.end(),
        "ComponentKeywords must be sorted and hold each keyword once, as isKnownKeyword searches it");

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
    return std::ranges::binary_search(Keywords, text) || std::ranges::binary_search(ComponentKeywords, text)
           || opcodeNamed(text).has_value();
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
