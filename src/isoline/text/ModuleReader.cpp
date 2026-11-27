// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/ModuleReader.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/meta/EnumRows.hpp>
#include <isoline/text/Annotations.hpp>
#include <isoline/text/FieldReader.hpp>
#include <isoline/text/InstructionReader.hpp>
#include <isoline/text/Keywords.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

namespace
{
    using binary::AddressType;
    using binary::ConstExpr;
    using binary::ElementSegment;
    using binary::ExternKind;
    using binary::Import;
    using binary::Instruction;
    using binary::Limits;
    using binary::MemoryType;
    using binary::NameAssoc;
    using binary::NameKind;
    using binary::Opcode;
    using binary::SegmentMode;
    using binary::ValueType;

    /// The row of a descriptor keyword.
    /// @return The row, or null when @p keyword names no kind of entity.
    ExternRow const* externRowOf(std::string_view keyword) noexcept
    {
        auto const found = std::ranges::find(ExternRows, keyword, &ExternRow::keyword);
        return found == ExternRows.end() ? nullptr : &*found;
    }

    /// The name of the synthesized section the reader builds from identifiers, and where it goes.
    inline constexpr auto NameSectionRow = *std::ranges::find(
        binary::SynthesizedSections, binary::NameSectionName, &binary::SynthesizedSection::name);

    static_assert(NameSectionRow.placement == binary::SynthesizedPlacement::Last,
                  "the reader appends the name section after every other section, as SynthesizedSections "
                  "places it");

    /// Whether an import descriptor may bind an identifier: not when several compact items share it.
    enum class ImportIdentifiers : uint8_t
    {
        Refused, ///< The descriptor stands for several imports, or none.
        Allowed, ///< The descriptor stands for one import.
    };

    /// A step of reading one module field.
    using FieldStep = Outcome (FieldReader::*)(NodeId field);

    /// One kind of module field: its keyword and how each phase reads it.
    struct FieldRow
    {
        std::string_view keyword; ///< The field's keyword.
        FieldStep bind;           ///< Phase 1: binds the field's identifiers, in source order.
        FieldStep define;         ///< Phase 2: defines explicit types; reads nothing else.
        FieldStep read;           ///< Phase 3: reads the field into the module.
    };

    /// Every kind of module field, by keyword. Adding a kind of field is adding a row.
    inline constexpr auto FieldRows = std::to_array<FieldRow>({
        { .keyword = "type",
          .bind = &FieldReader::bindNamed,
          .define = &FieldReader::defineType,
          .read = &FieldReader::skip },
        { .keyword = "import",
          .bind = &FieldReader::bindImport,
          .define = &FieldReader::skip,
          .read = &FieldReader::readImport },
        { .keyword = "func",
          .bind = &FieldReader::bindNamed,
          .define = &FieldReader::skip,
          .read = &FieldReader::readFunction },
        { .keyword = "table",
          .bind = &FieldReader::bindTable,
          .define = &FieldReader::skip,
          .read = &FieldReader::readTable },
        { .keyword = "memory",
          .bind = &FieldReader::bindMemory,
          .define = &FieldReader::skip,
          .read = &FieldReader::readMemory },
        { .keyword = "global",
          .bind = &FieldReader::bindNamed,
          .define = &FieldReader::skip,
          .read = &FieldReader::readGlobal },
        { .keyword = "tag",
          .bind = &FieldReader::bindNamed,
          .define = &FieldReader::skip,
          .read = &FieldReader::readTag },
        { .keyword = "export",
          .bind = &FieldReader::skip,
          .define = &FieldReader::skip,
          .read = &FieldReader::readExport },
        { .keyword = "start",
          .bind = &FieldReader::skip,
          .define = &FieldReader::skip,
          .read = &FieldReader::readStart },
        { .keyword = "elem",
          .bind = &FieldReader::bindNamed,
          .define = &FieldReader::skip,
          .read = &FieldReader::readElement },
        { .keyword = "data",
          .bind = &FieldReader::bindNamed,
          .define = &FieldReader::skip,
          .read = &FieldReader::readData },
    });
    static_assert(meta::firstRepeatedKey(FieldRows, &FieldRow::keyword) == meta::NoRepeatedKey,
                  "FieldRows must hold each field keyword once, as the reader takes the first row it names");

    /// The index space a field of each FieldRows keyword binds its own identifier into.
    struct FieldSpace
    {
        std::string_view keyword; ///< The field's keyword.
        IndexSpace space;         ///< The space its `$id` binds into.
    };

    /// The fields that bind an identifier of their own, and the space each binds it into.
    inline constexpr auto FieldSpaces = std::to_array<FieldSpace>({
        { .keyword = "type", .space = IndexSpace::Type },
        { .keyword = "func", .space = IndexSpace::Function },
        { .keyword = "table", .space = IndexSpace::Table },
        { .keyword = "memory", .space = IndexSpace::Memory },
        { .keyword = "global", .space = IndexSpace::Global },
        { .keyword = "tag", .space = IndexSpace::Tag },
        { .keyword = "elem", .space = IndexSpace::Element },
        { .keyword = "data", .space = IndexSpace::Data },
    });
    static_assert(
        meta::firstRepeatedKey(FieldSpaces, &FieldSpace::keyword) == meta::NoRepeatedKey,
        "FieldSpaces must hold each field keyword once, as the reader takes the first row it names");
} // namespace

// ---------------------------------------------------------------------------------------------
// Field helpers

void FieldReader::skipIdentifier(Cursor& cursor) const noexcept
{
    if (isAtom(cursor.peek(), TokenKind::Identifier))
        cursor.take();
}

Outcome FieldReader::readInlineExports(Cursor& cursor, ExternKind kind, uint32_t index)
{
    while (isListOf(cursor.peek(), "export"))
    {
        auto inner = bodyOf(cursor.take());
        auto name = readName(inner);
        if (!name)
            return std::unexpected { std::move(name.error()) };
        if (auto const end = expectEnd(inner); !end)
            return end;
        _module.exports.push_back(binary::Export { .name = std::move(*name), .kind = kind, .index = index });
    }
    return {};
}

