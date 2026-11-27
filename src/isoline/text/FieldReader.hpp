// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/text/Annotations.hpp>
#include <isoline/text/FunctionTypes.hpp>
#include <isoline/text/Instructions.hpp>
#include <isoline/text/IntegerLiteral.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

// The module reader's own vocabulary, shared by its translation units: ModuleReader.cpp reads the
// module fields, FieldReader.cpp the identifiers, literals and types they are made of, and
// InstructionReader.cpp the instructions of function bodies and constant expressions. Not installed.

namespace isoline::text
{

class InstructionReader;

/// The outcome of a step that produces nothing but may fail.
using Outcome = std::expected<void, TextError>;

/// The number of index spaces a module has once: every IndexSpace before Local.
inline constexpr auto ModuleSpaceCount = std::size_t { std::to_underlying(IndexSpace::Local) };

/// One index space: how the reference interpreter's messages name it, and the name subsection that
/// names its entities.
struct SpaceRow
{
    IndexSpace space;          ///< The space; equal to the row's index.
    std::string_view category; ///< Its name in "unknown …" and "duplicate …" messages (parser.mly).
    binary::NameKind names;    ///< The name subsection its identifiers go to.
};

/// Every index space, one row per IndexSpace enumerator, in enumerator order.
inline constexpr auto SpaceRows = std::to_array<SpaceRow>({
    { .space = IndexSpace::Type, .category = "type", .names = binary::NameKind::Type },
    { .space = IndexSpace::Function, .category = "function", .names = binary::NameKind::Function },
    { .space = IndexSpace::Table, .category = "table", .names = binary::NameKind::Table },
    { .space = IndexSpace::Memory, .category = "memory", .names = binary::NameKind::Memory },
    { .space = IndexSpace::Global, .category = "global", .names = binary::NameKind::Global },
    { .space = IndexSpace::Tag, .category = "tag", .names = binary::NameKind::Tag },
    { .space = IndexSpace::Element, .category = "elem segment", .names = binary::NameKind::Element },
    { .space = IndexSpace::Data, .category = "data segment", .names = binary::NameKind::Data },
    { .space = IndexSpace::Local, .category = "local", .names = binary::NameKind::Local },
    { .space = IndexSpace::Label, .category = "label", .names = binary::NameKind::Label },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: SpaceRows)
        {
            if (std::to_underlying(row.space) != index)
                return false;
            ++index;
        }
        return index == std::to_underlying(LastIndexSpace) + std::size_t { 1 };
    }(),
    "SpaceRows must hold one row per IndexSpace, at the index of its enumerator");

/// The row of an index space.
/// @param space The space.
/// @return Its SpaceRows row.
[[nodiscard]] constexpr SpaceRow const& rowOf(IndexSpace space) noexcept
{
    return SpaceRows[std::to_underlying(space)];
}

/// The node at or after @p node that is no annotation, among its siblings.
/// @param tree The tree.
/// @param node A node of @p tree, or NoNode.
/// @return The node, or NoNode when only annotations follow.
[[nodiscard]] inline NodeId skipAnnotations(SExpressionTree const& tree, NodeId node) noexcept
{
    while (node != NoNode && tree.nodes()[node].kind == NodeKind::Annotation)
        node = tree.nodes()[node].nextSibling;
    return node;
}

/// The nodes of a list, read front to back, annotations skipped: annotations are read apart from the
/// grammar of what holds them.
class Cursor
{
  public:
    /// A cursor at the first child of @p list that is no annotation.
    /// @param tree The tree.
    /// @param list A list of @p tree.
    Cursor(SExpressionTree const& tree, NodeId list) noexcept:
        Cursor { tree, list, tree.nodes()[list].firstChild }
    {
    }

    /// A cursor at @p first, a child of @p list, or at the first child after it that is no annotation.
    /// @param tree The tree.
    /// @param list A list of @p tree.
    /// @param first A child of @p list, or NoNode for a cursor at the list's end.
    Cursor(SExpressionTree const& tree, NodeId list, NodeId first) noexcept:
        _tree { &tree }, _list { list }, _node { skipAnnotations(tree, first) }
    {
    }

