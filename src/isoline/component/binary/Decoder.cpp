// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/binary/Decoder.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/ByteReader.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Grammar.hpp>

namespace isoline::component::binary
{

namespace
{

    using Reader = isoline::binary::ByteReader;

    template <typename T>
    using Result = std::expected<T, DecodeError>;

    /// The layer the four bytes after the magic number name.
    [[nodiscard]] LayerKind layerOfWord(std::span<std::byte const> word) noexcept
    {
        auto const row = std::ranges::find_if(PreambleTable, [&](PreambleRow const& candidate) {
            return std::ranges::equal(candidate.word, word);
        });
        return row == PreambleTable.end() ? LayerKind::Unknown : row->layer;
    }

    /// How a core ByteReader error reads in wasm-tools' words.
    struct CoreReadErrorRow
    {
        isoline::binary::DecodeErrorCode core; ///< What ByteReader reported.
        DecodeErrorCode component;             ///< The component decoder's code for it.
    };

    /// Every error ByteReader reports. A length past the bytes left is wasm-tools' end of file: it
    /// reads on until the bytes run out.
    constexpr auto CoreReadErrors = std::to_array<CoreReadErrorRow>({
        { .core = isoline::binary::DecodeErrorCode::UnexpectedEnd,
          .component = DecodeErrorCode::UnexpectedEnd },
        { .core = isoline::binary::DecodeErrorCode::LengthOutOfBounds,
          .component = DecodeErrorCode::UnexpectedEnd },
        { .core = isoline::binary::DecodeErrorCode::IntegerTooLarge,
          .component = DecodeErrorCode::IntegerTooLarge },
        { .core = isoline::binary::DecodeErrorCode::IntegerRepresentationTooLong,
          .component = DecodeErrorCode::IntegerRepresentationTooLong },
        { .core = isoline::binary::DecodeErrorCode::MalformedUtf8Encoding,
          .component = DecodeErrorCode::MalformedUtf8Encoding },
    });

    /// The component decoder's error for one ByteReader reported.
    [[nodiscard]] DecodeError fromCore(isoline::binary::DecodeError const& error)
    {
        auto const row = std::ranges::find(CoreReadErrors, error.code, &CoreReadErrorRow::core);
        Require(row != CoreReadErrors.end());
        return DecodeError { row->component, error.offset };
    }

    // A value extern type carries a value bound, which needs the value types of P11 Task 2 and is
    // read nowhere: the gate keeps the decoder from ever meeting one.
    static_assert(componentFeatureRow(ComponentFeature::Values).p11 == Implementation::Refused,
                  "the decoder reads no value bound; admitting 🪙 needs one");

    /// What a byte at the start of a core `rectype` begins, as wasmparser reads one.
    enum class CoreTypeForm : uint8_t
    {
        Function,  ///< 0x60: a function type, which the core decoder reads.
        RecGroup,  ///< 0x4E: a GC rec group.
        SubType,   ///< 0x50 or 0x4F: a GC subtype, its supertypes, then its composite type.
        Composite, ///< A GC composite type (struct, array, continuation) or one of its prefixes.
    };

    /// One byte that may begin a core `rectype`, and what it begins.
    struct CoreTypeFormRow
    {
        uint8_t byte;      ///< The byte.
        CoreTypeForm form; ///< What it begins.
    };

    /// Every byte wasmparser reads at the start of a core `rectype` or a subtype's composite type.
    constexpr auto CoreTypeForms = std::to_array<CoreTypeFormRow>({
        { .byte = CoreFuncTypeForm, .form = CoreTypeForm::Function },
        { .byte = 0x4E, .form = CoreTypeForm::RecGroup },
        { .byte = 0x50, .form = CoreTypeForm::SubType },
        { .byte = 0x4F, .form = CoreTypeForm::SubType },
        { .byte = 0x5E, .form = CoreTypeForm::Composite }, // array
        { .byte = 0x5F, .form = CoreTypeForm::Composite }, // struct
        { .byte = 0x5D, .form = CoreTypeForm::Composite }, // cont
        { .byte = 0x65, .form = CoreTypeForm::Composite }, // shared
        { .byte = 0x4C, .form = CoreTypeForm::Composite }, // describes
        { .byte = 0x4D, .form = CoreTypeForm::Composite }, // descriptor
    });

    /// The refusal of a core GC type at @p offset: what Isoline's core decoder, which has no GC, reports
    /// for one in a type section.
    [[nodiscard]] constexpr DecodeError coreGcRefusal(uint64_t offset) noexcept
    {
        return DecodeError { DecodeErrorCode::MalformedCoreType,
                             offset,
                             static_cast<uint64_t>(isoline::binary::DecodeErrorCode::MalformedFunctionType) };
    }

    /// The core features a component's core types are decoded under: every one Isoline's core decodes.
    /// The validator checks them against the engine's.
    constexpr auto CoreTypeFeatures = isoline::FeatureSet::all();

    /// The bit of a section id that makes it malformed whatever its value, as wasm-tools reads ids.
    constexpr auto SectionIdHighBit = uint8_t { 0x80 };

    /// Makes the Attribute of alternative @p Index from its value.
    template <std::size_t Index>
    [[nodiscard]] Attribute makeAttribute(std::string value)
    {
        return Attribute { std::in_place_index<Index>,
                           std::variant_alternative_t<Index, Attribute> { std::move(value) } };
    }

    /// The factories of the Attribute alternatives @p Index.
    template <std::size_t... Index>
    consteval auto attributeFactories(std::index_sequence<Index...> /*indices*/)
    {
        return std::array<Attribute (*)(std::string), sizeof...(Index)> { &makeAttribute<Index>... };
    }

    /// One factory per Attribute alternative, at the index of its NameAttributeTable row.
    constexpr auto AttributeFactories =
        attributeFactories(std::make_index_sequence<std::variant_size_v<Attribute>> {});

    /// The bytes of a section and where it sits.
    struct SectionContext
    {
        uint8_t id;         ///< The section id.
        uint32_t component; ///< The pool index of the component it belongs to.
        uint32_t depth;     ///< That component's nesting depth, the outermost at 1.
        Reader reader;      ///< Over the section's contents, at their absolute offset.
    };

    /// Whether a section's decoder reads its contents to their end, or hands them to a new frame.
    enum class SectionShape : uint8_t
    {
        Contents, ///< Read to the end; anything left over is a size mismatch.
        Scope,    ///< A nested component, read by the frame it opens.
    };

    /// Decodes one component and its nested components, frame by frame, never recursively.
    class ComponentDecoder
    {
      public:
        ComponentDecoder(ComponentFeatureSet features, ComponentLimits const& limits) noexcept:
            _features { features }, _limits { limits }
        {
        }

        [[nodiscard]] Result<ComponentPool> decode(std::span<std::byte const> bytes);

        // The section decoders, one per row of SectionTable.
        [[nodiscard]] Result<void> customSection(SectionContext& section);
        [[nodiscard]] Result<void> coreModuleSection(SectionContext& section);
        [[nodiscard]] Result<void> coreInstanceSection(SectionContext& section);
        [[nodiscard]] Result<void> componentSection(SectionContext& section);
        [[nodiscard]] Result<void> instanceSection(SectionContext& section);
        [[nodiscard]] Result<void> aliasSection(SectionContext& section);
        [[nodiscard]] Result<void> importSection(SectionContext& section);
        [[nodiscard]] Result<void> exportSection(SectionContext& section);
        [[nodiscard]] Result<void> coreTypeSection(SectionContext& section);
        [[nodiscard]] Result<void> typeSection(SectionContext& section);
        [[nodiscard]] Result<void> canonSection(SectionContext& section);
        [[nodiscard]] Result<void> unimplementedSection(SectionContext& section);

        // The decoders of the compound defvaltypes, one per row of DefinedValueTypeTable, reading what
        // follows the type's byte.
        [[nodiscard]] Result<DefinedValueType> recordType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> variantType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> listType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> fixedListType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> tupleType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> flagsType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> enumType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> optionType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> resultType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> ownType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> borrowType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> streamType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> futureType(Reader& reader);
        [[nodiscard]] Result<DefinedValueType> mapType(Reader& reader);

      private:
        /// One component being read: its pool entry, the reader over what is left of it, its depth.
        struct Frame
        {
            uint32_t component; ///< Its index in the pool.
            Reader reader;      ///< Over its sections not yet read.
            uint32_t depth;     ///< Its nesting depth, the outermost at 1.
        };

        /// A scope whose declarations are being read from a type section: the section's component, or
        /// a component or instance type opened in it.
        struct TypeScopeFrame
        {
            uint32_t scope;     ///< Its index in the pool.
            uint32_t remaining; ///< Its definitions or declarations not yet read.
            uint32_t depth;     ///< Its nesting depth, the outermost component at 1.
        };

        /// The scopes open in one type section, innermost last.
        using TypeScopeStack = std::vector<TypeScopeFrame>;

        [[nodiscard]] Result<void> nextSection(std::size_t frame);
        [[nodiscard]] static Result<void> checkPreamble(Reader reader, LayerKind expected);
        [[nodiscard]] Result<void> admit(std::optional<ComponentFeature> gate, uint64_t offset) const;
        [[nodiscard]] Result<void> chargeItem(uint64_t offset);
        [[nodiscard]] Result<void> roomForDefinition(uint32_t component, uint64_t offset) const;
        [[nodiscard]] Result<void> addDefinition(uint32_t component, uint64_t offset, Definition definition);

