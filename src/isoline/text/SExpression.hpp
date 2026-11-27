// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/meta/EnumRows.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

/// The index of a node in its SExpressionTree's nodes().
using NodeId = uint32_t;

/// The id of no node: what a node without children or without a next sibling links to.
inline constexpr auto NoNode = NodeId { 0xFFFF'FFFF };

/// What a node of an S-expression tree is.
enum class NodeKind : uint8_t
{
    List,       ///< `(` … `)`: a parenthesized list of nodes.
    Annotation, ///< `(@id` … `)`: an annotation, whose contents are any balanced tokens (the annotations
                ///< proposal).
    Atom,       ///< One token that is no parenthesis.
};

/// One node of an S-expression tree. It keeps offsets only; positionAt() computes a node's line and
/// column when an error reports it.
struct Node
{
    NodeKind kind = NodeKind::Atom; ///< What the node is.
    TokenKind tokenKind =
        TokenKind::End;          ///< The kind of its token: an atom's, or a list's or annotation's opener.
    uint32_t offset = 0;         ///< The offset of its first byte: an atom's token, a list's `(`.
    uint32_t length = 0;         ///< Its length in bytes: a list's runs up to and including its `)`.
    NodeId firstChild = NoNode;  ///< A list's or annotation's first child; NoNode for none, and for an atom.
    NodeId nextSibling = NoNode; ///< The node after it in its list, or at the top level; NoNode for none.
};

/// The annotations Isoline understands: what each one means.
enum class AnnotationKind : uint8_t
{
    Custom,     ///< `@custom`: a custom section (the annotations proposal).
    BranchHint, ///< `@metadata.code.branch_hint`: a branch hint (the branch hinting proposal).
    Name,       ///< `@name`: a name in the name section (the annotations proposal).
};

/// The last AnnotationKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastAnnotationKind = AnnotationKind::Name;

/// One annotation Isoline understands: its id, and what it means.
struct KnownAnnotation
{
    std::string_view id; ///< The id after `(@`, as the name it denotes.
    AnnotationKind kind; ///< What it means.
};

/// The annotations Isoline understands (the annotations proposal names the reference's), in id order,
/// which is the order the reference interpreter reads them in: every other annotation is ignored
/// wherever it appears.
inline constexpr auto KnownAnnotations = std::to_array<KnownAnnotation>({
    { .id = "custom", .kind = AnnotationKind::Custom },
    { .id = "metadata.code.branch_hint", .kind = AnnotationKind::BranchHint },
    { .id = "name", .kind = AnnotationKind::Name },
});

static_assert(
    meta::firstMisplacedRow<LastAnnotationKind>(KnownAnnotations, &KnownAnnotation::kind)
            == meta::NoMisplacedRow
        && std::ranges::adjacent_find(KnownAnnotations, std::ranges::greater_equal {}, &KnownAnnotation::id)
               == KnownAnnotations.end(),
    "KnownAnnotations must hold one row per AnnotationKind, at the index of its enumerator, in id order");

/// What an annotation id means.
/// @param id The annotation's id, as the name it denotes (a quoted id's escapes resolved).
/// @return The kind of its KnownAnnotations row, or nothing when it has none.
[[nodiscard]] std::optional<AnnotationKind> annotationKindNamed(std::string_view id) noexcept;

/// Whether Isoline understands an annotation.
/// @param id The annotation's id, as the name it denotes (a quoted id's escapes resolved).
/// @return Whether @p id is a row of KnownAnnotations.
[[nodiscard]] inline bool isKnownAnnotation(std::string_view id) noexcept
{
    return annotationKindNamed(id).has_value();
}

class SExpressionTree;

/// Which of a node's siblings a SiblingIterator visits.
enum class SiblingFilter : uint8_t
{
    All,         ///< Every sibling, annotations included.
    Significant, ///< Every sibling but the annotations Isoline does not understand.
};

/// A forward iterator over a node and its next siblings, yielding their ids.
class SiblingIterator
{
  public:
    // std::iter_value_t and std::iter_difference_t bind to these two names by spelling.
    using value_type = NodeId;              // NOLINT(readability-identifier-naming)
    using difference_type = std::ptrdiff_t; // NOLINT(readability-identifier-naming)

    /// The past-the-end iterator, as std::forward_iterator requires a default constructor.
    SiblingIterator() noexcept = default;

    /// An iterator at @p node, or at the first sibling from it that @p filter visits.
    /// @param tree The tree.
    /// @param node The node to start at; NoNode is past the end.
    /// @param filter Which siblings to visit.
    SiblingIterator(SExpressionTree const& tree, NodeId node, SiblingFilter filter) noexcept;

    /// The id of the node the iterator is at.
    [[nodiscard]] NodeId operator*() const noexcept { return _node; }

    /// Steps to the next sibling the filter visits.
    SiblingIterator& operator++() noexcept;

    /// Steps to the next sibling the filter visits.
    /// @return The iterator as it was.
    SiblingIterator operator++(int) noexcept
    {
        auto const previous = *this;
        ++*this;
        return previous;
    }

    /// Whether two iterators are at the same node.
    [[nodiscard]] bool operator==(SiblingIterator const& other) const noexcept
    {
        return _node == other._node;
    }

  private:
    /// Moves forward from the current node to the first one the filter visits.
    void skipFiltered() noexcept;

    SExpressionTree const* _tree = nullptr;     ///< The tree; null only in the past-the-end iterator.
    NodeId _node = NoNode;                      ///< The node the iterator is at.
    SiblingFilter _filter = SiblingFilter::All; ///< Which siblings to visit.
};

/// A run of siblings: a forward range of their ids, iterable through const.
using Siblings = std::ranges::subrange<SiblingIterator>;

/// The S-expressions of a source text, as a flat vector of nodes linked by offsets: each list links
/// its first child, and each node its next sibling. The tree refers to the source, which must outlive
/// it.
class SExpressionTree
{
  public:
    /// The text the tree was read from.
    [[nodiscard]] std::string_view source() const noexcept { return _source; }

    /// Every node, in source order of their first token.
    [[nodiscard]] std::span<Node const> nodes() const noexcept { return _nodes; }

    /// The first node at the top level; the others are its next siblings.
    /// @return Its id, or NoNode for a source of no tokens.
    [[nodiscard]] NodeId firstTopLevel() const noexcept { return _firstTopLevel; }

    /// The nodes at the top level, annotations included.
    /// @return Their ids, in source order.
    [[nodiscard]] Siblings topLevel() const noexcept
    {
        return siblingsFrom(_firstTopLevel, SiblingFilter::All);
    }

    /// The nodes at the top level that mean something: all but the annotations Isoline does not
    /// understand.
    /// @return Their ids, in source order.
    [[nodiscard]] Siblings significantTopLevel() const noexcept
    {
        return siblingsFrom(_firstTopLevel, SiblingFilter::Significant);
    }

    /// The siblings after a node, annotations included.
    /// @param node A node.
    /// @return Their ids, in source order.
    [[nodiscard]] Siblings siblingsAfter(NodeId node) const noexcept
    {
        return siblingsFrom(_nodes[node].nextSibling, SiblingFilter::All);
    }

    /// The children of a list or annotation, annotations included.
    /// @param list A node; an atom, or NoNode, has none.
    /// @return Their ids, in source order.
    [[nodiscard]] Siblings children(NodeId list) const noexcept
    {
        return siblingsFrom(firstChildOf(list), SiblingFilter::All);
    }

    /// The children of a list or annotation that mean something: all but the annotations Isoline does
    /// not understand, which are ignored everywhere (the annotations proposal).
    /// @param list A node; an atom, or NoNode, has none.
    /// @return Their ids, in source order.
    [[nodiscard]] Siblings significantChildren(NodeId list) const noexcept
    {
        return siblingsFrom(firstChildOf(list), SiblingFilter::Significant);
    }

    /// Whether a node means something: whether it is no annotation, or a known one.
    /// @param node The node.
    /// @return Whether @p node is no Annotation, or one whose id isKnownAnnotation(). O(log n) in the
    ///         number of annotations.
    [[nodiscard]] bool isSignificant(NodeId node) const noexcept;

    /// The text of an atom or annotation.
    /// @param node The node.
    /// @return An atom's token text; an annotation's id as written after `(@` (a quoted one with its
    ///         quotes, escapes unresolved); empty for a list.
    [[nodiscard]] std::string_view textOf(NodeId node) const noexcept;

    /// The verbatim source of a node.
    /// @param node The node.
    /// @return Its text, from a list's `(` to its `)`.
    [[nodiscard]] std::string_view sourceOf(NodeId node) const noexcept
    {
        return _source.substr(_nodes[node].offset, _nodes[node].length);
    }

    /// Where a node is, computed from its offset.
    /// @param node The node.
    /// @return The line and column of its first byte.
    [[nodiscard]] SourcePosition positionOf(NodeId node) const noexcept
    {
        return positionAt(_source, _nodes[node].offset);
    }

    /// The token of an atom, as the lexer's functions take it.
    /// @param atom An Atom node.
    /// @return Its token.
    [[nodiscard]] Token tokenOf(NodeId atom) const noexcept
    {
        auto const& entry = _nodes[atom];
        return Token { .kind = entry.tokenKind, .offset = entry.offset, .length = entry.length };
    }

    /// What an annotation means.
    /// @param node A node.
    /// @return The kind of a known annotation; nothing for an annotation Isoline does not understand,
    ///         and for a node that is no annotation. O(log n) in the number of annotations.
    [[nodiscard]] std::optional<AnnotationKind> annotationKindOf(NodeId node) const noexcept;

    /// The node after the last node of a subtree: nodes are in source order, so a subtree's nodes are
    /// those from its root on that start before its end.
    /// @param node A node.
    /// @return That node's id; nodes().size() for a subtree that runs to the end. O(log n).
    [[nodiscard]] NodeId subtreeEnd(NodeId node) const noexcept;

  private:
    /// What the tree keeps of one annotation besides its node: its id's extent, which the node does
    /// not record, and what it means, decided once when the tree is read.
    struct AnnotationInfo
    {
        NodeId node = NoNode;               ///< The annotation's node.
        uint32_t idLength = 0;              ///< The length of `(@` and its id.
        std::optional<AnnotationKind> kind; ///< What it means; nothing when Isoline does not understand it.
    };

    friend class SExpressionTreeBuilder; // Builds the tree, in SExpression.cpp.

    SExpressionTree(std::string_view source,
                    std::vector<Node> nodes,
                    std::vector<AnnotationInfo> annotations,
                    NodeId firstTopLevel) noexcept:
        _source { source },
        _nodes { std::move(nodes) },
        _annotations { std::move(annotations) },
        _firstTopLevel { firstTopLevel }
    {
    }

    /// The first child of a node.
    /// @param list The node, or NoNode.
    /// @return Its first child, or NoNode for an atom, an empty list or NoNode.
    [[nodiscard]] NodeId firstChildOf(NodeId list) const noexcept
    {
        return list == NoNode ? NoNode : _nodes[list].firstChild;
    }

    /// The siblings from a node on.
    /// @param first The first node, or NoNode for none.
    /// @param filter Which to visit.
    /// @return The range.
    [[nodiscard]] Siblings siblingsFrom(NodeId first, SiblingFilter filter) const noexcept
    {
        return Siblings { SiblingIterator { *this, first, filter }, SiblingIterator {} };
    }

    /// What the tree keeps of an annotation.
    /// @param node An Annotation node.
    /// @return Its entry in _annotations.
    [[nodiscard]] AnnotationInfo const& annotationOf(NodeId node) const noexcept;

    std::string_view _source;                 ///< The text the tree was read from.
    std::vector<Node> _nodes;                 ///< Every node.
    std::vector<AnnotationInfo> _annotations; ///< One entry per Annotation node, in node order.
    NodeId _firstTopLevel;                    ///< The first top-level node, or NoNode.
};

inline SiblingIterator::SiblingIterator(SExpressionTree const& tree,
                                        NodeId node,
                                        SiblingFilter filter) noexcept:
    _tree { &tree }, _node { node }, _filter { filter }
{
    skipFiltered();
}

inline SiblingIterator& SiblingIterator::operator++() noexcept
{
    _node = _tree->nodes()[_node].nextSibling;
    skipFiltered();
    return *this;
}

inline void SiblingIterator::skipFiltered() noexcept
{
    if (_filter == SiblingFilter::All)
        return;
    while (_node != NoNode && !_tree->isSignificant(_node))
        _node = _tree->nodes()[_node].nextSibling;
}

/// Reads the S-expressions of a source text (spec §6.2) into a tree, iteratively: one loop over the
/// lexer's tokens with an explicit stack of the open lists, so no input reaches the call stack.
///
/// The source is lexed twice: once to count the nodes, so that the tree's vectors are allocated once
/// at their exact size and hold at most 20 bytes per source byte whatever a standard library's growth
/// factor, and once to build them.
/// @param source The text; the tree refers to it, so it must outlive the tree.
/// @param limits The caps: maxSourceBytes, and maxNesting for the depth of lists and annotations.
/// @return The tree, or the error: the lexer's, UnexpectedToken at an unbalanced `)` or at the end
///         of a source whose lists are not all closed, or NestingLimitExceeded at the `(` of a list
///         deeper than TextLimits::maxNesting.
[[nodiscard]] std::expected<SExpressionTree, TextError> parseSExpressions(std::string_view source,
                                                                          TextLimits limits);

} // namespace isoline::text
