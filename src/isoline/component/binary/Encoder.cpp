// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/binary/Encoder.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/Grammar.hpp>
#include <isoline/meta/Check.hpp>

namespace isoline::component::binary
{

namespace
{

    using Bytes = std::vector<std::byte>;
    using isoline::binary::AbstractHeapType;
    using isoline::binary::ExternKind;
    using isoline::binary::Nullable;
    using isoline::binary::ValueKind;

    /// One core number or vector type's byte, as the core binary format encodes it.
    struct CoreNumberTypeRow
    {
        ValueKind kind; ///< The type.
        uint8_t byte;   ///< Its byte.
    };

    /// The core number and vector types.
    constexpr auto CoreNumberTypes = std::to_array<CoreNumberTypeRow>({
        { .kind = ValueKind::I32, .byte = 0x7F },
        { .kind = ValueKind::I64, .byte = 0x7E },
        { .kind = ValueKind::F32, .byte = 0x7D },
        { .kind = ValueKind::F64, .byte = 0x7C },
        { .kind = ValueKind::V128, .byte = 0x7B },
    });

    /// One abstract heap type's code, which is also the shorthand of a nullable reference to it.
    struct AbstractHeapRow
    {
        AbstractHeapType kind; ///< The heap.
        uint8_t code;          ///< Its code, an s7.
    };

    /// Every abstract heap type a decoded core type may name: the core decoder's own rows, no GC heap.
    constexpr auto AbstractHeapCodes = std::to_array<AbstractHeapRow>({
        { .kind = AbstractHeapType::Func, .code = 0x70 },
        { .kind = AbstractHeapType::Extern, .code = 0x6F },
        { .kind = AbstractHeapType::Exn, .code = 0x69 },
        { .kind = AbstractHeapType::NoExn, .code = 0x74 },
    });

    /// The bytes of a reference type's long form: nullable or not, its heap type after it.
    constexpr auto NullableReference = uint8_t { 0x63 };
    constexpr auto NonNullReference = uint8_t { 0x64 };

    /// The bits of a limits flag byte: a maximum follows, i64 addresses, a page size follows.
    constexpr auto LimitsHasMaximum = uint8_t { 0x01 };
    constexpr auto LimitsI64Addresses = uint8_t { 0x04 };
    constexpr auto LimitsCustomPageSize = uint8_t { 0x08 };

    /// A global's mutability byte.
    constexpr auto GlobalConst = uint8_t { 0x00 };
    constexpr auto GlobalVar = uint8_t { 0x01 };

    /// The attribute byte of a core tag type: an exception.
    constexpr auto TagException = uint8_t { 0x00 };

    /// The largest section a u32 size can announce.
    constexpr auto MaxSectionSize = uint64_t { std::numeric_limits<uint32_t>::max() };

    /// Where a nested scope's body is written: the body of ComponentPool::components[scope].
    struct ScopeReference
    {
        uint32_t scope; ///< The scope's pool index.
    };

    /// A part of a scope's encoding: bytes, or a nested scope's body, written in place.
    using Piece = std::variant<Bytes, ScopeReference>;

    /// A scope's encoding with its nested scopes left as references, and the size it has written out.
    struct ScopeLayout
    {
        std::vector<Piece> pieces; ///< The parts, in order.
        uint64_t size = 0;         ///< The bytes they come to, nested scopes included.
    };

    /// Collects the encoding of one scope, or of a part of it, as pieces.
    class Layout
    {
      public:
        /// A layout of the scope @p owner, whose nested scopes are laid out in @p scopes already.
        Layout(std::span<ScopeLayout const> scopes, uint32_t owner) noexcept:
            _scopes { scopes }, _owner { owner }
        {
        }

        /// A layout of the same scope as @p parent, for a part of it measured on its own.
        [[nodiscard]] Layout part() const noexcept { return Layout { _scopes, _owner }; }

        void byte(uint8_t value)
        {
            tail().push_back(std::byte { value });
            ++_size;
        }

        /// An unsigned LEB128 in its shortest form.
        void leb(uint64_t value)
        {
            constexpr auto Low = uint64_t { 0x7F };
            constexpr auto More = uint8_t { 0x80 };
            while (value > Low)
            {
                byte(static_cast<uint8_t>((value & Low) | More));
                value >>= 7U;
            }
            byte(static_cast<uint8_t>(value));
        }