    /// The node the cursor is at.
    /// @return Its id, or NoNode at the end of the list.
    [[nodiscard]] NodeId peek() const noexcept { return _node; }

    /// The node after the one the cursor is at.
    /// @return Its id, or NoNode when the cursor is at the list's last node or past it.
    [[nodiscard]] NodeId peekNext() const noexcept
    {
        return _node == NoNode ? NoNode : skipAnnotations(*_tree, _tree->nodes()[_node].nextSibling);
    }

    /// Whether the cursor is past the list's last node.
    [[nodiscard]] bool atEnd() const noexcept { return _node == NoNode; }

    /// Steps past the node the cursor is at.
    /// @return The node stepped past; NoNode at the end.
    NodeId take() noexcept
    {
        auto const node = _node;
        _node = peekNext();
        return node;
    }

    /// Steps past every node left.
    void finish() noexcept { _node = NoNode; }

    /// Where the list ends: its `)`, the token the reference reports a missing node at.
    [[nodiscard]] SourcePosition endPosition() const noexcept
    {
        auto const& list = _tree->nodes()[_list];
        return positionAt(_tree->source(), list.offset + list.length - 1);
    }

  private:
    SExpressionTree const* _tree; ///< The tree.
    NodeId _list;                 ///< The list.
    NodeId _node;                 ///< The node the cursor is at, or NoNode.
};

/// How many of a sequence's nodes an instruction sequence is.
enum class NodeExtent : uint8_t
{
    One,  ///< The first node only: a folded instruction.
    Rest, ///< The first node and every sibling after it: the rest of a list.
};

/// The nodes of an instruction sequence: a function body, a constant expression, or an offset.
struct InstructionNodes
{
    NodeId list = NoNode;                 ///< The list the sequence is in, whose `)` ends it.
    NodeId first = NoNode;                ///< The first node, or NoNode for an empty sequence.
    NodeExtent extent = NodeExtent::Rest; ///< How many nodes from it the sequence is.
};

/// One parameter a type use names: its index among the parameters, its identifier, and where `@name`
/// annotations naming it stand.
struct ParamName
{
    uint32_t index = 0;         ///< The parameter's index.
    NodeId identifier = NoNode; ///< Its `$id` atom; NoNode for one that only an `@name` names.
    NodeId namedAfter = NoNode; ///< The node its `@name` annotations follow: its identifier, or `param`.
};

/// A type use as written (spec §6.5.6): an optional `(type x)`, and inline parameters and results.
struct TypeUse
{
    NodeId explicitNode = NoNode;      ///< The `x` of `(type x)`, or NoNode without one.
    uint32_t explicitIndex = 0;        ///< The type index `x` denotes, when there is one.
    binary::FuncType inlineType;       ///< The inline parameters and results.
    std::vector<ParamName> paramNames; ///< The parameters that bind an identifier.
};

/// Whether a type use's parameters may bind identifiers: a function's, an import's and a tag's may,
/// an instruction's may not (spec §6.5.6).
enum class ParamNames : uint8_t
{
    Refused, ///< `(param t*)` only.
    Allowed, ///< `(param $id t)` too.
};

/// Whether an element list may be bare function indices, as an active segment's without a table use
/// may (spec §6.6.11).
enum class IndexAbbreviation : uint8_t
{
    Refused, ///< It starts with `func` or a reference type.
    Allowed, ///< It may be function indices only.
};

/// A deferred error: one the reference reports only once every later field is read. It keeps the
/// node, not its position: a position is computed from the source's start, and a source can defer an
/// error per field, so only the one reported is located.
struct DeferredError
{
    /// Builds a deferred error; every member is required.
    /// @param ordinal The field whose check raises it.
    /// @param code The error.
    /// @param node Where it is reported.
    /// @param subject What its message names, if anything.
    DeferredError(std::size_t ordinal, TextErrorCode code, NodeId node, std::string subject):
        ordinal { ordinal }, code { code }, node { node }, subject { std::move(subject) }
    {
    }

    std::size_t ordinal; ///< The field whose check raises it; the latest one is reported.
    TextErrorCode code;  ///< The error.
    NodeId node;         ///< Where it is reported.
    std::string subject; ///< What its message names, if anything.
};