        /// Reads a vector of section definitions, each by @p read, into @p section's component.
        template <typename Read>
        [[nodiscard]] Result<void> definitions(SectionContext& section, Read read);

        /// Reads a vector, calling @p read once per element, each charged as a decoded item before it
        /// is read (chargeItem()); nothing is reserved for the count.
        template <typename Read>
        [[nodiscard]] Result<void> forEach(Reader& reader, Read read);

        [[nodiscard]] static Result<uint8_t> u8(Reader& reader);
        [[nodiscard]] static Result<uint32_t> u32(Reader& reader);
        [[nodiscard]] Result<std::string> name(Reader& reader) const;
        [[nodiscard]] Result<Sort> externalKind(Reader& reader);
        [[nodiscard]] Result<SortIndex> sortIndex(Reader& reader);
        [[nodiscard]] Result<ExternDesc> externDesc(Reader& reader);
        [[nodiscard]] Result<NameAttributes> nameAttributes(Reader& reader);
        [[nodiscard]] Result<CoreInstanceDefinition> coreInstance(Reader& reader);
        [[nodiscard]] Result<InstanceDefinition> instance(Reader& reader);
        [[nodiscard]] Result<AliasDefinition> alias(Reader& reader);
        [[nodiscard]] Result<ImportDefinition> import(Reader& reader);
        [[nodiscard]] Result<ExportDefinition> exportDefinition(Reader& reader);

        [[nodiscard]] Result<void> chargeItems(uint64_t count, uint64_t offset);
        [[nodiscard]] Result<void> countType(uint32_t scope, uint64_t offset);
        [[nodiscard]] static Result<uint8_t> peek(Reader const& reader);
        /// Reads a wire boolean, `async?` or `sh?`: 0x00 or 0x01, anything else refused.
        [[nodiscard]] static Result<bool> boolean(Reader& reader);
        [[nodiscard]] Result<ValueTypeRef> valueType(Reader& reader) const;
        [[nodiscard]] Result<std::optional<ValueTypeRef>> optionalValueType(Reader& reader) const;
        [[nodiscard]] Result<std::optional<ValueTypeRef>> resultList(Reader& reader) const;
        [[nodiscard]] static Result<CoreValueKind> coreValueKind(Reader& reader);
        [[nodiscard]] Result<std::vector<std::string>> labels(Reader& reader);
        [[nodiscard]] Result<TypeDefinition> definedType(Reader& reader);
        [[nodiscard]] Result<FunctionTypeDefinition> functionType(Reader& reader, Asyncness asyncness);
        [[nodiscard]] static Result<ResourceTypeDefinition> resourceType(Reader& reader);
        [[nodiscard]] Result<void> typeEntry(Reader& reader,
                                             TypeScopeFrame const& frame,
                                             TypeScopeStack& stack);
        [[nodiscard]] Result<void> declaration(Reader& reader,
                                               TypeScopeFrame const& frame,
                                               TypeScopeStack& stack);
        [[nodiscard]] Result<isoline::binary::FuncType> coreFuncType(Reader& reader);
        [[nodiscard]] static Result<CoreExternType> coreExternType(Reader& reader);
        [[nodiscard]] Result<CoreModuleDeclaration> coreModuleDeclaration(Reader& reader);
        [[nodiscard]] Result<isoline::binary::FuncType> coreRecType(Reader& reader);
        [[nodiscard]] Result<CoreTypeDefinition> coreType(Reader& reader);
        [[nodiscard]] Result<void> immediate(Reader& reader,
                                             CanonBuiltinRow const& row,
                                             Immediate kind,
                                             CanonDefinition& definition);
        [[nodiscard]] Result<CanonDefinition> canon(Reader& reader);

        ComponentFeatureSet _features;
        ComponentLimits const& _limits;
        ComponentPool _pool;
        std::vector<Frame> _frames;
        uint64_t _decodedItems = 0;        ///< The items charged so far, of the whole pool.
        std::vector<uint32_t> _typeCounts; ///< The types and core types defined, by pool scope.
    };

    static_assert(std::ranges::all_of(CanonBuiltinTable,
                                      [](CanonBuiltinRow const& row) {
                                          return std::ranges::find(row.shape, Immediate::Shared)
                                                     == row.shape.end()
                                                 || row.gate == ComponentFeature::SharedThreading;
                                      }),
                  "`sh?` reads `shared`, which is 🧵②: only a built-in gated on 🧵② may take it");

    /// A decoder of one compound defvaltype.
    using DefinedValueTypeDecoder = Result<DefinedValueType> (ComponentDecoder::*)(Reader&);

    /// The decoder of each row of DefinedValueTypeTable, at the row's index.
    constexpr auto DefinedValueTypeDecoders = std::to_array<DefinedValueTypeDecoder>({
        &ComponentDecoder::recordType,
        &ComponentDecoder::variantType,
        &ComponentDecoder::listType,
        &ComponentDecoder::fixedListType,
        &ComponentDecoder::tupleType,
        &ComponentDecoder::flagsType,
        &ComponentDecoder::enumType,
        &ComponentDecoder::optionType,
        &ComponentDecoder::resultType,
        &ComponentDecoder::ownType,
        &ComponentDecoder::borrowType,
        &ComponentDecoder::streamType,
        &ComponentDecoder::futureType,
        &ComponentDecoder::mapType,
    });

    static_assert(DefinedValueTypeDecoders.size() == DefinedValueTypeTable.size(),
                  "every row of DefinedValueTypeTable has its decoder");

    /// One section: its id, the feature that introduced it, its decoder and its shape.
    struct SectionRow
    {
        uint8_t id;                                                ///< The section id.
        std::optional<ComponentFeature> gate;                      ///< Its gate, if any.
        Result<void> (ComponentDecoder::*decode)(SectionContext&); ///< Decodes it.
        SectionShape shape = SectionShape::Contents;               ///< How its end is checked.
    };

    /// Every section of Binary.md. Sections may repeat and come in any order.
    constexpr auto SectionTable = std::to_array<SectionRow>({
        { .id = 0, .gate = std::nullopt, .decode = &ComponentDecoder::customSection },
        { .id = 1, .gate = std::nullopt, .decode = &ComponentDecoder::coreModuleSection },
        { .id = 2, .gate = std::nullopt, .decode = &ComponentDecoder::coreInstanceSection },
        { .id = 3, .gate = std::nullopt, .decode = &ComponentDecoder::coreTypeSection },
        { .id = ComponentSectionId,
          .gate = std::nullopt,
          .decode = &ComponentDecoder::componentSection,
          .shape = SectionShape::Scope },
        { .id = 5, .gate = std::nullopt, .decode = &ComponentDecoder::instanceSection },
        { .id = 6, .gate = std::nullopt, .decode = &ComponentDecoder::aliasSection },
        { .id = 7, .gate = std::nullopt, .decode = &ComponentDecoder::typeSection },
        { .id = 8, .gate = std::nullopt, .decode = &ComponentDecoder::canonSection },
        // 🪙, which P11 refuses, so their decoders are never reached.
        { .id = 9, .gate = ComponentFeature::Values, .decode = &ComponentDecoder::unimplementedSection },
        { .id = 10, .gate = std::nullopt, .decode = &ComponentDecoder::importSection },
        { .id = 11, .gate = std::nullopt, .decode = &ComponentDecoder::exportSection },
        // wasm-tools has no values section and reports this one as "malformed section id: 12"; P11
        // names the gate of the production Binary.md gives it.
        { .id = 12, .gate = ComponentFeature::Values, .decode = &ComponentDecoder::unimplementedSection },
    });

    static_assert(std::ranges::all_of(DefinitionSectionTable,
                                      [](DefinitionSectionRow const& written) {
                                          return written.grouping == SectionGrouping::Declaration
                                                 || std::ranges::any_of(
                                                     SectionTable, [&](SectionRow const& read) {
                                                         return read.id == written.id && !read.gate
                                                                && (read.shape == SectionShape::Scope)
                                                                       == (written.id == ComponentSectionId);
                                                     });
                                      }),
                  "every section the encoder writes is one the decoder reads, ungated and of the same shape");

    static_assert(std::ranges::all_of(SectionTable,
                                      [](SectionRow const& read) {
                                          return read.gate.has_value()
                                                 || std::ranges::any_of(
                                                     DefinitionSectionTable,
                                                     [&](DefinitionSectionRow const& written) {
                                                         return written.id == read.id
                                                                && written.grouping
                                                                       != SectionGrouping::Declaration;
                                                     });
                                      }),
                  "every ungated section the decoder reads is one the encoder writes");

    /// The sort the kind bytes @p first and @p second name in @p rows.
    /// @param second The byte after a @p first of 0x00, or nothing.
    /// @param offset Where @p first was read; @p second follows it.
    /// @param code The error for bytes @p rows does not hold.
    [[nodiscard]] Result<KindRow> kindOf(std::span<KindRow const> rows,
                                         uint8_t first,
                                         std::optional<uint8_t> second,
                                         uint64_t offset,
                                         DecodeErrorCode code)
    {
        auto const row = std::ranges::find_if(rows, [&](KindRow const& candidate) {
            return candidate.byte == first && candidate.coreByte == second;
        });
        if (row != rows.end())
            return *row;
        if (second)
            return std::unexpected { DecodeError { code, offset + 1, *second } };
        return std::unexpected { DecodeError { code, offset, first } };
    }