        /// A signed LEB128 of a non-negative value in its shortest form: an s33 type index.
        void s33(uint32_t value)
        {
            constexpr auto Low = uint64_t { 0x7F };
            constexpr auto Sign = uint64_t { 0x40 };
            constexpr auto More = uint8_t { 0x80 };
            auto rest = uint64_t { value };
            while (rest >= Sign)
            {
                byte(static_cast<uint8_t>((rest & Low) | More));
                rest >>= 7U;
            }
            byte(static_cast<uint8_t>(rest));
        }

        void name(std::string_view text)
        {
            leb(text.size());
            raw(std::as_bytes(std::span { text }));
        }

        void raw(std::span<std::byte const> bytes)
        {
            auto& out = tail();
            out.insert(out.end(), bytes.begin(), bytes.end());
            _size += bytes.size();
        }

        /// The body of the scope @p scope, which must come after this layout's scope in the pool.
        void scope(uint32_t scope)
        {
            Require(scope > _owner && scope < _scopes.size());
            _pieces.emplace_back(ScopeReference { .scope = scope });
            _size += _scopes[scope].size;
        }

        /// Appends @p part, which this layout measured on its own.
        void append(Layout part)
        {
            for (auto& piece: part._pieces)
            {
                if (auto* bytes = std::get_if<Bytes>(&piece))
                {
                    auto& out = tail();
                    out.insert(out.end(), bytes->begin(), bytes->end());
                }
                else
                    _pieces.push_back(std::move(piece));
            }
            _size += part._size;
        }

        [[nodiscard]] uint64_t size() const noexcept { return _size; }

        [[nodiscard]] ScopeLayout finish() &&
        {
            return ScopeLayout { .pieces = std::move(_pieces), .size = _size };
        }

      private:
        Bytes& tail()
        {
            if (_pieces.empty() || !std::holds_alternative<Bytes>(_pieces.back()))
                _pieces.emplace_back(Bytes {});
            return std::get<Bytes>(_pieces.back());
        }

        std::span<ScopeLayout const> _scopes;
        uint32_t _owner;
        std::vector<Piece> _pieces;
        uint64_t _size = 0;
    };

    [[nodiscard]] std::string_view attributeValue(Implements const& attribute) noexcept
    {
        return attribute.interfaceName;
    }

    [[nodiscard]] std::string_view attributeValue(VersionSuffix const& attribute) noexcept
    {
        return attribute.suffix;
    }

    [[nodiscard]] std::string_view attributeValue(ExternalId const& attribute) noexcept
    {
        return attribute.id;
    }

    /// Lays out the scopes of one pool, each from its definitions, the inverse of the decoder's frames.
    class ComponentEncoder
    {
      public:
        explicit ComponentEncoder(ComponentPool const& pool) noexcept: _pool { pool } {}

        [[nodiscard]] ScopeLayout scope(std::span<ScopeLayout const> scopes, uint32_t index) const;

        // The entry of each Definition alternative: what its section's vector, or its own section,
        // holds of it, or what follows a declaration's byte.
        void entry(Layout& out, CoreModuleDefinition const& definition) const;
        void entry(Layout& out, CoreInstanceDefinition const& definition) const;
        void entry(Layout& out, CoreTypeDefinition const& definition) const;
        void entry(Layout& out, NestedComponent const& definition) const;
        void entry(Layout& out, InstanceDefinition const& definition) const;
        void entry(Layout& out, AliasDefinition const& definition) const;
        void entry(Layout& out, TypeDefinition const& definition) const;
        void entry(Layout& out, CanonDefinition const& definition) const;
        void entry(Layout& out, ImportDefinition const& definition) const;
        void entry(Layout& out, ExportDefinition const& definition) const;
        void entry(Layout& out, CustomDefinition const& definition) const;
        void entry(Layout& out, ExportDeclaration const& definition) const;

      private:
        void componentBody(Layout& out, Component const& component) const;
        void declarationsBody(Layout& out, Component const& scope) const;
        void entryOf(Layout& out, Definition const& definition) const;
        static void section(Layout& out, uint8_t id, Layout contents);

