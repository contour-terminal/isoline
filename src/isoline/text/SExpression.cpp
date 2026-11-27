// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/SExpression.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/text/Lexer.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

namespace
{

    /// The length of `(@`, which an AnnotationStart token's text starts with.
    constexpr auto AnnotationPrefixLength = std::size_t { 2 };

    /// What a source's tokens make: how many nodes, how many of them annotations, and how deep the
    /// lists nest.
    struct NodeCounts
    {
        std::size_t nodes = 0;       ///< Every token but `)` and the end.
        std::size_t annotations = 0; ///< The AnnotationStart tokens.
        std::size_t depth = 0;       ///< The deepest nesting of lists and annotations.
    };

    /// Counts what a source's tokens make, up to its end, its first lexical error, or the first list
    /// that would nest past @p maxNesting, which the reading pass then reports where it meets it: so that
    /// what is reserved for the tree is what the reading pass may append, never what a source of open
    /// lists alone would ask for past the limit.
    /// @param lexer A lexer at the source's start.
    /// @param maxNesting The deepest nesting the reading pass accepts.
    /// @return The counts.
    [[nodiscard]] NodeCounts countNodes(Lexer lexer, std::size_t maxNesting)
    {
        auto counts = NodeCounts {};
        auto depth = std::size_t { 0 };
        while (true)
        {
            auto const token = lexer.next();
            if (!token || token->kind == TokenKind::End)
                return counts;
            if (token->kind == TokenKind::RightParen)
            {
                // An unbalanced `)` is the reading pass's error; until it, the count goes on.
                depth = depth > 0 ? depth - 1 : 0;
                continue;
            }
            auto const opens =
                token->kind == TokenKind::LeftParen || token->kind == TokenKind::AnnotationStart;
            if (opens && depth >= maxNesting)
                return counts;
            ++counts.nodes;
            if (token->kind == TokenKind::AnnotationStart)
                ++counts.annotations;
            if (opens)
                counts.depth = std::max(counts.depth, ++depth);
        }
    }

    /// What an annotation means, by the name its id denotes.
    /// @param source The source.
    /// @param start The annotation's AnnotationStart token.
    /// @return The kind of the KnownAnnotations row of its id, a quoted one decoded; nothing for none.
    [[nodiscard]] std::optional<AnnotationKind> kindOf(std::string_view source, Token start)
    {
        auto const id = textOf(source, start).substr(AnnotationPrefixLength);
        if (!id.starts_with('"'))
            return annotationKindNamed(id);
        // The lexer has checked that a quoted id is a well-formed UTF-8 name.
        return decodeName(source,
                          Token { .kind = TokenKind::String,
                                  .offset = static_cast<uint32_t>(start.offset + AnnotationPrefixLength),
                                  .length = static_cast<uint32_t>(id.size()) })
            .transform([](std::string const& name) { return annotationKindNamed(name); })
            .value_or(std::nullopt);
    }

} // namespace

/// The nodes of a tree while it is read: each new node is linked after the last node of the innermost
/// open list, or of the top level.
class SExpressionTreeBuilder
{
  public:
    /// A builder whose vectors hold exactly what @p counts says the source makes, so none of them grows.
    /// @param source The text being read.
    /// @param counts Its nodes and annotations.
    SExpressionTreeBuilder(std::string_view source, NodeCounts counts): _source { source }
    {
        _nodes.reserve(counts.nodes);
        _annotations.reserve(counts.annotations);
        _open.reserve(counts.depth);
    }

    /// Whether no list is open.
    [[nodiscard]] bool isBalanced() const noexcept { return _open.empty(); }

    /// How many lists are open.
    [[nodiscard]] std::size_t depth() const noexcept { return _open.size(); }

    /// Appends a node of one token, linked into the innermost open list or the top level.
    /// @param kind What the node is.
    /// @param token Its token.
    /// @return Its id.
    NodeId append(NodeKind kind, Token token)
    {
        auto const node = static_cast<NodeId>(_nodes.size());
        _nodes.push_back(
            Node { .kind = kind, .tokenKind = token.kind, .offset = token.offset, .length = token.length });
        auto& previous = _open.empty() ? _lastTopLevel : _open.back().lastChild;
        if (previous != NoNode)
            _nodes[previous].nextSibling = node;
        else if (_open.empty())
            _firstTopLevel = node;
        else
            _nodes[_open.back().node].firstChild = node;
        previous = node;
        return node;
    }

    /// Appends a list and opens it, so that the nodes up to its close are its children.
    /// @param token Its `(`.
    void openList(Token token) { _open.push_back(OpenList { .node = append(NodeKind::List, token) }); }