std::expected<std::pair<std::string, std::string>, TextError> FieldReader::readInlineImport(NodeId list) const
{
    auto cursor = bodyOf(list);
    auto moduleName = readName(cursor);
    if (!moduleName)
        return std::unexpected { std::move(moduleName.error()) };
    auto name = readName(cursor);
    if (!name)
        return std::unexpected { std::move(name.error()) };
    return expectEnd(cursor).transform(
        [&] { return std::pair { std::move(*moduleName), std::move(*name) }; });
}

void FieldReader::noteImport(NodeId field)
{
    // parser.mly checks, per definition, that no import follows it, once every later field is
    // read: the latest definition an import follows is reported, at the first import after it.
    if (_lastDefinition && _importsSinceDefinition == 0)
        defer(DeferredError { _lastDefinitionOrdinal,
                              TextErrorCode::ImportAfterDefinition,
                              field,
                              std::string { rowOf(*_lastDefinition).category } + " definition" });
    ++_importsSinceDefinition;
}

void FieldReader::noteDefinition(IndexSpace space)
{
    _lastDefinition = space;
    _lastDefinitionOrdinal = _ordinal;
    _importsSinceDefinition = 0;
}

void FieldReader::addImport(std::string moduleName, Import import)
{
    import.moduleIndex = static_cast<uint32_t>(_module.importModules.size());
    _module.importModules.push_back(std::move(moduleName));
    _module.imports.push_back(std::move(import));
}

void FieldReader::addParamNames(NameKind kind, uint32_t index, std::vector<ParamName> const& names)
{
    auto map = binary::IndirectNameMap { .index = index, .names = {} };
    for (auto const& param: names)
    {
        noteNames(param.namedAfter, NameTarget { .kind = kind, .index = index, .inner = param.index });
        if (param.identifier == NoNode)
            continue;
        if (auto name = identifierOf(param.identifier); name)
            map.names.push_back(NameAssoc { .index = param.index, .name = std::move(*name) });
    }
    if (!map.names.empty())
        _names.indirect[std::to_underlying(kind)].push_back(std::move(map));
}

std::expected<Import, TextError> FieldReader::readDescriptor(NodeId list, TypeUse& use)
{
    auto const* row = externRowOf(headKeyword(_tree, list));
    if (!isList(list) || row == nullptr)
        return std::unexpected { isList(list) ? unexpectedHead(list) : unexpected(list) };
    auto cursor = bodyOf(list);
    skipIdentifier(cursor);
    auto import = Import {};
    import.kind = row->kind;
    switch (row->kind)
    {
        case ExternKind::Func:
        case ExternKind::Tag: {
            auto typeUse = readTypeUse(cursor, ParamNames::Allowed);
            if (!typeUse)
                return std::unexpected { std::move(typeUse.error()) };
            use = std::move(*typeUse);
            auto const typeIndex = resolve(use);
            if (!typeIndex)
                return std::unexpected { typeIndex.error() };
            import.typeIndex = *typeIndex;
            return expectEndAfterLists(cursor).transform([&] { return std::move(import); });
        }
        case ExternKind::Table: {
            auto const type = readTableType(cursor);
            if (!type)
                return std::unexpected { type.error() };
            import.table = *type;
            break;
        }
        case ExternKind::Memory: {
            auto const type = readMemoryType(cursor);
            if (!type)
                return std::unexpected { type.error() };
            import.memory = *type;
            break;
        }
        case ExternKind::Global: {
            auto const type = readGlobalType(cursor);
            if (!type)
                return std::unexpected { type.error() };
            import.global = *type;
            break;
        }
    }
    return expectEnd(cursor).transform([&] { return std::move(import); });
}

std::expected<ConstExpr, TextError> FieldReader::readExpression(InstructionNodes nodes)
{
    // A constant expression has no locals and no labels around it; its side tables are dropped,
    // as the decoder drops them: no constant instruction has any.
    auto const noLocals = Symbols {};
    auto code = binary::Function {};
    auto reader = InstructionReader { *this, noLocals, code };
    return reader.read(nodes).transform([&] { return std::move(code.body); });
}

std::expected<ConstExpr, TextError> FieldReader::readOffset(NodeId node)
{
    if (isListOf(node, "offset"))
        return readExpression(InstructionNodes { .list = node, .first = bodyOf(node).peek() });
    return readExpression(InstructionNodes { .first = node, .extent = NodeExtent::One });
}

ConstExpr FieldReader::zeroOffset(AddressType addressType)
{
    auto const opcode = addressType == AddressType::I64 ? Opcode::I64Const : Opcode::I32Const;
    return ConstExpr { Instruction { .opcode = opcode }, Instruction { .opcode = Opcode::End } };
}

Outcome FieldReader::readIndices(Cursor& cursor, ElementSegment& segment) const
{
    auto indices = std::vector<uint32_t> {};
    while (!cursor.atEnd())
    {
        auto const index = lookup(IndexSpace::Function, cursor.take());
        if (!index)
            return std::unexpected { index.error() };
        indices.push_back(*index);
    }
    segment.type = binary::NonNullFuncRef;
    segment.items = std::move(indices);
    return {};
}

Outcome FieldReader::readExpressions(Cursor& cursor, ElementSegment& segment)
{
    auto expressions = std::vector<ConstExpr> {};
    while (!cursor.atEnd())
    {
        auto const node = cursor.take();
        if (!isList(node))
            return std::unexpected { unexpected(node) };
        auto expression =
            isListOf(node, "item")
                ? readExpression(InstructionNodes { .list = node, .first = bodyOf(node).peek() })
                : readExpression(InstructionNodes { .first = node, .extent = NodeExtent::One });
        if (!expression)
            return std::unexpected { std::move(expression.error()) };
        expressions.push_back(std::move(*expression));
    }
    segment.items = std::move(expressions);
    return {};
}