        static void sortBytes(Layout& out, std::span<KindRow const> rows, Sort sort);
        static void coreExternSortBytes(Layout& out, Sort sort);
        static void storedCoreValueKind(Layout& out, uint32_t value);
        static void storedBoolean(Layout& out, uint32_t value);
        static void sortIndex(Layout& out, SortIndex const& item);
        static void externDesc(Layout& out, ExternDesc const& desc);
        static void nameAttributes(Layout& out, NameAttributes const& name);
        static void valueType(Layout& out, ValueTypeRef const& type);
        static void optionalValueType(Layout& out, std::optional<ValueTypeRef> const& type);
        static void resultList(Layout& out, std::optional<ValueTypeRef> const& result);
        static void labeledTypes(Layout& out, std::vector<LabeledType> const& types);
        static void labels(Layout& out, std::vector<std::string> const& labels);
        void scopeType(Layout& out, ScopeKind kind, uint32_t scope) const;

        // The payload of each compound defvaltype, after its byte.
        static void payload(Layout& out, PrimitiveValueType type);
        static void payload(Layout& out, RecordType const& type);
        static void payload(Layout& out, VariantType const& type);
        static void payload(Layout& out, ListType const& type);
        static void payload(Layout& out, FixedListType const& type);
        static void payload(Layout& out, TupleType const& type);
        static void payload(Layout& out, FlagsType const& type);
        static void payload(Layout& out, EnumType const& type);
        static void payload(Layout& out, OptionType const& type);
        static void payload(Layout& out, ResultType const& type);
        static void payload(Layout& out, OwnType const& type);
        static void payload(Layout& out, BorrowType const& type);
        static void payload(Layout& out, StreamType const& type);
        static void payload(Layout& out, FutureType const& type);
        static void payload(Layout& out, MapType const& type);
        static void definedValueType(Layout& out, DefinedValueType const& type);

        static void coreValueType(Layout& out, isoline::binary::ValueType const& type);
        static void heapType(Layout& out, isoline::binary::HeapType const& heap);
        static void coreFuncType(Layout& out, isoline::binary::FuncType const& type);
        static void limits(Layout& out, isoline::binary::Limits const& limits, uint8_t pageSizeLog2);
        static void coreExternType(Layout& out, CoreExternType const& type);
        static void coreModuleDeclaration(Layout& out, CoreModuleDeclaration const& declaration);
        static void canonImmediates(Layout& out,
                                    CanonBuiltinRow const& row,
                                    CanonDefinition const& definition);

        ComponentPool const& _pool;
    };

    ScopeLayout ComponentEncoder::scope(std::span<ScopeLayout const> scopes, uint32_t index) const
    {
        auto out = Layout { scopes, index };
        auto const& component = _pool.components[index];
        if (component.kind == ScopeKind::Component)
            componentBody(out, component);
        else
            declarationsBody(out, component);
        return std::move(out).finish();
    }

    void ComponentEncoder::componentBody(Layout& out, Component const& component) const
    {
        auto const preamble = std::ranges::find(PreambleTable, LayerKind::Component, &PreambleRow::layer);
        out.raw(Magic);
        out.raw(preamble->word);
        auto const& definitions = component.definitions;
        auto next = definitions.begin();
        while (next != definitions.end())
        {
            auto const& row = DefinitionSectionTable[next->index()];
            Require(row.grouping != SectionGrouping::Declaration);
            auto contents = out.part();
            if (row.grouping == SectionGrouping::Single)
                entryOf(contents, *next++);
            else
            {
                // A run of adjacent definitions of one alternative shares its section's vector.
                auto const end = std::ranges::find_if(next, definitions.end(), [&](Definition const& other) {
                    return other.index() != next->index();
                });
                contents.leb(static_cast<uint64_t>(end - next));
                for (auto const& definition: std::ranges::subrange(next, end))
                    entryOf(contents, definition);
                next = end;
            }
            section(out, row.id, std::move(contents));
        }
    }

    void ComponentEncoder::declarationsBody(Layout& out, Component const& scope) const
    {
        out.leb(scope.definitions.size());
        for (auto const& definition: scope.definitions)
        {
            auto const kind = DefinitionDeclarationTable[definition.index()];
            Require(kind.has_value());
            auto const row = std::ranges::find(DeclarationTable, *kind, &DeclarationRow::kind);
            out.byte(row->byte);
            entryOf(out, definition);
        }
    }

    void ComponentEncoder::entryOf(Layout& out, Definition const& definition) const
    {
        std::visit([&](auto const& alternative) { entry(out, alternative); }, definition);
    }

    void ComponentEncoder::section(Layout& out, uint8_t id, Layout contents)
    {
        Require(contents.size() <= MaxSectionSize);
        out.byte(id);
        out.leb(contents.size());
        out.append(std::move(contents));
    }