    /// Appends an annotation and opens it, so that the nodes up to its close are its children.
    /// @param token Its AnnotationStart token.
    void openAnnotation(Token token)
    {
        auto const node = append(NodeKind::Annotation, token);
        _annotations.push_back(SExpressionTree::AnnotationInfo {
            .node = node, .idLength = token.length, .kind = kindOf(_source, token) });
        _open.push_back(OpenList { .node = node });
    }

    /// Closes the innermost open list, which must exist.
    /// @param closing Its `)`.
    void close(Token closing) noexcept
    {
        auto& list = _nodes[_open.back().node];
        list.length = closing.offset + closing.length - list.offset;
        _open.pop_back();
    }

    /// The tree, once every list is closed.
    /// @return The tree; the builder is spent.
    [[nodiscard]] SExpressionTree finish() &&
    {
        return SExpressionTree { _source, std::move(_nodes), std::move(_annotations), _firstTopLevel };
    }

  private:
    /// One list or annotation that is open.
    struct OpenList
    {
        NodeId node = NoNode;      ///< The list's node.
        NodeId lastChild = NoNode; ///< Its last child so far, or NoNode.
    };

    std::string_view _source;                                  ///< The text being read.
    std::vector<Node> _nodes;                                  ///< The nodes so far.
    std::vector<SExpressionTree::AnnotationInfo> _annotations; ///< The annotations so far.
    std::vector<OpenList> _open;                               ///< The open lists, innermost last.
    NodeId _firstTopLevel = NoNode;                            ///< The first top-level node, or NoNode.
    NodeId _lastTopLevel = NoNode;                             ///< The last top-level node so far, or NoNode.
};

std::optional<AnnotationKind> annotationKindNamed(std::string_view id) noexcept
{
    auto const found = std::ranges::find(KnownAnnotations, id, &KnownAnnotation::id);
    if (found == KnownAnnotations.end())
        return std::nullopt;
    return found->kind;
}

SExpressionTree::AnnotationInfo const& SExpressionTree::annotationOf(NodeId node) const noexcept
{
    return *std::ranges::lower_bound(_annotations, node, {}, &AnnotationInfo::node);
}

bool SExpressionTree::isSignificant(NodeId node) const noexcept
{
    return _nodes[node].kind != NodeKind::Annotation || annotationOf(node).kind.has_value();
}

std::optional<AnnotationKind> SExpressionTree::annotationKindOf(NodeId node) const noexcept
{
    if (_nodes[node].kind != NodeKind::Annotation)
        return std::nullopt;
    return annotationOf(node).kind;
}

NodeId SExpressionTree::subtreeEnd(NodeId node) const noexcept
{
    auto const end = _nodes[node].offset + _nodes[node].length;
    auto const after = std::span { _nodes }.subspan(std::size_t { node } + 1);
    auto const found = std::ranges::lower_bound(after, end, {}, &Node::offset);
    return static_cast<NodeId>(_nodes.size() - after.size()
                               + static_cast<std::size_t>(found - after.begin()));
}

std::string_view SExpressionTree::textOf(NodeId node) const noexcept
{
    switch (_nodes[node].kind)
    {
        case NodeKind::Atom: return sourceOf(node);
        case NodeKind::Annotation:
            return sourceOf(node).substr(AnnotationPrefixLength,
                                         annotationOf(node).idLength - AnnotationPrefixLength);
        case NodeKind::List: break;
    }
    return {};
}

std::expected<SExpressionTree, TextError> parseSExpressions(std::string_view source, TextLimits limits)
{
    auto lexer = Lexer::create(source, limits);
    if (!lexer)
        return std::unexpected { std::move(lexer.error()) };

    auto const counts = countNodes(*lexer, limits.maxNesting);
    auto builder = SExpressionTreeBuilder { source, counts };
    auto const errorAt = [source](TextErrorCode code, Token token) {
        return std::unexpected { TextError { code, positionAt(source, token.offset) } };
    };
    while (true)
    {
        auto token = lexer->next();
        if (!token)
            return std::unexpected { std::move(token.error()) };
        switch (token->kind)
        {
            case TokenKind::End:
                if (!builder.isBalanced())
                    return errorAt(TextErrorCode::UnexpectedToken, *token);
                return std::move(builder).finish();
            case TokenKind::RightParen:
                if (builder.isBalanced())
                    return errorAt(TextErrorCode::UnexpectedToken, *token);
                builder.close(*token);
                break;
            case TokenKind::LeftParen:
            case TokenKind::AnnotationStart:
                if (builder.depth() >= limits.maxNesting)
                    return errorAt(TextErrorCode::NestingLimitExceeded, *token);
                if (token->kind == TokenKind::LeftParen)
                    builder.openList(*token);
                else
                    builder.openAnnotation(*token);
                break;
            case TokenKind::Keyword:
            case TokenKind::Identifier:
            case TokenKind::String:
            case TokenKind::Reserved: builder.append(NodeKind::Atom, *token); break;
        }
    }
}

} // namespace isoline::text