Outcome FieldReader::readElementList(Cursor& cursor, ElementSegment& segment, IndexAbbreviation abbreviation)
{
    if (isKeyword(cursor.peek(), "func"))
    {
        cursor.take();
        return readIndices(cursor, segment);
    }
    if (abbreviation == IndexAbbreviation::Allowed && (cursor.atEnd() || isIndex(cursor.peek())))
        return readIndices(cursor, segment);
    auto const type = readReferenceType(cursor);
    if (!type)
        return std::unexpected { type.error() };
    segment.type = *type;
    return readExpressions(cursor, segment);
}

Outcome FieldReader::readLocals(NodeId list,
                                uint32_t functionIndex,
                                binary::Function& function,
                                LocalScope& scope,
                                TypeUse const& use,
                                std::optional<uint32_t>& paramCount)
{
    auto const append = [&function](ValueType type) {
        if (function.locals.empty() || function.locals.back().type != type)
            function.locals.push_back(binary::LocalRun { .count = 0, .type = type });
        ++function.locals.back().count;
        ++function.localCount;
    };

    auto const keyword = Cursor { _tree, list }.peek();
    auto cursor = bodyOf(list);
    auto const identifier = isAtom(cursor.peek(), TokenKind::Identifier) ? cursor.peek() : NoNode;
    // A local an identifier or an `@name` names is one type, as a parameter is (readParams()).
    if (identifier == NoNode && !isNamedAfter(keyword))
    {
        while (!cursor.atEnd())
        {
            auto const type = readValueType(cursor.take());
            if (!type)
                return std::unexpected { type.error() };
            append(*type);
        }
        return {};
    }

    if (identifier != NoNode)
        cursor.take();
    auto const typeNode = required(cursor);
    if (!typeNode)
        return std::unexpected { typeNode.error() };
    auto const type = readValueType(*typeNode);
    if (!type)
        return std::unexpected { type.error() };
    if (!paramCount)
    {
        auto const count = paramCountOf(use);
        if (!count)
            return std::unexpected { count.error() };
        paramCount = *count;
    }
    auto const index = *paramCount + function.localCount;
    if (identifier != NoNode)
        if (auto const bound = bindLocal(scope, identifier, index); !bound)
            return bound;
    noteNames(identifier != NoNode ? identifier : keyword,
              NameTarget { .kind = NameKind::Local, .index = functionIndex, .inner = index });
    append(*type);
    return expectEnd(cursor);
}

// ---------------------------------------------------------------------------------------------
// Annotations

void FieldReader::noteModuleAnnotations(NodeId module)
{
    // The module's own nodes, annotations included: the `(module …)` list's children, or the whole top
    // level of a bare module. Every one of them that is an annotation is a module field, where
    // `@custom` may stand; an `@name` before every field names the module (handler_name.ml's
    // locate_module), the `module` keyword and identifier aside.
    auto const first = module == NoNode ? _tree.firstTopLevel() : _tree.nodes()[module].firstChild;
    auto isBeforeFields = true;
    for (auto const node:
         Siblings { SiblingIterator { _tree, first, SiblingFilter::All }, SiblingIterator {} })
    {
        auto const kind = _tree.annotationKindOf(node);
        if (kind == AnnotationKind::Custom)
            _sites.moduleFields.insert(node);
        else if (kind == AnnotationKind::Name && isBeforeFields)
            _sites.names.insert_or_assign(node, NameTarget { .kind = NameKind::Module });
        else if (isList(node))
            isBeforeFields = false;
    }
}

void FieldReader::noteBranchHints(NodeId field, uint32_t functionIndex, InstructionReader const& reader)
{
    // A hint anywhere in a defined function's field names the instruction after it in source order.
    // The field's annotations are a run of the module's, found by its nodes' range.
    auto const first = std::ranges::lower_bound(_annotations, field + 1);
    auto const last = std::ranges::lower_bound(first, _annotations.end(), _tree.subtreeEnd(field));
    auto hints = std::vector<NodeId> {};
    std::ranges::copy_if(first, last, std::back_inserter(hints), [this](NodeId node) {
        return _tree.annotationKindOf(node) == AnnotationKind::BranchHint;
    });
    if (hints.empty())
        return;
    auto ends = std::vector<uint32_t> {};
    ends.reserve(hints.size());
    for (auto const hint: hints)
        ends.push_back(_tree.nodes()[hint].offset + _tree.nodes()[hint].length);
    auto const instructions = reader.instructionsAfter(ends);
    for (auto const index: std::views::iota(std::size_t { 0 }, hints.size()))
        _sites.hints.insert_or_assign(
            hints[index], HintTarget { .function = functionIndex, .instruction = instructions[index] });
}

// ---------------------------------------------------------------------------------------------
// Phase 1: binding

Outcome FieldReader::skip(NodeId /*field*/)
{
    return {};
}

Outcome FieldReader::bindNamed(NodeId field)
{
    auto const keyword = headKeyword(_tree, field);
    auto const& row = *std::ranges::find(FieldSpaces, keyword, &FieldSpace::keyword);
    auto const head = Cursor { _tree, field }.peek();
    auto cursor = bodyOf(field);
    auto const identifier = isAtom(cursor.peek(), TokenKind::Identifier) ? cursor.peek() : NoNode;
    return bind(row.space, identifier).transform([&](uint32_t index) {
        noteNames(identifier != NoNode ? identifier : head,
                  NameTarget { .kind = rowOf(row.space).names, .index = index });
    });
}

Outcome FieldReader::bindTable(NodeId field)
{
    // A table with an inline `(elem …)` defines an element segment too, after the table.
    auto const bound = bindNamed(field);
    if (!bound)
        return bound;
    auto const holdsSegment =
        std::ranges::any_of(_tree.children(field), [&](NodeId child) { return isListOf(child, "elem"); });
    if (!holdsSegment)
        return {};
    return bind(IndexSpace::Element, NoNode).transform([](uint32_t) {});
}

Outcome FieldReader::bindMemory(NodeId field)
{
    // A memory with an inline `(data …)` defines a data segment too, after the memory.
    auto const bound = bindNamed(field);
    if (!bound)
        return bound;
    auto const holdsSegment =
        std::ranges::any_of(_tree.children(field), [&](NodeId child) { return isListOf(child, "data"); });
    if (!holdsSegment)
        return {};
    return bind(IndexSpace::Data, NoNode).transform([](uint32_t) {});
}

