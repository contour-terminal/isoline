// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/text/Lexer.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

using isoline::text::isKnownAnnotation;
using isoline::text::Node;
using isoline::text::NodeId;
using isoline::text::NodeKind;
using isoline::text::NoNode;
using isoline::text::parseSExpressions;
using isoline::text::SExpressionTree;
using isoline::text::specMessage;
using isoline::text::TextError;
using isoline::text::TextErrorCode;
using isoline::text::TextLimits;
using isoline::text::TokenKind;

namespace
{

/// Parses @p source, which must parse.
/// @param source The text.
/// @param limits The caps.
/// @return The tree.
SExpressionTree parsed(std::string_view source, TextLimits limits = {})
{
    auto tree = parseSExpressions(source, limits);
    REQUIRE(tree.has_value());
    return std::move(*tree);
}

/// Parses @p source, which must not parse.
/// @param source The text.
/// @param limits The caps.
/// @return The error.
TextError parseError(std::string_view source, TextLimits limits = {})
{
    auto tree = parseSExpressions(source, limits);
    REQUIRE_FALSE(tree.has_value());
    return tree.error();
}

/// The ids of a range of node ids, collected.
/// @param range A range of NodeId.
/// @return Its ids, in order.
std::vector<NodeId> collect(auto&& range)
{
    auto ids = std::vector<NodeId> {};
    for (auto const id: range)
        ids.push_back(id);
    return ids;
}

/// The texts of the children of a list, an annotation's id or a list's verbatim source for each.
/// @param tree The tree.
/// @param list The list.
/// @return One text per child.
std::vector<std::string> childTexts(SExpressionTree const& tree, NodeId list)
{
    auto texts = std::vector<std::string> {};
    for (auto const id: tree.children(list))
        texts.emplace_back(tree.nodes()[id].kind == NodeKind::List ? tree.sourceOf(id) : tree.textOf(id));
    return texts;
}

} // namespace

TEST_CASE("SExpression.buildsSiblings")
{
    auto const source = std::string_view { "(a (b c) d)" };
    auto const tree = parsed(source);
    CHECK(tree.source() == source);
    auto const top = tree.firstTopLevel();
    REQUIRE(top != NoNode);
    auto const& topNode = tree.nodes()[top];
    CHECK(topNode.kind == NodeKind::List);
    CHECK(topNode.tokenKind == TokenKind::LeftParen);
    CHECK(topNode.nextSibling == NoNode);
    CHECK(tree.sourceOf(top) == source);
    CHECK(tree.textOf(top).empty());

    auto const children = collect(tree.children(top));
    REQUIRE(children.size() == 3);
    CHECK(tree.textOf(children[0]) == "a");
    CHECK(tree.nodes()[children[0]].kind == NodeKind::Atom);
    // One letter is no keyword: the reference lexer's keywords have two characters at least.
    CHECK(tree.nodes()[children[0]].tokenKind == TokenKind::Reserved);
    CHECK(tree.sourceOf(children[0]) == "a");
    CHECK(tree.nodes()[children[1]].kind == NodeKind::List);
    CHECK(tree.sourceOf(children[1]) == "(b c)");
    CHECK(childTexts(tree, children[1]) == std::vector<std::string> { "b", "c" });
    CHECK(tree.textOf(children[2]) == "d");
    CHECK(collect(tree.significantChildren(top)) == children);

    auto const position = tree.positionOf(children[1]);
    CHECK(position.line == 1);
    CHECK(position.column == 4);
    CHECK(position.offset == 3);

    SECTION("an empty list has no children")
    {
        auto const empty = parsed("()");
        CHECK(collect(empty.children(empty.firstTopLevel())).empty());
        CHECK(empty.sourceOf(empty.firstTopLevel()) == "()");
    }
    SECTION("every token kind is an atom")
    {
        auto const atoms = parsed(R"(($x "s" 1 nop , $"q"))");
        auto const ids = collect(atoms.children(atoms.firstTopLevel()));
        auto kinds = std::vector<TokenKind> {};
        for (auto const id: ids)
            kinds.push_back(atoms.nodes()[id].tokenKind);
        CHECK(kinds
              == std::vector<TokenKind> { TokenKind::Identifier,
                                          TokenKind::String,
                                          TokenKind::Keyword,
                                          TokenKind::Keyword,
                                          TokenKind::Reserved,
                                          TokenKind::Identifier });
        CHECK(atoms.textOf(ids[1]) == "\"s\"");
    }
    SECTION("the children range is a forward range that can be walked twice")
    {
        auto const range = tree.children(top);
        static_assert(std::ranges::forward_range<decltype(range)>);
        CHECK(std::ranges::distance(range) == 3);
        CHECK(collect(range) == children);
        auto iterator = range.begin();
        auto const before = iterator++;
        CHECK(*before == children[0]);
        CHECK(*iterator == children[1]);
    }
}