    void ComponentEncoder::entry(Layout& out, CoreModuleDefinition const& definition) const
    {
        // Verbatim (D10): the module is the canonicalizer's, never the encoder's.
        Require(_pool.bytes && definition.offset <= _pool.bytes->size()
                && definition.size <= _pool.bytes->size() - definition.offset);
        out.raw(std::span { *_pool.bytes }.subspan(definition.offset, definition.size));
    }

    void ComponentEncoder::entry(Layout& out, CoreInstanceDefinition const& definition) const
    {
        if (auto const* instantiate = std::get_if<CoreInstantiate>(&definition.expression))
        {
            out.byte(InstantiateForm);
            out.leb(instantiate->module);
            out.leb(instantiate->arguments.size());
            for (auto const& argument: instantiate->arguments)
            {
                out.name(argument.name);
                out.byte(CoreInstantiateArgumentKind);
                out.leb(argument.instance);
            }
            return;
        }
        auto const& exports = std::get<CoreInlineExports>(definition.expression).exports;
        out.byte(InlineExportsForm);
        out.leb(exports.size());
        for (auto const& item: exports)
        {
            out.name(item.name);
            auto const row = std::ranges::find(CoreExternKinds, item.sort, &CoreExternKindRow::sort);
            Require(row != CoreExternKinds.end());
            out.byte(row->byte);
            out.leb(item.index);
        }
    }

    void ComponentEncoder::entry(Layout& out, CoreTypeDefinition const& definition) const
    {
        if (auto const* function = std::get_if<isoline::binary::FuncType>(&definition.type))
        {
            coreFuncType(out, *function);
            return;
        }
        auto const& module = std::get<CoreModuleTypeDefinition>(definition.type);
        out.byte(ModuleTypeForm);
        out.leb(module.declarations.size());
        for (auto const& declaration: module.declarations)
            coreModuleDeclaration(out, declaration);
    }

    void ComponentEncoder::entry(Layout& out, NestedComponent const& definition) const
    {
        Require(definition.index < _pool.components.size()
                && _pool.components[definition.index].kind == ScopeKind::Component);
        out.scope(definition.index);
    }

    void ComponentEncoder::entry(Layout& out, InstanceDefinition const& definition) const
    {
        if (auto const* instantiate = std::get_if<Instantiate>(&definition.expression))
        {
            out.byte(InstantiateForm);
            out.leb(instantiate->component);
            out.leb(instantiate->arguments.size());
            for (auto const& argument: instantiate->arguments)
            {
                out.name(argument.name);
                sortIndex(out, argument.item);
            }
            return;
        }
        auto const& exports = std::get<InlineExports>(definition.expression).exports;
        out.byte(InlineExportsForm);
        out.leb(exports.size());
        for (auto const& item: exports)
        {
            nameAttributes(out, item.name);
            sortIndex(out, item.item);
        }
    }

    void ComponentEncoder::entry(Layout& out, AliasDefinition const& definition) const
    {
        auto const target = std::ranges::find(AliasTargets, definition.target, &AliasTargetRow::target);
        switch (definition.target)
        {
            case AliasTarget::Export: sortBytes(out, ExternalKinds, definition.sort); break;
            case AliasTarget::CoreExport: coreExternSortBytes(out, definition.sort); break;
            case AliasTarget::Outer:
                sortBytes(out, OuterAliasKinds, definition.sort);
                out.byte(target->byte);
                out.leb(definition.outerCount);
                out.leb(definition.outerIndex);
                return;
        }
        out.byte(target->byte);
        out.leb(definition.instance);
        out.name(definition.name);
    }

    void ComponentEncoder::entry(Layout& out, TypeDefinition const& definition) const
    {
        if (auto const* value = std::get_if<DefinedValueType>(&definition.type))
            definedValueType(out, *value);
        else if (auto const* function = std::get_if<FunctionTypeDefinition>(&definition.type))
        {
            auto const form =
                std::ranges::find(FunctionTypeForms, function->asyncness, &FunctionTypeFormRow::asyncness);
            out.byte(form->byte);
            labeledTypes(out, function->params);
            resultList(out, function->result);
        }
        else if (auto const* component = std::get_if<ComponentTypeDefinition>(&definition.type))
            scopeType(out, ScopeKind::ComponentType, component->scope);
        else if (auto const* instance = std::get_if<InstanceTypeDefinition>(&definition.type))
            scopeType(out, ScopeKind::InstanceType, instance->scope);
        else
        {
            auto const& resource = std::get<ResourceTypeDefinition>(definition.type);
            out.byte(ResourceTypeForm);
            out.byte(CoreValueKindTable[static_cast<std::size_t>(resource.rep)].byte);
            if (resource.destructor)
            {
                out.byte(Present);
                out.leb(*resource.destructor);
            }
            else
                out.byte(Absent);
        }
    }