Outcome FieldReader::bindImport(NodeId field)
{
    // What does not parse is left to phase 3, which reports it.
    auto cursor = bodyOf(field);
    cursor.take(); // The module name.
    auto const bindDescriptor =
        [&](NodeId list, std::size_t count, ImportIdentifiers identifiers) -> Outcome {
        auto const* row = externRowOf(headKeyword(_tree, list));
        if (!isList(list) || row == nullptr)
            return {};
        auto const keyword = Cursor { _tree, list }.peek();
        auto inner = bodyOf(list);
        auto const identifier = isAtom(inner.peek(), TokenKind::Identifier) ? inner.peek() : NoNode;
        if (identifier != NoNode && identifiers == ImportIdentifiers::Refused)
            return std::unexpected { TextError { TextErrorCode::IdentifierNotAllowed,
                                                 _tree.positionOf(identifier) } };
        for ([[maybe_unused]] auto const item: std::views::iota(std::size_t { 0 }, count))
        {
            auto const index = bind(row->space, identifier);
            if (!index)
                return std::unexpected { index.error() };
            // A descriptor several imports share names none of them.
            if (identifiers == ImportIdentifiers::Allowed)
                noteNames(identifier != NoNode ? identifier : keyword,
                          NameTarget { .kind = rowOf(row->space).names, .index = *index });
        }
        return {};
    };

    if (isAtom(cursor.peek(), TokenKind::String))
    {
        cursor.take();
        return bindDescriptor(cursor.peek(), 1, ImportIdentifiers::Allowed);
    }
    // The compact forms: items with a descriptor each, or names sharing one descriptor after them.
    auto sharing = std::size_t { 0 };
    while (isListOf(cursor.peek(), "item"))
    {
        auto item = bodyOf(cursor.take());
        item.take(); // The item's name.
        if (item.atEnd())
            ++sharing;
        else if (auto const bound = bindDescriptor(item.peek(), 1, ImportIdentifiers::Allowed); !bound)
            return bound;
    }
    if (cursor.atEnd())
        return {};
    return bindDescriptor(cursor.peek(), sharing, ImportIdentifiers::Refused);
}

// ---------------------------------------------------------------------------------------------
// Phase 2: explicit types

Outcome FieldReader::defineType(NodeId field)
{
    auto cursor = bodyOf(field);
    skipIdentifier(cursor);
    auto const definition = required(cursor);
    if (!definition)
        return std::unexpected { definition.error() };
    if (!isListOf(*definition, "func"))
        return std::unexpected { isList(*definition) ? unexpectedHead(*definition)
                                                     : unexpected(*definition) };
    auto inner = bodyOf(*definition);
    auto use = TypeUse {};
    if (auto const signature = readSignature(inner, use, ParamNames::Allowed); !signature)
        return signature;
    if (auto const end = expectEndAfterLists(inner); !end)
        return end;
    if (auto const end = expectEnd(cursor); !end)
        return end;
    auto const index = _types.define(std::move(use.inlineType));
    addParamNames(NameKind::Parameter, index, use.paramNames);
    return {};
}

// ---------------------------------------------------------------------------------------------
// Phase 3: fields

Outcome FieldReader::readFunction(NodeId field)
{
    auto cursor = bodyOf(field);
    skipIdentifier(cursor);
    auto const index = allocate(IndexSpace::Function);
    if (auto const exports = readInlineExports(cursor, ExternKind::Func, index); !exports)
        return exports;

    if (isListOf(cursor.peek(), "import"))
    {
        auto names = readInlineImport(cursor.take());
        if (!names)
            return std::unexpected { std::move(names.error()) };
        auto const use = readTypeUse(cursor, ParamNames::Allowed);
        if (!use)
            return std::unexpected { use.error() };
        auto const typeIndex = resolve(*use);
        if (!typeIndex)
            return std::unexpected { typeIndex.error() };
        if (auto const end = expectEndAfterLists(cursor); !end)
            return end;
        noteImport(field);
        addParamNames(NameKind::Local, index, use->paramNames);
        addImport(
            std::move(names->first),
            Import { .name = std::move(names->second), .kind = ExternKind::Func, .typeIndex = *typeIndex });
        return {};
    }

    noteDefinition(IndexSpace::Function);
    auto const use = readTypeUse(cursor, ParamNames::Allowed);
    if (!use)
        return std::unexpected { use.error() };
    auto const typeIndex = resolve(*use);
    if (!typeIndex)
        return std::unexpected { typeIndex.error() };

    auto scope = LocalScope {};
    for (auto const& param: use->paramNames)
    {
        noteNames(param.namedAfter,
                  NameTarget { .kind = NameKind::Local, .index = index, .inner = param.index });
        if (param.identifier != NoNode)
            if (auto const bound = bindLocal(scope, param.identifier, param.index); !bound)
                return bound;
    }
    auto function = binary::Function {};
    function.typeIndex = *typeIndex;
    auto paramCount = std::optional<uint32_t> {};
    while (isListOf(cursor.peek(), "local"))
        if (auto const locals = readLocals(cursor.take(), index, function, scope, *use, paramCount); !locals)
            return locals;
    auto reader = InstructionReader { *this, scope.symbols, function };
    if (auto const body = reader.read(InstructionNodes { .list = field, .first = cursor.peek() }); !body)
        return body;
    cursor.finish();
    for (auto const& label: reader.takeLabelAnnotations())
        noteNames(label.namedAfter,
                  NameTarget { .kind = NameKind::Label, .index = index, .inner = label.index });
    noteBranchHints(field, index, reader);

    if (!scope.names.empty())
        _names.indirect[std::to_underlying(NameKind::Local)].push_back(
            binary::IndirectNameMap { .index = index, .names = std::move(scope.names) });
    if (auto labels = reader.takeLabelNames(); !labels.empty())
        _names.indirect[std::to_underlying(NameKind::Label)].push_back(
            binary::IndirectNameMap { .index = index, .names = std::move(labels) });
    _module.functions.push_back(*typeIndex);
    _module.code.push_back(std::move(function));
    return {};
}