/// What a function's parameters and locals bind.
struct LocalScope
{
    std::map<std::string, uint32_t, std::less<>> symbols; ///< Each identifier's local index.
    std::vector<binary::NameAssoc> names;                 ///< The names, for the name section.
};

/// The identifiers of one index space, by name.
using Symbols = std::map<std::string, uint32_t, std::less<>>;

/// Reads the fields of one module, in the reference interpreter's three phases: every identifier is
/// bound first, so that a field may name one defined after it; then every explicit type is defined, so
/// that implicit types come after them all; then every field is read in source order.
///
/// Its public reading vocabulary (nodes, errors, identifiers, literals and types) is what
/// InstructionReader reads instructions with.
class FieldReader
{
  public:
    /// A reader of fields of @p tree.
    /// @param tree The tree; its source must outlive the reader.
    /// @param limits The caps on reading text.
    FieldReader(SExpressionTree const& tree, TextLimits const& limits):
        _tree { tree }, _limits { limits }, _types { _module.types }
    {
    }

    FieldReader(FieldReader const&) = delete;
    FieldReader(FieldReader&&) = delete;
    FieldReader& operator=(FieldReader const&) = delete;
    FieldReader& operator=(FieldReader&&) = delete;
    ~FieldReader() = default;

    /// Reads the fields from @p first on, and the known annotations of the module.
    /// @param module The `(module …)` list; NoNode for a bare module, whose fields are the whole tree.
    /// @param first The first field, or NoNode for none.
    /// @param moduleName The module's `$id`, or NoNode without one.
    /// @return The module, or the first error.
    std::expected<binary::Module, TextError> read(NodeId module, NodeId first, NodeId moduleName);

    // The steps of ModuleReader.cpp's FieldRows, one per phase and kind of field.
    Outcome bindNamed(NodeId field);
    Outcome bindTable(NodeId field);
    Outcome bindMemory(NodeId field);
    Outcome bindImport(NodeId field);
    Outcome skip(NodeId field);
    Outcome defineType(NodeId field);
    Outcome readFunction(NodeId field);
    Outcome readTable(NodeId field);
    Outcome readMemory(NodeId field);
    Outcome readGlobal(NodeId field);
    Outcome readTag(NodeId field);
    Outcome readElement(NodeId field);
    Outcome readData(NodeId field);
    Outcome readImport(NodeId field);
    Outcome readExport(NodeId field);
    Outcome readStart(NodeId field);

    // ---- The reading vocabulary (FieldReader.cpp) ----

    /// The tree the fields are in.
    [[nodiscard]] SExpressionTree const& tree() const noexcept { return _tree; }

    /// The caps on reading text.
    [[nodiscard]] TextLimits const& limits() const noexcept { return _limits; }

    // Nodes.
    [[nodiscard]] bool isKeyword(NodeId node, std::string_view keyword) const noexcept;
    [[nodiscard]] bool isListOf(NodeId node, std::string_view keyword) const noexcept;
    [[nodiscard]] bool isList(NodeId node) const noexcept;
    [[nodiscard]] bool isAtom(NodeId node, TokenKind kind) const noexcept;
    /// Whether @p node is a Keyword atom spelled as an unsigned integer (the spec's `nat`).
    [[nodiscard]] bool isNatural(NodeId node) const noexcept;
    /// Whether @p node is a Keyword atom spelled as a number (isNumberLiteral()).
    [[nodiscard]] bool isNumber(NodeId node) const;
    /// Whether @p node is an index: an identifier or a `nat`.
    [[nodiscard]] bool isIndex(NodeId node) const noexcept;
    [[nodiscard]] Cursor bodyOf(NodeId list) const noexcept;

    // Errors.
    [[nodiscard]] TextError unexpected(NodeId node) const;
    [[nodiscard]] TextError unexpectedHead(NodeId list) const;
    [[nodiscard]] static TextError missing(Cursor const& cursor);
    [[nodiscard]] Outcome expectEnd(Cursor const& cursor) const;
    [[nodiscard]] Outcome expectEndAfterLists(Cursor const& cursor) const;
    [[nodiscard]] std::expected<NodeId, TextError> required(Cursor& cursor) const;