    void ComponentEncoder::entry(Layout& out, CanonDefinition const& definition) const
    {
        auto const row = std::ranges::find(CanonBuiltinTable, definition.builtin, &CanonBuiltinRow::builtin);
        out.byte(row->opcode);
        canonImmediates(out, *row, definition);
    }

    void ComponentEncoder::entry(Layout& out, ImportDefinition const& definition) const
    {
        nameAttributes(out, definition.name);
        externDesc(out, definition.type);
    }

    void ComponentEncoder::entry(Layout& out, ExportDefinition const& definition) const
    {
        nameAttributes(out, definition.name);
        sortIndex(out, definition.item);
        if (definition.type)
        {
            out.byte(Present);
            externDesc(out, *definition.type);
        }
        else
            out.byte(Absent);
    }

    void ComponentEncoder::entry(Layout& out, CustomDefinition const& definition) const
    {
        Require(_pool.bytes && definition.offset <= _pool.bytes->size()
                && definition.size <= _pool.bytes->size() - definition.offset);
        out.name(definition.name);
        out.raw(std::span { *_pool.bytes }.subspan(definition.offset, definition.size));
    }

    void ComponentEncoder::entry(Layout& out, ExportDeclaration const& definition) const
    {
        nameAttributes(out, definition.name);
        externDesc(out, definition.type);
    }

    void ComponentEncoder::sortBytes(Layout& out, std::span<KindRow const> rows, Sort sort)
    {
        auto const row = std::ranges::find(rows, sort, &KindRow::sort);
        Require(row != rows.end());
        out.byte(row->byte);
        if (row->coreByte)
            out.byte(*row->coreByte);
    }

    void ComponentEncoder::coreExternSortBytes(Layout& out, Sort sort)
    {
        // A core sort's prefix, then its core extern kind: what a core export alias names.
        auto const row = std::ranges::find_if(CoreExternKinds, [&](CoreExternKindRow const& candidate) {
            return sortOf(candidate.sort) == sort;
        });
        Require(row != CoreExternKinds.end());
        out.byte(CoreSortPrefix);
        out.byte(row->byte);
    }

    void ComponentEncoder::storedCoreValueKind(Layout& out, uint32_t value)
    {
        Require(value < CoreValueKindTable.size());
        out.byte(CoreValueKindTable[value].byte);
    }

    void ComponentEncoder::storedBoolean(Layout& out, uint32_t value)
    {
        Require(value <= 1);
        out.byte(value == 0 ? Absent : Present);
    }

    void ComponentEncoder::sortIndex(Layout& out, SortIndex const& item)
    {
        sortBytes(out, ExternalKinds, item.sort);
        out.leb(item.index);
    }

    void ComponentEncoder::externDesc(Layout& out, ExternDesc const& desc)
    {
        sortBytes(out, ExternalKinds, desc.sort);
        if (desc.sort == Sort::Type)
        {
            out.byte(std::ranges::find(TypeBounds, desc.bound, &TypeBoundRow::bound)->byte);
            if (desc.bound == TypeBound::SubResource)
                return;
        }
        out.leb(desc.index);
    }

    void ComponentEncoder::nameAttributes(Layout& out, NameAttributes const& name)
    {
        // The first row of a form is its canonical byte: 0x00 for a plain name, never the legacy 0x01.
        auto const form = name.attributes.empty() ? NameForm::Plain : NameForm::Attributed;
        out.byte(std::ranges::find(NameForms, form, &NameFormRow::form)->byte);
        out.name(name.name);
        if (form == NameForm::Plain)
            return;
        out.leb(name.attributes.size());
        for (auto const& attribute: name.attributes)
        {
            out.byte(NameAttributeTable[attribute.index()].byte);
            out.name(std::visit([](auto const& value) { return attributeValue(value); }, attribute));
        }
    }