TEST_CASE("SExpression.empty")
{
    for (auto const source: { std::string_view {}, std::string_view { "  ;; nothing\n (; at all ;)" } })
    {
        auto const tree = parsed(source);
        CHECK(tree.firstTopLevel() == NoNode);
        CHECK(tree.nodes().empty());
    }
}

TEST_CASE("SExpression.topLevelAtoms")
{
    // The tree is the script's and the module's grammar's neutral ground: atoms at the top are theirs
    // to refuse.
    auto const tree = parsed("a (b) c");
    auto const first = tree.firstTopLevel();
    CHECK(tree.textOf(first) == "a");
    auto const second = tree.nodes()[first].nextSibling;
    CHECK(tree.sourceOf(second) == "(b)");
    auto const third = tree.nodes()[second].nextSibling;
    CHECK(tree.textOf(third) == "c");
    CHECK(tree.nodes()[third].nextSibling == NoNode);
}

TEST_CASE("SExpression.unknownAnnotationIgnored")
{
    auto const tree = parsed("(a (@foo (x) \"y\") b)");
    auto const top = tree.firstTopLevel();
    auto const children = collect(tree.children(top));
    REQUIRE(children.size() == 3);
    auto const annotation = children[1];
    CHECK(tree.nodes()[annotation].kind == NodeKind::Annotation);
    CHECK(tree.nodes()[annotation].tokenKind == TokenKind::AnnotationStart);
    CHECK(tree.textOf(annotation) == "foo");
    CHECK(tree.sourceOf(annotation) == "(@foo (x) \"y\")");
    CHECK(childTexts(tree, annotation) == std::vector<std::string> { "(x)", "\"y\"" });
    auto const significant = collect(tree.significantChildren(top));
    CHECK(significant == std::vector<NodeId> { children[0], children[2] });

    SECTION("known annotations are significant")
    {
        auto const known = parsed(R"((a (@custom "c" "") (@name "n") (@metadata.code.branch_hint "\01") b))");
        CHECK(collect(known.significantChildren(known.firstTopLevel())).size() == 5);
    }
    SECTION("a quoted id is the name it denotes")
    {
        auto const quoted = parsed(R"((a (@"custom" "c") (@"cust\6fm" "c") (@"foo") b))");
        auto const ids = collect(quoted.children(quoted.firstTopLevel()));
        REQUIRE(ids.size() == 5);
        CHECK(quoted.textOf(ids[1]) == "\"custom\"");
        CHECK(quoted.textOf(ids[2]) == R"("cust\6fm")");
        CHECK(collect(quoted.significantChildren(quoted.firstTopLevel()))
              == std::vector<NodeId> { ids[0], ids[1], ids[2], ids[4] });
    }
    SECTION("annotations nest, and are atoms' and lists' siblings at the top")
    {
        auto const nested = parsed("(@a , ; ] [ } (@b (c))) (d)");
        auto const first = nested.firstTopLevel();
        CHECK(nested.textOf(first) == "a");
        auto const inner = collect(nested.children(first));
        REQUIRE(inner.size() == 6);
        CHECK(nested.nodes()[inner[0]].tokenKind == TokenKind::Reserved);
        CHECK(nested.nodes()[inner[5]].kind == NodeKind::Annotation);
        CHECK(nested.sourceOf(inner[5]) == "(@b (c))");
        CHECK(nested.sourceOf(nested.nodes()[first].nextSibling) == "(d)");
    }
    SECTION("the known annotations are a table")
    {
        CHECK(isKnownAnnotation("custom"));
        CHECK(isKnownAnnotation("name"));
        CHECK(isKnownAnnotation("metadata.code.branch_hint"));
        CHECK_FALSE(isKnownAnnotation("foo"));
        CHECK_FALSE(isKnownAnnotation(""));
        CHECK_FALSE(isKnownAnnotation("metadata.code"));
    }
}