    // Annotations.
    /// Whether an `@name` annotation directly follows @p node: among the annotations right after it.
    [[nodiscard]] bool isNamedAfter(NodeId node) const noexcept;
    /// Accepts the `@name` annotations directly after @p node as naming @p target.
    void noteNames(NodeId node, NameTarget target);
    // Identifiers and indices.
    [[nodiscard]] std::expected<std::string, TextError> identifierOf(NodeId node) const;
    /// Looks an index up in a module index space: an identifier by name, a `nat` as written.
    [[nodiscard]] std::expected<uint32_t, TextError> lookup(IndexSpace space, NodeId node) const;
    /// Looks an index up in the identifiers of @p symbols, which bind into @p space.
    [[nodiscard]] std::expected<uint32_t, TextError> lookup(Symbols const& symbols,
                                                            IndexSpace space,
                                                            NodeId node) const;

    // Literals.
    /// Reads a `nat` of @p width: "i8 constant out of range" and so on when it exceeds it.
    [[nodiscard]] std::expected<uint64_t, TextError> readNatural(NodeId node, IntegerWidth width) const;
    /// Reads the `nat` after @p prefix in a Keyword atom's text, as in `offset=8`.
    [[nodiscard]] std::expected<uint64_t, TextError> readNaturalAfter(NodeId node,
                                                                      std::string_view prefix,
                                                                      IntegerWidth width) const;
    [[nodiscard]] std::expected<std::string, TextError> readName(Cursor& cursor) const;
    [[nodiscard]] std::expected<std::vector<std::byte>, TextError> readStrings(Cursor& cursor) const;

    // Types.
    [[nodiscard]] std::expected<binary::ValueType, TextError> readValueType(NodeId node) const;
    /// Reads the value types of a `(param …)` or `(result …)` list without identifiers, appending them.
    [[nodiscard]] Outcome readValueTypes(NodeId list, std::vector<binary::ValueType>& types) const;
    [[nodiscard]] std::expected<binary::HeapType, TextError> readHeapType(NodeId node) const;
    [[nodiscard]] std::expected<binary::ValueType, TextError> readReferenceType(Cursor& cursor) const;
    [[nodiscard]] std::expected<TypeUse, TextError> readTypeUse(Cursor& cursor, ParamNames names) const;
    /// The type index a type use denotes, appending an implicit type when it needs one.
    [[nodiscard]] std::expected<uint32_t, TextError> resolve(TypeUse const& use);

    // What instructions add to the module.
    /// Appends a v128.const's or i8x16.shuffle's 16 bytes to binary::Module::v128Literals.
    /// @return Their index there.
    [[nodiscard]] uint32_t addV128Literal(std::array<std::byte, 16> const& bytes);

  private:
    // Errors.
    /// Defers an error to the end of the module, replacing an earlier one of an earlier field.
    /// @param error The error, located only if it is reported.
    void defer(DeferredError error);

    // Identifiers and indices.
    [[nodiscard]] std::expected<uint32_t, TextError> bind(IndexSpace space, NodeId identifier);
    [[nodiscard]] Outcome bindLocal(LocalScope& scope, NodeId identifier, uint32_t index) const;
    uint32_t allocate(IndexSpace space) noexcept { return _next[std::to_underlying(space)]++; }

    // Types.
    [[nodiscard]] std::expected<binary::GlobalType, TextError> readGlobalType(Cursor& cursor) const;
    [[nodiscard]] binary::AddressType readAddressType(Cursor& cursor) const noexcept;
    [[nodiscard]] std::expected<binary::Limits, TextError> readLimits(Cursor& cursor,
                                                                      binary::AddressType addressType) const;
    [[nodiscard]] std::expected<uint8_t, TextError> readPageSize(NodeId list) const;
    [[nodiscard]] std::expected<binary::TableType, TextError> readTableType(Cursor& cursor) const;
    [[nodiscard]] std::expected<binary::MemoryType, TextError> readMemoryType(Cursor& cursor) const;
    [[nodiscard]] Outcome readParams(NodeId list, TypeUse& use, ParamNames names) const;
    [[nodiscard]] Outcome readSignature(Cursor& cursor, TypeUse& use, ParamNames names) const;
    [[nodiscard]] std::expected<uint32_t, TextError> paramCountOf(TypeUse const& use) const;