    void ComponentEncoder::valueType(Layout& out, ValueTypeRef const& type)
    {
        if (auto const* primitive = std::get_if<PrimitiveValueType>(&type))
            out.byte(PrimitiveValueTypeTable[static_cast<std::size_t>(*primitive)].byte);
        else
            out.s33(std::get<TypeIndex>(type).index);
    }

    void ComponentEncoder::optionalValueType(Layout& out, std::optional<ValueTypeRef> const& type)
    {
        if (!type)
        {
            out.byte(Absent);
            return;
        }
        out.byte(Present);
        valueType(out, *type);
    }

    void ComponentEncoder::resultList(Layout& out, std::optional<ValueTypeRef> const& result)
    {
        if (!result)
        {
            out.byte(NoResults);
            out.byte(NoResultsCount);
            return;
        }
        out.byte(OneResult);
        valueType(out, *result);
    }

    void ComponentEncoder::labeledTypes(Layout& out, std::vector<LabeledType> const& types)
    {
        out.leb(types.size());
        for (auto const& type: types)
        {
            out.name(type.label);
            valueType(out, type.type);
        }
    }

    void ComponentEncoder::labels(Layout& out, std::vector<std::string> const& labels)
    {
        out.leb(labels.size());
        for (auto const& label: labels)
            out.name(label);
    }

    void ComponentEncoder::scopeType(Layout& out, ScopeKind kind, uint32_t scope) const
    {
        Require(scope < _pool.components.size() && _pool.components[scope].kind == kind);
        out.byte(std::ranges::find(TypeScopeForms, kind, &TypeScopeFormRow::scope)->byte);
        out.scope(scope);
    }

    void ComponentEncoder::payload(Layout& out, PrimitiveValueType type)
    {
        out.byte(PrimitiveValueTypeTable[static_cast<std::size_t>(type)].byte);
    }

    void ComponentEncoder::payload(Layout& out, RecordType const& type)
    {
        labeledTypes(out, type.fields);
    }

    void ComponentEncoder::payload(Layout& out, VariantType const& type)
    {
        out.leb(type.cases.size());
        for (auto const& item: type.cases)
        {
            out.name(item.label);
            optionalValueType(out, item.type);
            out.byte(CaseEnd);
        }
    }

    void ComponentEncoder::payload(Layout& out, ListType const& type)
    {
        valueType(out, type.element);
    }

    void ComponentEncoder::payload(Layout& out, FixedListType const& type)
    {
        valueType(out, type.element);
        out.leb(type.length);
    }

    void ComponentEncoder::payload(Layout& out, TupleType const& type)
    {
        out.leb(type.elements.size());
        for (auto const& element: type.elements)
            valueType(out, element);
    }

    void ComponentEncoder::payload(Layout& out, FlagsType const& type)
    {
        labels(out, type.labels);
    }

    void ComponentEncoder::payload(Layout& out, EnumType const& type)
    {
        labels(out, type.labels);
    }

    void ComponentEncoder::payload(Layout& out, OptionType const& type)
    {
        valueType(out, type.type);
    }

    void ComponentEncoder::payload(Layout& out, ResultType const& type)
    {
        optionalValueType(out, type.ok);
        optionalValueType(out, type.error);
    }

    void ComponentEncoder::payload(Layout& out, OwnType const& type)
    {
        out.leb(type.resource);
    }

    void ComponentEncoder::payload(Layout& out, BorrowType const& type)
    {
        out.leb(type.resource);
    }

    void ComponentEncoder::payload(Layout& out, StreamType const& type)
    {
        optionalValueType(out, type.element);
    }

    void ComponentEncoder::payload(Layout& out, FutureType const& type)
    {
        optionalValueType(out, type.element);
    }

    void ComponentEncoder::payload(Layout& out, MapType const& type)
    {
        valueType(out, type.key);
        valueType(out, type.value);
    }

    void ComponentEncoder::definedValueType(Layout& out, DefinedValueType const& type)
    {
        // A primitive is its own byte; a compound type is its row's byte, then its payload.
        if (type.index() != 0)
            out.byte(DefinedValueTypeTable[type.index() - 1].byte);
        std::visit([&](auto const& alternative) { payload(out, alternative); }, type);
    }