TEST_CASE("SExpression.unbalanced")
{
    auto const unclosed = parseError("(a");
    CHECK(unclosed.code == TextErrorCode::UnexpectedToken);
    CHECK(unclosed.position.line == 1);
    CHECK(unclosed.position.column == 3);
    CHECK(specMessage(unclosed) == "unexpected token");

    auto const extra = parseError("a)");
    CHECK(extra.code == TextErrorCode::UnexpectedToken);
    CHECK(extra.position.column == 2);

    CHECK(parseError("(@x").code == TextErrorCode::UnclosedAnnotation);
    CHECK(parseError("(a\n  (b)").position.line == 2);
    CHECK(parseError("(a \"").code == TextErrorCode::UnclosedString);
}

TEST_CASE("SExpression.deepNestingIsRefused")
{
    auto const million = parseError(std::string(1'000'000, '('));
    CHECK(million.code == TextErrorCode::NestingLimitExceeded);
    CHECK(million.position.offset == TextLimits {}.maxNesting);
    CHECK(specMessage(million) == "nesting limit exceeded");

    auto const limits = TextLimits { .maxNesting = 3 };
    auto const deepest = parsed("(((x)))", limits);
    CHECK(deepest.sourceOf(deepest.firstTopLevel()) == "(((x)))");
    auto const refused = parseError("((((x))))", limits);
    CHECK(refused.code == TextErrorCode::NestingLimitExceeded);
    CHECK(refused.position.column == 4);
    // An annotation is a parenthesized level too.
    CHECK(parseError("((((@a))))", limits).code == TextErrorCode::NestingLimitExceeded);
    CHECK(parseError("(((@a ())))", limits).code == TextErrorCode::NestingLimitExceeded);

    auto const atDefault =
        std::string(TextLimits {}.maxNesting, '(') + std::string(TextLimits {}.maxNesting, ')');
    CHECK(parseSExpressions(atDefault, TextLimits {}).has_value());
}

TEST_CASE("SExpression.multipleTopLevel")
{
    auto const tree = parsed("(module) (module)");
    auto const first = tree.firstTopLevel();
    CHECK(tree.sourceOf(first) == "(module)");
    auto const second = tree.nodes()[first].nextSibling;
    REQUIRE(second != NoNode);
    CHECK(tree.sourceOf(second) == "(module)");
    CHECK(tree.positionOf(second).column == 10);
    CHECK(tree.nodes()[second].nextSibling == NoNode);
}

TEST_CASE("SExpression.sourceTooLarge")
{
    CHECK(parseError("(a)", TextLimits { .maxSourceBytes = 2 }).code == TextErrorCode::SourceTooLarge);
}

TEST_CASE("SExpression.nodeIsSmall")
{
    static_assert(sizeof(Node) <= 24);
    CHECK(sizeof(Node) <= 24);
}

TEST_CASE("SExpression.ranges")
{
    auto const tree = parsed(R"((@foo) a (b (@custom "c") (@bar)) (@name "m"))");
    auto const top = collect(tree.topLevel());
    REQUIRE(top.size() == 4);
    CHECK(top.front() == tree.firstTopLevel());
    CHECK(collect(tree.significantTopLevel()) == std::vector<NodeId> { top[1], top[2], top[3] });

    SECTION("an atom, and no node, have no children")
    {
        CHECK(collect(tree.children(top[1])).empty());
        CHECK(collect(tree.significantChildren(top[1])).empty());
        CHECK(collect(tree.children(NoNode)).empty());
        auto const empty = parsed("");
        CHECK(collect(empty.topLevel()).empty());
        CHECK(collect(empty.children(empty.firstTopLevel())).empty());
    }
    SECTION("the significant children are a const forward range")
    {
        auto const significant = tree.significantChildren(top[2]);
        static_assert(std::ranges::forward_range<decltype(significant)>);
        auto texts = std::vector<std::string_view> {};
        for (auto const id: significant)
            texts.push_back(tree.textOf(id));
        CHECK(texts == std::vector<std::string_view> { "b", "custom" });
        CHECK(std::ranges::distance(significant) == 2);
    }
    SECTION("an annotation has a position, and its id")
    {
        auto const annotations = collect(tree.children(top[2]));
        REQUIRE(annotations.size() == 3);
        auto const position = tree.positionOf(annotations[1]);
        CHECK(position.line == 1);
        CHECK(position.column == 13);
        CHECK(tree.textOf(annotations[2]) == "bar");
        CHECK_FALSE(tree.isSignificant(annotations[2]));
        CHECK(tree.isSignificant(annotations[1]));
        CHECK(tree.isSignificant(annotations[0]));
    }
}
