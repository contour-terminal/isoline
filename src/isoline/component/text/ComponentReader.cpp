// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/text/ComponentReader.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/meta/Check.hpp>
#include <isoline/text/Annotations.hpp>
#include <isoline/text/IntegerLiteral.hpp>
#include <isoline/text/Keywords.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/ModuleReader.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>

namespace isoline::component::text
{

namespace
{
    using isoline::text::AnnotationKind;
    using isoline::text::NodeId;
    using isoline::text::NodeKind;
    using isoline::text::NoNode;
    using isoline::text::SExpressionTree;
    using isoline::text::SourcePosition;
    using isoline::text::TextError;
    using isoline::text::TextErrorCode;
    using isoline::text::TextLimits;
    using isoline::text::TokenKind;

    template <typename T>
    using Result = std::expected<T, TextError>;

    /// What a scope of text is: what its fields or declarations may be, and where they go.
    enum class ScopeForm : uint8_t
    {
        Component,     ///< A component: every field; a pool entry of its own.
        ComponentType, ///< A component type's declarations; a pool entry of its own.
        InstanceType,  ///< An instance type's declarations; a pool entry of its own.
        ModuleType,    ///< A core module type's declarations, held in place by their CoreTypeDefinition.
    };

    /// The kind of pool entry each ScopeForm but ModuleType has, by enumerator.
    constexpr auto ScopeKinds = std::to_array<binary::ScopeKind>({
        binary::ScopeKind::Component,
        binary::ScopeKind::ComponentType,
        binary::ScopeKind::InstanceType,
    });

    /// How a reference is written.
    enum class ReferenceForm : uint8_t
    {
        Number,     ///< A `u32` index, kept as written.
        Identifier, ///< A `$id`, resolved in its scope.
        Generated,  ///< A definition the reader made: Reference::value is its item.
    };

    /// Whether an identifier that its scope does not define is looked up outward, for an outer alias.
    enum class Reach : uint8_t
    {
        Local,     ///< In its scope only: a core type's index, as wast resolves those.
        Enclosing, ///< Outward too: wast's resolve_ns.
    };

    /// What export names after a reference's index stand for.
    enum class AliasSugar : uint8_t
    {
        None,       ///< No names: the index itself.
        Export,     ///< `(func $i "a" "b")`: a chain of export aliases, an instance's each but the last.
        CoreExport, ///< `(core func $i "a")`: one core export alias.
    };

    /// One reference of an item: the index space it names and how it is written, resolved once its
    /// scope's identifiers are known.
    struct Reference
    {
        binary::Sort space = binary::Sort::Type;    ///< The index space it names.
        ReferenceForm form = ReferenceForm::Number; ///< How it is written.
        Reach reach = Reach::Enclosing;             ///< Where an identifier is looked up.
        AliasSugar sugar = AliasSugar::None;        ///< What its export names stand for.
        NodeId node = NoNode;                       ///< Its index atom, where errors are reported.
        uint32_t value = 0;                         ///< Number: the index; Generated: the item.
        std::vector<NodeId> exportNames;            ///< The export names after the index.
        uint32_t resolved = 0;                      ///< The index it resolves to.
    };

    /// A reference to @p value of @p space, written as @p form at @p node.
    Reference referenceTo(binary::Sort space, ReferenceForm form, Reach reach, NodeId node, uint32_t value)
    {
        return Reference { .space = space,
                           .form = form,
                           .reach = reach,
                           .sugar = AliasSugar::None,
                           .node = node,
                           .value = value,
                           .exportNames = {},
                           .resolved = 0 };
    }

    /// An explicit outer alias's target, resolved against the enclosing scopes.
    struct OuterTarget
    {
        NodeId count = NoNode;                  ///< The scope: a `u32` count outward, or the scope's `$id`.
        NodeId index = NoNode;                  ///< The definition in that scope: a `u32` or a `$id`.
        binary::Sort sort = binary::Sort::Type; ///< Its index space.
    };

    /// What an item is: a definition of a component or type scope, or a declaration of a module type.
    using ItemValue = std::variant<std::monostate, binary::Definition, binary::CoreModuleDeclaration>;

    /// One definition of a scope, as read, its index fields holding the ids of its references.
    struct Item
    {
        ItemValue value;                          ///< What it is.
        NodeId position = NoNode;                 ///< Where errors about it are reported.
        NodeId identifier = NoNode;               ///< Its `$id`, if it has one.
        std::optional<std::string> annotatedName; ///< Its `@name`, if it has one.
        std::vector<uint32_t> references;         ///< Its references, in the order wast resolves them.
        std::optional<OuterTarget> outer;         ///< An explicit outer alias's target.
        std::optional<uint32_t> child;            ///< The scope it opens, if any.
    };

    /// One name per identifier of an index space, to its index.
    using Namespace = std::map<std::string, uint32_t, std::less<>>;

    /// A scope's identifiers, one Namespace per sort.
    using Namespaces = std::array<Namespace, binary::SortCount>;

    /// Where a module type's declarations go: the definition of its CoreTypeDefinition.
    struct ModuleTarget
    {
        uint32_t scope = 0;         ///< The pool entry.
        std::size_t definition = 0; ///< The definition's index in it.
    };

    /// One scope of text: a component, a component or instance type, or a module type.
    struct Scope
    {
        ScopeForm form = ScopeForm::Component;    ///< What it is.
        NodeId list = NoNode;                     ///< The list that holds it.
        NodeId first = NoNode;                    ///< Its first field or declaration; NoNode for none.
        NodeId identifier = NoNode;               ///< The `$id` that names it, for outer aliases.
        std::optional<std::string> annotatedName; ///< A component's `@name`.
        std::optional<uint32_t> parent;           ///< The scope it is nested in.
        uint32_t depth = 1;                       ///< The scopes open at once with it, as the decoder counts.
        uint32_t pool = 0;                        ///< Its pool entry (not for a module type).
        ModuleTarget target;                      ///< A module type's CoreTypeDefinition.

        std::vector<Item> items;           ///< Every item read, by id.
        std::vector<Reference> references; ///< Every reference of its items, by id.
        std::vector<uint32_t> expanded;    ///< The items in field order, sugar expanded.
        std::vector<uint32_t> appended;    ///< The exports inline exports add, after every field.
        std::map<isoline::binary::FuncType, uint32_t>
            moduleTypeKeys;             ///< A module type's function types by key.
        Namespaces names;               ///< Its identifiers, once resolved.
        std::vector<uint32_t> indices;  ///< Each item's index in its sort, once resolved.
        std::vector<uint32_t> children; ///< The scopes nested in it, to read once it is resolved.
    };

    /// One primitive value type's alternative keyword, besides its PrimitiveValueTypeTable text.
    struct PrimitiveAlias
    {
        std::string_view keyword;        ///< The keyword.
        binary::PrimitiveValueType type; ///< The primitive it spells.
    };

    /// The legacy spellings wast still reads.
    constexpr auto PrimitiveAliases = std::to_array<PrimitiveAlias>({
        { .keyword = "float32", .type = binary::PrimitiveValueType::F32 },
        { .keyword = "float64", .type = binary::PrimitiveValueType::F64 },
    });

    /// The sorts an outer alias may name, by wast's ComponentOuterAliasKind.
    constexpr auto OuterAliasSorts = std::to_array<binary::Sort>({
        binary::Sort::CoreModule,
        binary::Sort::CoreType,
        binary::Sort::Type,
        binary::Sort::Component,
    });

    /// The core sorts a core instance exports and a core export alias names.
    constexpr auto CoreExternSorts = std::to_array<binary::Sort>({
        binary::Sort::CoreFunc,
        binary::Sort::CoreTable,
        binary::Sort::CoreMemory,
        binary::Sort::CoreGlobal,
        binary::Sort::CoreTag,
    });

    /// The sorts a component export, an instantiation argument and an export alias name.
    constexpr auto ExportSorts = std::to_array<binary::Sort>({
        binary::Sort::CoreModule,
        binary::Sort::Func,
        binary::Sort::Type,
        binary::Sort::Component,
        binary::Sort::Instance,
    });

    /// Builds an 🏷️ `(implements "…")` attribute.
    binary::Attribute implementsAttribute(std::string value)
    {
        return binary::Implements { .interfaceName = std::move(value) };
    }

    /// Builds a 🔗 `(version-suffix "…")` attribute.
    binary::Attribute versionSuffixAttribute(std::string value)
    {
        return binary::VersionSuffix { .suffix = std::move(value) };
    }

    /// Builds an 🏷️ `(external-id "…")` attribute.
    binary::Attribute externalIdAttribute(std::string value)
    {
        return binary::ExternalId { .id = std::move(value) };
    }

    /// What builds a name attribute from its value.
    using AttributeMaker = binary::Attribute (*)(std::string);

    /// What builds each name attribute, in NameAttributeTable's order, which is binary::Attribute's.
    constexpr std::array<AttributeMaker, 3> AttributeMakers { &implementsAttribute,
                                                              &versionSuffixAttribute,
                                                              &externalIdAttribute };

    static_assert(AttributeMakers.size() == std::variant_size_v<binary::Attribute>,
                  "AttributeMakers holds one maker per name attribute");

    /// The name of the custom section a component's identifiers are recorded in.
    constexpr auto ComponentNameSection = std::string_view { "component-name" };

    /// The name map subsection ids of `component-name`: the component's own name, and a sort's.
    constexpr auto ComponentNameSubsection = uint8_t { 0x00 };
    constexpr auto SortNamesSubsection = uint8_t { 0x01 };

    /// The index space an item adds an index to.
    /// @param item The item.
    /// @return Its sort, or nothing for a custom section or a module type's import or export.
    std::optional<binary::Sort> sortOf(Item const& item)
    {
        if (auto const* definition = std::get_if<binary::Definition>(&item.value))
            return binary::introducedSort(*definition);
        if (auto const* declaration = std::get_if<binary::CoreModuleDeclaration>(&item.value))
            if (std::holds_alternative<binary::CoreTypeDeclaration>(*declaration)
                || std::holds_alternative<binary::CoreOuterAliasDeclaration>(*declaration))
                return binary::Sort::CoreType;
        return std::nullopt;
    }

    /// Appends @p value as an unsigned LEB128.
    void appendLeb(std::vector<std::byte>& bytes, uint64_t value)
    {
        do
        {
            auto byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if (value != 0)
                byte |= 0x80;
            bytes.push_back(std::byte { byte });
        } while (value != 0);
    }

    /// Appends @p name as a `name`: its length, then its bytes.
    void appendName(std::vector<std::byte>& bytes, std::string_view name)
    {
        appendLeb(bytes, name.size());
        std::ranges::transform(
            name, std::back_inserter(bytes), [](char c) { return static_cast<std::byte>(c); });
    }

    /// Appends one subsection: its id, its size, its payload.
    void appendSubsection(std::vector<std::byte>& bytes, uint8_t id, std::span<std::byte const> payload)
    {
        bytes.push_back(std::byte { id });
        appendLeb(bytes, payload.size());
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }

    // The index fields of the AST that references fill, by alternative: forEachIndex(x, visit) calls
    // visit(field) on each, in no particular order. Leaves come first, as each overload finds those
    // declared before it.

    /// A core value type's index: a concrete heap's.
    template <typename Visit>
    void forEachIndex(isoline::binary::ValueType& type, Visit const& visit)
    {
        if (type.kind == isoline::binary::ValueKind::Ref
            && type.heap.kind == isoline::binary::AbstractHeapType::Concrete)
            visit(type.heap.typeIndex);
    }