Outcome FieldReader::readTable(NodeId field)
{
    auto cursor = bodyOf(field);
    skipIdentifier(cursor);
    auto const index = allocate(IndexSpace::Table);
    if (auto const exports = readInlineExports(cursor, ExternKind::Table, index); !exports)
        return exports;

    if (isListOf(cursor.peek(), "import"))
    {
        auto names = readInlineImport(cursor.take());
        if (!names)
            return std::unexpected { std::move(names.error()) };
        auto const type = readTableType(cursor);
        if (!type)
            return std::unexpected { type.error() };
        if (auto const end = expectEnd(cursor); !end)
            return end;
        noteImport(field);
        addImport(std::move(names->first),
                  Import { .name = std::move(names->second), .kind = ExternKind::Table, .table = *type });
        return {};
    }

    noteDefinition(IndexSpace::Table);
    auto const addressType = readAddressType(cursor);
    if (isNatural(cursor.peek()))
    {
        auto const limits = readLimits(cursor, addressType);
        if (!limits)
            return std::unexpected { limits.error() };
        auto const element = readReferenceType(cursor);
        if (!element)
            return std::unexpected { element.error() };
        auto table = binary::Table { .type = { .element = *element, .limits = *limits }, .init = {} };
        if (!cursor.atEnd())
        {
            auto init = readExpression(InstructionNodes { .list = field, .first = cursor.peek() });
            if (!init)
                return std::unexpected { std::move(init.error()) };
            table.init = std::move(*init);
        }
        cursor.finish();
        _module.tables.push_back(std::move(table));
        return {};
    }

    // `(table addrtype? reftype (elem …))`: a table exactly as large as the segment it holds.
    auto const element = readReferenceType(cursor);
    if (!element)
        return std::unexpected { element.error() };
    auto const list = required(cursor);
    if (!list)
        return std::unexpected { list.error() };
    if (!isListOf(*list, "elem"))
        return std::unexpected { isList(*list) ? unexpectedHead(*list) : unexpected(*list) };
    if (auto const end = expectEnd(cursor); !end)
        return end;

    auto segment = ElementSegment { .mode = SegmentMode::Active,
                                    .table = index,
                                    .offset = zeroOffset(addressType),
                                    .type = binary::FuncRef,
                                    .items = std::vector<uint32_t> {} };
    auto items = bodyOf(*list);
    auto const read = isList(items.peek()) ? readExpressions(items, segment) : readIndices(items, segment);
    if (!read)
        return read;
    // Function indices express only function references; in a table of another type they are
    // `ref.func` items of the table's type, as the reference's elem_var_list makes them.
    if (auto const* const indices = std::get_if<std::vector<uint32_t>>(&segment.items);
        indices && *element != binary::FuncRef && *element != binary::NonNullFuncRef)
    {
        auto expressions = std::vector<ConstExpr> {};
        expressions.reserve(indices->size());
        for (auto const function: *indices)
            expressions.push_back(ConstExpr { Instruction { .opcode = Opcode::RefFunc, .a = function },
                                              Instruction { .opcode = Opcode::End } });
        segment.items = std::move(expressions);
    }
    if (std::holds_alternative<std::vector<ConstExpr>>(segment.items))
        segment.type = *element;
    auto const size =
        std::visit([](auto const& entries) { return uint64_t { entries.size() }; }, segment.items);
    allocate(IndexSpace::Element);
    _module.tables.push_back(
        binary::Table { .type = { .element = *element,
                                  .limits = Limits { .min = size, .max = size, .addressType = addressType } },
                        .init = {} });
    _module.elements.push_back(std::move(segment));
    return {};
}

Outcome FieldReader::readMemory(NodeId field)
{
    auto cursor = bodyOf(field);
    skipIdentifier(cursor);
    auto const index = allocate(IndexSpace::Memory);
    if (auto const exports = readInlineExports(cursor, ExternKind::Memory, index); !exports)
        return exports;

    if (isListOf(cursor.peek(), "import"))
    {
        auto names = readInlineImport(cursor.take());
        if (!names)
            return std::unexpected { std::move(names.error()) };
        auto const type = readMemoryType(cursor);
        if (!type)
            return std::unexpected { type.error() };
        if (auto const end = expectEnd(cursor); !end)
            return end;
        noteImport(field);
        addImport(std::move(names->first),
                  Import { .name = std::move(names->second), .kind = ExternKind::Memory, .memory = *type });
        return {};
    }

    noteDefinition(IndexSpace::Memory);
    auto const addressType = readAddressType(cursor);
    if (isNatural(cursor.peek()))
    {
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
        if (auto const end = expectEnd(cursor); !end)
            return end;
        _module.memories.push_back(type);
        return {};
    }

    // `(memory addrtype? (pagesize n)? (data …))`: a memory exactly as many pages large as its data.
    auto pageSizeLog2 = binary::DefaultPageSizeLog2;
    if (isListOf(cursor.peek(), "pagesize"))
    {
        auto const declared = readPageSize(cursor.take());
        if (!declared)
            return std::unexpected { declared.error() };
        pageSizeLog2 = *declared;
    }
    auto const list = required(cursor);
    if (!list)
        return std::unexpected { list.error() };
    if (!isListOf(*list, "data"))
        return std::unexpected { isList(*list) ? unexpectedHead(*list) : unexpected(*list) };
    if (auto const end = expectEnd(cursor); !end)
        return end;
    auto strings = bodyOf(*list);
    auto bytes = readStrings(strings);
    if (!bytes)
        return std::unexpected { bytes.error() };

    auto const pageSize = uint64_t { 1 } << pageSizeLog2;
    auto const pages = (uint64_t { bytes->size() } + pageSize - 1) / pageSize;
    _module.memories.push_back(
        MemoryType { .limits = Limits { .min = pages, .max = pages, .addressType = addressType },
                     .pageSizeLog2 = pageSizeLog2 });
    allocate(IndexSpace::Data);
    _module.data.push_back(binary::DataSegment { .mode = SegmentMode::Active,
                                                 .memory = index,
                                                 .offset = zeroOffset(addressType),
                                                 .bytes = std::move(*bytes) });
    return {};
}