    Result<ComponentPool> ComponentDecoder::decode(std::span<std::byte const> bytes)
    {
        if (bytes.size() > _limits.maxComponentBytes)
            return std::unexpected { DecodeError {
                DecodeErrorCode::LimitExceeded,
                0,
                limitSubject(ComponentLimit::ComponentBytes, _limits.maxComponentBytes) } };
        if (_limits.maxNesting < 1)
            return std::unexpected { DecodeError {
                DecodeErrorCode::LimitExceeded,
                0,
                limitSubject(ComponentLimit::Nesting, _limits.maxNesting) } };

        _pool.bytes = std::make_shared<std::vector<std::byte> const>(bytes.begin(), bytes.end());
        auto reader = Reader { *_pool.bytes };
        if (auto result = checkPreamble(reader, LayerKind::Component); !result)
            return std::unexpected { result.error() };
        std::ignore = reader.readBytes(PreambleSize);
        _pool.components.emplace_back();
        _frames.push_back(Frame { .component = 0, .reader = reader, .depth = 1 });

        while (!_frames.empty())
        {
            if (_frames.back().reader.atEnd())
            {
                _frames.pop_back();
                continue;
            }
            if (auto result = nextSection(_frames.size() - 1); !result)
                return std::unexpected { result.error() };
        }
        return std::move(_pool);
    }

    Result<void> ComponentDecoder::nextSection(std::size_t frame)
    {
        auto& reader = _frames[frame].reader;
        auto const idOffset = reader.offset();
        // The loop of decode() reads a section only where its frame's reader has bytes left.
        auto const id = u8(reader);
        Require(id.has_value());
        if ((*id & SectionIdHighBit) != 0)
            return std::unexpected { DecodeError { DecodeErrorCode::MalformedSectionId, idOffset } };
        auto const sizeOffset = reader.offset();
        auto const size = u32(reader);
        if (!size)
            return std::unexpected { size.error() };
        // A nested component's sections end with its section; the outermost one's run to the end.
        if (*size > reader.remaining() && _frames[frame].depth > 1)
            return std::unexpected { DecodeError { DecodeErrorCode::SectionTooLarge, sizeOffset } };
        auto const contentsOffset = reader.offset();
        auto const contents = reader.readBytes(*size);
        if (!contents)
            return std::unexpected { fromCore(contents.error()) };

        auto section = SectionContext { .id = *id,
                                        .component = _frames[frame].component,
                                        .depth = _frames[frame].depth,
                                        .reader = Reader { *contents, contentsOffset } };
        auto const row = std::ranges::find(SectionTable, *id, &SectionRow::id);
        if (row == SectionTable.end())
            return std::unexpected { DecodeError {
                DecodeErrorCode::MalformedSectionId, contentsOffset, *id } };
        if (auto result = admit(row->gate, contentsOffset); !result)
            return result;
        // The decoder may open a frame, so nothing past this point refers to _frames.
        if (auto result = (this->*row->decode)(section); !result)
            return result;
        if (row->shape == SectionShape::Contents && !section.reader.atEnd())
            return std::unexpected { DecodeError { DecodeErrorCode::SectionSizeMismatch,
                                                   section.reader.offset() } };
        return {};
    }

    Result<void> ComponentDecoder::checkPreamble(Reader reader, LayerKind expected)
    {
        auto const start = reader.offset();
        auto const magic = reader.readBytes(Magic.size());
        if (!magic)
            return std::unexpected { fromCore(magic.error()) };
        if (!std::ranges::equal(*magic, Magic))
            return std::unexpected { DecodeError { DecodeErrorCode::MagicHeaderNotDetected, start } };
        auto const word = reader.readBytes(PreambleSize - Magic.size());
        if (!word)
            return std::unexpected { fromCore(word.error()) };
        auto const layer = layerOfWord(*word);
        if (layer == LayerKind::Unknown)
        {
            auto value = uint64_t { 0 };
            for (auto const index: std::views::iota(std::size_t { 0 }, word->size()))
                value |= static_cast<uint64_t>((*word)[index]) << (8U * index);
            return std::unexpected { DecodeError {
                DecodeErrorCode::UnknownBinaryVersion, start + Magic.size(), value } };
        }
        if (layer != expected)
            return std::unexpected { DecodeError { expected == LayerKind::Component
                                                       ? DecodeErrorCode::ExpectedComponentHeader
                                                       : DecodeErrorCode::ExpectedModuleHeader,
                                                   start } };
        return {};
    }

    Result<void> ComponentDecoder::admit(std::optional<ComponentFeature> gate, uint64_t offset) const
    {
        if (!gate || decodeAdmits(_features, *gate))
            return {};
        return std::unexpected { DecodeError {
            DecodeErrorCode::FeatureDisabled, offset, std::nullopt, gate } };
    }

    Result<void> ComponentDecoder::chargeItem(uint64_t offset)
    {
        if (_decodedItems >= _limits.maxDecodedItems)
            return std::unexpected { DecodeError {
                DecodeErrorCode::LimitExceeded,
                offset,
                limitSubject(ComponentLimit::DecodedItems, _limits.maxDecodedItems) } };
        ++_decodedItems;
        return {};
    }

    Result<void> ComponentDecoder::roomForDefinition(uint32_t component, uint64_t offset) const
    {
        if (_pool.components[component].definitions.size() < _limits.maxDefinitions)
            return {};
        return std::unexpected { DecodeError {
            DecodeErrorCode::LimitExceeded,
            offset,
            limitSubject(ComponentLimit::Definitions, _limits.maxDefinitions) } };
    }

    Result<void> ComponentDecoder::addDefinition(uint32_t component, uint64_t offset, Definition definition)
    {
        if (auto room = roomForDefinition(component, offset); !room)
            return room;
        _pool.components[component].definitions.push_back(std::move(definition));
        return {};
    }

    template <typename Read>
    Result<void> ComponentDecoder::definitions(SectionContext& section, Read read)
    {
        return forEach(section.reader, [&]() -> Result<void> {
            auto const offset = section.reader.offset();
            // Refused before the element is read, so a component past the limit is not decoded on.
            if (auto room = roomForDefinition(section.component, offset); !room)
                return room;
            auto definition = read(section.reader);
            if (!definition)
                return std::unexpected { definition.error() };
            return addDefinition(section.component, offset, std::move(*definition));
        });
    }

