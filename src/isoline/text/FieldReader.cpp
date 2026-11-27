// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/FieldReader.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/text/IntegerLiteral.hpp>
#include <isoline/text/Keywords.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/LiteralError.hpp>
#include <isoline/text/ModuleReader.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>

namespace isoline::text
{

namespace
{
    using binary::AbstractHeapType;
    using binary::AddressType;
    using binary::Limits;
    using binary::MemoryType;
    using binary::NameAssoc;
    using binary::ValueKind;
    using binary::ValueType;

    /// How an identifier is printed in a message, as the reference interpreter's `print` does: `$` and
    /// the name, or `$"…"` with escapes when the name holds a character `string_of_name` escapes.
    /// @param name The identifier's name, valid UTF-8.
    /// @return The identifier as the reference prints it.
    std::string printedIdentifier(std::string_view name)
    {
        auto escaped = std::string {};
        auto isPlain = true;
        auto remaining = name;
        while (!remaining.empty())
        {
            auto const lead = static_cast<unsigned char>(remaining.front());
            // A UTF-8 lead byte's leading ones count the sequence's bytes; an ASCII byte has none.
            auto const length = static_cast<std::size_t>(std::max(1, std::countl_one(lead)));
            auto codePoint = uint32_t { lead } & (length == 1 ? 0x7FU : 0x7FU >> length);
            for (auto const byte: remaining.substr(1, length - 1))
                codePoint = (codePoint << 6U) | (static_cast<unsigned char>(byte) & 0x3FU);
            remaining.remove_prefix(length);
            if (codePoint < 0x20 || codePoint >= 0x7F)
            {
                escaped += std::format("\\u{{{:02x}}}", codePoint);
                isPlain = false;
                continue;
            }
            if (codePoint == '"' || codePoint == '\\')
            {
                escaped += '\\';
                isPlain = false;
            }
            escaped += static_cast<char>(codePoint);
        }
        return isPlain ? "$" + escaped : "$\"" + escaped + '"';
    }
} // namespace

// ---------------------------------------------------------------------------------------------
// Nodes and errors

bool FieldReader::isList(NodeId node) const noexcept
{
    return node != NoNode && _tree.nodes()[node].kind == NodeKind::List;
}

bool FieldReader::isAtom(NodeId node, TokenKind kind) const noexcept
{
    return node != NoNode && _tree.nodes()[node].kind == NodeKind::Atom
           && _tree.nodes()[node].tokenKind == kind;
}

bool FieldReader::isKeyword(NodeId node, std::string_view keyword) const noexcept
{
    return isAtom(node, TokenKind::Keyword) && _tree.textOf(node) == keyword;
}

bool FieldReader::isListOf(NodeId node, std::string_view keyword) const noexcept
{
    return isList(node) && headKeyword(_tree, node) == keyword;
}

bool FieldReader::isNatural(NodeId node) const noexcept
{
    if (!isAtom(node, TokenKind::Keyword))
        return false;
    auto const first = _tree.textOf(node).front();
    return first >= '0' && first <= '9';
}

bool FieldReader::isNumber(NodeId node) const
{
    return isAtom(node, TokenKind::Keyword) && isNumberLiteral(_tree.textOf(node));
}

bool FieldReader::isIndex(NodeId node) const noexcept
{
    return isAtom(node, TokenKind::Identifier) || isNatural(node);
}

Cursor FieldReader::bodyOf(NodeId list) const noexcept
{
    auto cursor = Cursor { _tree, list };
    cursor.take(); // The keyword the list starts with.
    return cursor;
}

TextError FieldReader::unexpected(NodeId node) const
{
    return unexpectedKeyword(_tree, node);
}

TextError FieldReader::unexpectedHead(NodeId list) const
{
    auto cursor = Cursor { _tree, list };
    if (cursor.atEnd())
        return missing(cursor);
    return unexpected(cursor.peek());
}

TextError FieldReader::missing(Cursor const& cursor)
{
    return TextError { TextErrorCode::UnexpectedToken, cursor.endPosition() };
}

Outcome FieldReader::expectEnd(Cursor const& cursor) const
{
    if (cursor.atEnd())
        return {};
    return std::unexpected { unexpected(cursor.peek()) };
}

Outcome FieldReader::expectEndAfterLists(Cursor const& cursor) const
{
    if (cursor.atEnd())
        return {};
    if (isList(cursor.peek()))
        return std::unexpected { unexpectedHead(cursor.peek()) };
    return std::unexpected { unexpected(cursor.peek()) };
}

std::expected<NodeId, TextError> FieldReader::required(Cursor& cursor) const
{
    if (cursor.atEnd())
        return std::unexpected { missing(cursor) };
    return cursor.take();
}

void FieldReader::defer(DeferredError error)
{
    if (!_deferred || _deferred->ordinal <= error.ordinal)
        _deferred = std::move(error);
}

// ---------------------------------------------------------------------------------------------
// Annotations

namespace
{
    /// The annotations right after a node, up to its next sibling that is none.
    /// @param tree The tree.
    /// @param node The node.
    /// @return Their ids, in source order.
    [[nodiscard]] auto annotationsAfter(SExpressionTree const& tree, NodeId node) noexcept
    {
        return tree.siblingsAfter(node) | std::views::take_while([&tree](NodeId next) {
                   return tree.nodes()[next].kind == NodeKind::Annotation;
               });
    }
} // namespace

bool FieldReader::isNamedAfter(NodeId node) const noexcept
{
    return std::ranges::any_of(annotationsAfter(_tree, node), [this](NodeId next) {
        return _tree.annotationKindOf(next) == AnnotationKind::Name;
    });
}

void FieldReader::noteNames(NodeId node, NameTarget target)
{
    for (auto const next: annotationsAfter(_tree, node))
        if (_tree.annotationKindOf(next) == AnnotationKind::Name)
            _sites.names.insert_or_assign(next, target);
}

// ---------------------------------------------------------------------------------------------
// Identifiers and indices

std::expected<std::string, TextError> FieldReader::identifierOf(NodeId node) const
{
    return identifierName(_tree.source(), _tree.tokenOf(node));
}

std::expected<uint32_t, TextError> FieldReader::bind(IndexSpace space, NodeId identifier)
{
    auto const index = _bound[std::to_underlying(space)]++;
    if (identifier == NoNode)
        return index;
    auto name = identifierOf(identifier);
    if (!name)
        return std::unexpected { std::move(name.error()) };
    auto const& row = rowOf(space);
    auto const [entry, isNew] = _symbols[std::to_underlying(space)].try_emplace(*name, index);
    if (!isNew)
        return std::unexpected { TextError { TextErrorCode::DuplicateIdentifier,
                                             _tree.positionOf(identifier),
                                             std::string { row.category } + ' '
                                                 + printedIdentifier(*name) } };
    _names.direct[std::to_underlying(row.names)].push_back(
        NameAssoc { .index = index, .name = std::move(*name) });
    return index;
}

Outcome FieldReader::bindLocal(LocalScope& scope, NodeId identifier, uint32_t index) const
{
    auto name = identifierOf(identifier);
    if (!name)
        return std::unexpected { std::move(name.error()) };
    if (!scope.symbols.try_emplace(*name, index).second)
        return std::unexpected { TextError { TextErrorCode::DuplicateIdentifier,
                                             _tree.positionOf(identifier),
                                             std::string { rowOf(IndexSpace::Local).category } + ' '
                                                 + printedIdentifier(*name) } };
    scope.names.push_back(NameAssoc { .index = index, .name = std::move(*name) });
    return {};
}

std::expected<uint32_t, TextError> FieldReader::lookup(IndexSpace space, NodeId node) const
{
    Require(std::to_underlying(space) < ModuleSpaceCount);
    return lookup(_symbols[std::to_underlying(space)], space, node);
}

std::expected<uint32_t, TextError> FieldReader::lookup(Symbols const& symbols,
                                                       IndexSpace space,
                                                       NodeId node) const
{
    if (isAtom(node, TokenKind::Identifier))
    {
        auto name = identifierOf(node);
        if (!name)
            return std::unexpected { std::move(name.error()) };
        auto const found = symbols.find(*name);
        if (found == symbols.end())
            return std::unexpected { TextError { TextErrorCode::UnknownIdentifier,
                                                 _tree.positionOf(node),
                                                 std::string { rowOf(space).category } + ' '
                                                     + printedIdentifier(*name) } };
        return found->second;
    }
    return readNatural(node, IntegerWidth::I32).transform([](uint64_t value) {
        return static_cast<uint32_t>(value);
    });
}

// ---------------------------------------------------------------------------------------------
// Literals

std::expected<uint64_t, TextError> FieldReader::readNatural(NodeId node, IntegerWidth width) const
{
    return readNaturalAfter(node, {}, width);
}

std::expected<uint64_t, TextError> FieldReader::readNaturalAfter(NodeId node,
                                                                 std::string_view prefix,
                                                                 IntegerWidth width) const
{
    if (!isAtom(node, TokenKind::Keyword) || !_tree.textOf(node).starts_with(prefix))
        return std::unexpected { unexpected(node) };
    auto value =
        parseInteger(_tree.textOf(node).substr(prefix.size()), width, IntegerSyntax::Unsigned, _limits);
    if (value)
        return *value;
    switch (value.error())
    {
        case LiteralError::NotANumber: return std::unexpected { unexpected(node) };
        case LiteralError::OutOfRange:
            // The reference's nat8, nat32 and nat64 name the type: "i8 constant out of range".
            return std::unexpected { TextError { TextErrorCode::ConstantOutOfRange,
                                                 _tree.positionOf(node),
                                                 std::format("i{}", std::to_underlying(width)) } };
        case LiteralError::TooLong: break;
    }
    return std::unexpected { TextError { TextErrorCode::NumberTooLong, _tree.positionOf(node) } };
}

std::expected<std::string, TextError> FieldReader::readName(Cursor& cursor) const
{
    if (cursor.atEnd())
        return std::unexpected { missing(cursor) };
    if (!isAtom(cursor.peek(), TokenKind::String))
        return std::unexpected { unexpected(cursor.peek()) };
    return decodeName(_tree.source(), _tree.tokenOf(cursor.take()));
}

std::expected<std::vector<std::byte>, TextError> FieldReader::readStrings(Cursor& cursor) const
{
    auto bytes = std::vector<std::byte> {};
    while (!cursor.atEnd())
    {
        if (!isAtom(cursor.peek(), TokenKind::String))
            return std::unexpected { unexpected(cursor.peek()) };
        auto const string = decodeString(_tree.source(), _tree.tokenOf(cursor.take()));
        Require(string.has_value()); // The lexer read it as a String token, so it decodes.
        std::ranges::transform(*string, std::back_inserter(bytes), [](char byte) { return std::byte(byte); });
    }
    return bytes;
}

// ---------------------------------------------------------------------------------------------
// Types

std::expected<ValueType, TextError> FieldReader::readValueType(NodeId node) const
{
    if (isAtom(node, TokenKind::Keyword))
    {
        auto const found =
            std::ranges::find(ValueTypeKeywords, _tree.textOf(node), &ValueTypeKeyword::keyword);
        if (found != ValueTypeKeywords.end())
            return found->type;
        return std::unexpected { unexpected(node) };
    }
    if (!isListOf(node, "ref"))
        return std::unexpected { isList(node) ? unexpectedHead(node) : unexpected(node) };

    auto cursor = bodyOf(node);
    auto type = ValueType { .kind = ValueKind::Ref, .nullable = binary::Nullable::No };
    if (isKeyword(cursor.peek(), "null"))
    {
        cursor.take();
        type.nullable = binary::Nullable::Yes;
    }
    auto const heapNode = required(cursor);
    if (!heapNode)
        return std::unexpected { heapNode.error() };
    auto const heap = readHeapType(*heapNode);
    if (!heap)
        return std::unexpected { heap.error() };
    type.heap = *heap;
    return expectEnd(cursor).transform([&] { return type; });
}

std::expected<binary::HeapType, TextError> FieldReader::readHeapType(NodeId node) const
{
    if (isIndex(node))
        return lookup(IndexSpace::Type, node).transform([](uint32_t index) {
            return binary::HeapType { .kind = AbstractHeapType::Concrete, .typeIndex = index };
        });
    auto const found =
        isAtom(node, TokenKind::Keyword)
            ? std::ranges::find(HeapTypeKeywords, _tree.textOf(node), &HeapTypeKeyword::keyword)
            : HeapTypeKeywords.end();
    if (found == HeapTypeKeywords.end())
        return std::unexpected { unexpected(node) };
    return binary::HeapType { .kind = found->heap };
}

std::expected<ValueType, TextError> FieldReader::readReferenceType(Cursor& cursor) const
{
    auto const node = required(cursor);
    if (!node)
        return std::unexpected { node.error() };
    auto const type = readValueType(*node);
    if (type && type->kind != ValueKind::Ref)
        return std::unexpected { unexpected(*node) };
    return type;
}

std::expected<binary::GlobalType, TextError> FieldReader::readGlobalType(Cursor& cursor) const
{
    auto const node = required(cursor);
    if (!node)
        return std::unexpected { node.error() };
    if (!isListOf(*node, "mut"))
        return readValueType(*node).transform(
            [](ValueType type) { return binary::GlobalType { .type = type }; });
    auto inner = bodyOf(*node);
    auto const typeNode = required(inner);
    if (!typeNode)
        return std::unexpected { typeNode.error() };
    auto const type = readValueType(*typeNode);
    if (!type)
        return std::unexpected { type.error() };
    return expectEnd(inner).transform(
        [&] { return binary::GlobalType { .type = *type, .mutability = binary::Mutability::Var }; });
}

AddressType FieldReader::readAddressType(Cursor& cursor) const noexcept
{
    if (!isAtom(cursor.peek(), TokenKind::Keyword))
        return AddressType::I32;
    auto const found =
        std::ranges::find(AddressTypeKeywords, _tree.textOf(cursor.peek()), &AddressTypeKeyword::keyword);
    if (found == AddressTypeKeywords.end())
        return AddressType::I32;
    cursor.take();
    return found->type;
}

std::expected<Limits, TextError> FieldReader::readLimits(Cursor& cursor, AddressType addressType) const
{
    // Limits are read as u64 whatever the address type: an i32 limit beyond 2^32 is the validator's
    // to refuse, as "table size" or "memory size".
    auto const minimumNode = required(cursor);
    if (!minimumNode)
        return std::unexpected { minimumNode.error() };
    auto const minimum = readNatural(*minimumNode, IntegerWidth::I64);
    if (!minimum)
        return std::unexpected { minimum.error() };
    auto limits = Limits { .min = *minimum, .max = std::nullopt, .addressType = addressType };
    if (isNatural(cursor.peek()))
    {
        auto const maximum = readNatural(cursor.take(), IntegerWidth::I64);
        if (!maximum)
            return std::unexpected { maximum.error() };
        limits.max = *maximum;
    }
    return limits;
}

std::expected<uint8_t, TextError> FieldReader::readPageSize(NodeId list) const
{
    auto cursor = bodyOf(list);
    auto const node = required(cursor);
    if (!node)
        return std::unexpected { node.error() };
    auto const size = readNatural(*node, IntegerWidth::I32);
    if (!size)
        return std::unexpected { size.error() };
    if (!std::has_single_bit(*size))
        return std::unexpected { TextError { TextErrorCode::InvalidCustomPageSize, _tree.positionOf(list) } };
    return expectEnd(cursor).transform([&] { return static_cast<uint8_t>(std::countr_zero(*size)); });
}

std::expected<binary::TableType, TextError> FieldReader::readTableType(Cursor& cursor) const
{
    auto const addressType = readAddressType(cursor);
    auto const limits = readLimits(cursor, addressType);
    if (!limits)
        return std::unexpected { limits.error() };
    return readReferenceType(cursor).transform(
        [&](ValueType element) { return binary::TableType { .element = element, .limits = *limits }; });
}

std::expected<MemoryType, TextError> FieldReader::readMemoryType(Cursor& cursor) const
{
    auto const addressType = readAddressType(cursor);
    auto const limits = readLimits(cursor, addressType);
    if (!limits)
        return std::unexpected { limits.error() };
    auto type = MemoryType { .limits = *limits };
    if (isListOf(cursor.peek(), "pagesize"))
    {
        auto const pageSizeLog2 = readPageSize(cursor.take());
        if (!pageSizeLog2)
            return std::unexpected { pageSizeLog2.error() };
        type.pageSizeLog2 = *pageSizeLog2;
    }
    return type;
}

Outcome FieldReader::readParams(NodeId list, TypeUse& use, ParamNames names) const
{
    auto const keyword = Cursor { _tree, list }.peek();
    auto cursor = bodyOf(list);
    auto const identifier = isAtom(cursor.peek(), TokenKind::Identifier) ? cursor.peek() : NoNode;
    // A parameter an identifier or an `@name` names is one type, `(param $id? (@name "…")? t)`, as
    // wasm-tools reads it; where none may be named, an `@name` is left misplaced.
    if (identifier != NoNode && names == ParamNames::Refused)
        return std::unexpected { unexpected(identifier) };
    if (identifier != NoNode || (names == ParamNames::Allowed && isNamedAfter(keyword)))
    {
        if (identifier != NoNode)
            cursor.take();
        auto const typeNode = required(cursor);
        if (!typeNode)
            return std::unexpected { typeNode.error() };
        auto const type = readValueType(*typeNode);
        if (!type)
            return std::unexpected { type.error() };
        use.paramNames.push_back(ParamName { .index = static_cast<uint32_t>(use.inlineType.params.size()),
                                             .identifier = identifier,
                                             .namedAfter = identifier != NoNode ? identifier : keyword });
        use.inlineType.params.push_back(*type);
        return expectEnd(cursor);
    }
    return readValueTypes(list, use.inlineType.params);
}

Outcome FieldReader::readValueTypes(NodeId list, std::vector<ValueType>& types) const
{
    auto cursor = bodyOf(list);
    while (!cursor.atEnd())
    {
        auto const type = readValueType(cursor.take());
        if (!type)
            return std::unexpected { type.error() };
        types.push_back(*type);
    }
    return {};
}

std::expected<TypeUse, TextError> FieldReader::readTypeUse(Cursor& cursor, ParamNames names) const
{
    auto use = TypeUse {};
    if (isListOf(cursor.peek(), "type"))
    {
        auto inner = bodyOf(cursor.take());
        auto const node = required(inner);
        if (!node)
            return std::unexpected { node.error() };
        if (auto const end = expectEnd(inner); !end)
            return std::unexpected { end.error() };
        use.explicitNode = *node;
    }
    if (auto const signature = readSignature(cursor, use, names); !signature)
        return std::unexpected { signature.error() };
    // The type use is looked up once it is parsed whole: the grammar's errors come first.
    if (use.explicitNode == NoNode)
        return use;
    return lookup(IndexSpace::Type, use.explicitNode).transform([&](uint32_t index) {
        use.explicitIndex = index;
        return std::move(use);
    });
}

Outcome FieldReader::readSignature(Cursor& cursor, TypeUse& use, ParamNames names) const
{
    while (isListOf(cursor.peek(), "param"))
        if (auto const params = readParams(cursor.take(), use, names); !params)
            return params;
    while (isListOf(cursor.peek(), "result"))
        if (auto const results = readValueTypes(cursor.take(), use.inlineType.results); !results)
            return results;
    // A grammar error comes before any semantic one, as the reference parses before it resolves:
    // a type use or parameter out of order is refused here, before the type use is looked up.
    if (isListOf(cursor.peek(), "param") || isListOf(cursor.peek(), "type"))
        return std::unexpected { unexpectedHead(cursor.peek()) };
    return {};
}

std::expected<uint32_t, TextError> FieldReader::paramCountOf(TypeUse const& use) const
{
    auto const isWrittenInline = !use.inlineType.params.empty() || !use.inlineType.results.empty();
    if (use.explicitNode == NoNode || isWrittenInline)
        return static_cast<uint32_t>(use.inlineType.params.size());
    // A bare `(type x)` is looked up only when its parameters are needed, as parser.mly's
    // inline_functype_explicit defers it: an unknown one is otherwise the validator's to refuse.
    if (use.explicitIndex >= _module.types.size())
        return std::unexpected { TextError { TextErrorCode::UnknownIdentifier,
                                             _tree.positionOf(use.explicitNode),
                                             "type " + std::to_string(use.explicitIndex) } };
    return static_cast<uint32_t>(_module.types[use.explicitIndex].params.size());
}

std::expected<uint32_t, TextError> FieldReader::resolve(TypeUse const& use)
{
    if (use.explicitNode == NoNode)
        return _types.use(use.inlineType);
    if (use.inlineType.params.empty() && use.inlineType.results.empty())
        return use.explicitIndex;
    if (use.explicitIndex >= _module.types.size())
        return std::unexpected { TextError { TextErrorCode::UnknownIdentifier,
                                             _tree.positionOf(use.explicitNode),
                                             "type " + std::to_string(use.explicitIndex) } };
    if (_module.types[use.explicitIndex] != use.inlineType)
        return std::unexpected { TextError { TextErrorCode::InlineFunctionType,
                                             _tree.positionOf(use.explicitNode) } };
    return use.explicitIndex;
}

uint32_t FieldReader::addV128Literal(std::array<std::byte, 16> const& bytes)
{
    _module.v128Literals.push_back(bytes);
    return static_cast<uint32_t>(_module.v128Literals.size() - 1);
}

} // namespace isoline::text