Outcome FieldReader::readGlobal(NodeId field)
{
    auto cursor = bodyOf(field);
    skipIdentifier(cursor);
    auto const index = allocate(IndexSpace::Global);
    if (auto const exports = readInlineExports(cursor, ExternKind::Global, index); !exports)
        return exports;

    if (isListOf(cursor.peek(), "import"))
    {
        auto names = readInlineImport(cursor.take());
        if (!names)
            return std::unexpected { std::move(names.error()) };
        auto const type = readGlobalType(cursor);
        if (!type)
            return std::unexpected { type.error() };
        if (auto const end = expectEnd(cursor); !end)
            return end;
        noteImport(field);
        addImport(std::move(names->first),
                  Import { .name = std::move(names->second), .kind = ExternKind::Global, .global = *type });
        return {};
    }

    noteDefinition(IndexSpace::Global);
    auto const type = readGlobalType(cursor);
    if (!type)
        return std::unexpected { type.error() };
    auto init = readExpression(InstructionNodes { .list = field, .first = cursor.peek() });
    if (!init)
        return std::unexpected { std::move(init.error()) };
    cursor.finish();
    _module.globals.push_back(binary::Global { .type = *type, .init = std::move(*init) });
    return {};
}

Outcome FieldReader::readTag(NodeId field)
{
    auto cursor = bodyOf(field);
    skipIdentifier(cursor);
    auto const index = allocate(IndexSpace::Tag);
    if (auto const exports = readInlineExports(cursor, ExternKind::Tag, index); !exports)
        return exports;

    auto names = std::optional<std::pair<std::string, std::string>> {};
    if (isListOf(cursor.peek(), "import"))
    {
        auto inlineImport = readInlineImport(cursor.take());
        if (!inlineImport)
            return std::unexpected { std::move(inlineImport.error()) };
        names = std::move(*inlineImport);
    }
    auto const use = readTypeUse(cursor, ParamNames::Allowed);
    if (!use)
        return std::unexpected { use.error() };
    auto const typeIndex = resolve(*use);
    if (!typeIndex)
        return std::unexpected { typeIndex.error() };
    if (auto const end = expectEndAfterLists(cursor); !end)
        return end;

    if (names)
    {
        noteImport(field);
        addImport(
            std::move(names->first),
            Import { .name = std::move(names->second), .kind = ExternKind::Tag, .typeIndex = *typeIndex });
        return {};
    }
    noteDefinition(IndexSpace::Tag);
    // wasm-tools names a defined tag's parameters, not an imported one's.
    addParamNames(NameKind::TagParameter, index, use->paramNames);
    _module.tags.push_back(binary::Tag { .typeIndex = *typeIndex });
    return {};
}

Outcome FieldReader::readElement(NodeId field)
{
    auto cursor = bodyOf(field);
    skipIdentifier(cursor);
    allocate(IndexSpace::Element);
    auto segment = ElementSegment {};
    auto abbreviation = IndexAbbreviation::Refused;
    if (isKeyword(cursor.peek(), "declare"))
    {
        cursor.take();
        segment.mode = SegmentMode::Declarative;
    }
    else if (isListOf(cursor.peek(), "table") || (isList(cursor.peek()) && !isListOf(cursor.peek(), "ref")))
    {
        segment.mode = SegmentMode::Active;
        if (isListOf(cursor.peek(), "table"))
        {
            auto inner = bodyOf(cursor.take());
            auto const node = required(inner);
            if (!node)
                return std::unexpected { node.error() };
            auto const table = lookup(IndexSpace::Table, *node);
            if (!table)
                return std::unexpected { table.error() };
            if (auto const end = expectEnd(inner); !end)
                return end;
            segment.table = *table;
        }
        else
            abbreviation = IndexAbbreviation::Allowed;
        auto const offset = required(cursor);
        if (!offset)
            return std::unexpected { offset.error() };
        if (!isList(*offset))
            return std::unexpected { unexpected(*offset) };
        auto expression = readOffset(*offset);
        if (!expression)
            return std::unexpected { std::move(expression.error()) };
        segment.offset = std::move(*expression);
    }
    if (auto const list = readElementList(cursor, segment, abbreviation); !list)
        return list;
    _module.elements.push_back(std::move(segment));
    return {};
}

Outcome FieldReader::readData(NodeId field)
{
    auto cursor = bodyOf(field);
    skipIdentifier(cursor);
    allocate(IndexSpace::Data);
    auto segment = binary::DataSegment {};
    if (isList(cursor.peek()))
    {
        segment.mode = SegmentMode::Active;
        if (isListOf(cursor.peek(), "memory"))
        {
            auto inner = bodyOf(cursor.take());
            auto const node = required(inner);
            if (!node)
                return std::unexpected { node.error() };
            auto const memory = lookup(IndexSpace::Memory, *node);
            if (!memory)
                return std::unexpected { memory.error() };
            if (auto const end = expectEnd(inner); !end)
                return end;
            segment.memory = *memory;
        }
        auto const offset = required(cursor);
        if (!offset)
            return std::unexpected { offset.error() };
        if (!isList(*offset))
            return std::unexpected { unexpected(*offset) };
        auto expression = readOffset(*offset);
        if (!expression)
            return std::unexpected { std::move(expression.error()) };
        segment.offset = std::move(*expression);
    }
    auto bytes = readStrings(cursor);
    if (!bytes)
        return std::unexpected { bytes.error() };
    segment.bytes = std::move(*bytes);
    _module.data.push_back(std::move(segment));
    return {};
}