    template <typename Read>
    Result<void> ComponentDecoder::forEach(Reader& reader, Read read)
    {
        auto const count = reader.readLength();
        if (!count)
            return std::unexpected { fromCore(count.error()) };
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, *count))
        {
            if (auto charged = chargeItem(reader.offset()); !charged)
                return charged;
            if (auto result = read(); !result)
                return result;
        }
        return {};
    }

    Result<uint8_t> ComponentDecoder::u8(Reader& reader)
    {
        auto const value = reader.readByte();
        if (!value)
            return std::unexpected { fromCore(value.error()) };
        return static_cast<uint8_t>(*value);
    }

    Result<uint32_t> ComponentDecoder::u32(Reader& reader)
    {
        auto const value = reader.readU32();
        if (!value)
            return std::unexpected { fromCore(value.error()) };
        return *value;
    }

    Result<std::string> ComponentDecoder::name(Reader& reader) const
    {
        auto const offset = reader.offset();
        auto const view = reader.readName();
        if (!view)
            return std::unexpected { fromCore(view.error()) };
        if (view->size() > _limits.maxNameBytes)
            return std::unexpected { DecodeError {
                DecodeErrorCode::LimitExceeded,
                offset,
                limitSubject(ComponentLimit::NameBytes, _limits.maxNameBytes) } };
        return std::string { *view };
    }

    Result<Sort> ComponentDecoder::externalKind(Reader& reader)
    {
        auto const offset = reader.offset();
        auto const first = u8(reader);
        if (!first)
            return std::unexpected { first.error() };
        auto second = std::optional<uint8_t> {};
        if (*first == CoreSortPrefix)
        {
            auto const core = u8(reader);
            if (!core)
                return std::unexpected { core.error() };
            second = *core;
        }
        // A sort's gate is validation's, as wasmparser's reader admits every sort (Task 4's audit): only
        // an extern type checks it here, where it decides how the bytes after the sort read.
        auto const row = kindOf(ExternalKinds, *first, second, offset, DecodeErrorCode::InvalidExternalKind);
        if (!row)
            return std::unexpected { row.error() };
        return row->sort;
    }

    Result<SortIndex> ComponentDecoder::sortIndex(Reader& reader)
    {
        auto const sort = externalKind(reader);
        if (!sort)
            return std::unexpected { sort.error() };
        auto const index = u32(reader);
        if (!index)
            return std::unexpected { index.error() };
        return SortIndex { .sort = *sort, .index = *index };
    }

    Result<ExternDesc> ComponentDecoder::externDesc(Reader& reader)
    {
        auto const offset = reader.offset();
        auto const sort = externalKind(reader);
        if (!sort)
            return std::unexpected { sort.error() };
        // A value extern type carries a value bound, which the AST has no form for while P11 refuses
        // 🪙: the one sort gate the decoder keeps.
        auto const row = std::ranges::find(ExternalKinds, *sort, &KindRow::sort);
        if (auto admitted = admit(row->gate, offset); !admitted)
            return std::unexpected { admitted.error() };
        auto desc = ExternDesc { .sort = *sort };
        if (*sort == Sort::Type)
        {
            auto const boundOffset = reader.offset();
            auto const bound = u8(reader);
            if (!bound)
                return std::unexpected { bound.error() };
            auto const boundRow = std::ranges::find(TypeBounds, *bound, &TypeBoundRow::byte);
            if (boundRow == TypeBounds.end())
                return std::unexpected { DecodeError {
                    DecodeErrorCode::InvalidTypeBound, boundOffset, *bound } };
            desc.bound = boundRow->bound;
            if (desc.bound == TypeBound::SubResource)
                return desc;
        }
        auto const index = u32(reader);
        if (!index)
            return std::unexpected { index.error() };
        desc.index = *index;
        return desc;
    }

    Result<NameAttributes> ComponentDecoder::nameAttributes(Reader& reader)
    {
        auto const offset = reader.offset();
        auto const formByte = u8(reader);
        if (!formByte)
            return std::unexpected { formByte.error() };
        auto const form = std::ranges::find(NameForms, *formByte, &NameFormRow::byte);
        if (form == NameForms.end())
            return std::unexpected { DecodeError { DecodeErrorCode::InvalidNameKind, offset, *formByte } };
        // The attributed form is admitted when the set enables any attribute's gate, as wasmparser's
        // reader admits it under cm-implements or cm-canon-names (readers/component/imports.rs:183):
        // the one gate of a name the decoder keeps. Each attribute's own gate is validation's.
        if (form->form == NameForm::Attributed
            && std::ranges::none_of(NameAttributeTable, [&](NameAttributeRow const& row) {
                   return _features.contains(row.gate);
               }))
            return std::unexpected { DecodeError {
                DecodeErrorCode::FeatureDisabled, offset, std::nullopt, NameAttributeTable.front().gate } };

        auto text = name(reader);
        if (!text)
            return std::unexpected { text.error() };
        auto result = NameAttributes { .name = std::move(*text), .attributes = {} };
        if (form->form == NameForm::Plain)
            return result;

        auto seen = std::array<bool, NameAttributeTable.size()> {};
        auto const attributes = forEach(reader, [&]() -> Result<void> {
            auto const attributeOffset = reader.offset();
            auto const kind = u8(reader);
            if (!kind)
                return std::unexpected { kind.error() };
            auto const row = std::ranges::find(NameAttributeTable, *kind, &NameAttributeRow::byte);
            if (row == NameAttributeTable.end())
                return std::unexpected { DecodeError {
                    DecodeErrorCode::InvalidNameAttribute, attributeOffset, *kind } };
            auto value = name(reader);
            if (!value)
                return std::unexpected { value.error() };
            auto const index = static_cast<std::size_t>(row - NameAttributeTable.begin());
            if (seen[index])
                return std::unexpected { DecodeError {
                    DecodeErrorCode::DuplicateNameAttribute, attributeOffset, index } };
            seen[index] = true;
            result.attributes.push_back(AttributeFactories[index](std::move(*value)));
            return {};
        });
        if (!attributes)
            return std::unexpected { attributes.error() };
        return result;
    }

    Result<CoreInstanceDefinition> ComponentDecoder::coreInstance(Reader& reader)
    {
        auto const offset = reader.offset();
        auto const kind = u8(reader);
        if (!kind)
            return std::unexpected { kind.error() };
        switch (*kind)
        {
            case InstantiateForm: {
                auto const module = u32(reader);
                if (!module)
                    return std::unexpected { module.error() };
                auto instantiate = CoreInstantiate { .module = *module, .arguments = {} };
                auto const arguments = forEach(reader, [&]() -> Result<void> {
                    auto argumentName = name(reader);
                    if (!argumentName)
                        return std::unexpected { argumentName.error() };
                    auto const kindOffset = reader.offset();
                    auto const argumentKind = u8(reader);
                    if (!argumentKind)
                        return std::unexpected { argumentKind.error() };
                    if (*argumentKind != CoreInstantiateArgumentKind)
                        return std::unexpected { DecodeError {
                            DecodeErrorCode::InvalidInstantiationArgKind, kindOffset, *argumentKind } };
                    auto const instance = u32(reader);
                    if (!instance)
                        return std::unexpected { instance.error() };
                    instantiate.arguments.push_back(
                        { .name = std::move(*argumentName), .instance = *instance });
                    return {};
                });
                if (!arguments)
                    return std::unexpected { arguments.error() };
                return CoreInstanceDefinition { .expression = std::move(instantiate) };
            }
            case InlineExportsForm: {
                auto inlineExports = CoreInlineExports {};
                auto const exports = forEach(reader, [&]() -> Result<void> {
                    auto exportName = name(reader);
                    if (!exportName)
                        return std::unexpected { exportName.error() };
                    auto const kindOffset = reader.offset();
                    auto const exportKind = u8(reader);
                    if (!exportKind)
                        return std::unexpected { exportKind.error() };
                    auto const row =
                        std::ranges::find(CoreExternKinds, *exportKind, &CoreExternKindRow::byte);
                    if (row == CoreExternKinds.end())
                        return std::unexpected { DecodeError {
                            DecodeErrorCode::InvalidCoreExternKind, kindOffset, *exportKind } };
                    auto const index = u32(reader);
                    if (!index)
                        return std::unexpected { index.error() };
                    inlineExports.exports.push_back(
                        { .name = std::move(*exportName), .sort = row->sort, .index = *index });
                    return {};
                });
                if (!exports)
                    return std::unexpected { exports.error() };
                return CoreInstanceDefinition { .expression = std::move(inlineExports) };
            }
            default:
                return std::unexpected { DecodeError {
                    DecodeErrorCode::InvalidCoreInstanceKind, offset, *kind } };
        }
    }

    Result<InstanceDefinition> ComponentDecoder::instance(Reader& reader)
    {
        auto const offset = reader.offset();
        auto const kind = u8(reader);
        if (!kind)
            return std::unexpected { kind.error() };
        switch (*kind)
        {
            case InstantiateForm: {
                auto const component = u32(reader);
                if (!component)
                    return std::unexpected { component.error() };
                auto instantiate = Instantiate { .component = *component, .arguments = {} };
                auto const arguments = forEach(reader, [&]() -> Result<void> {
                    auto argumentName = name(reader);
                    if (!argumentName)
                        return std::unexpected { argumentName.error() };
                    auto const item = sortIndex(reader);
                    if (!item)
                        return std::unexpected { item.error() };
                    instantiate.arguments.push_back({ .name = std::move(*argumentName), .item = *item });
                    return {};
                });
                if (!arguments)
                    return std::unexpected { arguments.error() };
                return InstanceDefinition { .expression = std::move(instantiate) };
            }
            case InlineExportsForm: {
                auto inlineExports = InlineExports {};
                auto const exports = forEach(reader, [&]() -> Result<void> {
                    auto exportName = nameAttributes(reader);
                    if (!exportName)
                        return std::unexpected { exportName.error() };
                    auto const item = sortIndex(reader);
                    if (!item)
                        return std::unexpected { item.error() };
                    inlineExports.exports.push_back({ .name = std::move(*exportName), .item = *item });
                    return {};
                });
                if (!exports)
                    return std::unexpected { exports.error() };
                return InstanceDefinition { .expression = std::move(inlineExports) };
            }
            default:
                return std::unexpected { DecodeError {
                    DecodeErrorCode::InvalidInstanceKind, offset, *kind } };
        }
    }

    Result<AliasDefinition> ComponentDecoder::alias(Reader& reader)
    {
        // wasm-tools reads the sort's bytes and the target before it interprets the sort, whose
        // meaning depends on the target; so do its errors.
        auto const offset = reader.offset();
        auto const first = u8(reader);
        if (!first)
            return std::unexpected { first.error() };
        auto second = std::optional<uint8_t> {};
        if (*first == CoreSortPrefix)
        {
            auto const core = u8(reader);
            if (!core)
                return std::unexpected { core.error() };
            second = *core;
        }
        auto const targetOffset = reader.offset();
        auto const targetByte = u8(reader);
        if (!targetByte)
            return std::unexpected { targetByte.error() };
        auto const target = std::ranges::find(AliasTargets, *targetByte, &AliasTargetRow::byte);
        if (target == AliasTargets.end())
            return std::unexpected { DecodeError {
                DecodeErrorCode::InvalidAliasTarget, targetOffset, *targetByte } };

        auto definition = AliasDefinition { .sort = Sort::Func,
                                            .target = target->target,
                                            .instance = 0,
                                            .name = {},
                                            .outerCount = 0,
                                            .outerIndex = 0 };
        switch (target->target)
        {
            case AliasTarget::Export: {
                auto const row =
                    kindOf(ExternalKinds, *first, second, offset, DecodeErrorCode::InvalidExternalKind);
                if (!row)
                    return std::unexpected { row.error() };
                definition.sort = row->sort;
                break;
            }
            case AliasTarget::CoreExport: {
                if (!second)
                    return std::unexpected { DecodeError {
                        DecodeErrorCode::InvalidCoreExportAliasKind, offset, *first } };
                auto const row = std::ranges::find(CoreExternKinds, *second, &CoreExternKindRow::byte);
                if (row == CoreExternKinds.end())
                    return std::unexpected { DecodeError {
                        DecodeErrorCode::InvalidCoreExternKind, offset, *second } };
                definition.sort = sortOf(row->sort);
                break;
            }
            case AliasTarget::Outer: {
                auto const row =
                    kindOf(OuterAliasKinds, *first, second, offset, DecodeErrorCode::InvalidOuterAliasKind);
                if (!row)
                    return std::unexpected { row.error() };
                definition.sort = row->sort;
                auto const count = u32(reader);
                if (!count)
                    return std::unexpected { count.error() };
                auto const index = u32(reader);
                if (!index)
                    return std::unexpected { index.error() };
                definition.outerCount = *count;
                definition.outerIndex = *index;
                return definition;
            }
        }

        auto const instance = u32(reader);
        if (!instance)
            return std::unexpected { instance.error() };
        auto exportName = name(reader);
        if (!exportName)
            return std::unexpected { exportName.error() };
        definition.instance = *instance;
        definition.name = std::move(*exportName);
        return definition;
    }

    Result<ImportDefinition> ComponentDecoder::import(Reader& reader)
    {
        auto importName = nameAttributes(reader);
        if (!importName)
            return std::unexpected { importName.error() };
        auto const type = externDesc(reader);
        if (!type)
            return std::unexpected { type.error() };
        return ImportDefinition { .name = std::move(*importName), .type = *type };
    }

    Result<ExportDefinition> ComponentDecoder::exportDefinition(Reader& reader)
    {
        auto exportName = nameAttributes(reader);
        if (!exportName)
            return std::unexpected { exportName.error() };
        auto const item = sortIndex(reader);
        if (!item)
            return std::unexpected { item.error() };
        auto const offset = reader.offset();
        auto const hasType = u8(reader);
        if (!hasType)
            return std::unexpected { hasType.error() };
        auto definition =
            ExportDefinition { .name = std::move(*exportName), .item = *item, .type = std::nullopt };
        switch (*hasType)
        {
            case Absent: return definition;
            case Present: {
                auto const type = externDesc(reader);
                if (!type)
                    return std::unexpected { type.error() };
                definition.type = *type;
                return definition;
            }
            default:
                return std::unexpected { DecodeError {
                    DecodeErrorCode::InvalidOptionalExportType, offset, *hasType } };
        }
    }

    Result<void> ComponentDecoder::customSection(SectionContext& section)
    {
        auto const offset = section.reader.offset();
        if (auto charged = chargeItem(offset); !charged)
            return charged;
        auto sectionName = name(section.reader);
        if (!sectionName)
            return std::unexpected { sectionName.error() };
        auto definition = CustomDefinition { .name = std::move(*sectionName),
                                             .offset = section.reader.offset(),
                                             .size = section.reader.remaining() };
        std::ignore = section.reader.readBytes(section.reader.remaining());
        return addDefinition(section.component, offset, std::move(definition));
    }

    Result<void> ComponentDecoder::coreModuleSection(SectionContext& section)
    {
        // The module is decoded by the core decoder once the component is validated (D10); here only
        // its range and its preamble are checked.
        auto const offset = section.reader.offset();
        if (auto charged = chargeItem(offset); !charged)
            return charged;
        if (auto result = checkPreamble(section.reader, LayerKind::CoreModule); !result)
            return result;
        auto const size = section.reader.remaining();
        std::ignore = section.reader.readBytes(size);
        return addDefinition(
            section.component, offset, CoreModuleDefinition { .offset = offset, .size = size });
    }

    Result<void> ComponentDecoder::coreInstanceSection(SectionContext& section)
    {
        return definitions(section, [&](Reader& reader) { return coreInstance(reader); });
    }

    Result<void> ComponentDecoder::componentSection(SectionContext& section)
    {
        auto const offset = section.reader.offset();
        if (section.depth >= _limits.maxNesting)
            return std::unexpected { DecodeError {
                DecodeErrorCode::LimitExceeded,
                offset,
                limitSubject(ComponentLimit::Nesting, _limits.maxNesting) } };
        if (auto result = checkPreamble(section.reader, LayerKind::Component); !result)
            return result;
        if (auto charged = chargeItem(offset); !charged)
            return charged;
        auto const index = static_cast<uint32_t>(_pool.components.size());
        if (auto result = addDefinition(section.component, offset, NestedComponent { .index = index });
            !result)
            return result;
        _pool.components.emplace_back();
        std::ignore = section.reader.readBytes(PreambleSize);
        _frames.push_back(Frame { .component = index, .reader = section.reader, .depth = section.depth + 1 });
        return {};
    }

    Result<void> ComponentDecoder::instanceSection(SectionContext& section)
    {
        return definitions(section, [&](Reader& reader) { return instance(reader); });
    }

    Result<void> ComponentDecoder::aliasSection(SectionContext& section)
    {
        return definitions(section, [&](Reader& reader) { return alias(reader); });
    }

    Result<void> ComponentDecoder::importSection(SectionContext& section)
    {
        return definitions(section, [&](Reader& reader) { return import(reader); });
    }

    Result<void> ComponentDecoder::exportSection(SectionContext& section)
    {
        return definitions(section, [&](Reader& reader) { return exportDefinition(reader); });
    }

    Result<void> ComponentDecoder::chargeItems(uint64_t count, uint64_t offset)
    {
        if (count > _limits.maxDecodedItems - std::min<uint64_t>(_decodedItems, _limits.maxDecodedItems))
            return std::unexpected { DecodeError {
                DecodeErrorCode::LimitExceeded,
                offset,
                limitSubject(ComponentLimit::DecodedItems, _limits.maxDecodedItems) } };
        _decodedItems += count;
        return {};
    }

    Result<void> ComponentDecoder::countType(uint32_t scope, uint64_t offset)
    {
        if (_typeCounts.size() <= scope)
            _typeCounts.resize(std::size_t { scope } + 1);
        if (_typeCounts[scope] >= _limits.maxTypes)
            return std::unexpected { DecodeError { DecodeErrorCode::LimitExceeded,
                                                   offset,
                                                   limitSubject(ComponentLimit::Types, _limits.maxTypes) } };
        ++_typeCounts[scope];
        return {};
    }

    Result<uint8_t> ComponentDecoder::peek(Reader const& reader)
    {
        auto probe = reader;
        return u8(probe);
    }

    Result<bool> ComponentDecoder::boolean(Reader& reader)
    {
        auto const offset = reader.offset();
        auto const byte = u8(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        switch (*byte)
        {
            case Absent: return false;
            case Present: return true;
            default: return std::unexpected { DecodeError { DecodeErrorCode::InvalidBooleanValue, offset } };
        }
    }

    Result<ValueTypeRef> ComponentDecoder::valueType(Reader& reader) const
    {
        // A primitive's byte, else a non-negative s33 type index, as wasm-tools reads `valtype`.
        auto const offset = reader.offset();
        auto const byte = peek(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        auto const primitive =
            std::ranges::find(PrimitiveValueTypeTable, *byte, &PrimitiveValueTypeRow::byte);
        if (primitive != PrimitiveValueTypeTable.end())
        {
            std::ignore = u8(reader);
            return ValueTypeRef { primitive->type };
        }
        auto const index = reader.readS33();
        if (!index)
            return std::unexpected { fromCore(index.error()) };
        if (*index < 0)
            return std::unexpected { DecodeError { DecodeErrorCode::InvalidValueType, offset, *byte } };
        return ValueTypeRef { TypeIndex { .index = static_cast<uint32_t>(*index) } };
    }

    Result<std::optional<ValueTypeRef>> ComponentDecoder::optionalValueType(Reader& reader) const
    {
        auto const offset = reader.offset();
        auto const byte = u8(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        switch (*byte)
        {
            case Absent: return std::optional<ValueTypeRef> {};
            case Present:
                return valueType(reader).transform([](ValueTypeRef type) { return std::optional { type }; });
            default:
                return std::unexpected { DecodeError {
                    DecodeErrorCode::InvalidOptionalValueType, offset, *byte } };
        }
    }

    Result<std::optional<ValueTypeRef>> ComponentDecoder::resultList(Reader& reader) const
    {
        auto const offset = reader.offset();
        auto const byte = u8(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        switch (*byte)
        {
            case OneResult:
                return valueType(reader).transform([](ValueTypeRef type) { return std::optional { type }; });
            case NoResults: {
                auto const countOffset = reader.offset();
                auto const count = u8(reader);
                if (!count)
                    return std::unexpected { count.error() };
                if (*count != NoResultsCount)
                    return std::unexpected { DecodeError {
                        DecodeErrorCode::InvalidResultCount, countOffset, *count } };
                return std::optional<ValueTypeRef> {};
            }
            default:
                return std::unexpected { DecodeError { DecodeErrorCode::InvalidResults, offset, *byte } };
        }
    }

    Result<CoreValueKind> ComponentDecoder::coreValueKind(Reader& reader)
    {
        auto const offset = reader.offset();
        auto const byte = u8(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        auto const row = std::ranges::find(CoreValueKindTable, *byte, &CoreValueKindRow::byte);
        if (row == CoreValueKindTable.end())
            return std::unexpected { DecodeError { DecodeErrorCode::InvalidCoreValueType, offset, *byte } };
        return row->kind;
    }

    Result<std::vector<std::string>> ComponentDecoder::labels(Reader& reader)
    {
        auto result = std::vector<std::string> {};
        auto const read = forEach(reader, [&]() -> Result<void> {
            return name(reader).transform([&](std::string label) { result.push_back(std::move(label)); });
        });
        if (!read)
            return std::unexpected { read.error() };
        return result;
    }

    Result<DefinedValueType> ComponentDecoder::recordType(Reader& reader)
    {
        auto record = RecordType {};
        auto const read = forEach(reader, [&]() -> Result<void> {
            auto label = name(reader);
            if (!label)
                return std::unexpected { label.error() };
            return valueType(reader).transform([&](ValueTypeRef type) {
                record.fields.push_back({ .label = std::move(*label), .type = type });
            });
        });
        if (!read)
            return std::unexpected { read.error() };
        return DefinedValueType { std::move(record) };
    }

    Result<DefinedValueType> ComponentDecoder::variantType(Reader& reader)
    {
        auto variant = VariantType {};
        auto const read = forEach(reader, [&]() -> Result<void> {
            auto label = name(reader);
            if (!label)
                return std::unexpected { label.error() };
            auto const payload = optionalValueType(reader);
            if (!payload)
                return std::unexpected { payload.error() };
            // Once the case's `refines`; it must be zero.
            auto const zeroOffset = reader.offset();
            auto const zero = u8(reader);
            if (!zero)
                return std::unexpected { zero.error() };
            if (*zero != CaseEnd)
                return std::unexpected { DecodeError {
                    DecodeErrorCode::ZeroByteRequired, zeroOffset, *zero } };
            variant.cases.push_back({ .label = std::move(*label), .type = *payload });
            return {};
        });
        if (!read)
            return std::unexpected { read.error() };
        return DefinedValueType { std::move(variant) };
    }

    Result<DefinedValueType> ComponentDecoder::listType(Reader& reader)
    {
        return valueType(reader).transform(
            [](ValueTypeRef element) { return DefinedValueType { ListType { .element = element } }; });
    }

    Result<DefinedValueType> ComponentDecoder::fixedListType(Reader& reader)
    // uncovered-begin(fixed-length-lists)
    {
        auto const element = valueType(reader);
        if (!element)
            return std::unexpected { element.error() };
        auto const length = u32(reader);
        if (!length)
            return std::unexpected { length.error() };
        return DefinedValueType { FixedListType { .element = *element, .length = *length } };
    }
    // uncovered-end(fixed-length-lists)

    Result<DefinedValueType> ComponentDecoder::tupleType(Reader& reader)
    {
        auto tuple = TupleType {};
        auto const read = forEach(reader, [&]() -> Result<void> {
            return valueType(reader).transform([&](ValueTypeRef type) { tuple.elements.push_back(type); });
        });
        if (!read)
            return std::unexpected { read.error() };
        return DefinedValueType { std::move(tuple) };
    }

    Result<DefinedValueType> ComponentDecoder::flagsType(Reader& reader)
    {
        return labels(reader).transform([](std::vector<std::string> labels) {
            return DefinedValueType { FlagsType { .labels = std::move(labels) } };
        });
    }

    Result<DefinedValueType> ComponentDecoder::enumType(Reader& reader)
    {
        return labels(reader).transform([](std::vector<std::string> labels) {
            return DefinedValueType { EnumType { .labels = std::move(labels) } };
        });
    }

    Result<DefinedValueType> ComponentDecoder::optionType(Reader& reader)
    {
        return valueType(reader).transform(
            [](ValueTypeRef type) { return DefinedValueType { OptionType { .type = type } }; });
    }

    Result<DefinedValueType> ComponentDecoder::resultType(Reader& reader)
    {
        auto const ok = optionalValueType(reader);
        if (!ok)
            return std::unexpected { ok.error() };
        auto const error = optionalValueType(reader);
        if (!error)
            return std::unexpected { error.error() };
        return DefinedValueType { ResultType { .ok = *ok, .error = *error } };
    }

    Result<DefinedValueType> ComponentDecoder::ownType(Reader& reader)
    {
        return u32(reader).transform(
            [](uint32_t resource) { return DefinedValueType { OwnType { .resource = resource } }; });
    }

    Result<DefinedValueType> ComponentDecoder::borrowType(Reader& reader)
    {
        return u32(reader).transform(
            [](uint32_t resource) { return DefinedValueType { BorrowType { .resource = resource } }; });
    }

    Result<DefinedValueType> ComponentDecoder::streamType(Reader& reader)
    {
        return optionalValueType(reader).transform([](std::optional<ValueTypeRef> element) {
            return DefinedValueType { StreamType { .element = element } };
        });
    }

    Result<DefinedValueType> ComponentDecoder::futureType(Reader& reader)
    {
        return optionalValueType(reader).transform([](std::optional<ValueTypeRef> element) {
            return DefinedValueType { FutureType { .element = element } };
        });
    }

    Result<DefinedValueType> ComponentDecoder::mapType(Reader& reader)
    {
        auto const key = valueType(reader);
        if (!key)
            return std::unexpected { key.error() };
        return valueType(reader).transform(
            [&](ValueTypeRef value) { return DefinedValueType { MapType { .key = *key, .value = value } }; });
    }

    Result<FunctionTypeDefinition> ComponentDecoder::functionType(Reader& reader, Asyncness asyncness)
    {
        auto function =
            FunctionTypeDefinition { .asyncness = asyncness, .params = {}, .result = std::nullopt };
        auto const params = forEach(reader, [&]() -> Result<void> {
            auto label = name(reader);
            if (!label)
                return std::unexpected { label.error() };
            return valueType(reader).transform([&](ValueTypeRef type) {
                function.params.push_back({ .label = std::move(*label), .type = type });
            });
        });
        if (!params)
            return std::unexpected { params.error() };
        auto const result = resultList(reader);
        if (!result)
            return std::unexpected { result.error() };
        function.result = *result;
        return function;
    }

    Result<ResourceTypeDefinition> ComponentDecoder::resourceType(Reader& reader)
    {
        auto const rep = coreValueKind(reader);
        if (!rep)
            return std::unexpected { rep.error() };
        auto resource = ResourceTypeDefinition { .rep = *rep, .destructor = std::nullopt };
        auto const offset = reader.offset();
        auto const byte = u8(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        switch (*byte)
        {
            case Absent: return resource;
            case Present:
                return u32(reader).transform([&](uint32_t destructor) {
                    resource.destructor = destructor;
                    return resource;
                });
            default:
                return std::unexpected { DecodeError { DecodeErrorCode::InvalidDestructor, offset, *byte } };
        }
    }

    Result<TypeDefinition> ComponentDecoder::definedType(Reader& reader)
    {
        // Every `deftype` but the scopes, which typeEntry() opens, having peeked the byte read here.
        auto const offset = reader.offset();
        auto const byte = u8(reader);
        Require(byte.has_value());
        if (*byte == ResourceTypeForm)
            return resourceType(reader).transform(
                [](ResourceTypeDefinition resource) { return TypeDefinition { .type = resource }; });
        if (auto const form = std::ranges::find(FunctionTypeForms, *byte, &FunctionTypeFormRow::byte);
            form != FunctionTypeForms.end())
        {
            return functionType(reader, form->asyncness).transform([](FunctionTypeDefinition function) {
                return TypeDefinition { .type = std::move(function) };
            });
        }
        if (auto const primitive =
                std::ranges::find(PrimitiveValueTypeTable, *byte, &PrimitiveValueTypeRow::byte);
            primitive != PrimitiveValueTypeTable.end())
        {
            return TypeDefinition { .type = DefinedValueType { primitive->type } };
        }
        auto const row = std::ranges::find(DefinedValueTypeTable, *byte, &DefinedValueTypeRow::byte);
        if (row == DefinedValueTypeTable.end())
            return std::unexpected { DecodeError { DecodeErrorCode::InvalidDefinedType, offset, *byte } };
        auto const decoder =
            DefinedValueTypeDecoders[static_cast<std::size_t>(row - DefinedValueTypeTable.begin())];
        return (this->*decoder)(reader).transform(
            [](DefinedValueType type) { return TypeDefinition { .type = std::move(type) }; });
    }

    Result<void> ComponentDecoder::typeEntry(Reader& reader,
                                             TypeScopeFrame const& frame,
                                             TypeScopeStack& stack)
    {
        auto const offset = reader.offset();
        auto const byte = peek(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        if (auto const form = std::ranges::find(TypeScopeForms, *byte, &TypeScopeFormRow::byte);
            form != TypeScopeForms.end())
        {
            // A component or instance type: a scope of the pool, its declarations read by the loop of
            // typeSection() on the stack, never by recursion.
            if (frame.depth >= _limits.maxNesting)
                return std::unexpected { DecodeError {
                    DecodeErrorCode::LimitExceeded,
                    offset,
                    limitSubject(ComponentLimit::Nesting, _limits.maxNesting) } };
            std::ignore = u8(reader);
            auto const count = reader.readLength();
            if (!count)
                return std::unexpected { fromCore(count.error()) };
            if (auto counted = countType(frame.scope, offset); !counted)
                return counted;
            auto const scope = static_cast<uint32_t>(_pool.components.size());
            auto definition = form->scope == ScopeKind::ComponentType
                                  ? TypeDefinition { .type = ComponentTypeDefinition { .scope = scope } }
                                  : TypeDefinition { .type = InstanceTypeDefinition { .scope = scope } };
            return addDefinition(frame.scope, offset, std::move(definition)).transform([&] {
                _pool.components.push_back(Component { .kind = form->scope, .definitions = {} });
                stack.push_back(
                    TypeScopeFrame { .scope = scope, .remaining = *count, .depth = frame.depth + 1 });
            });
        }
        auto type = definedType(reader);
        if (!type)
            return std::unexpected { type.error() };
        if (auto counted = countType(frame.scope, offset); !counted)
            return counted;
        return addDefinition(frame.scope, offset, std::move(*type));
    }

    Result<void> ComponentDecoder::declaration(Reader& reader,
                                               TypeScopeFrame const& frame,
                                               TypeScopeStack& stack)
    {
        auto const offset = reader.offset();
        auto const byte = u8(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        auto const kind = _pool.components[frame.scope].kind;
        auto const row = std::ranges::find(DeclarationTable, *byte, &DeclarationRow::byte);
        if (row == DeclarationTable.end() || (row->scope && *row->scope != kind))
            return std::unexpected { DecodeError { DecodeErrorCode::InvalidTypeDeclaration, offset, *byte } };
        switch (row->kind)
        {
            case DeclarationKind::CoreType: {
                auto type = coreType(reader);
                if (!type)
                    return std::unexpected { type.error() };
                if (auto counted = countType(frame.scope, offset); !counted)
                    return counted;
                return addDefinition(frame.scope, offset, std::move(*type));
            }
            case DeclarationKind::Type: return typeEntry(reader, frame, stack);
            case DeclarationKind::Alias:
                return alias(reader).and_then([&](AliasDefinition definition) {
                    return addDefinition(frame.scope, offset, std::move(definition));
                });
            case DeclarationKind::Import:
                return import(reader).and_then([&](ImportDefinition definition) {
                    return addDefinition(frame.scope, offset, std::move(definition));
                });
            case DeclarationKind::Export: break;
        }
        auto exportName = nameAttributes(reader);
        if (!exportName)
            return std::unexpected { exportName.error() };
        auto const type = externDesc(reader);
        if (!type)
            return std::unexpected { type.error() };
        return addDefinition(
            frame.scope, offset, ExportDeclaration { .name = std::move(*exportName), .type = *type });
    }

    /// The component decoder's error for one the core decoder reported while reading a core type:
    /// ByteReader's errors as everywhere, the core grammar's own as MalformedCoreType.
    [[nodiscard]] DecodeError fromCoreType(isoline::binary::DecodeError const& error)
    {
        if (std::ranges::find(CoreReadErrors, error.code, &CoreReadErrorRow::core) != CoreReadErrors.end())
            return fromCore(error);
        return DecodeError { DecodeErrorCode::MalformedCoreType,
                             error.offset,
                             static_cast<uint64_t>(error.code) };
    }

    Result<isoline::binary::FuncType> ComponentDecoder::coreFuncType(Reader& reader)
    {
        auto const offset = reader.offset();
        auto type = isoline::binary::decodeEmbeddedFuncType(reader, CoreTypeFeatures);
        if (!type)
            return std::unexpected { fromCoreType(type.error()) };
        // Charged once read, the one exception to charging before growth: the core decoder bounds one
        // type's vectors by maxParams and maxResults (1000 each), and its readVector reserves at most
        // min(count, 4096) elements up front, so one type holds a bounded number before it is charged.
        if (auto charged = chargeItems(type->params.size() + type->results.size(), offset); !charged)
            return std::unexpected { charged.error() };
        return std::move(*type);
    }

    Result<CoreExternType> ComponentDecoder::coreExternType(Reader& reader)
    {
        auto const import = isoline::binary::decodeEmbeddedExternType(reader, CoreTypeFeatures);
        if (!import)
            return std::unexpected { fromCoreType(import.error()) };
        return CoreExternType { .kind = import->kind,
                                .typeIndex = import->typeIndex,
                                .table = import->table,
                                .memory = import->memory,
                                .global = import->global };
    }

    Result<CoreModuleDeclaration> ComponentDecoder::coreModuleDeclaration(Reader& reader)
    {
        auto const offset = reader.offset();
        auto const byte = u8(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        auto const row =
            std::ranges::find(CoreModuleDeclarationTable, *byte, &CoreModuleDeclarationRow::byte);
        if (row == CoreModuleDeclarationTable.end())
            return std::unexpected { DecodeError {
                DecodeErrorCode::InvalidModuleTypeDeclaration, offset, *byte } };
        switch (row->kind)
        {
            case CoreDeclarationKind::Import: {
                auto module = name(reader);
                if (!module)
                    return std::unexpected { module.error() };
                auto field = name(reader);
                if (!field)
                    return std::unexpected { field.error() };
                return coreExternType(reader).transform([&](CoreExternType type) {
                    return CoreModuleDeclaration { CoreImportDeclaration {
                        .module = std::move(*module), .name = std::move(*field), .type = type } };
                });
            }
            case CoreDeclarationKind::Type:
                // A core:rectype in the core type section's encoding: a bare 0x50 is a subtype here.
                return coreRecType(reader).transform([](isoline::binary::FuncType type) {
                    return CoreModuleDeclaration { CoreTypeDeclaration { .type = std::move(type) } };
                });
            case CoreDeclarationKind::Alias: {
                auto const sortOffset = reader.offset();
                auto const sort = u8(reader);
                if (!sort)
                    return std::unexpected { sort.error() };
                if (*sort != CoreOuterAliasSort)
                    return std::unexpected { DecodeError {
                        DecodeErrorCode::InvalidCoreOuterAliasKind, sortOffset, *sort } };
                auto const targetOffset = reader.offset();
                auto const target = u8(reader);
                if (!target)
                    return std::unexpected { target.error() };
                if (*target != CoreOuterAliasTarget)
                    return std::unexpected { DecodeError {
                        DecodeErrorCode::InvalidCoreOuterAliasTarget, targetOffset, *target } };
                auto const count = u32(reader);
                if (!count)
                    return std::unexpected { count.error() };
                return u32(reader).transform([&](uint32_t index) {
                    return CoreModuleDeclaration { CoreOuterAliasDeclaration { .outerCount = *count,
                                                                               .outerIndex = index } };
                });
            }
            case CoreDeclarationKind::Export: break;
        }
        auto exportName = name(reader);
        if (!exportName)
            return std::unexpected { exportName.error() };
        return coreExternType(reader).transform([&](CoreExternType type) {
            return CoreModuleDeclaration { CoreExportDeclaration { .name = std::move(*exportName),
                                                                   .type = type } };
        });
    }

    Result<isoline::binary::FuncType> ComponentDecoder::coreRecType(Reader& reader)
    {
        // As wasmparser reads a rec group, up to its composite type's byte; Isoline's core has no GC, so
        // every form but a function type is then refused as the core decoder refuses one.
        auto const offset = reader.offset();
        auto const byte = peek(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        auto const formOf = [](uint8_t candidate) {
            auto const row = std::ranges::find(CoreTypeForms, candidate, &CoreTypeFormRow::byte);
            return row == CoreTypeForms.end() ? std::optional<CoreTypeForm> {} : std::optional { row->form };
        };
        auto const form = formOf(*byte);
        if (!form)
            return std::unexpected { DecodeError { DecodeErrorCode::InvalidCoreTypeForm, offset, *byte } };
        switch (*form)
        {
            case CoreTypeForm::Function: return coreFuncType(reader);
            case CoreTypeForm::RecGroup:
            case CoreTypeForm::Composite: return std::unexpected { coreGcRefusal(offset) };
            case CoreTypeForm::SubType: break;
        }
        // A subtype: its supertypes, then its composite type's byte.
        std::ignore = u8(reader);
        auto const supertypes = forEach(reader, [&]() { return u32(reader).transform([](uint32_t) {}); });
        if (!supertypes)
            return std::unexpected { supertypes.error() };
        auto const compositeOffset = reader.offset();
        auto const composite = peek(reader);
        if (!composite)
            return std::unexpected { composite.error() };
        auto const compositeForm = formOf(*composite);
        if (compositeForm != CoreTypeForm::Function && compositeForm != CoreTypeForm::Composite)
            return std::unexpected { DecodeError {
                DecodeErrorCode::InvalidCoreTypeForm, compositeOffset, *composite } };
        return std::unexpected { coreGcRefusal(offset) };
    }

    Result<CoreTypeDefinition> ComponentDecoder::coreType(Reader& reader)
    {
        // 0x50 is a module type here, so a non-final subtype takes a 0x00 prefix (Binary.md's wart).
        auto const byte = peek(reader);
        if (!byte)
            return std::unexpected { byte.error() };
        if (*byte == ModuleTypeForm)
        {
            std::ignore = u8(reader);
            auto module = CoreModuleTypeDefinition {};
            auto const declarations = forEach(reader, [&]() -> Result<void> {
                return coreModuleDeclaration(reader).transform([&](CoreModuleDeclaration declaration) {
                    module.declarations.push_back(std::move(declaration));
                });
            });
            if (!declarations)
                return std::unexpected { declarations.error() };
            return CoreTypeDefinition { .type = std::move(module) };
        }
        if (*byte == NonFinalSubTypePrefix)
        {
            std::ignore = u8(reader);
            auto const formOffset = reader.offset();
            auto const form = peek(reader);
            if (!form)
                return std::unexpected { form.error() };
            if (*form != ModuleTypeForm)
                return std::unexpected { DecodeError {
                    DecodeErrorCode::InvalidNonFinalSubType, formOffset, *form } };
        }
        return coreRecType(reader).transform(
            [](isoline::binary::FuncType type) { return CoreTypeDefinition { .type = std::move(type) }; });
    }

    Result<void> ComponentDecoder::immediate(Reader& reader,
                                             CanonBuiltinRow const& row,
                                             Immediate kind,
                                             CanonDefinition& definition)
    {
        auto const offset = reader.offset();
        switch (kind)
        {
            case Immediate::None: return {};
            case Immediate::SortByte: {
                auto const sort = u8(reader);
                if (!sort)
                    return std::unexpected { sort.error() };
                if (*sort != CanonFuncSort)
                    return std::unexpected { DecodeError {
                        DecodeErrorCode::InvalidCanonSort, offset, canonSortSubject(row.opcode, *sort) } };
                return {};
            }
            case Immediate::Index:
                return u32(reader).transform([&](uint32_t index) { definition.immediates.push_back(index); });
            case Immediate::Options:
                return forEach(reader, [&]() -> Result<void> {
                    auto const optionOffset = reader.offset();
                    auto const byte = u8(reader);
                    if (!byte)
                        return std::unexpected { byte.error() };
                    if (*byte >= CanonOptionTable.size())
                        return std::unexpected { DecodeError {
                            DecodeErrorCode::InvalidCanonOption, optionOffset, *byte } };
                    auto const& option = CanonOptionTable[*byte];
                    if (option.immediate == OptionImmediate::None)
                    {
                        definition.options.push_back({ .kind = option.kind, .index = 0 });
                        return {};
                    }
                    return u32(reader).transform([&](uint32_t index) {
                        definition.options.push_back({ .kind = option.kind, .index = index });
                    });
                });
            case Immediate::ResultList:
                return resultList(reader).transform(
                    [&](std::optional<ValueTypeRef> result) { definition.result = result; });
            case Immediate::CoreValueType:
                return coreValueKind(reader).transform([&](CoreValueKind value) {
                    definition.immediates.push_back(static_cast<uint32_t>(value));
                });
            case Immediate::Async:
                return boolean(reader).transform(
                    [&](bool isAsync) { definition.immediates.push_back(isAsync ? 1 : 0); });
            // `shared` is 🧵②, the gate of every built-in that reads it (static_assert above), which
            // validation refuses.
            case Immediate::Shared:
                return boolean(reader).transform(
                    [&](bool isShared) { definition.immediates.push_back(isShared ? 1 : 0); });
            case Immediate::Reserved: break;
        }
        auto const reserved = boolean(reader);
        if (!reserved)
            return std::unexpected { reserved.error() };
        if (*reserved)
            return std::unexpected { DecodeError { DecodeErrorCode::NonZeroReservedByte, offset } };
        return {};
    }

    Result<CanonDefinition> ComponentDecoder::canon(Reader& reader)
    {
        auto const offset = reader.offset();
        auto const opcode = u8(reader);
        if (!opcode)
            return std::unexpected { opcode.error() };
        auto const row = std::ranges::lower_bound(CanonBuiltinTable, *opcode, {}, &CanonBuiltinRow::opcode);
        if (row == CanonBuiltinTable.end() || row->opcode != *opcode)
            return std::unexpected { DecodeError { DecodeErrorCode::InvalidCanonFunction, offset, *opcode } };
        auto definition = CanonDefinition {
            .builtin = row->builtin, .immediates = {}, .options = {}, .result = std::nullopt
        };
        for (auto const kind: row->shape)
        {
            if (auto read = immediate(reader, *row, kind, definition); !read)
                return std::unexpected { read.error() };
        }
        return definition;
    }

    Result<void> ComponentDecoder::coreTypeSection(SectionContext& section)
    {
        return definitions(section, [&](Reader& reader) -> Result<CoreTypeDefinition> {
            auto const offset = reader.offset();
            auto type = coreType(reader);
            if (!type)
                return type;
            if (auto counted = countType(section.component, offset); !counted)
                return std::unexpected { counted.error() };
            return type;
        });
    }

    Result<void> ComponentDecoder::typeSection(SectionContext& section)
    {
        // The section's vector and the declarations of every component and instance type in it, on
        // one explicit stack of scopes: the section's component at the bottom, the innermost type last.
        auto& reader = section.reader;
        auto const count = reader.readLength();
        if (!count)
            return std::unexpected { fromCore(count.error()) };
        auto stack = TypeScopeStack { TypeScopeFrame {
            .scope = section.component, .remaining = *count, .depth = section.depth } };
        while (!stack.empty())
        {
            if (stack.back().remaining == 0)
            {
                stack.pop_back();
                continue;
            }
            --stack.back().remaining;
            // A copy: reading the entry may open a scope, which grows the stack.
            auto const frame = stack.back();
            auto const offset = reader.offset();
            if (auto charged = chargeItem(offset); !charged)
                return charged;
            if (auto room = roomForDefinition(frame.scope, offset); !room)
                return room;
            auto const read = _pool.components[frame.scope].kind == ScopeKind::Component
                                  ? typeEntry(reader, frame, stack)
                                  : declaration(reader, frame, stack);
            if (!read)
                return read;
        }
        return {};
    }

    Result<void> ComponentDecoder::canonSection(SectionContext& section)
    {
        return definitions(section, [&](Reader& reader) { return canon(reader); });
    }

    Result<void> ComponentDecoder::unimplementedSection(SectionContext& section)
    // uncovered-begin(values-sections)
    {
        return std::unexpected { DecodeError {
            DecodeErrorCode::UnimplementedSection, section.reader.offset(), section.id } };
    }
    // uncovered-end(values-sections)

} // namespace

LayerKind layerOf(std::span<std::byte const> bytes) noexcept
{
    if (bytes.size() < PreambleSize || !std::ranges::equal(bytes.first(Magic.size()), Magic))
        return LayerKind::Unknown;
    return layerOfWord(bytes.subspan(Magic.size(), PreambleSize - Magic.size()));
}

std::string specMessage(DecodeError const& error)
{
    auto const& row = DecodeErrorMessages[static_cast<std::size_t>(error.code)];
    auto text = std::string { row.text };
    switch (row.detail)
    {
        case DecodeErrorDetail::None: break;
        case DecodeErrorDetail::LeadingByte:
            return std::format("invalid leading byte (0x{:x}) for {}", error.subject.value_or(0), row.text);
        case DecodeErrorDetail::SectionId:
            return error.subject ? std::format("{}: {}", row.text, *error.subject) : text;
        case DecodeErrorDetail::BinaryVersion:
            return error.subject ? std::format("{}: {:#10x}", row.text, *error.subject) : text;
        case DecodeErrorDetail::Attribute:
            if (!error.subject || *error.subject >= NameAttributeTable.size())
                return text;
            return std::format("duplicate '{}' {}", NameAttributeTable[*error.subject].keyword, row.text);
        case DecodeErrorDetail::Feature: {
            if (!error.disabled)
                return text;
            auto const& feature = componentFeatureRow(*error.disabled);
            return std::format("{} ({})", feature.refusal, feature.gate);
        }
        case DecodeErrorDetail::Limit: {
            auto const limit = exceededLimit(error);
            if (!limit)
                return text;
            return limitMessage(*limit, *error.subject >> 8U);
        }
        case DecodeErrorDetail::CoreError:
            if (!error.subject
                || *error.subject > static_cast<uint64_t>(isoline::binary::LastDecodeErrorCode))
                return text;
            return std::string { isoline::binary::specMessage(
                static_cast<isoline::binary::DecodeErrorCode>(*error.subject)) };
        case DecodeErrorDetail::CanonSort: {
            auto const subject = error.subject.value_or(0);
            auto const opcode = static_cast<uint8_t>(subject >> 8U);
            auto const builtin = std::ranges::find(CanonBuiltinTable, opcode, &CanonBuiltinRow::opcode);
            if (builtin == CanonBuiltinTable.end())
                return text;
            return std::format(
                "invalid leading byte (0x{:x}) for {} {}", subject & 0xFFU, row.text, builtin->text);
        }
    }
    return text;
}

std::expected<ComponentPool, DecodeError> decodeComponent(std::span<std::byte const> bytes,
                                                          ComponentFeatureSet features,
                                                          ComponentLimits const& limits)
{
    return ComponentDecoder { features, limits }.decode(bytes);
}

std::vector<DefinitionPosition> coreModulePositions(ComponentPool const& pool)
{
    // Pre-order over the pool, on an explicit stack: a component's definitions up to a nested one,
    // that one's, then the rest.
    struct Visit
    {
        uint32_t component; ///< The pool index of the component.
        std::size_t next;   ///< Its next definition to visit.
    };
    auto positions = std::vector<DefinitionPosition> {};
    auto stack = std::vector<Visit> { Visit { .component = 0, .next = 0 } };
    while (!stack.empty())
    {
        auto& visit = stack.back();
        auto const& definitions = pool.components[visit.component].definitions;
        if (visit.next == definitions.size())
        {
            stack.pop_back();
            continue;
        }
        auto const position = DefinitionPosition { .scope = visit.component, .definition = visit.next++ };
        auto const& definition = definitions[position.definition];
        if (std::holds_alternative<CoreModuleDefinition>(definition))
            positions.push_back(position);
        else if (auto const* nested = std::get_if<NestedComponent>(&definition))
        {
            Require(nested->index > position.scope && nested->index < pool.components.size());
            stack.push_back(Visit { .component = nested->index, .next = 0 });
        }
    }
    return positions;
}

std::expected<std::vector<std::span<std::byte const>>, DecodeError> splitCoreModules(
    std::span<std::byte const> bytes, ComponentFeatureSet features, ComponentLimits const& limits)
{
    auto const pool = decodeComponent(bytes, features, limits);
    if (!pool)
        return std::unexpected { pool.error() };

    auto modules = std::vector<std::span<std::byte const>> {};
    for (auto const& position: coreModulePositions(*pool))
    {
        auto const& module =
            std::get<CoreModuleDefinition>(pool->components[position.scope].definitions[position.definition]);
        modules.push_back(bytes.subspan(module.offset, module.size));
    }
    return modules;
}

} // namespace isoline::component::binary