    void ComponentEncoder::coreValueType(Layout& out, isoline::binary::ValueType const& type)
    {
        if (type.kind != ValueKind::Ref)
        {
            out.byte(std::ranges::find(CoreNumberTypes, type.kind, &CoreNumberTypeRow::kind)->byte);
            return;
        }
        // A nullable reference to an abstract heap has a shorthand, its heap's code.
        if (type.nullable == Nullable::No || type.heap.kind == AbstractHeapType::Concrete)
            out.byte(type.nullable == Nullable::Yes ? NullableReference : NonNullReference);
        heapType(out, type.heap);
    }

    void ComponentEncoder::heapType(Layout& out, isoline::binary::HeapType const& heap)
    {
        if (heap.kind == AbstractHeapType::Concrete)
        {
            out.s33(heap.typeIndex);
            return;
        }
        auto const row = std::ranges::find(AbstractHeapCodes, heap.kind, &AbstractHeapRow::kind);
        Require(row != AbstractHeapCodes.end());
        out.byte(row->code);
    }

    void ComponentEncoder::coreFuncType(Layout& out, isoline::binary::FuncType const& type)
    {
        out.byte(CoreFuncTypeForm);
        for (auto const* types: { &type.params, &type.results })
        {
            out.leb(types->size());
            for (auto const& element: *types)
                coreValueType(out, element);
        }
    }

    void ComponentEncoder::limits(Layout& out, isoline::binary::Limits const& limits, uint8_t pageSizeLog2)
    {
        auto const customPageSize = pageSizeLog2 != isoline::binary::DefaultPageSizeLog2;
        auto flags = uint8_t { 0 };
        if (limits.max)
            flags |= LimitsHasMaximum;
        if (limits.addressType == isoline::binary::AddressType::I64)
            flags |= LimitsI64Addresses;
        if (customPageSize)
            flags |= LimitsCustomPageSize;
        out.byte(flags);
        out.leb(limits.min);
        if (limits.max)
            out.leb(*limits.max);
        if (customPageSize)
            out.leb(pageSizeLog2);
    }

    void ComponentEncoder::coreExternType(Layout& out, CoreExternType const& type)
    {
        out.byte(CoreExternKinds[static_cast<std::size_t>(type.kind)].byte);
        switch (type.kind)
        {
            case ExternKind::Func: out.leb(type.typeIndex); break;
            case ExternKind::Table:
                coreValueType(out, type.table.element);
                limits(out, type.table.limits, isoline::binary::DefaultPageSizeLog2);
                break;
            case ExternKind::Memory: limits(out, type.memory.limits, type.memory.pageSizeLog2); break;
            case ExternKind::Global:
                coreValueType(out, type.global.type);
                out.byte(type.global.mutability == isoline::binary::Mutability::Var ? GlobalVar
                                                                                    : GlobalConst);
                break;
            case ExternKind::Tag:
                out.byte(TagException);
                out.leb(type.typeIndex);
                break;
        }
    }

    void ComponentEncoder::coreModuleDeclaration(Layout& out, CoreModuleDeclaration const& declaration)
    {
        out.byte(CoreModuleDeclarationTable[declaration.index()].byte);
        if (auto const* import = std::get_if<CoreImportDeclaration>(&declaration))
        {
            out.name(import->module);
            out.name(import->name);
            coreExternType(out, import->type);
        }
        else if (auto const* type = std::get_if<CoreTypeDeclaration>(&declaration))
            coreFuncType(out, type->type);
        else if (auto const* alias = std::get_if<CoreOuterAliasDeclaration>(&declaration))
        {
            out.byte(CoreOuterAliasSort);
            out.byte(CoreOuterAliasTarget);
            out.leb(alias->outerCount);
            out.leb(alias->outerIndex);
        }
        else
        {
            auto const& exported = std::get<CoreExportDeclaration>(declaration);
            out.name(exported.name);
            coreExternType(out, exported.type);
        }
    }