Outcome FieldReader::readImport(NodeId field)
{
    auto cursor = bodyOf(field);
    auto moduleName = readName(cursor);
    if (!moduleName)
        return std::unexpected { std::move(moduleName.error()) };

    // The items go straight into the module, whose imports read() reserved exactly: a group of many
    // items is never held twice. Their module name is added once, after them, if there are any.
    auto& imports = _module.imports;
    auto const firstItem = imports.size();
    auto const moduleIndex = static_cast<uint32_t>(_module.importModules.size());
    auto const addOne = [&](std::string name, NodeId descriptor) -> Outcome {
        auto use = TypeUse {};
        auto import = readDescriptor(descriptor, use);
        if (!import)
            return std::unexpected { std::move(import.error()) };
        auto const index = allocate(externRowOf(headKeyword(_tree, descriptor))->space);
        if (import->kind == ExternKind::Func)
            addParamNames(NameKind::Local, index, use.paramNames);
        import->name = std::move(name);
        import->moduleIndex = moduleIndex;
        imports.push_back(std::move(*import));
        return {};
    };

    if (isAtom(cursor.peek(), TokenKind::String))
    {
        // `(import "m" "n" desc)`.
        auto name = readName(cursor);
        if (!name)
            return std::unexpected { std::move(name.error()) };
        auto const descriptor = required(cursor);
        if (!descriptor)
            return std::unexpected { descriptor.error() };
        if (auto const added = addOne(std::move(*name), *descriptor); !added)
            return added;
        if (auto const end = expectEnd(cursor); !end)
            return end;
        noteImport(field);
        _module.importModules.push_back(std::move(*moduleName));
        return {};
    }

    // The compact forms (compact-import-section): `(item "n" desc)…`, or `(item "n")… desc`, whose
    // items share one descriptor; which one the first item decides.
    auto itemsHaveDescriptors = false;
    if (isListOf(cursor.peek(), "item"))
    {
        auto probe = bodyOf(cursor.peek());
        probe.take(); // The item's name.
        itemsHaveDescriptors = !probe.atEnd();
    }
    auto sharedNames = std::vector<std::string> {};
    while (isListOf(cursor.peek(), "item"))
    {
        auto item = bodyOf(cursor.take());
        auto name = readName(item);
        if (!name)
            return std::unexpected { std::move(name.error()) };
        if (!itemsHaveDescriptors)
        {
            if (auto const end = expectEnd(item); !end)
                return end;
            sharedNames.push_back(std::move(*name));
            continue;
        }
        auto const descriptor = required(item);
        if (!descriptor)
            return std::unexpected { descriptor.error() };
        if (auto const added = addOne(std::move(*name), *descriptor); !added)
            return added;
        if (auto const end = expectEnd(item); !end)
            return end;
    }
    // `(import "m")` is a compact import of no items and no descriptor: it imports nothing.
    auto const isEmpty = !itemsHaveDescriptors && sharedNames.empty() && cursor.atEnd();
    if (!itemsHaveDescriptors && !isEmpty)
    {
        auto const descriptor = required(cursor);
        if (!descriptor)
            return std::unexpected { descriptor.error() };
        auto use = TypeUse {};
        auto const import = readDescriptor(*descriptor, use);
        if (!import)
            return std::unexpected { import.error() };
        auto const space = externRowOf(headKeyword(_tree, *descriptor))->space;
        for (auto& name: sharedNames)
        {
            allocate(space);
            auto shared = *import;
            shared.name = std::move(name);
            shared.moduleIndex = moduleIndex;
            imports.push_back(std::move(shared));
        }
    }
    if (auto const end = expectEnd(cursor); !end)
        return end;
    // An import of no items imports nothing, so no definition it follows is refused for it, and it names
    // no module.
    if (imports.size() == firstItem)
        return {};
    noteImport(field);
    _module.importModules.push_back(std::move(*moduleName));
    return {};
}

Outcome FieldReader::readExport(NodeId field)
{
    auto cursor = bodyOf(field);
    auto name = readName(cursor);
    if (!name)
        return std::unexpected { std::move(name.error()) };
    auto const descriptor = required(cursor);
    if (!descriptor)
        return std::unexpected { descriptor.error() };
    auto const* row = externRowOf(headKeyword(_tree, *descriptor));
    if (!isList(*descriptor) || row == nullptr)
        return std::unexpected { isList(*descriptor) ? unexpectedHead(*descriptor)
                                                     : unexpected(*descriptor) };
    auto inner = bodyOf(*descriptor);
    auto const node = required(inner);
    if (!node)
        return std::unexpected { node.error() };
    auto const index = lookup(row->space, *node);
    if (!index)
        return std::unexpected { index.error() };
    if (auto const end = expectEnd(inner); !end)
        return end;
    if (auto const end = expectEnd(cursor); !end)
        return end;
    _module.exports.push_back(
        binary::Export { .name = std::move(*name), .kind = row->kind, .index = *index });
    return {};
}

Outcome FieldReader::readStart(NodeId field)
{
    auto cursor = bodyOf(field);
    auto const node = required(cursor);
    if (!node)
        return std::unexpected { node.error() };
    auto const function = lookup(IndexSpace::Function, *node);
    if (!function)
        return std::unexpected { function.error() };
    if (auto const end = expectEnd(cursor); !end)
        return end;
    // parser.mly checks, per start field, that no other follows it, once every later field is
    // read: the error names the last start field but one.
    if (_lastStart)
        defer(DeferredError { _lastStartOrdinal, TextErrorCode::MultipleStart, *_lastStart, {} });
    _lastStart = *node;
    _lastStartOrdinal = _ordinal;
    _module.start = *function;
    return {};
}

// ---------------------------------------------------------------------------------------------
// The phases