    template <typename Visit>
    void forEachIndex(isoline::binary::FuncType& type, Visit const& visit)
    {
        for (auto& param: type.params)
            forEachIndex(param, visit);
        for (auto& result: type.results)
            forEachIndex(result, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::CoreExternType& type, Visit const& visit)
    {
        switch (type.kind)
        {
            case isoline::binary::ExternKind::Func:
            case isoline::binary::ExternKind::Tag: visit(type.typeIndex); break;
            case isoline::binary::ExternKind::Table: forEachIndex(type.table.element, visit); break;
            case isoline::binary::ExternKind::Memory: break;
            case isoline::binary::ExternKind::Global: forEachIndex(type.global.type, visit); break;
        }
    }

    template <typename Visit>
    void forEachIndex(binary::ValueTypeRef& type, Visit const& visit)
    {
        if (auto* index = std::get_if<binary::TypeIndex>(&type))
            visit(index->index);
    }

    template <typename Visit>
    void forEachIndex(std::optional<binary::ValueTypeRef>& type, Visit const& visit)
    {
        if (type)
            forEachIndex(*type, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::ExternDesc& type, Visit const& visit)
    {
        if (type.sort != binary::Sort::Type || type.bound == binary::TypeBound::Equal)
            visit(type.index);
    }

    template <typename Visit>
    void forEachIndex(binary::RecordType& type, Visit const& visit)
    {
        for (auto& field: type.fields)
            forEachIndex(field.type, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::VariantType& type, Visit const& visit)
    {
        for (auto& entry: type.cases)
            forEachIndex(entry.type, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::TupleType& type, Visit const& visit)
    {
        for (auto& element: type.elements)
            forEachIndex(element, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::ResultType& type, Visit const& visit)
    {
        forEachIndex(type.ok, visit);
        forEachIndex(type.error, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::MapType& type, Visit const& visit)
    {
        forEachIndex(type.key, visit);
        forEachIndex(type.value, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::DefinedValueType& type, Visit const& visit)
    {
        std::visit(
            [&](auto& alternative) {
                using Alternative = std::remove_cvref_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Alternative, binary::ListType>
                              || std::is_same_v<Alternative, binary::FixedListType>
                              || std::is_same_v<Alternative, binary::StreamType>
                              || std::is_same_v<Alternative, binary::FutureType>)
                    forEachIndex(alternative.element, visit);
                else if constexpr (std::is_same_v<Alternative, binary::OptionType>)
                    forEachIndex(alternative.type, visit);
                else if constexpr (std::is_same_v<Alternative, binary::OwnType>
                                   || std::is_same_v<Alternative, binary::BorrowType>)
                    visit(alternative.resource);
                else if constexpr (std::is_same_v<Alternative, binary::PrimitiveValueType>
                                   || std::is_same_v<Alternative, binary::FlagsType>
                                   || std::is_same_v<Alternative, binary::EnumType>)
                    static_cast<void>(alternative); // no index
                else
                    forEachIndex(alternative, visit);
            },
            type);
    }

    template <typename Visit>
    void forEachIndex(binary::FunctionTypeDefinition& type, Visit const& visit)
    {
        for (auto& param: type.params)
            forEachIndex(param.type, visit);
        forEachIndex(type.result, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::ResourceTypeDefinition& type, Visit const& visit)
    {
        if (type.destructor)
            visit(*type.destructor);
    }

    /// A component or instance type holds its declarations in a scope of its own.
    template <typename Visit>
    void forEachIndex(binary::ComponentTypeDefinition& /*type*/, Visit const& /*visit*/)
    {
    }

    template <typename Visit>
    void forEachIndex(binary::InstanceTypeDefinition& /*type*/, Visit const& /*visit*/)
    {
    }

    template <typename Visit>
    void forEachIndex(binary::TypeDefinition& definition, Visit const& visit)
    {
        std::visit([&](auto& type) { forEachIndex(type, visit); }, definition.type);
    }

    template <typename Visit>
    void forEachIndex(binary::CanonDefinition& canon, Visit const& visit)
    {
        auto const& shape =
            std::ranges::find(binary::CanonBuiltinTable, canon.builtin, &binary::CanonBuiltinRow::builtin)
                ->shape;
        auto const& syntaxes = canonText(canon.builtin).indices;
        auto stored = std::size_t { 0 };
        auto index = std::size_t { 0 };
        for (auto const immediate: shape)
        {
            if (immediate == binary::Immediate::Index)
            {
                if (IndexSyntaxSpaces[static_cast<std::size_t>(syntaxes[index++])].sort)
                    visit(canon.immediates[stored]);
                ++stored;
            }
            else if (immediate == binary::Immediate::CoreValueType || immediate == binary::Immediate::Async
                     || immediate == binary::Immediate::Shared)
                ++stored;
            else if (immediate == binary::Immediate::Options)
            {
                for (auto& option: canon.options)
                    if (CanonOptionSpaces[static_cast<std::size_t>(option.kind)])
                        visit(option.index);
            }
            else if (immediate == binary::Immediate::ResultList)
                forEachIndex(canon.result, visit);
        }
    }

    template <typename Visit>
    void forEachIndex(binary::CoreInstantiate& expression, Visit const& visit)
    {
        visit(expression.module);
        for (auto& argument: expression.arguments)
            visit(argument.instance);
    }

    template <typename Visit>
    void forEachIndex(binary::CoreInlineExports& expression, Visit const& visit)
    {
        for (auto& entry: expression.exports)
            visit(entry.index);
    }

    template <typename Visit>
    void forEachIndex(binary::CoreInstanceDefinition& definition, Visit const& visit)
    {
        std::visit([&](auto& expression) { forEachIndex(expression, visit); }, definition.expression);
    }

    template <typename Visit>
    void forEachIndex(binary::CoreModuleTypeDefinition& /*type*/, Visit const& /*visit*/)
    {
        // Its declarations are its own scope's, filled once that scope is read.
    }

    template <typename Visit>
    void forEachIndex(binary::CoreTypeDefinition& definition, Visit const& visit)
    {
        std::visit([&](auto& type) { forEachIndex(type, visit); }, definition.type);
    }

    template <typename Visit>
    void forEachIndex(binary::Instantiate& expression, Visit const& visit)
    {
        visit(expression.component);
        for (auto& argument: expression.arguments)
            visit(argument.item.index);
    }

    template <typename Visit>
    void forEachIndex(binary::InlineExports& expression, Visit const& visit)
    {
        for (auto& entry: expression.exports)
            visit(entry.item.index);
    }

    template <typename Visit>
    void forEachIndex(binary::InstanceDefinition& definition, Visit const& visit)
    {
        std::visit([&](auto& expression) { forEachIndex(expression, visit); }, definition.expression);
    }

    template <typename Visit>
    void forEachIndex(binary::AliasDefinition& alias, Visit const& visit)
    {
        // An outer alias's count and index are resolved against the enclosing scopes (resolveOuter()).
        if (alias.target != binary::AliasTarget::Outer)
            visit(alias.instance);
    }

    template <typename Visit>
    void forEachIndex(binary::ImportDefinition& import, Visit const& visit)
    {
        forEachIndex(import.type, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::ExportDeclaration& declaration, Visit const& visit)
    {
        forEachIndex(declaration.type, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::ExportDefinition& exported, Visit const& visit)
    {
        visit(exported.item.index);
        if (exported.type)
            forEachIndex(*exported.type, visit);
    }

    /// What holds no index a reference fills: bytes, a pool entry, a payload.
    template <typename Visit>
    void forEachIndex(binary::CoreModuleDefinition& /*definition*/, Visit const& /*visit*/)
    {
    }

    template <typename Visit>
    void forEachIndex(binary::NestedComponent& /*definition*/, Visit const& /*visit*/)
    {
    }

    template <typename Visit>
    void forEachIndex(binary::CustomDefinition& /*definition*/, Visit const& /*visit*/)
    {
    }

    template <typename Visit>
    void forEachIndex(binary::Definition& definition, Visit const& visit)
    {
        std::visit([&](auto& alternative) { forEachIndex(alternative, visit); }, definition);
    }

    template <typename Visit>
    void forEachIndex(binary::CoreImportDeclaration& declaration, Visit const& visit)
    {
        forEachIndex(declaration.type, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::CoreTypeDeclaration& declaration, Visit const& visit)
    {
        forEachIndex(declaration.type, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::CoreOuterAliasDeclaration& /*declaration*/, Visit const& /*visit*/)
    {
    }

    template <typename Visit>
    void forEachIndex(binary::CoreExportDeclaration& declaration, Visit const& visit)
    {
        forEachIndex(declaration.type, visit);
    }

    template <typename Visit>
    void forEachIndex(binary::CoreModuleDeclaration& declaration, Visit const& visit)
    {
        std::visit([&](auto& alternative) { forEachIndex(alternative, visit); }, declaration);
    }

    /// A position in a list's significant children.
    class Cursor
    {
      public:
        /// A cursor at @p node, a child of @p list, or at the first significant node after it.
        Cursor(SExpressionTree const& tree, NodeId list, NodeId node) noexcept:
            _tree { &tree }, _list { list }, _node { skip(tree, node) }
        {
        }

        /// The node the cursor is at; NoNode at the end.
        [[nodiscard]] NodeId peek() const noexcept { return _node; }

        /// Whether the cursor is past the last node.
        [[nodiscard]] bool atEnd() const noexcept { return _node == NoNode; }

        /// Steps past the current node.
        /// @return The node stepped past.
        NodeId take() noexcept
        {
            auto const node = _node;
            if (node != NoNode)
                _node = skip(*_tree, _tree->nodes()[node].nextSibling);
            return node;
        }

        /// Steps to the end.
        void finish() noexcept { _node = NoNode; }

        /// Where the list closes: where a missing node is reported.
        [[nodiscard]] SourcePosition endPosition() const noexcept
        {
            auto const& list = _tree->nodes()[_list];
            return isoline::text::positionAt(_tree->source(), list.offset + list.length - 1);
        }

        /// The list the cursor walks.
        [[nodiscard]] NodeId list() const noexcept { return _list; }

      private:
        /// The first significant node from @p node on.
        static NodeId skip(SExpressionTree const& tree, NodeId node) noexcept
        {
            while (node != NoNode && !tree.isSignificant(node))
                node = tree.nodes()[node].nextSibling;
            return node;
        }

        SExpressionTree const* _tree; ///< The tree.
        NodeId _list;                 ///< The list.
        NodeId _node;                 ///< The node the cursor is at, or NoNode.
    };

    /// Whether a compound value type is walked for its operands or built from their resolved types.
    enum class Assembly : uint8_t
    {
        Collect, ///< Only its operand nodes are collected; no reference is made.
        Build,   ///< It is built, its operands resolved.
    };

    /// A compound value type's operands: collected as nodes, or handed out resolved, in one order.
    class Operands
    {
      public:
        Operands(Assembly assembly, std::span<binary::ValueTypeRef const> resolved) noexcept:
            _assembly { assembly }, _resolved { resolved }
        {
        }

        /// The operand at @p node: collected, a placeholder returned; or its resolved type.
        binary::ValueTypeRef next(NodeId node)
        {
            if (_assembly == Assembly::Collect)
            {
                _nodes.push_back(node);
                return binary::PrimitiveValueType::Bool;
            }
            return _resolved[_next++];
        }

        /// Whether the compound is built rather than collected.
        [[nodiscard]] bool building() const noexcept { return _assembly == Assembly::Build; }

        /// The nodes collected.
        [[nodiscard]] std::vector<NodeId> take() noexcept { return std::move(_nodes); }

      private:
        Assembly _assembly;                              ///< Collect or build.
        std::span<binary::ValueTypeRef const> _resolved; ///< The resolved operands, when building.
        std::size_t _next = 0;                           ///< The next resolved operand.
        std::vector<NodeId> _nodes;                      ///< The nodes collected.
    };

    class Reading;

    /// What reads one kind of field or declaration.
    using FieldReaderFunction = std::expected<void, TextError> (Reading::*)(NodeId);

    /// The bit of a ScopeForm in FieldRow::forms.
    consteval uint8_t formBit(ScopeForm form) noexcept
    {
        return static_cast<uint8_t>(uint8_t { 1 } << static_cast<uint8_t>(form));
    }

    /// One row of FieldRows: a field's keywords, the scopes it may stand in, and what reads it.
    struct FieldRow
    {
        std::string_view first;   ///< Its keyword.
        std::string_view second;  ///< The keyword after `core`; empty otherwise.
        uint8_t forms;            ///< The ScopeForms it may stand in, as formBit()s.
        FieldReaderFunction read; ///< What reads it.
    };

    /// One read: the scopes of one component, read one after another from a work list.
    class Reading
    {
      public:
        Reading(SExpressionTree const& tree,
                TextLimits const& text,
                ComponentLimits const& limits,
                ComponentFeatureSet features):
            _tree { tree }, _text { text }, _limits { limits }, _features { features }
        {
        }

        Result<binary::ComponentPool> read(NodeId component);

      private:
        // ---- Syntax ----
        [[nodiscard]] bool isAtom(NodeId node, TokenKind kind) const noexcept
        {
            return node != NoNode && _tree.nodes()[node].kind == NodeKind::Atom
                   && _tree.nodes()[node].tokenKind == kind;
        }
        /// Whether @p node is a word: a Keyword atom, or a Reserved one, as the lexer reads a word with
        /// `-` or `=` (`error-context`, `string-encoding=utf8`), which component text is full of.
        [[nodiscard]] bool isWord(NodeId node) const noexcept
        {
            return isAtom(node, TokenKind::Keyword) || isAtom(node, TokenKind::Reserved);
        }
        [[nodiscard]] bool isKeyword(NodeId node, std::string_view keyword) const noexcept
        {
            return isWord(node) && _tree.textOf(node) == keyword;
        }
        /// The word a list starts with, Reserved ones included; empty for anything else.
        [[nodiscard]] std::string_view headWord(NodeId list) const noexcept
        {
            if (!isList(list))
                return {};
            auto const head = Cursor { _tree, list, _tree.nodes()[list].firstChild }.peek();
            return isWord(head) ? _tree.textOf(head) : std::string_view {};
        }
        [[nodiscard]] bool isList(NodeId node) const noexcept
        {
            return node != NoNode && _tree.nodes()[node].kind == NodeKind::List;
        }
        [[nodiscard]] bool isListOf(NodeId node, std::string_view keyword) const noexcept
        {
            return headWord(node) == keyword && !keyword.empty();
        }
        /// Whether @p node is a list of `core` then @p keyword.
        [[nodiscard]] bool isCoreListOf(NodeId node, std::string_view keyword) const noexcept
        {
            return isListOf(node, "core") && isKeyword(bodyOf(node).peek(), keyword);
        }
        [[nodiscard]] bool isString(NodeId node) const noexcept { return isAtom(node, TokenKind::String); }
        [[nodiscard]] bool isNatural(NodeId node) const noexcept
        {
            if (!isAtom(node, TokenKind::Keyword))
                return false;
            auto const first = _tree.textOf(node).front();
            return first >= '0' && first <= '9';
        }
        [[nodiscard]] bool isIndex(NodeId node) const noexcept
        {
            return isAtom(node, TokenKind::Identifier) || isNatural(node);
        }
        [[nodiscard]] Cursor bodyOf(NodeId list) const noexcept
        {
            auto cursor = Cursor { _tree, list, _tree.nodes()[list].firstChild };
            cursor.take();
            return cursor;
        }
        [[nodiscard]] TextError unexpected(NodeId node) const
        {
            return isoline::text::unexpectedKeyword(_tree, node);
        }
        /// The error a list is refused with for its first token: the token's, or the list's when empty.
        [[nodiscard]] TextError unexpectedHead(NodeId list) const
        {
            auto const head = _tree.nodes()[list].firstChild;
            return unexpected(head == NoNode ? list : head);
        }
        [[nodiscard]] static TextError missing(Cursor const& cursor)
        {
            return TextError { TextErrorCode::UnexpectedToken, cursor.endPosition() };
        }
        [[nodiscard]] Result<void> expectEnd(Cursor const& cursor) const
        {
            if (cursor.atEnd())
                return {};
            return std::unexpected { unexpected(cursor.peek()) };
        }
        [[nodiscard]] Result<NodeId> required(Cursor& cursor) const
        {
            if (cursor.atEnd())
                return std::unexpected { missing(cursor) };
            return cursor.take();
        }
        [[nodiscard]] Result<void> expectKeyword(Cursor& cursor, std::string_view keyword) const
        {
            if (!isKeyword(cursor.peek(), keyword))
                return std::unexpected { cursor.atEnd() ? missing(cursor) : unexpected(cursor.peek()) };
            cursor.take();
            return {};
        }
        [[nodiscard]] Result<std::string> nameOf(NodeId node) const
        {
            if (!isString(node))
                return std::unexpected { unexpected(node) };
            return isoline::text::decodeName(_tree.source(), _tree.tokenOf(node));
        }
        [[nodiscard]] Result<std::string> readName(Cursor& cursor) const
        {
            return required(cursor).and_then([this](NodeId node) { return nameOf(node); });
        }
        [[nodiscard]] Result<std::string> identifierOf(NodeId node) const
        {
            return isoline::text::identifierName(_tree.source(), _tree.tokenOf(node));
        }
        [[nodiscard]] Result<uint32_t> natural32(NodeId node) const;
        /// A Keyword atom's text; empty for any other node.
        [[nodiscard]] std::string_view keywordText(NodeId node) const noexcept
        {
            return isWord(node) ? _tree.textOf(node) : std::string_view {};
        }
        [[nodiscard]] NodeId optionalIdentifier(Cursor& cursor) const noexcept
        {
            return isAtom(cursor.peek(), TokenKind::Identifier) ? cursor.take() : NoNode;
        }
        [[nodiscard]] Result<std::optional<std::string>> optionalNameAnnotation(Cursor& cursor) const;
        [[nodiscard]] bool isInlineName(NodeId node, std::string_view keyword) const noexcept;
        [[nodiscard]] bool isTypeReference(NodeId node) const noexcept;
        [[nodiscard]] std::optional<binary::PrimitiveValueType> primitiveOf(NodeId node) const noexcept;

        // ---- Items and references ----
        Scope& scope() noexcept { return _scopes[_scope]; }
        uint32_t newItem(NodeId position);
        Item& item(uint32_t id) noexcept { return scope().items[id]; }
        uint32_t reference(uint32_t owner, Reference value);
        Result<uint32_t> indexReference(uint32_t owner,
                                        binary::Sort space,
                                        NodeId node,
                                        Reach reach = Reach::Enclosing);
        uint32_t generatedReference(uint32_t user, binary::Sort space, uint32_t generated);
        Result<uint32_t> itemReference(uint32_t owner, binary::Sort space, Cursor& body, AliasSugar sugar);
        Result<uint32_t> indexOrReference(uint32_t owner, binary::Sort space, NodeId node);
        Result<uint32_t> corePrefixedReference(uint32_t owner, binary::Sort space, NodeId node);
        uint32_t newScope(ScopeForm form, NodeId list, NodeId first, NodeId identifier);
        void setValue(uint32_t id, ItemValue value) { item(id).value = std::move(value); }

        // ---- Scopes ----
        Result<void> readScope(uint32_t scopeId);
        Result<void> readField(NodeId field);
        [[nodiscard]] Result<std::string> nameOfItem(Item const& entry) const;
        Result<void> expandAliases();
        Result<void> insertAliases(uint32_t referenceId,
                                   Namespaces const& early,
                                   std::vector<uint32_t>& order);
        Result<void> resolve();
        Result<void> resolveOuter(uint32_t id, OuterTarget const& outer);
        void emit();
        void emitNames(std::vector<binary::Definition>& definitions);

        // ---- Fields ----
        Result<void> coreModule(NodeId field);
        Result<void> coreInstance(NodeId field);
        Result<void> coreType(NodeId field);
        Result<void> coreFunc(NodeId field);
        Result<void> nestedComponent(NodeId field);
        Result<void> instance(NodeId field);
        Result<void> alias(NodeId field);
        Result<void> type(NodeId field);
        Result<void> canon(NodeId field);
        Result<void> func(NodeId field);
        Result<void> import(NodeId field);
        Result<void> exportDefinition(NodeId field);
        Result<void> exportDeclaration(NodeId field);
        Result<void> custom(NodeId field);
        Result<void> moduleTypeType(NodeId declaration);
        Result<void> moduleTypeAlias(NodeId declaration);
        Result<void> moduleTypeImport(NodeId declaration);
        Result<void> moduleTypeExport(NodeId declaration);

        // ---- Parts ----
        /// What a definition's leading tokens name it: its `$id` and `@name`.
        struct Head
        {
            NodeId identifier = NoNode;               ///< Its `$id`, or NoNode.
            std::optional<std::string> annotatedName; ///< Its `@name`, if any.
        };
        Result<Head> readHead(Cursor& cursor) const;
        Result<std::vector<binary::NameAttributes>> inlineExports(Cursor& cursor) const;
        Result<std::optional<binary::NameAttributes>> inlineImport(Cursor& cursor) const;
        void appendExports(std::vector<binary::NameAttributes> names,
                           binary::Sort sort,
                           uint32_t owner,
                           NodeId position);
        Result<binary::NameAttributes> externName(Cursor& cursor) const;
        Result<binary::AliasDefinition> exportAlias(uint32_t owner,
                                                    Cursor& body,
                                                    binary::AliasTarget target,
                                                    binary::Sort sort);
        Result<binary::SortIndex> exportKind(uint32_t owner, NodeId node);
        Result<binary::CoreInlineExports> coreExports(uint32_t owner, Cursor& body);
        Result<binary::InlineExports> inlineInstanceExports(uint32_t owner, Cursor& body);
        Result<binary::ExternDesc> itemSignature(uint32_t owner, NodeId list, Head* head);
        Result<uint32_t> typeUse(uint32_t owner, Cursor& cursor, ScopeForm inlineForm);
        Result<uint32_t> functionTypeUse(uint32_t owner, Cursor& cursor);
        Result<binary::FunctionTypeDefinition> functionType(uint32_t owner, Cursor& cursor);
        Result<binary::ValueTypeRef> valueType(uint32_t owner, NodeId node);
        Result<std::pair<std::vector<NodeId>, binary::DefinedValueType>> compound(
            uint32_t owner, NodeId list, Assembly assembly, std::span<binary::ValueTypeRef const> resolved);
        Result<binary::DefinedValueType> recordType(uint32_t owner,
                                                    std::string_view keyword,
                                                    Cursor& body,
                                                    Operands& operands);
        Result<binary::DefinedValueType> variantType(uint32_t owner,
                                                     std::string_view keyword,
                                                     Cursor& body,
                                                     Operands& operands);
        Result<binary::DefinedValueType> listType(uint32_t owner,
                                                  std::string_view keyword,
                                                  Cursor& body,
                                                  Operands& operands);
        Result<binary::DefinedValueType> mapType(uint32_t owner,
                                                 std::string_view keyword,
                                                 Cursor& body,
                                                 Operands& operands);
        Result<binary::DefinedValueType> tupleType(uint32_t owner,
                                                   std::string_view keyword,
                                                   Cursor& body,
                                                   Operands& operands);
        Result<binary::DefinedValueType> labelledType(uint32_t owner,
                                                      std::string_view keyword,
                                                      Cursor& body,
                                                      Operands& operands);
        Result<binary::DefinedValueType> optionType(uint32_t owner,
                                                    std::string_view keyword,
                                                    Cursor& body,
                                                    Operands& operands);
        Result<binary::DefinedValueType> resultType(uint32_t owner,
                                                    std::string_view keyword,
                                                    Cursor& body,
                                                    Operands& operands);
        Result<binary::DefinedValueType> handleType(uint32_t owner,
                                                    std::string_view keyword,
                                                    Cursor& body,
                                                    Operands& operands);
        Result<binary::DefinedValueType> asyncType(uint32_t owner,
                                                   std::string_view keyword,
                                                   Cursor& body,
                                                   Operands& operands);
        Result<binary::DefinedValueType> definedType(uint32_t owner, NodeId node);
        Result<binary::TypeDefinition> typeDefinition(uint32_t owner, NodeId node, NodeId identifier);
        Result<binary::CanonBuiltinRow const*> canonRowOf(Cursor& body) const;
        Result<binary::CanonDefinition> canonBody(uint32_t owner,
                                                  Cursor& body,
                                                  binary::CanonBuiltinRow const& row,
                                                  std::optional<uint32_t> liftType);
        Result<std::vector<binary::CanonOption>> canonOptions(uint32_t owner, Cursor& body);
        Result<uint32_t> canonIndex(uint32_t owner, Cursor& body, IndexSyntax syntax);
        Result<void> canonResult(uint32_t owner, Cursor& body, binary::CanonDefinition& canon);
        [[nodiscard]] Result<binary::CoreValueKind> coreValueKind(NodeId node) const;
        Result<isoline::binary::ValueType> coreValueType(uint32_t owner, NodeId node);
        Result<isoline::binary::FuncType> coreFunctionType(uint32_t owner, Cursor& body);
        Result<binary::CoreExternType> coreExternType(uint32_t owner, NodeId list);
        Result<void> coreFunctionExtern(uint32_t owner,
                                        NodeId list,
                                        Cursor& body,
                                        binary::CoreExternType& type);
        Result<void> coreTableExtern(uint32_t owner, NodeId list, Cursor& body, binary::CoreExternType& type);
        Result<void> coreMemoryExtern(uint32_t owner,
                                      NodeId list,
                                      Cursor& body,
                                      binary::CoreExternType& type);
        Result<void> coreGlobalExtern(uint32_t owner,
                                      NodeId list,
                                      Cursor& body,
                                      binary::CoreExternType& type);
        [[nodiscard]] isoline::binary::AddressType addressType(Cursor& body) const noexcept;
        Result<isoline::binary::Limits> limits(Cursor& body, isoline::binary::AddressType address) const;

        /// What reads one compound value type's body.
        using CompoundReader = Result<binary::DefinedValueType> (Reading::*)(uint32_t,
                                                                             std::string_view,
                                                                             Cursor&,
                                                                             Operands&);

        /// One row of CompoundRows: a compound's keyword and what reads it.
        struct CompoundRow
        {
            std::string_view keyword; ///< Its keyword, binary::DefinedValueTypeTable's.
            CompoundReader read;      ///< What reads its body.
        };

        /// Every compound value type's text (Explainer.md's `defvaltype`): `list` reads `(list t len)` too.
        static constexpr auto CompoundRows = std::to_array<CompoundRow>({
            { .keyword = "record", .read = &Reading::recordType },
            { .keyword = "variant", .read = &Reading::variantType },
            { .keyword = "list", .read = &Reading::listType },
            { .keyword = "map", .read = &Reading::mapType },
            { .keyword = "tuple", .read = &Reading::tupleType },
            { .keyword = "flags", .read = &Reading::labelledType },
            { .keyword = "enum", .read = &Reading::labelledType },
            { .keyword = "option", .read = &Reading::optionType },
            { .keyword = "result", .read = &Reading::resultType },
            { .keyword = "own", .read = &Reading::handleType },
            { .keyword = "borrow", .read = &Reading::handleType },
            { .keyword = "stream", .read = &Reading::asyncType },
            { .keyword = "future", .read = &Reading::asyncType },
        });

        /// What reads one core extern type's body after its `$id`.
        using CoreExternReader = Result<void> (Reading::*)(uint32_t,
                                                           NodeId,
                                                           Cursor&,
                                                           binary::CoreExternType&);

        /// One row of CoreExternRows: a core extern type's keyword, kind and reader.
        struct CoreExternRow
        {
            std::string_view keyword;         ///< Its keyword.
            isoline::binary::ExternKind kind; ///< What it types.
            CoreExternReader read;            ///< What reads its body.
        };

        /// Every core extern type a module type's import or export may name.
        static constexpr auto CoreExternRows = std::to_array<CoreExternRow>({
            { .keyword = "func",
              .kind = isoline::binary::ExternKind::Func,
              .read = &Reading::coreFunctionExtern },
            { .keyword = "table",
              .kind = isoline::binary::ExternKind::Table,
              .read = &Reading::coreTableExtern },
            { .keyword = "memory",
              .kind = isoline::binary::ExternKind::Memory,
              .read = &Reading::coreMemoryExtern },
            { .keyword = "global",
              .kind = isoline::binary::ExternKind::Global,
              .read = &Reading::coreGlobalExtern },
            { .keyword = "tag",
              .kind = isoline::binary::ExternKind::Tag,
              .read = &Reading::coreFunctionExtern },
        });

        static constexpr auto InComponent = formBit(ScopeForm::Component);
        static constexpr auto InComponentType = formBit(ScopeForm::ComponentType);
        static constexpr auto InInstanceType = formBit(ScopeForm::InstanceType);
        static constexpr auto InModuleType = formBit(ScopeForm::ModuleType);

        /// Every field and declaration, by its keywords and the scopes it stands in (Explainer.md's
        /// `componentfield`, `componentdecl`, `instancedecl` and `core:moduledecl`).
        static constexpr auto FieldRows = std::to_array<FieldRow>({
            { .first = "core", .second = "module", .forms = InComponent, .read = &Reading::coreModule },
            { .first = "core", .second = "instance", .forms = InComponent, .read = &Reading::coreInstance },
            { .first = "core",
              .second = "type",
              .forms = InComponent | InComponentType | InInstanceType,
              .read = &Reading::coreType },
            { .first = "core", .second = "func", .forms = InComponent, .read = &Reading::coreFunc },
            { .first = "component", .second = "", .forms = InComponent, .read = &Reading::nestedComponent },
            { .first = "instance", .second = "", .forms = InComponent, .read = &Reading::instance },
            { .first = "alias",
              .second = "",
              .forms = InComponent | InComponentType | InInstanceType,
              .read = &Reading::alias },
            { .first = "type",
              .second = "",
              .forms = InComponent | InComponentType | InInstanceType,
              .read = &Reading::type },
            { .first = "canon", .second = "", .forms = InComponent, .read = &Reading::canon },
            { .first = "func", .second = "", .forms = InComponent, .read = &Reading::func },
            { .first = "import",
              .second = "",
              .forms = InComponent | InComponentType,
              .read = &Reading::import },
            { .first = "export", .second = "", .forms = InComponent, .read = &Reading::exportDefinition },
            { .first = "export",
              .second = "",
              .forms = InComponentType | InInstanceType,
              .read = &Reading::exportDeclaration },
            { .first = "type", .second = "", .forms = InModuleType, .read = &Reading::moduleTypeType },
            { .first = "alias", .second = "", .forms = InModuleType, .read = &Reading::moduleTypeAlias },
            { .first = "import", .second = "", .forms = InModuleType, .read = &Reading::moduleTypeImport },
            { .first = "export", .second = "", .forms = InModuleType, .read = &Reading::moduleTypeExport },
        });

        SExpressionTree const& _tree;   ///< The tree read.
        TextLimits const& _text;        ///< The caps on reading text.
        ComponentLimits const& _limits; ///< The component caps.
        ComponentFeatureSet _features;  ///< What a binary form decodes under.
        std::deque<Scope> _scopes; ///< Every scope met, by id; a deque, so references to them stay valid.
        uint32_t _scope = 0;       ///< The scope being read.
        std::vector<binary::Component> _components; ///< The pool's entries, by Scope::pool.
        std::vector<std::byte> _bytes;              ///< The pool's bytes: core modules and custom payloads.
        std::vector<uint32_t> _types;               ///< The current field's types to place before it.
        std::vector<uint32_t> _fields; ///< The current field's bundles to place before it, after its types.
        std::vector<uint32_t> _own;    ///< The current field's own items.
    };

    Result<uint32_t> Reading::natural32(NodeId node) const
    {
        if (!isNatural(node))
            return std::unexpected { unexpected(node) };
        auto value = isoline::text::parseInteger(_tree.textOf(node),
                                                 isoline::text::IntegerWidth::I32,
                                                 isoline::text::IntegerSyntax::Unsigned,
                                                 _text);
        if (value)
            return static_cast<uint32_t>(*value);
        switch (value.error())
        {
            // The lexer reads a word that starts with a digit as a number or a Reserved token.
            // uncovered-begin(lexed-number)
            case isoline::text::LiteralError::NotANumber:
                return std::unexpected { unexpected(node) };
                // uncovered-end(lexed-number)
            case isoline::text::LiteralError::OutOfRange:
                return std::unexpected { TextError {
                    TextErrorCode::ConstantOutOfRange, _tree.positionOf(node), "i32" } };
            case isoline::text::LiteralError::TooLong: break;
        }
        return std::unexpected { TextError { TextErrorCode::NumberTooLong, _tree.positionOf(node) } };
    }

    Result<std::optional<std::string>> Reading::optionalNameAnnotation(Cursor& cursor) const
    {
        auto const node = cursor.peek();
        if (node == NoNode || _tree.annotationKindOf(node) != AnnotationKind::Name)
            return std::nullopt;
        cursor.take();
        auto contents = Cursor { _tree, node, _tree.nodes()[node].firstChild };
        if (!isString(contents.peek()))
            return std::unexpected { TextError { TextErrorCode::NameStringExpected,
                                                 contents.atEnd() ? contents.endPosition()
                                                                  : _tree.positionOf(contents.peek()) } };
        auto name = isoline::text::decodeName(_tree.source(), _tree.tokenOf(contents.take()));
        if (!name)
            return std::unexpected { TextError { TextErrorCode::NameMalformedUtf8, name.error().position } };
        if (!contents.atEnd())
            return std::unexpected { TextError { TextErrorCode::NameUnexpectedToken,
                                                 _tree.positionOf(contents.peek()) } };
        return std::optional<std::string> { std::move(*name) };
    }

    uint32_t Reading::newItem(NodeId position)
    {
        auto& items = scope().items;
        auto entry = Item {};
        entry.position = position;
        items.push_back(std::move(entry));
        return static_cast<uint32_t>(items.size() - 1);
    }

    uint32_t Reading::reference(uint32_t owner, Reference value)
    {
        auto& references = scope().references;
        references.push_back(std::move(value));
        auto const id = static_cast<uint32_t>(references.size() - 1);
        item(owner).references.push_back(id);
        return id;
    }

    Result<uint32_t> Reading::indexReference(uint32_t owner, binary::Sort space, NodeId node, Reach reach)
    {
        if (isAtom(node, TokenKind::Identifier))
            return reference(owner, referenceTo(space, ReferenceForm::Identifier, reach, node, 0));
        return natural32(node).transform([&](uint32_t index) {
            return reference(owner, referenceTo(space, ReferenceForm::Number, reach, node, index));
        });
    }

    uint32_t Reading::generatedReference(uint32_t user, binary::Sort space, uint32_t generated)
    {
        return reference(user, referenceTo(space, ReferenceForm::Generated, Reach::Local, NoNode, generated));
    }

    Result<uint32_t> Reading::itemReference(uint32_t owner,
                                            binary::Sort space,
                                            Cursor& body,
                                            AliasSugar sugar)
    {
        auto const index = required(body);
        if (!index)
            return std::unexpected { index.error() };
        auto id = indexReference(owner, space, *index);
        if (!id)
            return id;
        auto names = std::vector<NodeId> {};
        while (isString(body.peek()) && (sugar == AliasSugar::Export || names.empty()))
            names.push_back(body.take());
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        auto& entry = scope().references[*id];
        if (!names.empty())
            entry.sugar = sugar;
        entry.exportNames = std::move(names);
        return id;
    }

    Result<uint32_t> Reading::indexOrReference(uint32_t owner, binary::Sort space, NodeId node)
    {
        if (isIndex(node))
            return indexReference(owner, space, node);
        if (!isListOf(node, sortText(space).keyword))
            return std::unexpected { node == NoNode ? TextError { TextErrorCode::UnexpectedToken, {} }
                                                    : unexpected(node) };
        auto body = bodyOf(node);
        return itemReference(owner, space, body, AliasSugar::Export);
    }

    Result<uint32_t> Reading::corePrefixedReference(uint32_t owner, binary::Sort space, NodeId node)
    {
        if (isIndex(node))
            return indexReference(owner, space, node);
        if (!isCoreListOf(node, sortText(space).keyword))
            return std::unexpected { unexpected(node) };
        auto body = bodyOf(node);
        body.take();
        return itemReference(owner, space, body, AliasSugar::CoreExport);
    }

    uint32_t Reading::newScope(ScopeForm form, NodeId list, NodeId first, NodeId identifier)
    {
        auto const parent = _scope;
        auto const& enclosing = _scopes[parent];
        auto child = Scope {};
        child.form = form;
        child.list = list;
        child.first = first;
        child.identifier = identifier;
        child.parent = parent;
        child.depth = enclosing.depth + (form == ScopeForm::ModuleType ? 0 : 1);
        if (form != ScopeForm::ModuleType)
        {
            child.pool = static_cast<uint32_t>(_components.size());
            _components.push_back(
                binary::Component { .kind = ScopeKinds[static_cast<std::size_t>(form)], .definitions = {} });
        }
        _scopes.push_back(std::move(child));
        auto const id = static_cast<uint32_t>(_scopes.size() - 1);
        _scopes[parent].children.push_back(id);
        return id;
    }

    Result<Reading::Head> Reading::readHead(Cursor& cursor) const
    {
        auto head = Head { .identifier = optionalIdentifier(cursor), .annotatedName = std::nullopt };
        auto name = optionalNameAnnotation(cursor);
        if (!name)
            return std::unexpected { name.error() };
        head.annotatedName = std::move(*name);
        return head;
    }

    Result<binary::NameAttributes> Reading::externName(Cursor& cursor) const
    {
        auto result = binary::NameAttributes {};
        auto const node = cursor.peek();
        if (isListOf(node, "interface"))
        {
            // The legacy `(interface "…")` form, which wast still reads as the plain name.
            cursor.take();
            auto body = bodyOf(node);
            auto name = readName(body);
            if (!name)
                return std::unexpected { name.error() };
            if (auto const end = expectEnd(body); !end)
                return std::unexpected { end.error() };
            result.name = std::move(*name);
        }
        else
        {
            auto name = readName(cursor);
            if (!name)
                return std::unexpected { name.error() };
            result.name = std::move(*name);
        }
        // Attributes in any order, each once; encoded in NameAttributeTable's order, as wasm-encoder does.
        auto attributes = std::array<std::optional<std::string>, binary::NameAttributeTable.size()> {};
        while (isList(cursor.peek()))
        {
            auto const keyword = headWord(cursor.peek());
            auto const row =
                std::ranges::find(binary::NameAttributeTable, keyword, &binary::NameAttributeRow::keyword);
            if (row == binary::NameAttributeTable.end())
                break;
            auto const list = cursor.take();
            auto& slot = attributes[static_cast<std::size_t>(row - binary::NameAttributeTable.begin())];
            if (slot)
                return std::unexpected { TextError { TextErrorCode::DuplicateIdentifier,
                                                     _tree.positionOf(list),
                                                     std::format("{} attribute", keyword) } };
            auto body = bodyOf(list);
            auto value = readName(body);
            if (!value)
                return std::unexpected { value.error() };
            if (auto const end = expectEnd(body); !end)
                return std::unexpected { end.error() };
            slot = std::move(*value);
        }
        for (auto const index: std::views::iota(std::size_t { 0 }, attributes.size()))
            if (attributes[index])
                result.attributes.push_back(AttributeMakers[index](std::move(*attributes[index])));
        return result;
    }

    /// Whether @p node is `(keyword "name")` or `(keyword (interface "name"))`: an inline import or export,
    /// which carries no attribute.
    bool Reading::isInlineName(NodeId node, std::string_view keyword) const noexcept
    {
        if (!isListOf(node, keyword))
            return false;
        auto body = bodyOf(node);
        auto const name = body.take();
        if (isListOf(name, "interface"))
        {
            auto inner = bodyOf(name);
            if (!isString(inner.take()) || !inner.atEnd())
                return false;
        }
        else if (!isString(name))
            return false;
        return body.atEnd();
    }

    Result<std::vector<binary::NameAttributes>> Reading::inlineExports(Cursor& cursor) const
    {
        auto names = std::vector<binary::NameAttributes> {};
        while (isInlineName(cursor.peek(), "export"))
        {
            auto body = bodyOf(cursor.take());
            auto name = externName(body);
            if (!name)
                return std::unexpected { name.error() };
            names.push_back(std::move(*name));
        }
        return names;
    }

    Result<std::optional<binary::NameAttributes>> Reading::inlineImport(Cursor& cursor) const
    {
        if (!isInlineName(cursor.peek(), "import"))
            return std::nullopt;
        auto body = bodyOf(cursor.take());
        return externName(body).transform([](binary::NameAttributes name) {
            return std::optional<binary::NameAttributes> { std::move(name) };
        });
    }

    void Reading::appendExports(std::vector<binary::NameAttributes> names,
                                binary::Sort sort,
                                uint32_t owner,
                                NodeId position)
    {
        for (auto& name: names)
        {
            auto const exported = newItem(position);
            auto const target = owner;
            auto const index = generatedReference(exported, sort, target);
            setValue(exported,
                     binary::Definition { binary::ExportDefinition { .name = std::move(name),
                                                                     .item = { .sort = sort, .index = index },
                                                                     .type = std::nullopt } });
            scope().appended.push_back(exported);
        }
    }

    bool Reading::isTypeReference(NodeId node) const noexcept
    {
        if (!isListOf(node, "type"))
            return false;
        auto body = bodyOf(node);
        if (!isIndex(body.take()))
            return false;
        return body.atEnd() || isString(body.peek());
    }

    Result<uint32_t> Reading::typeUse(uint32_t owner, Cursor& cursor, ScopeForm inlineForm)
    {
        auto const sort = inlineForm == ScopeForm::ModuleType ? binary::Sort::CoreType : binary::Sort::Type;
        if (isTypeReference(cursor.peek()))
        {
            auto body = bodyOf(cursor.take());
            return itemReference(owner,
                                 sort,
                                 body,
                                 sort == binary::Sort::CoreType ? AliasSugar::CoreExport
                                                                : AliasSugar::Export);
        }
        auto const generated = newItem(cursor.list());
        auto const child = newScope(inlineForm, cursor.list(), cursor.peek(), NoNode);
        auto const pool = _scopes[child].pool;
        switch (inlineForm)
        {
            case ScopeForm::ComponentType:
                setValue(generated,
                         binary::Definition {
                             binary::TypeDefinition { binary::ComponentTypeDefinition { .scope = pool } } });
                break;
            case ScopeForm::InstanceType:
                setValue(generated,
                         binary::Definition {
                             binary::TypeDefinition { binary::InstanceTypeDefinition { .scope = pool } } });
                break;
            case ScopeForm::ModuleType:
            case ScopeForm::Component:
                setValue(generated,
                         binary::Definition {
                             binary::CoreTypeDefinition { binary::CoreModuleTypeDefinition {} } });
                break;
        }
        item(generated).child = child;
        cursor.finish();
        _types.push_back(generated);
        return generatedReference(owner, sort, generated);
    }

    Result<uint32_t> Reading::functionTypeUse(uint32_t owner, Cursor& cursor)
    {
        if (isTypeReference(cursor.peek()))
        {
            auto body = bodyOf(cursor.take());
            return itemReference(owner, binary::Sort::Type, body, AliasSugar::Export);
        }
        auto const generated = newItem(cursor.atEnd() ? cursor.list() : cursor.peek());
        auto function = functionType(generated, cursor);
        if (!function)
            return std::unexpected { function.error() };
        setValue(generated, binary::Definition { binary::TypeDefinition { std::move(*function) } });
        _types.push_back(generated);
        return generatedReference(owner, binary::Sort::Type, generated);
    }

    Result<binary::FunctionTypeDefinition> Reading::functionType(uint32_t owner, Cursor& cursor)
    {
        auto result = binary::FunctionTypeDefinition {};
        if (isKeyword(cursor.peek(), "async"))
        {
            cursor.take();
            result.asyncness = binary::Asyncness::Async;
        }
        while (isListOf(cursor.peek(), "param"))
        {
            auto body = bodyOf(cursor.take());
            auto label = readName(body);
            if (!label)
                return std::unexpected { label.error() };
            auto const node = required(body);
            if (!node)
                return std::unexpected { node.error() };
            auto type = valueType(owner, *node);
            if (!type)
                return std::unexpected { type.error() };
            if (auto const end = expectEnd(body); !end)
                return std::unexpected { end.error() };
            result.params.push_back(binary::LabeledType { .label = std::move(*label), .type = *type });
        }
        if (isListOf(cursor.peek(), "result"))
        {
            auto body = bodyOf(cursor.take());
            auto const node = required(body);
            if (!node)
                return std::unexpected { node.error() };
            auto type = valueType(owner, *node);
            if (!type)
                return std::unexpected { type.error() };
            if (auto const end = expectEnd(body); !end)
                return std::unexpected { end.error() };
            result.result = *type;
        }
        return result;
    }

    std::optional<binary::PrimitiveValueType> Reading::primitiveOf(NodeId node) const noexcept
    {
        if (!isWord(node))
            return std::nullopt;
        auto const text = _tree.textOf(node);
        auto const row =
            std::ranges::find(binary::PrimitiveValueTypeTable, text, &binary::PrimitiveValueTypeRow::text);
        if (row != binary::PrimitiveValueTypeTable.end())
            return row->type;
        auto const alias = std::ranges::find(PrimitiveAliases, text, &PrimitiveAlias::keyword);
        if (alias != PrimitiveAliases.end())
            return alias->type;
        return std::nullopt;
    }

    Result<binary::ValueTypeRef> Reading::valueType(uint32_t owner, NodeId node)
    {
        if (isIndex(node))
            return indexReference(owner, binary::Sort::Type, node)
                .transform(
                    [](uint32_t id) -> binary::ValueTypeRef { return binary::TypeIndex { .index = id }; });
        if (auto const primitive = primitiveOf(node))
            return *primitive;
        if (!isList(node))
            return std::unexpected { unexpected(node) };

        // An inline compound: a type definition of its own before the item that uses it, its own inline
        // compounds before it, in the order wast expands them; on an explicit stack, as they nest freely.
        struct Frame
        {
            NodeId node = NoNode;                       ///< The compound.
            uint32_t item = 0;                          ///< Its type definition.
            std::vector<NodeId> operands;               ///< Its value type operands.
            std::vector<binary::ValueTypeRef> resolved; ///< Those resolved so far.
        };
        auto stack = std::vector<Frame> {};
        auto const open = [&](NodeId compound) -> Result<void> {
            auto operands = this->compound(0, compound, Assembly::Collect, {});
            if (!operands)
                return std::unexpected { operands.error() };
            auto frame = Frame { .node = compound,
                                 .item = newItem(compound),
                                 .operands = std::move(operands->first),
                                 .resolved = {} };
            stack.push_back(std::move(frame));
            return {};
        };
        if (auto const opened = open(node); !opened)
            return std::unexpected { opened.error() };
        while (true)
        {
            auto& top = stack.back();
            if (top.resolved.size() < top.operands.size())
            {
                auto const operand = top.operands[top.resolved.size()];
                auto const frameItem = top.item;
                if (isIndex(operand))
                {
                    auto const id = indexReference(frameItem, binary::Sort::Type, operand);
                    if (!id)
                        return std::unexpected { id.error() };
                    stack.back().resolved.emplace_back(binary::TypeIndex { .index = *id });
                }
                else if (auto const primitive = primitiveOf(operand))
                    top.resolved.emplace_back(*primitive);
                else if (isList(operand))
                {
                    if (auto const opened = open(operand); !opened)
                        return std::unexpected { opened.error() };
                }
                else
                    return std::unexpected { unexpected(operand) };
                continue;
            }
            auto assembled = compound(top.item, top.node, Assembly::Build, top.resolved);
            if (!assembled)
                return std::unexpected { assembled.error() };
            auto const finished = top.item;
            setValue(finished,
                     binary::Definition { binary::TypeDefinition { std::move(assembled->second) } });
            _types.push_back(finished);
            stack.pop_back();
            auto const user = stack.empty() ? owner : stack.back().item;
            auto const id = generatedReference(user, binary::Sort::Type, finished);
            auto const reference = binary::ValueTypeRef { binary::TypeIndex { .index = id } };
            if (stack.empty())
                return reference;
            stack.back().resolved.push_back(reference);
        }
    }

    Result<std::pair<std::vector<NodeId>, binary::DefinedValueType>> Reading::compound(
        uint32_t owner, NodeId list, Assembly assembly, std::span<binary::ValueTypeRef const> resolved)
    {
        auto const keyword = headWord(list);
        auto const row = std::ranges::find(CompoundRows, keyword, &CompoundRow::keyword);
        if (row == CompoundRows.end())
            return std::unexpected { unexpectedHead(list) };
        auto operands = Operands { assembly, resolved };
        auto body = bodyOf(list);
        auto type = (this->*(row->read))(owner, keyword, body, operands);
        if (!type)
            return std::unexpected { type.error() };
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        return std::pair { operands.take(), std::move(*type) };
    }

    Result<binary::DefinedValueType> Reading::recordType(uint32_t /*owner*/,
                                                         std::string_view /*keyword*/,
                                                         Cursor& body,
                                                         Operands& operands)
    {
        auto record = binary::RecordType {};
        while (!body.atEnd())
        {
            auto const field = body.take();
            if (!isListOf(field, "field"))
                return std::unexpected { isList(field) ? unexpectedHead(field) : unexpected(field) };
            auto inner = bodyOf(field);
            auto label = readName(inner);
            if (!label)
                return std::unexpected { label.error() };
            auto const type = required(inner);
            if (!type)
                return std::unexpected { type.error() };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            record.fields.push_back(
                binary::LabeledType { .label = std::move(*label), .type = operands.next(*type) });
        }
        return record;
    }

    Result<binary::DefinedValueType> Reading::variantType(uint32_t /*owner*/,
                                                          std::string_view /*keyword*/,
                                                          Cursor& body,
                                                          Operands& operands)
    {
        auto variant = binary::VariantType {};
        auto ids = Namespace {};
        while (!body.atEnd())
        {
            auto const entry = body.take();
            if (!isListOf(entry, "case"))
                return std::unexpected { isList(entry) ? unexpectedHead(entry) : unexpected(entry) };
            auto inner = bodyOf(entry);
            auto const identifier = optionalIdentifier(inner);
            if (identifier != NoNode && operands.building())
            {
                // A case's `$id` names nothing the binary keeps, but must be unique (wast resolve.rs).
                auto name = identifierOf(identifier);
                if (!name)
                    return std::unexpected { name.error() };
                if (!ids.emplace(*name, 0).second)
                    return std::unexpected { TextError { TextErrorCode::DuplicateIdentifier,
                                                         _tree.positionOf(identifier),
                                                         std::format("variant case ${}", *name) } };
            }
            auto label = readName(inner);
            if (!label)
                return std::unexpected { label.error() };
            auto payload = std::optional<binary::ValueTypeRef> {};
            if (!inner.atEnd())
                payload = operands.next(inner.take());
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            variant.cases.push_back(binary::VariantCase { .label = std::move(*label), .type = payload });
        }
        return variant;
    }

    Result<binary::DefinedValueType> Reading::listType(uint32_t /*owner*/,
                                                       std::string_view /*keyword*/,
                                                       Cursor& body,
                                                       Operands& operands)
    {
        auto const element = required(body);
        if (!element)
            return std::unexpected { element.error() };
        auto const type = operands.next(*element);
        if (!isNatural(body.peek()))
            return binary::ListType { .element = type };
        // 🔧 `(list t len)`.
        return natural32(body.take()).transform([&](uint32_t length) -> binary::DefinedValueType {
            return binary::FixedListType { .element = type, .length = length };
        });
    }

    Result<binary::DefinedValueType> Reading::mapType(uint32_t /*owner*/,
                                                      std::string_view /*keyword*/,
                                                      Cursor& body,
                                                      Operands& operands)
    {
        auto const key = required(body);
        if (!key)
            return std::unexpected { key.error() };
        auto const keyType = operands.next(*key);
        auto const value = required(body);
        if (!value)
            return std::unexpected { value.error() };
        return binary::MapType { .key = keyType, .value = operands.next(*value) };
    }

    Result<binary::DefinedValueType> Reading::tupleType(uint32_t /*owner*/,
                                                        std::string_view /*keyword*/,
                                                        Cursor& body,
                                                        Operands& operands)
    {
        auto tuple = binary::TupleType {};
        while (!body.atEnd())
            tuple.elements.push_back(operands.next(body.take()));
        return tuple;
    }

    Result<binary::DefinedValueType> Reading::labelledType(uint32_t /*owner*/,
                                                           std::string_view keyword,
                                                           Cursor& body,
                                                           Operands& /*operands*/)
    {
        auto labels = std::vector<std::string> {};
        while (!body.atEnd())
        {
            auto label = nameOf(body.take());
            if (!label)
                return std::unexpected { label.error() };
            labels.push_back(std::move(*label));
        }
        if (keyword == "flags")
            return binary::FlagsType { .labels = std::move(labels) };
        return binary::EnumType { .labels = std::move(labels) };
    }

    Result<binary::DefinedValueType> Reading::optionType(uint32_t /*owner*/,
                                                         std::string_view /*keyword*/,
                                                         Cursor& body,
                                                         Operands& operands)
    {
        return required(body).transform([&](NodeId type) -> binary::DefinedValueType {
            return binary::OptionType { .type = operands.next(type) };
        });
    }

    Result<binary::DefinedValueType> Reading::resultType(uint32_t /*owner*/,
                                                         std::string_view /*keyword*/,
                                                         Cursor& body,
                                                         Operands& operands)
    {
        auto result = binary::ResultType {};
        if (!body.atEnd() && !isListOf(body.peek(), "error"))
            result.ok = operands.next(body.take());
        if (isListOf(body.peek(), "error"))
        {
            auto inner = bodyOf(body.take());
            auto const type = required(inner);
            if (!type)
                return std::unexpected { type.error() };
            result.error = operands.next(*type);
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
        }
        return result;
    }

    Result<binary::DefinedValueType> Reading::handleType(uint32_t owner,
                                                         std::string_view keyword,
                                                         Cursor& body,
                                                         Operands& operands)
    {
        auto const resource = required(body);
        if (!resource)
            return std::unexpected { resource.error() };
        if (!isIndex(*resource))
            return std::unexpected { unexpected(*resource) };
        auto index = uint32_t { 0 };
        if (operands.building())
        {
            auto const id = indexReference(owner, binary::Sort::Type, *resource);
            if (!id)
                return std::unexpected { id.error() };
            index = *id;
        }
        if (keyword == "own")
            return binary::OwnType { .resource = index };
        return binary::BorrowType { .resource = index };
    }

    Result<binary::DefinedValueType> Reading::asyncType(uint32_t /*owner*/,
                                                        std::string_view keyword,
                                                        Cursor& body,
                                                        Operands& operands)
    {
        auto element = std::optional<binary::ValueTypeRef> {};
        if (!body.atEnd())
            element = operands.next(body.take());
        if (keyword == "stream")
            return binary::StreamType { .element = element };
        return binary::FutureType { .element = element };
    }

    Result<binary::DefinedValueType> Reading::definedType(uint32_t owner, NodeId node)
    {
        if (auto const primitive = primitiveOf(node))
            return binary::DefinedValueType { *primitive };
        if (!isList(node))
            return std::unexpected { unexpected(node) };
        auto operands = compound(owner, node, Assembly::Collect, {});
        if (!operands)
            return std::unexpected { operands.error() };
        auto resolved = std::vector<binary::ValueTypeRef> {};
        for (auto const operand: operands->first)
        {
            auto type = valueType(owner, operand);
            if (!type)
                return std::unexpected { type.error() };
            resolved.push_back(*type);
        }
        return compound(owner, node, Assembly::Build, resolved).transform([](auto built) {
            return std::move(built.second);
        });
    }

    Result<binary::TypeDefinition> Reading::typeDefinition(uint32_t owner, NodeId node, NodeId identifier)
    {
        auto const keyword = isList(node) ? headWord(node) : std::string_view {};
        if (keyword == "func")
        {
            auto body = bodyOf(node);
            auto function = functionType(owner, body);
            if (!function)
                return std::unexpected { function.error() };
            if (auto const end = expectEnd(body); !end)
                return std::unexpected { end.error() };
            return binary::TypeDefinition { std::move(*function) };
        }
        if (keyword == "component" || keyword == "instance")
        {
            auto const form = keyword == "component" ? ScopeForm::ComponentType : ScopeForm::InstanceType;
            auto body = bodyOf(node);
            auto const child = newScope(form, node, body.peek(), identifier);
            item(owner).child = child;
            auto const pool = _scopes[child].pool;
            if (form == ScopeForm::ComponentType)
                return binary::TypeDefinition { binary::ComponentTypeDefinition { .scope = pool } };
            return binary::TypeDefinition { binary::InstanceTypeDefinition { .scope = pool } };
        }
        if (keyword == "resource")
        {
            auto body = bodyOf(node);
            auto resource = binary::ResourceTypeDefinition {};
            auto const rep = required(body);
            if (!rep)
                return std::unexpected { rep.error() };
            if (!isListOf(*rep, "rep"))
                return std::unexpected { unexpected(*rep) };
            auto inner = bodyOf(*rep);
            auto const repType = required(inner);
            if (!repType)
                return std::unexpected { repType.error() };
            auto const kind = coreValueKind(*repType);
            if (!kind)
                return std::unexpected { kind.error() };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            resource.rep = *kind;
            if (isListOf(body.peek(), "dtor"))
            {
                auto dtor = bodyOf(body.take());
                auto const target = required(dtor);
                if (!target)
                    return std::unexpected { target.error() };
                auto const id = corePrefixedReference(owner, binary::Sort::CoreFunc, *target);
                if (!id)
                    return std::unexpected { id.error() };
                if (auto const end = expectEnd(dtor); !end)
                    return std::unexpected { end.error() };
                resource.destructor = *id;
            }
            if (auto const end = expectEnd(body); !end)
                return std::unexpected { end.error() };
            return binary::TypeDefinition { resource };
        }
        return definedType(owner, node).transform([](binary::DefinedValueType type) {
            return binary::TypeDefinition { std::move(type) };
        });
    }

    Result<binary::CoreValueKind> Reading::coreValueKind(NodeId node) const
    {
        auto const found =
            isWord(node)
                ? std::ranges::find(CoreValueKeywords, _tree.textOf(node), &CoreValueKeyword::keyword)
                : CoreValueKeywords.end();
        if (found == CoreValueKeywords.end())
            return std::unexpected { node == NoNode ? TextError { TextErrorCode::UnexpectedToken, {} }
                                                    : unexpected(node) };
        return found->kind;
    }

    Result<isoline::binary::ValueType> Reading::coreValueType(uint32_t owner, NodeId node)
    {
        if (isWord(node))
        {
            auto const found = std::ranges::find(isoline::text::ValueTypeKeywords,
                                                 _tree.textOf(node),
                                                 &isoline::text::ValueTypeKeyword::keyword);
            if (found != isoline::text::ValueTypeKeywords.end())
                return found->type;
            return std::unexpected { unexpected(node) };
        }
        if (!isListOf(node, "ref"))
            return std::unexpected { unexpected(node) };
        auto body = bodyOf(node);
        auto type = isoline::binary::ValueType { .kind = isoline::binary::ValueKind::Ref,
                                                 .nullable = isoline::binary::Nullable::No };
        if (isKeyword(body.peek(), "null"))
        {
            body.take();
            type.nullable = isoline::binary::Nullable::Yes;
        }
        auto const heap = required(body);
        if (!heap)
            return std::unexpected { heap.error() };
        if (isIndex(*heap))
        {
            auto const id = indexReference(owner, binary::Sort::CoreType, *heap, Reach::Local);
            if (!id)
                return std::unexpected { id.error() };
            type.heap = isoline::binary::HeapType { .kind = isoline::binary::AbstractHeapType::Concrete,
                                                    .typeIndex = *id };
        }
        else
        {
            auto const found = isWord(*heap) ? std::ranges::find(isoline::text::HeapTypeKeywords,
                                                                 _tree.textOf(*heap),
                                                                 &isoline::text::HeapTypeKeyword::keyword)
                                             : isoline::text::HeapTypeKeywords.end();
            if (found == isoline::text::HeapTypeKeywords.end())
                return std::unexpected { unexpected(*heap) };
            type.heap = isoline::binary::HeapType { .kind = found->heap };
        }
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        return type;
    }

    Result<isoline::binary::FuncType> Reading::coreFunctionType(uint32_t owner, Cursor& body)
    {
        auto type = isoline::binary::FuncType {};
        auto const readList = [&](std::string_view keyword,
                                  std::vector<isoline::binary::ValueType>& types) -> Result<void> {
            while (isListOf(body.peek(), keyword))
            {
                auto inner = bodyOf(body.take());
                auto const named = keyword == "param" && isAtom(inner.peek(), TokenKind::Identifier);
                if (named)
                    inner.take();
                auto count = std::size_t { 0 };
                while (!inner.atEnd() && (!named || count == 0))
                {
                    auto value = coreValueType(owner, inner.take());
                    if (!value)
                        return std::unexpected { value.error() };
                    types.push_back(*value);
                    ++count;
                }
                if (named && count == 0)
                    return std::unexpected { missing(inner) };
                if (auto const end = expectEnd(inner); !end)
                    return std::unexpected { end.error() };
            }
            return {};
        };
        if (auto const params = readList("param", type.params); !params)
            return std::unexpected { params.error() };
        if (auto const results = readList("result", type.results); !results)
            return std::unexpected { results.error() };
        return type;
    }

    Result<isoline::binary::Limits> Reading::limits(Cursor& body, isoline::binary::AddressType address) const
    {
        auto const readBound = [this](NodeId node) -> Result<uint64_t> {
            if (!isNatural(node))
                return std::unexpected { node == NoNode ? TextError { TextErrorCode::UnexpectedToken, {} }
                                                        : unexpected(node) };
            auto value = isoline::text::parseInteger(_tree.textOf(node),
                                                     isoline::text::IntegerWidth::I64,
                                                     isoline::text::IntegerSyntax::Unsigned,
                                                     _text);
            if (value)
                return *value;
            return std::unexpected { TextError {
                value.error() == isoline::text::LiteralError::TooLong ? TextErrorCode::NumberTooLong
                                                                      : TextErrorCode::ConstantOutOfRange,
                _tree.positionOf(node),
                value.error() == isoline::text::LiteralError::TooLong ? "" : "i64" } };
        };
        auto const minimum = required(body);
        if (!minimum)
            return std::unexpected { minimum.error() };
        auto result = isoline::binary::Limits { .min = 0, .max = std::nullopt, .addressType = address };
        auto const low = readBound(*minimum);
        if (!low)
            return std::unexpected { low.error() };
        result.min = *low;
        if (isNatural(body.peek()))
        {
            auto const high = readBound(body.take());
            if (!high)
                return std::unexpected { high.error() };
            result.max = *high;
        }
        return result;
    }

    isoline::binary::AddressType Reading::addressType(Cursor& body) const noexcept
    {
        auto const found = std::ranges::find(isoline::text::AddressTypeKeywords,
                                             keywordText(body.peek()),
                                             &isoline::text::AddressTypeKeyword::keyword);
        if (found == isoline::text::AddressTypeKeywords.end())
            return isoline::binary::AddressType::I32;
        body.take();
        return found->type;
    }

    Result<binary::CoreExternType> Reading::coreExternType(uint32_t owner, NodeId list)
    {
        if (!isList(list))
            return std::unexpected { list == NoNode ? TextError { TextErrorCode::UnexpectedToken, {} }
                                                    : unexpected(list) };
        auto const keyword = headWord(list);
        auto const row = std::ranges::find(CoreExternRows, keyword, &CoreExternRow::keyword);
        if (row == CoreExternRows.end())
            return std::unexpected { unexpectedHead(list) };
        auto body = bodyOf(list);
        std::ignore =
            optionalIdentifier(body); // A module type's items bind no identifier (wast registers none).
        auto type = binary::CoreExternType {};
        type.kind = row->kind;
        if (auto const read = (this->*(row->read))(owner, list, body, type); !read)
            return std::unexpected { read.error() };
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        return type;
    }

    Result<void> Reading::coreFunctionExtern(uint32_t owner,
                                             NodeId list,
                                             Cursor& body,
                                             binary::CoreExternType& type)
    {
        if (isListOf(body.peek(), "type"))
        {
            // An explicit type use; its inline signature, if any, is wast's to check, not the encoding's.
            auto use = bodyOf(body.take());
            auto const index = required(use);
            if (!index)
                return std::unexpected { index.error() };
            auto const id = indexReference(owner, binary::Sort::CoreType, *index, Reach::Local);
            if (!id)
                return std::unexpected { id.error() };
            if (auto const end = expectEnd(use); !end)
                return std::unexpected { end.error() };
            type.typeIndex = *id;
            auto const ignored = newItem(list);
            return coreFunctionType(ignored, body).transform([](auto const&) {});
        }
        // An inline signature: the module type's last identical explicit type, else a type of its own
        // before the declaration (wast's expand_module_ty, which does not reuse those).
        auto const generated = newItem(list);
        auto signature = coreFunctionType(generated, body);
        if (!signature)
            return std::unexpected { signature.error() };
        auto const known = scope().moduleTypeKeys.find(*signature);
        if (known != scope().moduleTypeKeys.end())
        {
            type.typeIndex = generatedReference(owner, binary::Sort::CoreType, known->second);
            return {};
        }
        setValue(
            generated,
            binary::CoreModuleDeclaration { binary::CoreTypeDeclaration { .type = std::move(*signature) } });
        _types.push_back(generated);
        type.typeIndex = generatedReference(owner, binary::Sort::CoreType, generated);
        return {};
    }

    Result<void> Reading::coreTableExtern(uint32_t owner,
                                          NodeId /*list*/,
                                          Cursor& body,
                                          binary::CoreExternType& type)
    {
        auto const limited = limits(body, addressType(body));
        if (!limited)
            return std::unexpected { limited.error() };
        auto const element = required(body);
        if (!element)
            return std::unexpected { element.error() };
        auto const reference = coreValueType(owner, *element);
        if (!reference)
            return std::unexpected { reference.error() };
        if (reference->kind != isoline::binary::ValueKind::Ref)
            return std::unexpected { unexpected(*element) };
        type.table = isoline::binary::TableType { .element = *reference, .limits = *limited };
        return {};
    }

    Result<void> Reading::coreMemoryExtern(uint32_t /*owner*/,
                                           NodeId /*list*/,
                                           Cursor& body,
                                           binary::CoreExternType& type)
    {
        auto const limited = limits(body, addressType(body));
        if (!limited)
            return std::unexpected { limited.error() };
        type.memory = isoline::binary::MemoryType { .limits = *limited };
        if (!isListOf(body.peek(), "pagesize"))
            return {};
        auto page = bodyOf(body.take());
        auto const size = required(page);
        if (!size)
            return std::unexpected { size.error() };
        auto const bytes = natural32(*size);
        if (!bytes)
            return std::unexpected { bytes.error() };
        if (!std::has_single_bit(*bytes))
            return std::unexpected { TextError { TextErrorCode::InvalidCustomPageSize,
                                                 _tree.positionOf(*size) } };
        type.memory.pageSizeLog2 = static_cast<uint8_t>(std::countr_zero(*bytes));
        return expectEnd(page);
    }

    Result<void> Reading::coreGlobalExtern(uint32_t owner,
                                           NodeId /*list*/,
                                           Cursor& body,
                                           binary::CoreExternType& type)
    {
        auto const node = required(body);
        if (!node)
            return std::unexpected { node.error() };
        auto inner = isListOf(*node, "mut") ? bodyOf(*node) : Cursor { _tree, *node, NoNode };
        auto const isMutable = isListOf(*node, "mut");
        auto const typeNode = isMutable ? required(inner) : Result<NodeId> { *node };
        if (!typeNode)
            return std::unexpected { typeNode.error() };
        auto const value = coreValueType(owner, *typeNode);
        if (!value)
            return std::unexpected { value.error() };
        type.global =
            isoline::binary::GlobalType { .type = *value,
                                          .mutability = isMutable ? isoline::binary::Mutability::Var
                                                                  : isoline::binary::Mutability::Const };
        return expectEnd(inner);
    }

    Result<std::vector<binary::CanonOption>> Reading::canonOptions(uint32_t owner, Cursor& body)
    {
        auto options = std::vector<binary::CanonOption> {};
        while (!body.atEnd())
        {
            auto const node = body.peek();
            auto const text = isList(node) ? headWord(node) : keywordText(node);
            auto const takesIndex =
                isList(node) ? binary::OptionImmediate::Index : binary::OptionImmediate::None;
            auto const row =
                std::ranges::find_if(binary::CanonOptionTable, [&](binary::CanonOptionRow const& option) {
                    return option.text == text && option.immediate == takesIndex;
                });
            if (text.empty() || row == binary::CanonOptionTable.end())
                break;
            body.take();
            auto option = binary::CanonOption { .kind = row->kind, .index = 0 };
            if (row->immediate == binary::OptionImmediate::Index)
            {
                auto inner = bodyOf(node);
                auto const target = required(inner);
                if (!target)
                    return std::unexpected { target.error() };
                auto const id = corePrefixedReference(
                    owner, *CanonOptionSpaces[static_cast<std::size_t>(row->kind)], *target);
                if (!id)
                    return std::unexpected { id.error() };
                if (auto const end = expectEnd(inner); !end)
                    return std::unexpected { end.error() };
                option.index = *id;
            }
            options.push_back(option);
        }
        return options;
    }

    Result<uint32_t> Reading::canonIndex(uint32_t owner, Cursor& body, IndexSyntax syntax)
    {
        auto const node = required(body);
        if (!node)
            return std::unexpected { node.error() };
        switch (syntax)
        {
            case IndexSyntax::Type: return indexOrReference(owner, binary::Sort::Type, *node);
            case IndexSyntax::Func: {
                if (!isListOf(*node, "func"))
                    return std::unexpected { unexpected(*node) };
                auto inner = bodyOf(*node);
                return itemReference(owner, binary::Sort::Func, inner, AliasSugar::Export);
            }
            case IndexSyntax::Memory: {
                if (!isListOf(*node, "memory"))
                    return std::unexpected { unexpected(*node) };
                auto inner = bodyOf(*node);
                auto const target = required(inner);
                if (!target)
                    return std::unexpected { target.error() };
                auto const id = corePrefixedReference(owner, binary::Sort::CoreMemory, *target);
                if (!id)
                    return id;
                return expectEnd(inner).transform([&] { return *id; });
            }
            case IndexSyntax::CoreFunc:
                if (!isCoreListOf(*node, "func"))
                    return std::unexpected { unexpected(*node) };
                return corePrefixedReference(owner, binary::Sort::CoreFunc, *node);
            case IndexSyntax::CoreType: return corePrefixedReference(owner, binary::Sort::CoreType, *node);
            case IndexSyntax::CoreTable: return corePrefixedReference(owner, binary::Sort::CoreTable, *node);
            case IndexSyntax::Slot: return natural32(*node);
            // uncovered-begin(exhaustive-switch)
            case IndexSyntax::None:
            case IndexSyntax::LiftType:
                break;
                // uncovered-end(exhaustive-switch)
        }
        return std::unexpected { unexpected(*node) }; // uncovered(exhaustive-switch)
    }

    Result<void> Reading::canonResult(uint32_t owner, Cursor& body, binary::CanonDefinition& canon)
    {
        if (!isListOf(body.peek(), "result"))
            return {};
        auto inner = bodyOf(body.take());
        auto const node = required(inner);
        if (!node)
            return std::unexpected { node.error() };
        auto type = valueType(owner, *node);
        if (!type)
            return std::unexpected { type.error() };
        canon.result = *type;
        return expectEnd(inner);
    }

    Result<binary::CanonDefinition> Reading::canonBody(uint32_t owner,
                                                       Cursor& body,
                                                       binary::CanonBuiltinRow const& row,
                                                       std::optional<uint32_t> liftType)
    {
        auto canon = binary::CanonDefinition {
            .builtin = row.builtin, .immediates = {}, .options = {}, .result = std::nullopt
        };
        auto const& syntaxes = canonText(row.builtin).indices;
        auto next = std::size_t { 0 };
        for (auto const immediate: row.shape)
        {
            auto read = Result<void> {};
            if (immediate == binary::Immediate::Index)
            {
                auto const syntax = syntaxes[next++];
                auto const value = syntax == IndexSyntax::LiftType ? Result<uint32_t> { liftType.value_or(0) }
                                                                   : canonIndex(owner, body, syntax);
                if (!value)
                    return std::unexpected { value.error() };
                canon.immediates.push_back(*value);
            }
            else if (immediate == binary::Immediate::Options)
            {
                auto options = canonOptions(owner, body);
                if (!options)
                    return std::unexpected { options.error() };
                canon.options = std::move(*options);
            }
            else if (immediate == binary::Immediate::ResultList)
                read = canonResult(owner, body, canon);
            else if (immediate == binary::Immediate::CoreValueType)
            {
                auto const kind =
                    required(body).and_then([this](NodeId node) { return coreValueKind(node); });
                if (!kind)
                    return std::unexpected { kind.error() };
                canon.immediates.push_back(static_cast<uint32_t>(*kind));
            }
            else if (immediate == binary::Immediate::Async || immediate == binary::Immediate::Shared)
            {
                // `async?` and 🧵② `shared?`: present or not.
                auto const present =
                    isKeyword(body.peek(), immediate == binary::Immediate::Async ? "async" : "shared");
                if (present)
                    body.take();
                canon.immediates.push_back(present ? 1 : 0);
            }
            if (!read)
                return std::unexpected { read.error() };
        }
        return canon;
    }

    // ---- Fields ----

    Result<void> Reading::coreModule(NodeId field)
    {
        auto body = bodyOf(field);
        body.take(); // `module`
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        auto exports = inlineExports(body);
        if (!exports)
            return std::unexpected { exports.error() };
        auto const owner = newItem(field);
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        auto imported = inlineImport(body);
        if (!imported)
            return std::unexpected { imported.error() };
        if (*imported)
        {
            auto const type = typeUse(owner, body, ScopeForm::ModuleType);
            if (!type)
                return std::unexpected { type.error() };
            if (auto const end = expectEnd(body); !end)
                return std::unexpected { end.error() };
            setValue(owner,
                     binary::Definition {
                         binary::ImportDefinition { .name = std::move(**imported),
                                                    .type = { .sort = binary::Sort::CoreModule,
                                                              .index = *type,
                                                              .bound = binary::TypeBound::Equal } } });
        }
        else
        {
            // P10's reader reads the fields, its name section naming the module by the same `$id`, as wast's
            // core encoder does; the bytes of every module together stay within maxComponentBytes.
            auto const spent = static_cast<uint64_t>(_bytes.size());
            auto const room =
                spent >= _limits.maxComponentBytes ? uint64_t { 0 } : _limits.maxComponentBytes - spent;
            auto const budget = std::min<uint64_t>(room, _text.maxModuleBytes);
            auto module =
                isoline::text::ModuleReader { _text }.readFields(_tree, field, body.peek(), head->identifier);
            if (!module)
                return std::unexpected { module.error() };
            auto bytes = isoline::binary::encodeModule(*module, static_cast<std::size_t>(budget));
            if (!bytes)
                return std::unexpected { TextError { TextErrorCode::OutputTooLarge,
                                                     _tree.positionOf(field) } };
            auto const offset = static_cast<uint64_t>(_bytes.size());
            _bytes.insert(_bytes.end(), bytes->begin(), bytes->end());
            setValue(owner,
                     binary::Definition {
                         binary::CoreModuleDefinition { .offset = offset, .size = bytes->size() } });
        }
        _own.push_back(owner);
        appendExports(std::move(*exports), binary::Sort::CoreModule, owner, field);
        return {};
    }

    Result<void> Reading::coreInstance(NodeId field)
    {
        auto body = bodyOf(field);
        body.take(); // `instance`
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        auto const owner = newItem(field);
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        auto instance = binary::CoreInstanceDefinition {};
        if (isListOf(body.peek(), "instantiate"))
        {
            auto inner = bodyOf(body.take());
            auto const module = required(inner);
            if (!module)
                return std::unexpected { module.error() };
            auto instantiate = binary::CoreInstantiate {};
            auto const moduleId = indexOrReference(owner, binary::Sort::CoreModule, *module);
            if (!moduleId)
                return std::unexpected { moduleId.error() };
            instantiate.module = *moduleId;
            while (!inner.atEnd())
            {
                auto const argument = inner.take();
                if (!isListOf(argument, "with"))
                    return std::unexpected { unexpected(argument) };
                auto with = bodyOf(argument);
                auto name = readName(with);
                if (!name)
                    return std::unexpected { name.error() };
                auto const target = required(with);
                if (!target)
                    return std::unexpected { target.error() };
                if (!isListOf(*target, "instance"))
                    return std::unexpected { unexpected(*target) };
                auto targetBody = bodyOf(*target);
                auto id = Result<uint32_t> { 0 };
                if (isIndex(targetBody.peek()))
                    id = itemReference(owner, binary::Sort::CoreInstance, targetBody, AliasSugar::CoreExport);
                else
                {
                    // A bundle of exports: a core instance of its own before the field.
                    auto const bundle = newItem(*target);
                    auto exports = coreExports(bundle, targetBody);
                    if (!exports)
                        return std::unexpected { exports.error() };
                    setValue(bundle,
                             binary::Definition { binary::CoreInstanceDefinition { std::move(*exports) } });
                    _fields.push_back(bundle);
                    id = generatedReference(owner, binary::Sort::CoreInstance, bundle);
                }
                if (!id)
                    return std::unexpected { id.error() };
                if (auto const end = expectEnd(with); !end)
                    return std::unexpected { end.error() };
                instantiate.arguments.push_back(
                    binary::CoreInstantiateArgument { .name = std::move(*name), .instance = *id });
            }
            instance.expression = std::move(instantiate);
        }
        else
        {
            auto exports = coreExports(owner, body);
            if (!exports)
                return std::unexpected { exports.error() };
            instance.expression = std::move(*exports);
        }
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        setValue(owner, binary::Definition { std::move(instance) });
        _own.push_back(owner);
        return {};
    }

    Result<binary::CoreInlineExports> Reading::coreExports(uint32_t owner, Cursor& body)
    {
        auto exports = binary::CoreInlineExports {};
        while (isListOf(body.peek(), "export"))
        {
            auto inner = bodyOf(body.take());
            auto name = readName(inner);
            if (!name)
                return std::unexpected { name.error() };
            auto const target = required(inner);
            if (!target)
                return std::unexpected { target.error() };
            auto const keyword = isList(*target) ? headWord(*target) : std::string_view {};
            auto const sort = std::ranges::find_if(CoreExternSorts, [&](binary::Sort candidate) {
                return sortText(candidate).keyword == keyword;
            });
            if (sort == CoreExternSorts.end())
                return std::unexpected { unexpected(*target) };
            auto targetBody = bodyOf(*target);
            auto const id = itemReference(owner, *sort, targetBody, AliasSugar::CoreExport);
            if (!id)
                return std::unexpected { id.error() };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            exports.exports.push_back(
                binary::CoreInlineExport { .name = std::move(*name),
                                           .sort = static_cast<binary::CoreSort>(static_cast<uint8_t>(*sort)),
                                           .index = *id });
        }
        return exports;
    }

    Result<void> Reading::coreType(NodeId field)
    {
        auto body = bodyOf(field);
        body.take(); // `type`
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        auto const owner = newItem(field);
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        auto const node = required(body);
        if (!node)
            return std::unexpected { node.error() };
        if (isListOf(*node, "module"))
        {
            auto inner = bodyOf(*node);
            auto const child = newScope(ScopeForm::ModuleType, *node, inner.peek(), head->identifier);
            item(owner).child = child;
            setValue(
                owner,
                binary::Definition { binary::CoreTypeDefinition { binary::CoreModuleTypeDefinition {} } });
        }
        else if (isListOf(*node, "func"))
        {
            auto inner = bodyOf(*node);
            auto function = coreFunctionType(owner, inner);
            if (!function)
                return std::unexpected { function.error() };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            setValue(owner, binary::Definition { binary::CoreTypeDefinition { std::move(*function) } });
        }
        else
            return std::unexpected { isList(*node) ? unexpectedHead(*node) : unexpected(*node) };
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        _own.push_back(owner);
        return {};
    }

    Result<void> Reading::coreFunc(NodeId field)
    {
        auto body = bodyOf(field);
        body.take(); // `func`
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        auto const owner = newItem(field);
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        auto const node = required(body);
        if (!node)
            return std::unexpected { node.error() };
        if (isListOf(*node, "alias"))
        {
            auto inner = bodyOf(*node);
            if (auto const core = expectKeyword(inner, "core"); !core)
                return std::unexpected { core.error() };
            if (auto const keyword = expectKeyword(inner, "export"); !keyword)
                return std::unexpected { keyword.error() };
            auto alias = exportAlias(owner, inner, binary::AliasTarget::CoreExport, binary::Sort::CoreFunc);
            if (!alias)
                return std::unexpected { alias.error() };
            setValue(owner, binary::Definition { std::move(*alias) });
        }
        else if (isListOf(*node, "canon"))
        {
            auto inner = bodyOf(*node);
            auto const row = canonRowOf(inner);
            if (!row)
                return std::unexpected { row.error() };
            if ((*row)->builtin == binary::CanonBuiltin::Lift)
                return std::unexpected { unexpectedHead(*node) };
            auto canon = canonBody(owner, inner, **row, std::nullopt);
            if (!canon)
                return std::unexpected { canon.error() };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            setValue(owner, binary::Definition { std::move(*canon) });
        }
        else
            return std::unexpected { unexpected(*node) };
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        _own.push_back(owner);
        return {};
    }

    Result<binary::CanonBuiltinRow const*> Reading::canonRowOf(Cursor& body) const
    {
        auto const node = required(body);
        if (!node)
            return std::unexpected { node.error() };
        auto const row = isWord(*node) ? std::ranges::find(binary::CanonBuiltinTable,
                                                           _tree.textOf(*node),
                                                           &binary::CanonBuiltinRow::text)
                                       : binary::CanonBuiltinTable.end();
        if (row == binary::CanonBuiltinTable.end())
            return std::unexpected { unexpected(*node) };
        return &*row;
    }

    Result<binary::AliasDefinition> Reading::exportAlias(uint32_t owner,
                                                         Cursor& body,
                                                         binary::AliasTarget target,
                                                         binary::Sort sort)
    {
        auto const instance = required(body);
        if (!instance)
            return std::unexpected { instance.error() };
        auto const space =
            target == binary::AliasTarget::CoreExport ? binary::Sort::CoreInstance : binary::Sort::Instance;
        auto const id = indexReference(owner, space, *instance);
        if (!id)
            return std::unexpected { id.error() };
        auto name = readName(body);
        if (!name)
            return std::unexpected { name.error() };
        return binary::AliasDefinition { .sort = sort,
                                         .target = target,
                                         .instance = *id,
                                         .name = std::move(*name),
                                         .outerCount = 0,
                                         .outerIndex = 0 };
    }

    Result<void> Reading::nestedComponent(NodeId field)
    {
        auto body = bodyOf(field);
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        auto exports = inlineExports(body);
        if (!exports)
            return std::unexpected { exports.error() };
        auto const owner = newItem(field);
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        auto imported = inlineImport(body);
        if (!imported)
            return std::unexpected { imported.error() };
        if (*imported)
        {
            auto const type = typeUse(owner, body, ScopeForm::ComponentType);
            if (!type)
                return std::unexpected { type.error() };
            if (auto const end = expectEnd(body); !end)
                return std::unexpected { end.error() };
            setValue(owner,
                     binary::Definition {
                         binary::ImportDefinition { .name = std::move(**imported),
                                                    .type = { .sort = binary::Sort::Component,
                                                              .index = *type,
                                                              .bound = binary::TypeBound::Equal } } });
        }
        else
        {
            auto const child = newScope(ScopeForm::Component, field, body.peek(), head->identifier);
            _scopes[child].annotatedName = head->annotatedName;
            item(owner).child = child;
            setValue(owner, binary::Definition { binary::NestedComponent { .index = _scopes[child].pool } });
        }
        _own.push_back(owner);
        appendExports(std::move(*exports), binary::Sort::Component, owner, field);
        return {};
    }

    Result<binary::SortIndex> Reading::exportKind(uint32_t owner, NodeId node)
    {
        if (!isList(node))
            return std::unexpected { node == NoNode ? TextError { TextErrorCode::UnexpectedToken, {} }
                                                    : unexpected(node) };
        auto body = bodyOf(node);
        auto sort = binary::Sort::CoreModule;
        if (headWord(node) == "core")
        {
            if (auto const module = expectKeyword(body, "module"); !module)
                return std::unexpected { module.error() };
        }
        else
        {
            auto const keyword = headWord(node);
            auto const found = std::ranges::find_if(ExportSorts, [&](binary::Sort candidate) {
                return sortText(candidate).prefix == SortPrefix::None
                       && sortText(candidate).keyword == keyword;
            });
            if (found == ExportSorts.end())
                return std::unexpected { unexpectedHead(node) };
            sort = *found;
        }
        return itemReference(owner, sort, body, AliasSugar::Export).transform([&](uint32_t id) {
            return binary::SortIndex { .sort = sort, .index = id };
        });
    }

    Result<void> Reading::instance(NodeId field)
    {
        auto body = bodyOf(field);
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        auto exports = inlineExports(body);
        if (!exports)
            return std::unexpected { exports.error() };
        auto const owner = newItem(field);
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        auto imported = inlineImport(body);
        if (!imported)
            return std::unexpected { imported.error() };
        if (*imported)
        {
            auto const type = typeUse(owner, body, ScopeForm::InstanceType);
            if (!type)
                return std::unexpected { type.error() };
            setValue(owner,
                     binary::Definition {
                         binary::ImportDefinition { .name = std::move(**imported),
                                                    .type = { .sort = binary::Sort::Instance,
                                                              .index = *type,
                                                              .bound = binary::TypeBound::Equal } } });
        }
        else if (isListOf(body.peek(), "instantiate"))
        {
            auto inner = bodyOf(body.take());
            auto const component = required(inner);
            if (!component)
                return std::unexpected { component.error() };
            auto instantiate = binary::Instantiate {};
            auto const componentId = indexOrReference(owner, binary::Sort::Component, *component);
            if (!componentId)
                return std::unexpected { componentId.error() };
            instantiate.component = *componentId;
            while (!inner.atEnd())
            {
                auto const argument = inner.take();
                if (!isListOf(argument, "with"))
                    return std::unexpected { unexpected(argument) };
                auto with = bodyOf(argument);
                auto name = readName(with);
                if (!name)
                    return std::unexpected { name.error() };
                auto const target = required(with);
                if (!target)
                    return std::unexpected { target.error() };
                auto item = Result<binary::SortIndex> { binary::SortIndex { .sort = binary::Sort::Instance,
                                                                            .index = 0 } };
                auto targetBody = isList(*target) ? bodyOf(*target) : Cursor { _tree, *target, NoNode };
                if (isListOf(*target, "instance") && !isIndex(targetBody.peek()))
                {
                    // A bundle of exports: an instance of its own before the field.
                    auto const bundle = newItem(*target);
                    auto bundled = inlineInstanceExports(bundle, targetBody);
                    if (!bundled)
                        return std::unexpected { bundled.error() };
                    setValue(bundle,
                             binary::Definition { binary::InstanceDefinition { std::move(*bundled) } });
                    _fields.push_back(bundle);
                    item =
                        binary::SortIndex { .sort = binary::Sort::Instance,
                                            .index =
                                                generatedReference(owner, binary::Sort::Instance, bundle) };
                }
                else
                    item = exportKind(owner, *target);
                if (!item)
                    return std::unexpected { item.error() };
                if (auto const end = expectEnd(with); !end)
                    return std::unexpected { end.error() };
                instantiate.arguments.push_back(
                    binary::InstantiateArgument { .name = std::move(*name), .item = *item });
            }
            if (auto const end = expectEnd(body); !end)
                return std::unexpected { end.error() };
            setValue(owner, binary::Definition { binary::InstanceDefinition { std::move(instantiate) } });
        }
        else
        {
            auto bundled = inlineInstanceExports(owner, body);
            if (!bundled)
                return std::unexpected { bundled.error() };
            setValue(owner, binary::Definition { binary::InstanceDefinition { std::move(*bundled) } });
        }
        _own.push_back(owner);
        appendExports(std::move(*exports), binary::Sort::Instance, owner, field);
        return {};
    }

    Result<binary::InlineExports> Reading::inlineInstanceExports(uint32_t owner, Cursor& body)
    {
        auto exports = binary::InlineExports {};
        while (!body.atEnd())
        {
            auto const node = body.take();
            if (!isListOf(node, "export"))
                return std::unexpected { unexpected(node) };
            auto inner = bodyOf(node);
            std::ignore =
                optionalIdentifier(inner); // An inline export's `$id` binds nothing (wast registers none).
            if (auto const name = optionalNameAnnotation(inner); !name)
                return std::unexpected { name.error() };
            auto name = externName(inner);
            if (!name)
                return std::unexpected { name.error() };
            auto const target = required(inner);
            if (!target)
                return std::unexpected { target.error() };
            auto const item = exportKind(owner, *target);
            if (!item)
                return std::unexpected { item.error() };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            exports.exports.push_back(binary::InlineExport { .name = std::move(*name), .item = *item });
        }
        return exports;
    }

    Result<void> Reading::alias(NodeId field)
    {
        auto body = bodyOf(field);
        auto const owner = newItem(field);
        auto const readKind = [&](NodeId kindNode,
                                  std::span<binary::Sort const> sorts,
                                  SortPrefix requiredPrefix) -> Result<binary::Sort> {
            if (!isList(kindNode))
                return std::unexpected { kindNode == NoNode ? missing(body) : unexpected(kindNode) };
            auto inner = bodyOf(kindNode);
            auto keyword = headWord(kindNode);
            auto prefix = SortPrefix::None;
            if (keyword == "core")
            {
                prefix = SortPrefix::Core;
                keyword = isWord(inner.peek()) ? _tree.textOf(inner.take()) : std::string_view {};
            }
            auto const found = std::ranges::find_if(sorts, [&](binary::Sort candidate) {
                return sortText(candidate).prefix == prefix && sortText(candidate).keyword == keyword;
            });
            if (found == sorts.end() || (requiredPrefix == SortPrefix::Core && prefix != SortPrefix::Core))
                return std::unexpected { unexpectedHead(kindNode) };
            auto const head = readHead(inner);
            if (!head)
                return std::unexpected { head.error() };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            item(owner).identifier = head->identifier;
            item(owner).annotatedName = head->annotatedName;
            return *found;
        };
        auto const kind = required(body);
        if (!kind)
            return std::unexpected { kind.error() };
        auto definition = binary::AliasDefinition {};
        if (isKeyword(*kind, "outer"))
        {
            auto const count = required(body);
            if (!count)
                return std::unexpected { count.error() };
            auto const index = required(body);
            if (!index)
                return std::unexpected { index.error() };
            if (!isIndex(*count))
                return std::unexpected { unexpected(*count) };
            if (!isIndex(*index))
                return std::unexpected { unexpected(*index) };
            auto const sort = readKind(body.peek(), OuterAliasSorts, SortPrefix::None);
            if (!sort)
                return std::unexpected { sort.error() };
            body.take();
            definition = binary::AliasDefinition { .sort = *sort,
                                                   .target = binary::AliasTarget::Outer,
                                                   .instance = 0,
                                                   .name = {},
                                                   .outerCount = 0,
                                                   .outerIndex = 0 };
            item(owner).outer = OuterTarget { .count = *count, .index = *index, .sort = *sort };
        }
        else if (isKeyword(*kind, "export") || isKeyword(*kind, "core"))
        {
            auto const core = isKeyword(*kind, "core");
            if (core)
                if (auto const keyword = expectKeyword(body, "export"); !keyword)
                    return std::unexpected { keyword.error() };
            auto const target = core ? binary::AliasTarget::CoreExport : binary::AliasTarget::Export;
            auto alias = exportAlias(owner, body, target, binary::Sort::Func);
            if (!alias)
                return std::unexpected { alias.error() };
            auto const sort = core ? readKind(body.peek(), CoreExternSorts, SortPrefix::Core)
                                   : readKind(body.peek(), ExportSorts, SortPrefix::None);
            if (!sort)
                return std::unexpected { sort.error() };
            body.take();
            alias->sort = *sort;
            definition = std::move(*alias);
        }
        else
            return std::unexpected { unexpected(*kind) };
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        setValue(owner, binary::Definition { std::move(definition) });
        _own.push_back(owner);
        return {};
    }

    Result<void> Reading::type(NodeId field)
    {
        auto body = bodyOf(field);
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        auto exports = std::vector<binary::NameAttributes> {};
        if (scope().form == ScopeForm::Component)
        {
            auto inlined = inlineExports(body);
            if (!inlined)
                return std::unexpected { inlined.error() };
            exports = std::move(*inlined);
        }
        auto const owner = newItem(field);
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        auto const node = required(body);
        if (!node)
            return std::unexpected { node.error() };
        auto definition = typeDefinition(owner, *node, head->identifier);
        if (!definition)
            return std::unexpected { definition.error() };
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        setValue(owner, binary::Definition { std::move(*definition) });
        _own.push_back(owner);
        appendExports(std::move(exports), binary::Sort::Type, owner, field);
        return {};
    }

    Result<void> Reading::canon(NodeId field)
    {
        auto body = bodyOf(field);
        auto const row = canonRowOf(body);
        if (!row)
            return std::unexpected { row.error() };
        // The last node defines the function: `(func $id? typeuse)` for a lift, `(core func $id?)` otherwise.
        auto last = NoNode;
        for (auto scan = body; !scan.atEnd();)
            last = scan.take();
        auto const owner = newItem(field);
        auto const lift = (*row)->builtin == binary::CanonBuiltin::Lift;
        if (lift ? !isListOf(last, "func") : !isCoreListOf(last, "func"))
            return std::unexpected { last == NoNode ? missing(body) : unexpected(last) };
        auto trailer = bodyOf(last);
        if (!lift)
            trailer.take();
        auto const head = readHead(trailer);
        if (!head)
            return std::unexpected { head.error() };
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        // A lift's type use first, as wast resolves it.
        auto liftType = std::optional<uint32_t> {};
        if (lift)
        {
            auto const type = functionTypeUse(owner, trailer);
            if (!type)
                return std::unexpected { type.error() };
            liftType = *type;
        }
        if (auto const end = expectEnd(trailer); !end)
            return std::unexpected { end.error() };
        auto definition = canonBody(owner, body, **row, liftType);
        if (!definition)
            return std::unexpected { definition.error() };
        if (body.peek() != last)
            return std::unexpected { unexpected(body.peek()) };
        body.take();
        setValue(owner, binary::Definition { std::move(*definition) });
        _own.push_back(owner);
        return {};
    }

    Result<void> Reading::func(NodeId field)
    {
        auto body = bodyOf(field);
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        auto exports = inlineExports(body);
        if (!exports)
            return std::unexpected { exports.error() };
        auto const owner = newItem(field);
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        auto imported = inlineImport(body);
        if (!imported)
            return std::unexpected { imported.error() };
        if (*imported)
        {
            auto const type = functionTypeUse(owner, body);
            if (!type)
                return std::unexpected { type.error() };
            setValue(owner,
                     binary::Definition {
                         binary::ImportDefinition { .name = std::move(**imported),
                                                    .type = { .sort = binary::Sort::Func,
                                                              .index = *type,
                                                              .bound = binary::TypeBound::Equal } } });
        }
        else if (isListOf(body.peek(), "alias"))
        {
            auto inner = bodyOf(body.take());
            if (auto const keyword = expectKeyword(inner, "export"); !keyword)
                return std::unexpected { keyword.error() };
            auto alias = exportAlias(owner, inner, binary::AliasTarget::Export, binary::Sort::Func);
            if (!alias)
                return std::unexpected { alias.error() };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            setValue(owner, binary::Definition { std::move(*alias) });
        }
        else
        {
            auto const type = functionTypeUse(owner, body);
            if (!type)
                return std::unexpected { type.error() };
            auto const node = required(body);
            if (!node)
                return std::unexpected { node.error() };
            if (!isListOf(*node, "canon"))
                return std::unexpected { unexpected(*node) };
            auto inner = bodyOf(*node);
            if (auto const keyword = expectKeyword(inner, "lift"); !keyword)
                return std::unexpected { keyword.error() };
            auto const& row = *std::ranges::find(
                binary::CanonBuiltinTable, binary::CanonBuiltin::Lift, &binary::CanonBuiltinRow::builtin);
            auto canon = canonBody(owner, inner, row, *type);
            if (!canon)
                return std::unexpected { canon.error() };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
            setValue(owner, binary::Definition { std::move(*canon) });
        }
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        _own.push_back(owner);
        appendExports(std::move(*exports), binary::Sort::Func, owner, field);
        return {};
    }

    Result<binary::ExternDesc> Reading::itemSignature(uint32_t owner, NodeId list, Head* head)
    {
        if (!isList(list))
            return std::unexpected { list == NoNode ? TextError { TextErrorCode::UnexpectedToken, {} }
                                                    : unexpected(list) };
        auto body = bodyOf(list);
        auto keyword = headWord(list);
        if (keyword == "core")
        {
            if (auto const module = expectKeyword(body, "module"); !module)
                return std::unexpected { module.error() };
            keyword = "core module";
        }
        if (head != nullptr)
        {
            auto read = readHead(body);
            if (!read)
                return std::unexpected { read.error() };
            *head = std::move(*read);
        }
        auto result = Result<binary::ExternDesc> { binary::ExternDesc {} };
        auto const described = [&](binary::Sort sort, Result<uint32_t> index) -> Result<binary::ExternDesc> {
            if (!index)
                return std::unexpected { index.error() };
            return binary::ExternDesc { .sort = sort, .index = *index, .bound = binary::TypeBound::Equal };
        };
        if (keyword == "core module")
            result = described(binary::Sort::CoreModule, typeUse(owner, body, ScopeForm::ModuleType));
        else if (keyword == "func")
            result = described(binary::Sort::Func, functionTypeUse(owner, body));
        else if (keyword == "component")
            result = described(binary::Sort::Component, typeUse(owner, body, ScopeForm::ComponentType));
        else if (keyword == "instance")
            result = described(binary::Sort::Instance, typeUse(owner, body, ScopeForm::InstanceType));
        else if (keyword == "type")
        {
            auto const bound = required(body);
            if (!bound)
                return std::unexpected { bound.error() };
            auto inner = isList(*bound) ? bodyOf(*bound) : Cursor { _tree, *bound, NoNode };
            if (isListOf(*bound, "eq"))
            {
                auto const index = required(inner);
                if (!index)
                    return std::unexpected { index.error() };
                result = described(binary::Sort::Type, indexReference(owner, binary::Sort::Type, *index));
            }
            else if (isListOf(*bound, "sub"))
            {
                if (auto const resource = expectKeyword(inner, "resource"); !resource)
                    return std::unexpected { resource.error() };
                result = binary::ExternDesc { .sort = binary::Sort::Type,
                                              .index = 0,
                                              .bound = binary::TypeBound::SubResource };
            }
            else
                return std::unexpected { unexpected(*bound) };
            if (auto const end = expectEnd(inner); !end)
                return std::unexpected { end.error() };
        }
        else
            return std::unexpected { unexpectedHead(list) };
        if (!result)
            return result;
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        return result;
    }

    Result<void> Reading::import(NodeId field)
    {
        auto body = bodyOf(field);
        auto name = externName(body);
        if (!name)
            return std::unexpected { name.error() };
        auto const owner = newItem(field);
        auto head = Head {};
        auto const type = itemSignature(owner, body.atEnd() ? NoNode : body.take(), &head);
        if (!type)
            return std::unexpected { type.error() };
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        item(owner).identifier = head.identifier;
        item(owner).annotatedName = std::move(head.annotatedName);
        setValue(owner,
                 binary::Definition { binary::ImportDefinition { .name = std::move(*name), .type = *type } });
        _own.push_back(owner);
        return {};
    }

    Result<void> Reading::exportDefinition(NodeId field)
    {
        auto body = bodyOf(field);
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        auto name = externName(body);
        if (!name)
            return std::unexpected { name.error() };
        auto const owner = newItem(field);
        item(owner).identifier = head->identifier;
        item(owner).annotatedName = head->annotatedName;
        auto const kindNode = required(body);
        if (!kindNode)
            return std::unexpected { kindNode.error() };
        // The ascribed type first, as wast resolves it.
        auto type = std::optional<binary::ExternDesc> {};
        if (!body.atEnd())
        {
            auto const ascribed = itemSignature(owner, body.take(), nullptr);
            if (!ascribed)
                return std::unexpected { ascribed.error() };
            type = *ascribed;
        }
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        auto const exported = exportKind(owner, *kindNode);
        if (!exported)
            return std::unexpected { exported.error() };
        setValue(owner,
                 binary::Definition { binary::ExportDefinition {
                     .name = std::move(*name), .item = *exported, .type = type } });
        _own.push_back(owner);
        return {};
    }

    Result<void> Reading::exportDeclaration(NodeId field)
    {
        auto body = bodyOf(field);
        auto name = externName(body);
        if (!name)
            return std::unexpected { name.error() };
        auto const owner = newItem(field);
        auto head = Head {};
        auto const type = itemSignature(owner, body.atEnd() ? NoNode : body.take(), &head);
        if (!type)
            return std::unexpected { type.error() };
        if (auto const end = expectEnd(body); !end)
            return std::unexpected { end.error() };
        item(owner).identifier = head.identifier;
        item(owner).annotatedName = std::move(head.annotatedName);
        setValue(
            owner,
            binary::Definition { binary::ExportDeclaration { .name = std::move(*name), .type = *type } });
        _own.push_back(owner);
        return {};
    }

    Result<void> Reading::custom(NodeId field)
    {
        auto annotation = isoline::text::readCustomAnnotation(_tree, field);
        if (!annotation)
            return std::unexpected { annotation.error() };
        // A component's custom section stands where it is written: a placement is a module's.
        for (auto const child: _tree.children(field))
            if (isList(child))
                return std::unexpected { TextError { TextErrorCode::CustomUnexpectedToken,
                                                     _tree.positionOf(child) } };
        auto const owner = newItem(field);
        auto const offset = static_cast<uint64_t>(_bytes.size());
        auto const& payload = annotation->section.bytes;
        _bytes.insert(_bytes.end(), payload.begin(), payload.end());
        setValue(owner,
                 binary::Definition { binary::CustomDefinition {
                     .name = annotation->section.name, .offset = offset, .size = payload.size() } });
        _own.push_back(owner);
        return {};
    }

    // ---- Module type declarations ----

    Result<void> Reading::moduleTypeType(NodeId declaration)
    {
        auto body = bodyOf(declaration);
        auto const owner = newItem(declaration);
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        item(owner).identifier = head->identifier;
        auto const node = required(body);
        if (!node)
            return std::unexpected { node.error() };
        if (!isListOf(*node, "func"))
            return std::unexpected { isList(*node) ? unexpectedHead(*node) : unexpected(*node) };
        auto inner = bodyOf(*node);
        auto function = coreFunctionType(owner, inner);
        if (!function)
            return std::unexpected { function.error() };
        if (auto const end = expectEnd(inner); !end)
            return end;
        if (auto const end = expectEnd(body); !end)
            return end;
        // Explicit types are found by their key, the last of equal ones (wast's func_type_to_idx).
        scope().moduleTypeKeys.insert_or_assign(*function, owner);
        setValue(
            owner,
            binary::CoreModuleDeclaration { binary::CoreTypeDeclaration { .type = std::move(*function) } });
        _own.push_back(owner);
        return {};
    }

    Result<void> Reading::moduleTypeAlias(NodeId declaration)
    {
        auto body = bodyOf(declaration);
        auto const owner = newItem(declaration);
        if (auto const outer = expectKeyword(body, "outer"); !outer)
            return outer;
        auto const count = required(body);
        if (!count)
            return std::unexpected { count.error() };
        auto const index = required(body);
        if (!index)
            return std::unexpected { index.error() };
        if (!isIndex(*count))
            return std::unexpected { unexpected(*count) };
        if (!isIndex(*index))
            return std::unexpected { unexpected(*index) };
        auto const kind = required(body);
        if (!kind)
            return std::unexpected { kind.error() };
        if (!isListOf(*kind, "type"))
            return std::unexpected { isList(*kind) ? unexpectedHead(*kind) : unexpected(*kind) };
        auto inner = bodyOf(*kind);
        auto const head = readHead(inner);
        if (!head)
            return std::unexpected { head.error() };
        if (auto const end = expectEnd(inner); !end)
            return end;
        if (auto const end = expectEnd(body); !end)
            return end;
        item(owner).identifier = head->identifier;
        item(owner).outer = OuterTarget { .count = *count, .index = *index, .sort = binary::Sort::CoreType };
        setValue(owner, binary::CoreModuleDeclaration { binary::CoreOuterAliasDeclaration {} });
        _own.push_back(owner);
        return {};
    }

    Result<void> Reading::moduleTypeImport(NodeId declaration)
    {
        auto body = bodyOf(declaration);
        auto const owner = newItem(declaration);
        auto module = readName(body);
        if (!module)
            return std::unexpected { module.error() };
        auto name = readName(body);
        if (!name)
            return std::unexpected { name.error() };
        auto const type = coreExternType(owner, body.atEnd() ? NoNode : body.take());
        if (!type)
            return std::unexpected { type.error() };
        if (auto const end = expectEnd(body); !end)
            return end;
        setValue(owner,
                 binary::CoreModuleDeclaration { binary::CoreImportDeclaration {
                     .module = std::move(*module), .name = std::move(*name), .type = *type } });
        _own.push_back(owner);
        return {};
    }

    Result<void> Reading::moduleTypeExport(NodeId declaration)
    {
        auto body = bodyOf(declaration);
        auto const owner = newItem(declaration);
        auto name = readName(body);
        if (!name)
            return std::unexpected { name.error() };
        auto const type = coreExternType(owner, body.atEnd() ? NoNode : body.take());
        if (!type)
            return std::unexpected { type.error() };
        if (auto const end = expectEnd(body); !end)
            return end;
        setValue(owner,
                 binary::CoreModuleDeclaration {
                     binary::CoreExportDeclaration { .name = std::move(*name), .type = *type } });
        _own.push_back(owner);
        return {};
    }

    // ---- Scopes ----

    Result<void> Reading::readField(NodeId field)
    {
        if (_tree.annotationKindOf(field) == AnnotationKind::Custom && scope().form == ScopeForm::Component)
            return custom(field);
        if (!isList(field))
            return std::unexpected { unexpected(field) };
        auto const first = headWord(field);
        auto const second = first == "core" ? keywordText(bodyOf(field).peek()) : std::string_view {};
        auto const form = static_cast<uint8_t>(uint8_t { 1 } << static_cast<uint8_t>(scope().form));
        auto const row = std::ranges::find_if(FieldRows, [&](FieldRow const& candidate) {
            return candidate.first == first && candidate.second == second && (candidate.forms & form) != 0;
        });
        if (row == FieldRows.end())
        {
            if (first == "core" && !bodyOf(field).atEnd())
                return std::unexpected { unexpected(bodyOf(field).peek()) };
            return std::unexpected { unexpectedHead(field) };
        }
        return (this->*(row->read))(field);
    }

    Result<void> Reading::readScope(uint32_t scopeId)
    {
        _scope = scopeId;
        if (scope().depth > _limits.maxNesting)
            return std::unexpected { TextError { TextErrorCode::NestingLimitExceeded,
                                                 _tree.positionOf(scope().list) } };
        for (auto fields = Cursor { _tree, scope().list, scope().first }; !fields.atEnd();)
        {
            _types.clear();
            _fields.clear();
            _own.clear();
            if (auto const read = readField(fields.take()); !read)
                return read;
            auto& expanded = scope().expanded;
            expanded.insert(expanded.end(), _types.begin(), _types.end());
            expanded.insert(expanded.end(), _fields.begin(), _fields.end());
            expanded.insert(expanded.end(), _own.begin(), _own.end());
        }
        auto& current = scope();
        current.expanded.insert(current.expanded.end(), current.appended.begin(), current.appended.end());
        if (auto const expanded = expandAliases(); !expanded)
            return expanded;
        if (auto const resolved = resolve(); !resolved)
            return resolved;
        emit();
        return {};
    }

    Result<std::string> Reading::nameOfItem(Item const& entry) const
    {
        return identifierOf(entry.identifier);
    }

    Result<void> Reading::expandAliases()
    {
        // wast's first resolution phase: the identifiers the scope defines, then, per item, the aliases its
        // references need, each inserted just before it.
        auto early = Namespaces {};
        for (auto const id: scope().expanded)
        {
            auto const& entry = scope().items[id];
            if (entry.identifier == NoNode)
                continue;
            auto name = nameOfItem(entry);
            if (!name)
                return std::unexpected { name.error() };
            auto const sort = sortOf(entry);
            if (!early[static_cast<std::size_t>(*sort)].emplace(*name, 0).second)
                return std::unexpected { TextError {
                    TextErrorCode::DuplicateIdentifier,
                    _tree.positionOf(entry.identifier),
                    std::format("{} ${}", sortText(*sort).category, *name) } };
        }
        auto order = std::vector<uint32_t> {};
        auto const expanded = scope().expanded;
        for (auto const id: expanded)
        {
            auto const references = scope().items[id].references;
            for (auto const reference: references)
                if (auto const inserted = insertAliases(reference, early, order); !inserted)
                    return inserted;
            order.push_back(id);
        }
        scope().expanded = std::move(order);
        return {};
    }

    Result<void> Reading::insertAliases(uint32_t referenceId,
                                        Namespaces const& early,
                                        std::vector<uint32_t>& order)
    {
        auto const original = scope().references[referenceId];
        auto const redirect = [&](uint32_t alias) {
            auto& reference = scope().references[referenceId];
            reference.form = ReferenceForm::Generated;
            reference.value = alias;
            reference.exportNames.clear();
            order.push_back(alias);
        };
        if (!original.exportNames.empty())
        {
            auto const core = original.sugar == AliasSugar::CoreExport;
            if (core && std::ranges::find(CoreExternSorts, original.space) == CoreExternSorts.end())
                // "core instances cannot export this kind of item" (wast resolve.rs).
                return std::unexpected { unexpected(original.exportNames.front()) };
            auto previous = std::optional<uint32_t> {};
            for (auto const position: std::views::iota(std::size_t { 0 }, original.exportNames.size()))
            {
                auto const exportName = original.exportNames[position];
                auto name = nameOf(exportName);
                if (!name)
                    return std::unexpected { name.error() };
                auto const last = position + 1 == original.exportNames.size();
                auto const alias = newItem(exportName);
                auto const space = core ? binary::Sort::CoreInstance : binary::Sort::Instance;
                auto const instance =
                    previous
                        ? generatedReference(alias, space, *previous)
                        : reference(
                              alias,
                              referenceTo(space, original.form, Reach::Local, original.node, original.value));
                setValue(alias,
                         binary::Definition { binary::AliasDefinition {
                             .sort = last ? original.space : binary::Sort::Instance,
                             .target = core ? binary::AliasTarget::CoreExport : binary::AliasTarget::Export,
                             .instance = instance,
                             .name = std::move(*name),
                             .outerCount = 0,
                             .outerIndex = 0 } });
                if (!last)
                    order.push_back(alias);
                previous = alias;
            }
            redirect(*previous);
            return {};
        }
        if (original.form != ReferenceForm::Identifier || original.reach != Reach::Enclosing)
            return {};
        auto name = identifierOf(original.node);
        if (!name)
            return std::unexpected { name.error() };
        auto const space = static_cast<std::size_t>(original.space);
        if (early[space].contains(*name))
            return {};
        auto depth = uint32_t { 1 };
        for (auto enclosing = scope().parent; enclosing; enclosing = _scopes[*enclosing].parent, ++depth)
        {
            auto const& names = _scopes[*enclosing].names[space];
            auto const found = names.find(*name);
            if (found == names.end())
                continue;
            if (std::ranges::find(OuterAliasSorts, original.space) == OuterAliasSorts.end())
                return std::unexpected { TextError {
                    TextErrorCode::UnknownIdentifier,
                    _tree.positionOf(original.node),
                    std::format("outer {} ${}", sortText(original.space).category, *name) } };
            auto const alias = newItem(original.node);
            setValue(alias,
                     binary::Definition { binary::AliasDefinition { .sort = original.space,
                                                                    .target = binary::AliasTarget::Outer,
                                                                    .instance = 0,
                                                                    .name = {},
                                                                    .outerCount = depth,
                                                                    .outerIndex = found->second } });
            redirect(alias);
            return {};
        }
        return {};
    }

    Result<void> Reading::resolve()
    {
        auto& current = scope();
        auto counters = std::array<uint32_t, binary::SortCount> {};
        current.indices.assign(current.items.size(), 0);
        for (auto const id: current.expanded)
        {
            auto const& entry = current.items[id];
            auto const sort = sortOf(entry);
            if (!sort)
                continue;
            auto const index = counters[static_cast<std::size_t>(*sort)]++;
            current.indices[id] = index;
            if (entry.identifier == NoNode)
                continue;
            // expandAliases() decoded every identifier already.
            auto const name = nameOfItem(entry);
            Guarantee(name.has_value());
            // The identifiers are expandAliases()'s, which refused duplicates; the aliases it added have
            // none.
            auto const inserted = current.names[static_cast<std::size_t>(*sort)].emplace(*name, index).second;
            Guarantee(inserted);
        }
        for (auto const id: current.expanded)
        {
            for (auto const referenceId: current.items[id].references)
            {
                auto& reference = current.references[referenceId];
                switch (reference.form)
                {
                    case ReferenceForm::Number: reference.resolved = reference.value; break;
                    case ReferenceForm::Generated:
                        reference.resolved = current.indices[reference.value];
                        break;
                    case ReferenceForm::Identifier: {
                        auto name = identifierOf(reference.node);
                        if (!name)
                            return std::unexpected { name.error() };
                        auto const& names = current.names[static_cast<std::size_t>(reference.space)];
                        auto const found = names.find(*name);
                        if (found == names.end())
                            return std::unexpected { TextError {
                                TextErrorCode::UnknownIdentifier,
                                _tree.positionOf(reference.node),
                                std::format("{} ${}", sortText(reference.space).category, *name) } };
                        reference.resolved = found->second;
                        break;
                    }
                }
            }
            if (auto const& outer = current.items[id].outer)
                if (auto const resolved = resolveOuter(id, *outer); !resolved)
                    return resolved;
        }
        return {};
    }

    Result<void> Reading::resolveOuter(uint32_t id, OuterTarget const& outer)
    {
        auto count = uint32_t { 0 };
        auto index = uint32_t { 0 };
        if (isNatural(outer.count) && isNatural(outer.index))
        {
            // Both numeric: kept as written, for validation to check (wast short-circuits here too).
            auto const written = natural32(outer.count);
            if (!written)
                return std::unexpected { written.error() };
            auto const target = natural32(outer.index);
            if (!target)
                return std::unexpected { target.error() };
            count = *written;
            index = *target;
        }
        else
        {
            auto chain = std::vector<uint32_t> {};
            for (auto at = std::optional { _scope }; at; at = _scopes[*at].parent)
                chain.push_back(*at);
            if (isNatural(outer.count))
            {
                auto const written = natural32(outer.count);
                if (!written)
                    return std::unexpected { written.error() };
                if (*written >= chain.size())
                    return std::unexpected { TextError { TextErrorCode::UnknownIdentifier,
                                                         _tree.positionOf(outer.count),
                                                         std::format("outer count {}", *written) } };
                count = *written;
            }
            else
            {
                auto name = identifierOf(outer.count);
                if (!name)
                    return std::unexpected { name.error() };
                auto const found = std::ranges::find_if(chain, [&](uint32_t at) {
                    auto const identifier = _scopes[at].identifier;
                    if (identifier == NoNode)
                        return false;
                    auto const scopeName = identifierOf(identifier);
                    return scopeName && *scopeName == *name;
                });
                if (found == chain.end())
                    return std::unexpected { TextError { TextErrorCode::UnknownIdentifier,
                                                         _tree.positionOf(outer.count),
                                                         std::format("outer component ${}", *name) } };
                count = static_cast<uint32_t>(found - chain.begin());
            }
            auto const& names = _scopes[chain[count]].names[static_cast<std::size_t>(outer.sort)];
            if (isNatural(outer.index))
            {
                auto const written = natural32(outer.index);
                if (!written)
                    return std::unexpected { written.error() };
                index = *written;
            }
            else
            {
                auto name = identifierOf(outer.index);
                if (!name)
                    return std::unexpected { name.error() };
                auto const found = names.find(*name);
                if (found == names.end())
                    return std::unexpected { TextError {
                        TextErrorCode::UnknownIdentifier,
                        _tree.positionOf(outer.index),
                        std::format("{} ${}", sortText(outer.sort).category, *name) } };
                index = found->second;
            }
        }
        auto& value = scope().items[id].value;
        if (auto* definition = std::get_if<binary::Definition>(&value))
        {
            auto& alias = std::get<binary::AliasDefinition>(*definition);
            alias.outerCount = count;
            alias.outerIndex = index;
        }
        else
        {
            auto& alias =
                std::get<binary::CoreOuterAliasDeclaration>(std::get<binary::CoreModuleDeclaration>(value));
            alias.outerCount = count;
            alias.outerIndex = index;
        }
        return {};
    }

    void Reading::emit()
    {
        auto& current = scope();
        auto const patch = [&](Item& entry, auto& value) {
            auto visited = std::size_t { 0 };
            forEachIndex(value, [&](uint32_t& field) {
                field = current.references[field].resolved;
                ++visited;
            });
            // Every reference an item made fills one index field of it, and only those.
            Guarantee(visited == entry.references.size());
        };
        if (current.form == ScopeForm::ModuleType)
        {
            auto declarations = std::vector<binary::CoreModuleDeclaration> {};
            for (auto const id: current.expanded)
            {
                auto& entry = current.items[id];
                auto& declaration = std::get<binary::CoreModuleDeclaration>(entry.value);
                patch(entry, declaration);
                declarations.push_back(std::move(declaration));
            }
            auto& owner = _components[current.target.scope].definitions[current.target.definition];
            std::get<binary::CoreModuleTypeDefinition>(std::get<binary::CoreTypeDefinition>(owner).type)
                .declarations = std::move(declarations);
            return;
        }
        auto definitions = std::vector<binary::Definition> {};
        for (auto const id: current.expanded)
        {
            auto& entry = current.items[id];
            auto& definition = std::get<binary::Definition>(entry.value);
            patch(entry, definition);
            if (entry.child && _scopes[*entry.child].form == ScopeForm::ModuleType)
                _scopes[*entry.child].target =
                    ModuleTarget { .scope = current.pool, .definition = definitions.size() };
            definitions.push_back(std::move(definition));
        }
        if (current.form == ScopeForm::Component)
            emitNames(definitions);
        _components[current.pool].definitions = std::move(definitions);
    }

    void Reading::emitNames(std::vector<binary::Definition>& definitions)
    {
        auto& current = scope();
        auto const nameOfEntry =
            [&](NodeId identifier,
                std::optional<std::string> const& annotated) -> std::optional<std::string> {
            if (annotated)
                return annotated;
            if (identifier == NoNode)
                return std::nullopt;
            auto name = identifierOf(identifier); // read once already: it decodes
            return name ? std::optional { std::move(*name) } : std::nullopt;
        };
        auto payload = std::vector<std::byte> {};
        if (auto const own = nameOfEntry(current.identifier, current.annotatedName))
        {
            auto subsection = std::vector<std::byte> {};
            appendName(subsection, *own);
            appendSubsection(payload, ComponentNameSubsection, subsection);
        }
        for (auto const& row: SortTexts)
        {
            auto entries = std::vector<std::pair<uint32_t, std::string>> {};
            for (auto const id: current.expanded)
            {
                auto const& entry = current.items[id];
                if (sortOf(entry) != row.sort)
                    continue;
                if (auto name = nameOfEntry(entry.identifier, entry.annotatedName))
                    entries.emplace_back(current.indices[id], std::move(*name));
            }
            if (entries.empty())
                continue;
            auto subsection = std::vector<std::byte> { std::byte { row.nameByte } };
            if (row.coreNameByte)
                subsection.push_back(std::byte { *row.coreNameByte });
            appendLeb(subsection, entries.size());
            for (auto const& [index, name]: entries)
            {
                appendLeb(subsection, index);
                appendName(subsection, name);
            }
            appendSubsection(payload, SortNamesSubsection, subsection);
        }
        if (payload.empty())
            return;
        auto const offset = static_cast<uint64_t>(_bytes.size());
        _bytes.insert(_bytes.end(), payload.begin(), payload.end());
        definitions.emplace_back(binary::CustomDefinition {
            .name = std::string { ComponentNameSection }, .offset = offset, .size = payload.size() });
    }

    Result<binary::ComponentPool> Reading::read(NodeId component)
    {
        if (headWord(component) != "component")
            return std::unexpected { isList(component) ? unexpectedHead(component) : unexpected(component) };
        auto body = bodyOf(component);
        if (isKeyword(body.peek(), "definition"))
            body.take();
        auto const head = readHead(body);
        if (!head)
            return std::unexpected { head.error() };
        if (binaryKeywordOf(_tree, component))
        {
            auto const keyword = body.take();
            auto const bytes = binaryFormBytes(_tree, component, keyword, _limits.maxComponentBytes);
            if (!bytes)
                return std::unexpected { bytes.error() };
            auto decoded = binary::decodeComponent(*bytes, _features, _limits);
            if (!decoded)
                return std::unexpected { TextError { TextErrorCode::ComponentBinaryMalformed,
                                                     _tree.positionOf(keyword),
                                                     binary::specMessage(decoded.error()) } };
            return std::move(*decoded);
        }
        auto root = Scope {};
        root.list = component;
        root.first = body.peek();
        root.identifier = head->identifier;
        root.annotatedName = head->annotatedName;
        _scopes.push_back(std::move(root));
        _components.push_back(binary::Component { .kind = binary::ScopeKind::Component, .definitions = {} });
        // Each scope is read once its enclosing scope is resolved, from a work list rather than recursively.
        auto work = std::vector<uint32_t> { 0 };
        while (!work.empty())
        {
            auto const next = work.back();
            work.pop_back();
            if (auto const read = readScope(next); !read)
                return std::unexpected { read.error() };
            auto const& children = _scopes[next].children;
            work.insert(work.end(), children.rbegin(), children.rend());
            // What its children no longer need of it.
            _scopes[next].items = {};
            _scopes[next].references = {};
            _scopes[next].expanded = {};
            _scopes[next].appended = {};
            _scopes[next].indices = {};
            _scopes[next].moduleTypeKeys = {};
        }
        return binary::ComponentPool { .bytes =
                                           std::make_shared<std::vector<std::byte> const>(std::move(_bytes)),
                                       .components = std::move(_components) };
    }

} // namespace

std::optional<NodeId> binaryKeywordOf(SExpressionTree const& tree, NodeId component)
{
    // What Reading::read() reads before it: the `component` keyword, `definition`, a `$id` and an `@name`.
    auto body = Cursor { tree, component, tree.nodes()[component].firstChild };
    auto const isWordAt = [&](std::string_view word) {
        auto const node = body.peek();
        return node != NoNode && tree.nodes()[node].kind == NodeKind::Atom && tree.textOf(node) == word;
    };
    body.take();
    if (isWordAt("definition"))
        body.take();
    if (!body.atEnd() && tree.nodes()[body.peek()].tokenKind == TokenKind::Identifier)
        body.take();
    if (!body.atEnd() && tree.annotationKindOf(body.peek()) == AnnotationKind::Name)
        body.take();
    if (!isWordAt("binary"))
        return std::nullopt;
    return body.peek();
}

namespace
{
    /// The strings of a binary or quote form after its keyword @p keyword, decoded; annotations among them
    /// are ignored.
    Result<std::vector<std::string>> formStrings(SExpressionTree const& tree, NodeId keyword)
    {
        auto strings = std::vector<std::string> {};
        for (auto const node: tree.siblingsAfter(keyword))
        {
            if (!tree.isSignificant(node))
                continue;
            if (tree.nodes()[node].kind != NodeKind::Atom
                || tree.nodes()[node].tokenKind != TokenKind::String)
                return std::unexpected { isoline::text::unexpectedKeyword(tree, node) };
            auto text = isoline::text::decodeString(tree.source(), tree.tokenOf(node));
            if (!text)
                return std::unexpected { text.error() }; // uncovered(lexed-string)
            strings.push_back(std::move(*text));
        }
        return strings;
    }
} // namespace

std::expected<std::vector<std::byte>, TextError> binaryFormBytes(SExpressionTree const& tree,
                                                                 NodeId component,
                                                                 NodeId keyword,
                                                                 std::size_t maxBytes)
{
    return formStrings(tree, keyword)
        .and_then([&](std::vector<std::string> const& strings) -> Result<std::vector<std::byte>> {
            auto bytes = std::vector<std::byte> {};
            for (auto const& text: strings)
            {
                if (bytes.size() + text.size() > maxBytes)
                    return std::unexpected { TextError { TextErrorCode::OutputTooLarge,
                                                         tree.positionOf(component) } };
                std::ranges::transform(
                    text, std::back_inserter(bytes), [](char c) { return static_cast<std::byte>(c); });
            }
            return bytes;
        });
}

std::expected<binary::ComponentPool, TextError> ComponentReader::read(SExpressionTree const& tree,
                                                                      NodeId component) const
{
    // `(component quote "…"*)`: the text the strings hold, read as the fields of a component, as wast reads
    // it.
    auto const head = tree.nodes()[component].firstChild;
    auto quote = Cursor { tree, component, head };
    quote.take();
    auto const quoting = isoline::text::headKeyword(tree, component) == "component" && !quote.atEnd()
                         && tree.nodes()[quote.peek()].kind == NodeKind::Atom
                         && tree.textOf(quote.peek()) == "quote";
    if (quoting)
    {
        auto const strings = formStrings(tree, quote.take());
        if (!strings)
            return std::unexpected { strings.error() };
        auto source = std::string { "(component" };
        for (auto const& text: *strings)
        {
            source += text;
            source += ' ';
        }
        source += ')';
        auto quoted = isoline::text::parseSExpressions(source, _text);
        if (!quoted)
            return std::unexpected { quoted.error() };
        return Reading { *quoted, _text, _limits, _features }.read(quoted->firstTopLevel());
    }
    return Reading { tree, _text, _limits, _features }.read(component);
}

} // namespace isoline::component::text