    void ComponentEncoder::canonImmediates(Layout& out,
                                           CanonBuiltinRow const& row,
                                           CanonDefinition const& definition)
    {
        // The row's shape, as the decoder reads it: each stored immediate is the next of immediates.
        auto next = definition.immediates.begin();
        auto const stored = [&] {
            Require(next != definition.immediates.end());
            return *next++;
        };
        Require(definition.options.empty() || std::ranges::contains(row.shape, Immediate::Options));
        Require(!definition.result || std::ranges::contains(row.shape, Immediate::ResultList));
        for (auto const kind: row.shape)
        {
            switch (kind)
            {
                case Immediate::None: break;
                case Immediate::SortByte: out.byte(CanonFuncSort); break;
                case Immediate::Index: out.leb(stored()); break;
                case Immediate::Options:
                    out.leb(definition.options.size());
                    for (auto const& option: definition.options)
                    {
                        auto const& optionRow = CanonOptionTable[static_cast<std::size_t>(option.kind)];
                        out.byte(optionRow.byte);
                        if (optionRow.immediate == OptionImmediate::Index)
                            out.leb(option.index);
                    }
                    break;
                case Immediate::ResultList: resultList(out, definition.result); break;
                case Immediate::CoreValueType: storedCoreValueKind(out, stored()); break;
                case Immediate::Async:
                case Immediate::Shared: storedBoolean(out, stored()); break;
                case Immediate::Reserved: out.byte(Absent); break;
            }
        }
        Require(next == definition.immediates.end());
    }

} // namespace

std::vector<std::byte> encodeComponent(ComponentPool const& pool)
{
    Require(!pool.components.empty() && pool.components.front().kind == ScopeKind::Component);
    // Laid out last scope first: every scope a definition names comes after it in the pool, so its size
    // is known when its parent's section sizes are written. Then written front to back from a stack.
    auto const encoder = ComponentEncoder { pool };
    auto scopes = std::vector<ScopeLayout>(pool.components.size());
    for (auto const index:
         std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(scopes.size())) | std::views::reverse)
        scopes[index] = encoder.scope(scopes, index);

    struct Visit
    {
        uint32_t scope;   ///< The scope being written.
        std::size_t next; ///< Its next piece.
    };
    auto out = std::vector<std::byte> {};
    out.reserve(scopes.front().size);
    auto stack = std::vector<Visit> { Visit { .scope = 0, .next = 0 } };
    while (!stack.empty())
    {
        auto& visit = stack.back();
        auto const& pieces = scopes[visit.scope].pieces;
        if (visit.next == pieces.size())
        {
            stack.pop_back();
            continue;
        }
        auto const& piece = pieces[visit.next++];
        if (auto const* bytes = std::get_if<Bytes>(&piece))
            out.insert(out.end(), bytes->begin(), bytes->end());
        else
            stack.push_back(Visit { .scope = std::get<ScopeReference>(piece).scope, .next = 0 });
    }
    return out;
}

std::expected<std::vector<std::byte>, std::string> VerbatimCoreModules::canonicalize(
    std::span<std::byte const> module)
{
    return std::vector<std::byte>(module.begin(), module.end());
}

std::expected<std::vector<std::byte>, DecodeError> canonicalizeComponent(std::span<std::byte const> bytes,
                                                                         ComponentFeatureSet features,
                                                                         ComponentLimits const& limits,
                                                                         ICoreModuleCanonicalizer& modules)
{
    auto pool = decodeComponent(bytes, features, limits);
    if (!pool)
        return std::unexpected { pool.error() };
    // The new bytes hold the canonical modules and the custom payloads, each definition re-pointed to
    // its copy: nothing else of the input is referenced, so the input is not copied whole.
    auto const& original = *pool->bytes;
    auto canonicalBytes = std::vector<std::byte> {};
    for (auto const& position: coreModulePositions(*pool))
    {
        auto& module =
            std::get<CoreModuleDefinition>(pool->components[position.scope].definitions[position.definition]);
        auto canonicalModule =
            modules.canonicalize(std::span { original }.subspan(module.offset, module.size));
        if (!canonicalModule)
            return std::unexpected { DecodeError { DecodeErrorCode::CoreModuleRefused, module.offset } };
        module.offset = canonicalBytes.size();
        module.size = canonicalModule->size();
        canonicalBytes.insert(canonicalBytes.end(), canonicalModule->begin(), canonicalModule->end());
    }
    for (auto& scope: pool->components)
    {
        for (auto& definition: scope.definitions)
        {
            if (auto* custom = std::get_if<CustomDefinition>(&definition))
            {
                auto const payload = std::span { original }.subspan(custom->offset, custom->size);
                custom->offset = canonicalBytes.size();
                canonicalBytes.insert(canonicalBytes.end(), payload.begin(), payload.end());
            }
        }
    }
    pool->bytes = std::make_shared<std::vector<std::byte> const>(std::move(canonicalBytes));
    return encodeComponent(*pool);
}

} // namespace isoline::component::binary