std::expected<binary::Module, TextError> FieldReader::read(NodeId module, NodeId first, NodeId moduleName)
{
    auto fields = Siblings { SiblingIterator { _tree, first, SiblingFilter::All }, SiblingIterator {} }
                  | std::views::filter(
                      [this](NodeId node) { return _tree.nodes()[node].kind != NodeKind::Annotation; });
    auto const rowOfField = [this](NodeId field) -> std::expected<FieldRow const*, TextError> {
        if (!isList(field))
            return std::unexpected { unexpected(field) };
        auto const found = std::ranges::find(FieldRows, headKeyword(_tree, field), &FieldRow::keyword);
        if (found == FieldRows.end())
            return std::unexpected { unexpectedHead(field) };
        return &*found;
    };

    if (moduleName != NoNode)
    {
        auto name = identifierOf(moduleName);
        if (!name)
            return std::unexpected { std::move(name.error()) };
        _names.module = std::move(*name);
    }
    // Every annotation within the `(module …)` list, or within the whole text of a bare module.
    _annotations = module == NoNode ? knownAnnotationsIn(_tree, 0, static_cast<NodeId>(_tree.nodes().size()))
                                    : knownAnnotationsIn(_tree, module + 1, _tree.subtreeEnd(module));
    noteModuleAnnotations(module);
    for (auto const field: fields)
    {
        auto const row = rowOfField(field);
        if (!row)
            return std::unexpected { row.error() };
        if (auto const bound = ((*this).*((*row)->bind))(field); !bound)
            return std::unexpected { bound.error() };
    }
    for (auto const field: fields)
        if (auto const defined = ((*this).*((*rowOfField(field))->define))(field); !defined)
            return std::unexpected { defined.error() };
    // A defined function is several vectors: they are reserved exactly, so that a module of many small
    // functions never holds a vector of them grown to twice what it uses.
    auto const definesFunction = [this](NodeId field) {
        return headKeyword(_tree, field) == "func"
               && std::ranges::none_of(_tree.children(field),
                                       [this](NodeId child) { return isListOf(child, "import"); });
    };
    auto const definedFunctions = static_cast<std::size_t>(std::ranges::count_if(fields, definesFunction));
    _module.functions.reserve(definedFunctions);
    _module.code.reserve(definedFunctions);
    // Imports alike: an import field's items (one without items imports one entity, or none), and one
    // per inline import.
    auto const importsOf = [this](NodeId field) -> std::size_t {
        auto const isItem = [this](NodeId child) {
            return isListOf(child, "item");
        };
        if (headKeyword(_tree, field) == "import")
            return std::max(std::size_t { 1 },
                            static_cast<std::size_t>(std::ranges::count_if(_tree.children(field), isItem)));
        return std::ranges::any_of(_tree.children(field),
                                   [this](NodeId child) { return isListOf(child, "import"); })
                   ? 1
                   : 0;
    };
    auto importCount = std::size_t { 0 };
    for (auto const field: fields)
        importCount += importsOf(field);
    _module.imports.reserve(importCount);
    for (auto const field: fields)
    {
        if (auto const read = ((*this).*((*rowOfField(field))->read))(field); !read)
            return std::unexpected { read.error() };
        ++_ordinal;
    }
    if (_deferred)
        return std::unexpected { TextError {
            _deferred->code, _tree.positionOf(_deferred->node), std::move(_deferred->subject) } };

    // The data count section follows the text rule (as wasm-tools and the reference's encode.ml):
    // present exactly when a function body names a data segment.
    if (binary::codeNamesDataSegment(_module))
        _module.dataCount = static_cast<uint32_t>(_module.data.size());
    // The annotations are read once the whole module has, as the reference reads them after parsing it.
    if (auto const read = readAnnotations(_tree, _annotations, _sites, _module, _names); !read)
        return std::unexpected { read.error() };
    if (_names != binary::NameSection {})
        _module.customs.push_back(binary::CustomSection { .name = std::string { NameSectionRow.name },
                                                          .bytes = binary::encodeNameSection(_names),
                                                          .after = binary::SectionAnchor::Data });
    return std::move(_module);
}

std::string_view headKeyword(SExpressionTree const& tree, NodeId list) noexcept
{
    if (list == NoNode)
        return {};
    auto const head = skipAnnotations(tree, tree.nodes()[list].firstChild);
    if (head == NoNode || tree.nodes()[head].kind != NodeKind::Atom
        || tree.nodes()[head].tokenKind != TokenKind::Keyword)
        return {};
    return tree.textOf(head);
}

NodeId firstTopLevelNode(SExpressionTree const& tree) noexcept
{
    return skipAnnotations(tree, tree.firstTopLevel());
}

std::expected<void, TextError> requireSoleTopLevelNode(SExpressionTree const& tree, NodeId form)
{
    auto const next = skipAnnotations(tree, tree.nodes()[form].nextSibling);
    if (next == NoNode)
        return {};
    return std::unexpected { unexpectedKeyword(tree, next) };
}

std::expected<void, TextError> refuseAnnotationsOutside(SExpressionTree const& tree, NodeId form)
{
    for (auto const node: tree.topLevel())
        if (node != form && tree.annotationKindOf(node))
            return std::unexpected { TextError { TextErrorCode::MisplacedAnnotation,
                                                 tree.positionOf(node) } };
    return {};
}

std::expected<binary::Module, TextError> ModuleReader::read(SExpressionTree const& tree, NodeId module) const
{
    auto cursor = Cursor { tree, module };
    auto const head = cursor.take();
    if (headKeyword(tree, module) != "module")
        return std::unexpected { head == NoNode
                                     ? TextError { TextErrorCode::UnexpectedToken, cursor.endPosition() }
                                     : unexpectedKeyword(tree, head) };
    auto const isAt = [&](TokenKind kind) {
        return cursor.peek() != NoNode && tree.nodes()[cursor.peek()].kind == NodeKind::Atom
               && tree.nodes()[cursor.peek()].tokenKind == kind;
    };
    if (isAt(TokenKind::Keyword) && tree.textOf(cursor.peek()) == "definition")
        cursor.take();
    auto const name = isAt(TokenKind::Identifier) ? cursor.take() : NoNode;
    auto reader = FieldReader { tree, _limits };
    return reader.read(module, cursor.peek(), name);
}

std::expected<binary::Module, TextError> ModuleReader::readInline(SExpressionTree const& tree) const
{
    auto reader = FieldReader { tree, _limits };
    return reader.read(NoNode, tree.firstTopLevel(), NoNode);
}

} // namespace isoline::text