    // Fields.
    void skipIdentifier(Cursor& cursor) const noexcept;
    [[nodiscard]] Outcome readInlineExports(Cursor& cursor, binary::ExternKind kind, uint32_t index);
    [[nodiscard]] std::expected<std::pair<std::string, std::string>, TextError> readInlineImport(
        NodeId list) const;
    void noteImport(NodeId field);
    void noteDefinition(IndexSpace space);
    void addImport(std::string moduleName, binary::Import import);
    void addParamNames(binary::NameKind kind, uint32_t index, std::vector<ParamName> const& names);
    /// Reads an import descriptor, `(func …)`, `(table …)` and so on, its identifier skipped (phase 1
    /// bound it). A function's or tag's is a full type use, `(type x)` and inline parameters and
    /// results together included: the reference grammar allows either alone in an `(import …)`
    /// descriptor, and Isoline accepts both together, as wasm-tools does, checking that they match.
    /// @param list The descriptor.
    /// @param use Receives a function's or tag's type use, for its parameter names.
    /// @return The import, without its names and module index.
    [[nodiscard]] std::expected<binary::Import, TextError> readDescriptor(NodeId list, TypeUse& use);
    /// Reads a constant expression: instructions without locals or labels around them.
    [[nodiscard]] std::expected<binary::ConstExpr, TextError> readExpression(InstructionNodes nodes);
    [[nodiscard]] std::expected<binary::ConstExpr, TextError> readOffset(NodeId node);
    [[nodiscard]] static binary::ConstExpr zeroOffset(binary::AddressType addressType);
    [[nodiscard]] Outcome readIndices(Cursor& cursor, binary::ElementSegment& segment) const;
    [[nodiscard]] Outcome readExpressions(Cursor& cursor, binary::ElementSegment& segment);
    [[nodiscard]] Outcome readElementList(Cursor& cursor,
                                          binary::ElementSegment& segment,
                                          IndexAbbreviation abbreviation);
    [[nodiscard]] Outcome readLocals(NodeId list,
                                     uint32_t functionIndex,
                                     binary::Function& function,
                                     LocalScope& scope,
                                     TypeUse const& use,
                                     std::optional<uint32_t>& paramCount);
    // Annotations.
    void noteModuleAnnotations(NodeId module);
    void noteBranchHints(NodeId field, uint32_t functionIndex, InstructionReader const& reader);

    SExpressionTree const& _tree; ///< The tree the fields are in.
    TextLimits const& _limits;    ///< The caps on reading text.
    binary::Module _module;       ///< The module read so far.

    FunctionTypes _types; ///< _module.types, with the index implicit types are looked up in.

    std::array<Symbols, ModuleSpaceCount> _symbols;   ///< Each module index space's identifiers.
    std::array<uint32_t, ModuleSpaceCount> _bound {}; ///< Phase 1's next index per space.
    std::array<uint32_t, ModuleSpaceCount> _next {};  ///< Phase 3's next index per space.
    binary::NameSection _names;                       ///< The names the identifiers give.

    std::size_t _ordinal = 0;                  ///< The field phase 3 reads, counted from 0.
    std::optional<IndexSpace> _lastDefinition; ///< The space of the last definition read.
    std::size_t _lastDefinitionOrdinal = 0;    ///< Its field.
    std::size_t _importsSinceDefinition = 0;   ///< Imports read since it.
    std::optional<NodeId> _lastStart;          ///< The index of the last start field read.
    std::size_t _lastStartOrdinal = 0;         ///< Its field.
    std::optional<DeferredError> _deferred;    ///< The deferred error to report, if any.
    AnnotationSites _sites;                    ///< Where the known annotations read are accepted.
    std::vector<NodeId> _annotations;          ///< The module's known annotations, in source order.
};

} // namespace isoline::text
