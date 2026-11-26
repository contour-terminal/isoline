// SPDX-License-Identifier: Apache-2.0
#include <isoline/binary/Decoder.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/ByteReader.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>

namespace isoline::binary
{

namespace
{

    /// The limit of a vector DecodeLimits does not bound: the bytes left still do.
    constexpr auto Unlimited = std::numeric_limits<uint32_t>::max();

    /// How many items readVector() reserves before reading the first; push_back grows beyond.
    constexpr auto MaxUpFrontReservation = std::size_t { 4096 };

    /// The magic number every module starts with, "\0asm", read as a little-endian word.
    constexpr auto Magic = uint32_t { 0x6D73'6100 };

    /// The only binary format version there is.
    constexpr auto Version = uint32_t { 1 };

    /// The id of a custom section, which may appear anywhere.
    constexpr auto CustomSectionId = uint8_t { 0 };

    /// The byte that introduces a function type in the type section.
    constexpr auto FuncTypeForm = uint8_t { 0x60 };

    /// The bit of a signed LEB128 byte that says another byte follows.
    constexpr auto LebContinuation = uint8_t { 0x80 };

    /// One encoding of a type byte: the byte, the type it stands for, and the proposal it needs.
    struct TypeEncoding
    {
        uint8_t byte;                    ///< The byte, an s7 in the binary grammar.
        ValueType type;                  ///< The type it encodes.
        std::optional<Feature> required; ///< The proposal that introduced it, unless it is MVP.
    };

    /// Every value type P1 decodes.
    constexpr auto ValueTypeEncodings = std::to_array<TypeEncoding>({
        { .byte = 0x7F, .type = I32, .required = std::nullopt },
        { .byte = 0x7E, .type = I64, .required = std::nullopt },
        { .byte = 0x7D, .type = F32, .required = std::nullopt },
        { .byte = 0x7C, .type = F64, .required = std::nullopt },
        { .byte = 0x7B, .type = V128, .required = Feature::Simd },
        { .byte = 0x70, .type = FuncRef, .required = Feature::ReferenceTypes },
        { .byte = 0x6F, .type = ExternRef, .required = Feature::ReferenceTypes },
    });

    /// Every reference type P1 decodes where only a reference type may stand: funcref was a table's
    /// element type in the MVP already.
    constexpr auto RefTypeEncodings = std::to_array<TypeEncoding>({
        { .byte = 0x70, .type = FuncRef, .required = std::nullopt },
        { .byte = 0x6F, .type = ExternRef, .required = Feature::ReferenceTypes },
    });

    /// What an import, or an export, names, by its kind byte.
    constexpr auto ExternKinds = std::to_array<ExternKind>({
        ExternKind::Func,
        ExternKind::Table,
        ExternKind::Memory,
        ExternKind::Global,
    });

    /// Whether a segment names its table or memory, or uses index 0 implicitly.
    enum class IndexEncoding : uint8_t
    {
        Implicit, ///< No index is encoded; the segment initializes table or memory 0.
        Explicit, ///< A u32 index precedes the offset expression.
    };

    /// How an element segment encodes the type of its items.
    enum class ElementTypeEncoding : uint8_t
    {
        Implicit, ///< Not encoded: funcref.
        ElemKind, ///< An element kind byte, of which 0x00 (funcref) is the only one.
        RefType,  ///< A reference type.
    };

    /// How an element segment encodes its items.
    enum class ItemEncoding : uint8_t
    {
        FunctionIndices, ///< Function indices, each standing for (ref.func index).
        Expressions,     ///< Constant expressions.
    };

    /// How an element segment's flags shape it (WebAssembly 2.0, §5.5.12).
    struct ElementForm
    {
        SegmentMode mode;                ///< How the segment is used.
        IndexEncoding table;             ///< Whether a table index precedes the offset.
        ElementTypeEncoding type;        ///< How the item type is encoded.
        ItemEncoding items;              ///< How the items are encoded.
        std::optional<Feature> required; ///< The proposal that introduced the form.
    };

    /// Every element segment form, indexed by its flags.
    constexpr auto ElementForms = std::to_array<ElementForm>({
        { SegmentMode::Active,
          IndexEncoding::Implicit,
          ElementTypeEncoding::Implicit,
          ItemEncoding::FunctionIndices,
          std::nullopt },
        { SegmentMode::Passive,
          IndexEncoding::Implicit,
          ElementTypeEncoding::ElemKind,
          ItemEncoding::FunctionIndices,
          Feature::BulkMemory },
        { SegmentMode::Active,
          IndexEncoding::Explicit,
          ElementTypeEncoding::ElemKind,
          ItemEncoding::FunctionIndices,
          Feature::BulkMemory },
        { SegmentMode::Declarative,
          IndexEncoding::Implicit,
          ElementTypeEncoding::ElemKind,
          ItemEncoding::FunctionIndices,
          Feature::ReferenceTypes },
        { SegmentMode::Active,
          IndexEncoding::Implicit,
          ElementTypeEncoding::Implicit,
          ItemEncoding::Expressions,
          Feature::BulkMemory },
        { SegmentMode::Passive,
          IndexEncoding::Implicit,
          ElementTypeEncoding::RefType,
          ItemEncoding::Expressions,
          Feature::BulkMemory },
        { SegmentMode::Active,
          IndexEncoding::Explicit,
          ElementTypeEncoding::RefType,
          ItemEncoding::Expressions,
          Feature::BulkMemory },
        { SegmentMode::Declarative,
          IndexEncoding::Implicit,
          ElementTypeEncoding::RefType,
          ItemEncoding::Expressions,
          Feature::ReferenceTypes },
    });

    /// How a data segment's flags shape it.
    struct DataForm
    {
        SegmentMode mode;                ///< How the segment is used.
        IndexEncoding memory;            ///< Whether a memory index precedes the offset.
        std::optional<Feature> required; ///< The proposal that introduced the form.
    };

    /// Every data segment form, indexed by its flags. Flags 2 was the MVP's memory index 2, which
    /// no MVP module could validly use.
    constexpr auto DataForms = std::to_array<DataForm>({
        { SegmentMode::Active, IndexEncoding::Implicit, std::nullopt },
        { SegmentMode::Passive, IndexEncoding::Implicit, Feature::BulkMemory },
        { SegmentMode::Active, IndexEncoding::Explicit, Feature::BulkMemory },
    });

    /// What a byte read where an instruction starts is.
    enum class OpcodeByteRole : uint8_t
    {
        Opcode, ///< A single-byte opcode, or none.
        Prefix, ///< A prefix: the sub-opcode follows as a u32.
    };

    /// The role of every byte, derived from OpcodeTable's prefixes once, at compile time.
    constexpr auto OpcodeByteRoles = [] {
        auto roles = std::array<OpcodeByteRole, 256> {};
        for (auto const& row: OpcodeTable)
            if (row.prefix != NoPrefix)
                roles[row.prefix] = OpcodeByteRole::Prefix;
        return roles;
    }();

    /// The prefixes whose unknown sub-opcodes the interpreter names alone, without the prefix byte:
    /// its vector decoder reports "illegal opcode 9a" where 0xFC's reports "illegal opcode fc 12".
    constexpr auto PrefixesNamedBySubOpcode = std::to_array<uint8_t>({ 0xFD });

    /// The subject of an IllegalOpcode error for the opcode read, spelled as the interpreter does.
    [[nodiscard]] constexpr uint64_t illegalOpcodeSubject(uint8_t prefix, uint32_t code) noexcept
    {
        if (std::ranges::find(PrefixesNamedBySubOpcode, prefix) != PrefixesNamedBySubOpcode.end())
            return opcodeSubject(NoPrefix, code);
        return opcodeSubject(prefix, code);
    }

    /// Which bounds a limits flag announces.
    enum class LimitsBound : uint8_t
    {
        Minimum,           ///< A minimum only.
        MinimumAndMaximum, ///< A minimum, then a maximum.
    };

    /// Whether a page size exponent follows a limits flag's bounds (custom-page-sizes).
    enum class PageSizeField : uint8_t
    {
        Default,  ///< None: the page size is 64 KiB.
        Exponent, ///< A u32 exponent, the page size's base-2 logarithm; memories only.
    };

    /// What limits are read for: a table's flags admit no page size.
    enum class LimitsOf : uint8_t
    {
        Table,
        Memory,
    };

    /// One limits flag byte WebAssembly 3.0 admits: what follows it, and the proposals it needs.
    struct LimitsFlagRow
    {
        uint8_t flags;           ///< The flag byte.
        LimitsBound bound;       ///< The bounds that follow.
        AddressType addressType; ///< The address type of the memory or table.
        PageSizeField pageSize;  ///< Whether a page size follows the bounds.
        FeatureSet required;     ///< The proposals that admit it; none for the MVP's flags.
    };

    constexpr auto NeedsMemory64 = FeatureSet::none().with(Feature::Memory64);
    constexpr auto NeedsPageSizes = FeatureSet::none().with(Feature::CustomPageSizes);
    constexpr auto NeedsMemory64AndPageSizes = NeedsMemory64.with(Feature::CustomPageSizes);

    /// The limits flags of tables and memories: bit 0 a maximum, bit 2 i64 addresses, bit 3 a custom
    /// page size (memories only). Any other byte is MalformedLimitsFlags; in WebAssembly 2.0's grammar
    /// only the first two rows exist.
    constexpr auto LimitsFlagTable = std::to_array<LimitsFlagRow>({
        { .flags = 0x00,
          .bound = LimitsBound::Minimum,
          .addressType = AddressType::I32,
          .pageSize = PageSizeField::Default,
          .required = FeatureSet::none() },
        { .flags = 0x01,
          .bound = LimitsBound::MinimumAndMaximum,
          .addressType = AddressType::I32,
          .pageSize = PageSizeField::Default,
          .required = FeatureSet::none() },
        { .flags = 0x04,
          .bound = LimitsBound::Minimum,
          .addressType = AddressType::I64,
          .pageSize = PageSizeField::Default,
          .required = NeedsMemory64 },
        { .flags = 0x05,
          .bound = LimitsBound::MinimumAndMaximum,
          .addressType = AddressType::I64,
          .pageSize = PageSizeField::Default,
          .required = NeedsMemory64 },
        { .flags = 0x08,
          .bound = LimitsBound::Minimum,
          .addressType = AddressType::I32,
          .pageSize = PageSizeField::Exponent,
          .required = NeedsPageSizes },
        { .flags = 0x09,
          .bound = LimitsBound::MinimumAndMaximum,
          .addressType = AddressType::I32,
          .pageSize = PageSizeField::Exponent,
          .required = NeedsPageSizes },
        { .flags = 0x0C,
          .bound = LimitsBound::Minimum,
          .addressType = AddressType::I64,
          .pageSize = PageSizeField::Exponent,
          .required = NeedsMemory64AndPageSizes },
        { .flags = 0x0D,
          .bound = LimitsBound::MinimumAndMaximum,
          .addressType = AddressType::I64,
          .pageSize = PageSizeField::Exponent,
          .required = NeedsMemory64AndPageSizes },
    });

    static_assert(
        [] {
            auto index = uint8_t { 0 };
            for (auto const& row: LimitsFlagTable | std::views::take(2))
                if (row.flags != index++ || row.required != FeatureSet::none()
                    || row.pageSize != PageSizeField::Default)
                    return false;
            return true;
        }(),
        "LimitsFlagTable's first two rows are WebAssembly 2.0's flags 0x00 and 0x01, indexed by them");

    /// Which grammar a table's or memory's limits follow.
    enum class LimitsGrammar : uint8_t
    {
        Wasm2, ///< A one-bit LEB128 flag, 0x00 or 0x01, and u32 bounds.
        Wasm3, ///< A flag byte of a LimitsFlagTable row whose proposals are on, and u64 bounds.
    };

    /// The grammar limits follow under @p features: WebAssembly 3.0's when a proposal a
    /// LimitsFlagTable row needs is on (memory64 or custom-page-sizes, which build on 3.0). Otherwise,
    /// as for 2.0 and for 3.0 without memory64, 2.0's, which memory64 widened to 3.0's.
    [[nodiscard]] constexpr LimitsGrammar limitsGrammarOf(FeatureSet features) noexcept
    {
        auto const widened = std::ranges::any_of(LimitsFlagTable, [features](LimitsFlagRow const& row) {
            return row.required != FeatureSet::none() && features.includes(row.required);
        });
        return widened ? LimitsGrammar::Wasm3 : LimitsGrammar::Wasm2;
    }

    /// Limits as the binary format encodes them: the bounds and, for a memory, its page size.
    struct EncodedLimits
    {
        Limits limits;                              ///< The bounds and address type.
        uint8_t pageSizeLog2 = DefaultPageSizeLog2; ///< The page size's base-2 logarithm.
    };

    /// Where a memarg's malformed flags are reported; the two interpreters' memop differ.
    enum class FlagsErrorPosition : uint8_t
    {
        LastByte,  ///< At the flags' last byte, as WebAssembly 2.0's memop does.
        FirstByte, ///< At the flags' first byte, as WebAssembly 3.0's memop does.
    };

    /// How a memarg's flags field splits into an alignment exponent and a memory index (spec
    /// decode.ml memop).
    struct MemArgGrammar
    {
        uint32_t flagsLimit;        ///< Flags at or above this are malformed memop flags.
        uint32_t memoryIndexBit;    ///< The flag saying a u32 memory index follows; 0 if none can.
        FlagsErrorPosition errorAt; ///< Where malformed flags are reported.
    };

    /// WebAssembly 2.0's memarg: the flags are the alignment exponent, below 32, of memory 0.
    constexpr auto MemArgGrammar2 =
        MemArgGrammar { .flagsLimit = 32, .memoryIndexBit = 0, .errorAt = FlagsErrorPosition::LastByte };

    /// Multi-memory's, as WebAssembly 3.0 has it: bit 6 says a memory index follows, the six bits
    /// below it are the alignment exponent, which the validator bounds, and no higher bit is set.
    constexpr auto MemArgGrammar3 = MemArgGrammar { .flagsLimit = 0x80,
                                                    .memoryIndexBit = 0x40,
                                                    .errorAt = FlagsErrorPosition::FirstByte };

    /// The proposal that brought memory indices: MemArgGrammar3, and a u32 index in memory.size,
    /// memory.grow, memory.fill, memory.copy and memory.init where 2.0 has the reserved zero byte.
    constexpr auto MemoryIndexFeature = Feature::MultiMemory;

    /// The block type byte of a block without a result.
    constexpr auto EmptyBlockType = uint8_t { 0x40 };

    /// Where decoded instructions go: a body, and the side tables its instructions index.
    struct CodeSink
    {
        std::vector<Instruction>& body;       ///< The instructions, in order.
        std::vector<uint32_t>& brTableLabels; ///< br_table labels (Function::brTableLabels).
        std::vector<ValueType>& selectTypes;  ///< Typed select operand types (Function::selectTypes).
    };

    /// A block, loop or if whose end has not been read yet.
    struct OpenBlock
    {
        uint32_t opener;                   ///< The index of its block, loop or if.
        std::optional<uint32_t> elseIndex; ///< The index of its else, once read.
    };

    /// Decodes one module, front to back, as the spec reference interpreter reads it.
    class ModuleDecoder
    {
      public:
        ModuleDecoder(std::span<std::byte const> bytes, FeatureSet features, DecodeLimits const& limits):
            _reader { bytes },
            _moduleSize { bytes.size() },
            _features { features },
            _limitsGrammar { limitsGrammarOf(features) },
            _limits { limits }
        {
        }

        [[nodiscard]] std::expected<Module, DecodeError> decode();

        // The section decoders, one per row of SectionTable.
        [[nodiscard]] std::expected<void, DecodeError> typeSection();
        [[nodiscard]] std::expected<void, DecodeError> importSection();
        [[nodiscard]] std::expected<void, DecodeError> functionSection();
        [[nodiscard]] std::expected<void, DecodeError> tableSection();
        [[nodiscard]] std::expected<void, DecodeError> memorySection();
        [[nodiscard]] std::expected<void, DecodeError> globalSection();
        [[nodiscard]] std::expected<void, DecodeError> exportSection();
        [[nodiscard]] std::expected<void, DecodeError> startSection();
        [[nodiscard]] std::expected<void, DecodeError> elementSection();
        [[nodiscard]] std::expected<void, DecodeError> dataCountSection();
        [[nodiscard]] std::expected<void, DecodeError> codeSection();
        [[nodiscard]] std::expected<void, DecodeError> dataSection();

      private:
        [[nodiscard]] std::expected<void, DecodeError> preamble();
        [[nodiscard]] std::expected<void, DecodeError> customSection();
        [[nodiscard]] std::expected<void, DecodeError> checkConsistency() const;

        [[nodiscard]] std::expected<uint8_t, DecodeError> readByte();
        [[nodiscard]] std::expected<uint32_t, DecodeError> readCount(uint32_t limit);

        /// Reads a vector: a count bounded by the bytes left and by @p limit, then that many items,
        /// appended to @p items. Nothing is reserved before the count passes both bounds.
        ///
        /// Only an empty @p items is reserved for, and for at most MaxUpFrontReservation items: an
        /// item may take many times the byte that bounds its count, so reserving a claimed count
        /// whole would let junk input allocate far more than its size before its first item fails.
        /// The rest grows by push_back's geometric steps, as does a side table that many
        /// instructions append to: reserving its exact new size on each append would copy the
        /// whole table every time and make decoding quadratic.
        /// @param readOne Reads the item at the index it is given.
        template <typename T, typename ReadOne>
        [[nodiscard]] std::expected<void, DecodeError> readVector(uint32_t limit,
                                                                  std::vector<T>& items,
                                                                  ReadOne readOne)
        {
            auto const count = readCount(limit);
            if (!count)
                return std::unexpected { count.error() };
            if (items.empty())
                items.reserve(std::min<std::size_t>(*count, MaxUpFrontReservation));
            for (auto const index: std::views::iota(uint32_t { 0 }, *count))
            {
                auto item = readOne(index);
                if (!item)
                    return std::unexpected { item.error() };
                items.push_back(std::move(*item));
            }
            return {};
        }
        [[nodiscard]] std::expected<uint8_t, DecodeError> readTypeByte();
        [[nodiscard]] std::expected<ValueType, DecodeError> readTypeFrom(
            std::span<TypeEncoding const> encodings);
        [[nodiscard]] std::expected<ValueType, DecodeError> readValueType();
        [[nodiscard]] std::expected<ValueType, DecodeError> readRefType();
        [[nodiscard]] std::expected<std::vector<ValueType>, DecodeError> readResultType(uint32_t limit);
        [[nodiscard]] std::expected<FuncType, DecodeError> readFuncType();
        /// Reads a table's or memory's limits, in the grammar _limitsGrammar names.
        [[nodiscard]] std::expected<EncodedLimits, DecodeError> readLimits(LimitsOf of);
        /// Reads limits in WebAssembly 3.0's grammar: flags of a LimitsFlagTable row whose
        /// proposals are on, u64 bounds, then the page size a memory's flags announce.
        [[nodiscard]] std::expected<EncodedLimits, DecodeError> readLimits3(LimitsOf of);
        /// Reads a custom page size's exponent; one of PageSizeLog2Limit or more is malformed.
        [[nodiscard]] std::expected<uint8_t, DecodeError> readPageSizeLog2();
        [[nodiscard]] std::expected<MemoryType, DecodeError> readMemoryType();
        /// Reads the bounds @p row's flags announce, each with @p readBound.
        template <typename ReadBound>
        [[nodiscard]] std::expected<Limits, DecodeError> readBounds(LimitsFlagRow const& row,
                                                                    ReadBound readBound);
        [[nodiscard]] std::expected<TableType, DecodeError> readTableType();
        [[nodiscard]] std::expected<GlobalType, DecodeError> readGlobalType();
        [[nodiscard]] std::expected<ExternKind, DecodeError> readExternKind(DecodeErrorCode malformed);
        [[nodiscard]] std::expected<Import, DecodeError> readImport();
        [[nodiscard]] std::expected<Global, DecodeError> readGlobal();
        [[nodiscard]] std::expected<Export, DecodeError> readExport();
        [[nodiscard]] std::expected<std::string, DecodeError> readName();
        [[nodiscard]] std::expected<ConstExpr, DecodeError> readConstExpr();
        [[nodiscard]] std::optional<uint8_t> peekByte() const;
        [[nodiscard]] std::expected<void, DecodeError> expectZeroByte();
        [[nodiscard]] std::expected<void, DecodeError> readIndex(uint32_t& index);
        /// Reads a memory index immediate into @p index: a u32 under multi-memory, else the reserved
        /// zero byte, which names memory 0.
        [[nodiscard]] std::expected<void, DecodeError> readMemoryIndex(uint32_t& index);
        /// Counts one more imported table or memory in @p count, refusing one past @p limit.
        /// @param offset Where the import starts, at which a refusal is reported.
        [[nodiscard]] static std::expected<void, DecodeError> countImport(uint32_t& count,
                                                                          uint32_t limit,
                                                                          std::size_t offset);
        [[nodiscard]] std::expected<void, DecodeError> readExpression(CodeSink sink);
        /// Reads the instruction whose first byte, @p byte, readExpression has peeked already.
        [[nodiscard]] std::expected<Instruction, DecodeError> readInstruction(uint8_t byte, CodeSink sink);
        [[nodiscard]] std::expected<void, DecodeError> readImmediates(Instruction& instruction,
                                                                      CodeSink sink);
        [[nodiscard]] std::expected<void, DecodeError> readBlockType(Instruction& instruction);
        [[nodiscard]] std::expected<void, DecodeError> readMemArg(Instruction& instruction);
        /// Reads 16 raw bytes into Module::v128Literals and points @p instruction's a at them.
        [[nodiscard]] std::expected<void, DecodeError> readV128Literal(Instruction& instruction);
        /// Reads a lane index byte into @p instruction's lane.
        [[nodiscard]] std::expected<void, DecodeError> readLane(Instruction& instruction);
        [[nodiscard]] std::expected<ElementSegment, DecodeError> readElementSegment();
        [[nodiscard]] std::expected<DataSegment, DecodeError> readDataSegment();
        [[nodiscard]] std::expected<Function, DecodeError> readFunction(uint32_t index);
        /// The locals of a function body: its runs and their total.
        struct Locals
        {
            std::vector<LocalRun> runs;
            uint32_t count = 0;
        };

        [[nodiscard]] std::expected<Locals, DecodeError> readLocals();

        [[nodiscard]] bool enabled(std::optional<Feature> required) const noexcept
        {
            return !required || _features.has(*required);
        }

        ByteReader _reader;
        std::size_t _moduleSize;
        FeatureSet _features;
        LimitsGrammar _limitsGrammar; ///< The grammar of limits under _features.
        DecodeLimits _limits;
        Module _module;
        /// The tables and memories the import section has declared, which count towards
        /// DecodeLimits::maxTables and maxMemories with those the module defines.
        uint32_t _importedTables = 0;
        uint32_t _importedMemories = 0;

        /// The side tables of the constant expression being read. No valid constant expression
        /// uses them, so they only take what an invalid one holds, and are cleared for the next.
        std::vector<uint32_t> _constBrTableLabels;
        std::vector<ValueType> _constSelectTypes;
    };

    /// One non-custom section: its id, the proposal it needs, and its decoder.
    struct SectionRow
    {
        uint8_t id;                                                  ///< The section id.
        std::optional<Feature> required;                             ///< The proposal that added it.
        std::expected<void, DecodeError> (ModuleDecoder::*decode)(); ///< Decodes its contents.
    };

    /// Every non-custom section, in the order a module must list them; each appears at most once.
    constexpr auto SectionTable = std::to_array<SectionRow>({
        { .id = 1, .required = std::nullopt, .decode = &ModuleDecoder::typeSection },
        { .id = 2, .required = std::nullopt, .decode = &ModuleDecoder::importSection },
        { .id = 3, .required = std::nullopt, .decode = &ModuleDecoder::functionSection },
        { .id = 4, .required = std::nullopt, .decode = &ModuleDecoder::tableSection },
        { .id = 5, .required = std::nullopt, .decode = &ModuleDecoder::memorySection },
        { .id = 6, .required = std::nullopt, .decode = &ModuleDecoder::globalSection },
        { .id = 7, .required = std::nullopt, .decode = &ModuleDecoder::exportSection },
        { .id = 8, .required = std::nullopt, .decode = &ModuleDecoder::startSection },
        { .id = 9, .required = std::nullopt, .decode = &ModuleDecoder::elementSection },
        { .id = 12, .required = Feature::BulkMemory, .decode = &ModuleDecoder::dataCountSection },
        { .id = 10, .required = std::nullopt, .decode = &ModuleDecoder::codeSection },
        { .id = 11, .required = std::nullopt, .decode = &ModuleDecoder::dataSection },
    });

    std::expected<Module, DecodeError> ModuleDecoder::decode()
    {
        if (auto result = preamble(); !result)
            return std::unexpected { result.error() };

        // The rank in SectionTable of the last non-custom section read.
        auto lastRank = std::optional<std::size_t> {};
        while (auto const id = peekByte())
        {
            auto const idOffset = _reader.offset();
            std::ignore = readByte(); // id, peeked
            if (*id == CustomSectionId)
            {
                if (auto result = customSection(); !result)
                    return std::unexpected { result.error() };
                continue;
            }

            auto const row = std::ranges::find(SectionTable, *id, &SectionRow::id);
            if (row == SectionTable.end() || !enabled(row->required))
                return std::unexpected { DecodeError { DecodeErrorCode::MalformedSectionId, idOffset } };
            auto const rank = static_cast<std::size_t>(row - SectionTable.begin());
            if (lastRank && rank == *lastRank)
                return std::unexpected { DecodeError { DecodeErrorCode::DuplicateSection, idOffset } };
            if (lastRank && rank < *lastRank)
                return std::unexpected { DecodeError { DecodeErrorCode::SectionOutOfOrder, idOffset } };
            lastRank = rank;

            auto const size = _reader.readLength();
            if (!size)
                return std::unexpected { size.error() };
            auto const start = _reader.offset();
            if (auto result = (this->*row->decode)(); !result)
                return std::unexpected { result.error() };
            if (_reader.offset() != start + *size)
                return std::unexpected { DecodeError { DecodeErrorCode::SectionSizeMismatch, start } };
        }

        if (auto result = checkConsistency(); !result)
            return std::unexpected { result.error() };
        return std::move(_module);
    }

    std::expected<void, DecodeError> ModuleDecoder::preamble()
    {
        auto const magic = _reader.readFixedU32();
        if (!magic)
            return std::unexpected { magic.error() };
        if (*magic != Magic)
            return std::unexpected { DecodeError { DecodeErrorCode::MagicHeaderNotDetected, 0 } };
        auto const version = _reader.readFixedU32();
        if (!version)
            return std::unexpected { version.error() };
        if (*version != Version)
            return std::unexpected { DecodeError { DecodeErrorCode::UnknownBinaryVersion, 4 } };
        return {};
    }

    std::expected<void, DecodeError> ModuleDecoder::customSection()
    {
        auto const size = _reader.readLength();
        if (!size)
            return std::unexpected { size.error() };
        auto const start = _reader.offset();
        auto name = readName();
        if (!name)
            return std::unexpected { name.error() };
        // A name longer than the section leaves a negative payload, which the interpreter reports
        // as running out.
        auto const consumed = _reader.offset() - start;
        if (consumed > *size)
            return std::unexpected { DecodeError { DecodeErrorCode::UnexpectedEnd, _moduleSize } };
        return _reader.readBytes(*size - consumed).transform([&](std::span<std::byte const> payload) {
            _module.customs.push_back(CustomSection {
                .name = std::move(*name), .bytes = std::vector<std::byte>(payload.begin(), payload.end()) });
        });
    }

    std::expected<void, DecodeError> ModuleDecoder::checkConsistency() const
    {
        // The interpreter checks these once the module is read, and reports them at its end.
        if (_module.functions.size() != _module.code.size())
            return std::unexpected { DecodeError { DecodeErrorCode::FunctionAndCodeSectionInconsistent,
                                                   _moduleSize } };
        if (_module.dataCount && *_module.dataCount != _module.data.size())
            return std::unexpected { DecodeError { DecodeErrorCode::DataCountAndDataSectionInconsistent,
                                                   _moduleSize } };
        // Code that names a data segment needs the data count section, so that a single pass can
        // validate it; initializers are not code here, as in the interpreter.
        auto const namesDataSegment = [](Instruction const& instruction) {
            auto const immediate = opcodeInfo(instruction.opcode).immediate;
            return immediate == ImmediateKind::MemoryInit || immediate == ImmediateKind::DataIndex;
        };
        if (!_module.dataCount && std::ranges::any_of(_module.code, [&](Function const& function) {
                return std::ranges::any_of(function.body, namesDataSegment);
            }))
            return std::unexpected { DecodeError { DecodeErrorCode::DataCountSectionRequired, _moduleSize } };
        return {};
    }

    std::expected<uint8_t, DecodeError> ModuleDecoder::readByte()
    {
        return _reader.readByte().transform([](std::byte byte) { return std::to_integer<uint8_t>(byte); });
    }

    std::expected<uint32_t, DecodeError> ModuleDecoder::readCount(uint32_t limit)
    {
        auto const countOffset = _reader.offset();
        return _reader.readLength().and_then(
            [countOffset, limit](uint32_t count) -> std::expected<uint32_t, DecodeError> {
                if (count > limit)
                    return std::unexpected { DecodeError { DecodeErrorCode::ImplementationLimitExceeded,
                                                           countOffset } };
                return count;
            });
    }

    std::expected<uint8_t, DecodeError> ModuleDecoder::readTypeByte()
    {
        // The grammar reads a type as an s7: a byte with its continuation bit set is a longer
        // encoding than seven bits allow, reported past that byte.
        return readByte().and_then([this](uint8_t byte) -> std::expected<uint8_t, DecodeError> {
            if ((byte & LebContinuation) != 0)
                return std::unexpected { DecodeError { DecodeErrorCode::IntegerRepresentationTooLong,
                                                       _reader.offset() } };
            return byte;
        });
    }

    std::expected<ValueType, DecodeError> ModuleDecoder::readTypeFrom(std::span<TypeEncoding const> encodings)
    {
        auto const typeOffset = _reader.offset();
        return readTypeByte().and_then([&](uint8_t byte) -> std::expected<ValueType, DecodeError> {
            auto const encoding = std::ranges::find(encodings, byte, &TypeEncoding::byte);
            if (encoding == encodings.end() || !enabled(encoding->required))
                return std::unexpected { DecodeError { DecodeErrorCode::MalformedReferenceType,
                                                       typeOffset } };
            return encoding->type;
        });
    }

    std::expected<ValueType, DecodeError> ModuleDecoder::readValueType()
    {
        // An unknown value type is reported as the interpreter's last alternative, a reference
        // type, reports it.
        return readTypeFrom(ValueTypeEncodings);
    }

    std::expected<ValueType, DecodeError> ModuleDecoder::readRefType()
    {
        return readTypeFrom(RefTypeEncodings);
    }

    std::expected<std::vector<ValueType>, DecodeError> ModuleDecoder::readResultType(uint32_t limit)
    {
        auto types = std::vector<ValueType> {};
        return readVector(limit, types, [this](uint32_t) { return readValueType(); }).transform([&] {
            return std::move(types);
        });
    }

    std::expected<FuncType, DecodeError> ModuleDecoder::readFuncType()
    {
        auto const formOffset = _reader.offset();
        auto const form = readTypeByte();
        if (!form)
            return std::unexpected { form.error() };
        if (*form != FuncTypeForm)
            return std::unexpected { DecodeError { DecodeErrorCode::MalformedFunctionType, formOffset } };
        auto params = readResultType(_limits.maxParams);
        if (!params)
            return std::unexpected { params.error() };
        auto results = readResultType(_limits.maxResults);
        if (!results)
            return std::unexpected { results.error() };
        return FuncType { .params = std::move(*params), .results = std::move(*results) };
    }

    std::expected<EncodedLimits, DecodeError> ModuleDecoder::readLimits(LimitsOf of)
    {
        if (_limitsGrammar == LimitsGrammar::Wasm3)
            return readLimits3(of);
        // WebAssembly 2.0: flags 0x00, a minimum, or 0x01, a minimum and a maximum, read as a one-bit
        // LEB128, so a continuation bit over a valid flag is too long a representation; u32 bounds.
        auto const flagsOffset = _reader.offset();
        auto const flags = readByte();
        if (!flags)
            return std::unexpected { flags.error() };
        constexpr auto Continuation = uint8_t { 0x80 };
        if ((*flags & Continuation) != 0 && (*flags & ~Continuation) <= 0x01)
            return std::unexpected { DecodeError { DecodeErrorCode::IntegerRepresentationTooLong,
                                                   flagsOffset + 1 } };
        if (*flags > 0x01)
            return std::unexpected { DecodeError { DecodeErrorCode::LimitsFlagTooLarge, flagsOffset } };
        return readBounds(LimitsFlagTable[*flags], [this] { return _reader.readU32(); })
            .transform([](Limits const& limits) { return EncodedLimits { .limits = limits }; });
    }

    std::expected<EncodedLimits, DecodeError> ModuleDecoder::readLimits3(LimitsOf of)
    {
        // The flags are a byte, a row of LimitsFlagTable whose proposals are on, and that a table
        // admits only without a page size; the bounds are u64s whatever the address type, which the
        // validator bounds (decode.ml's `limits u64`).
        auto const flagsOffset = _reader.offset();
        auto const flags = readByte();
        if (!flags)
            return std::unexpected { flags.error() };
        auto admitted = LimitsFlagTable | std::views::filter([this, of](LimitsFlagRow const& row) {
                            return _features.includes(row.required)
                                   && (of == LimitsOf::Memory || row.pageSize == PageSizeField::Default);
                        });
        auto const row = std::ranges::find(admitted, *flags, &LimitsFlagRow::flags);
        if (row == admitted.end())
            return std::unexpected { DecodeError { DecodeErrorCode::MalformedLimitsFlags, flagsOffset } };
        auto const limits = readBounds(*row, [this] { return _reader.readU64(); });
        if (!limits)
            return std::unexpected { limits.error() };
        if (row->pageSize == PageSizeField::Default)
            return EncodedLimits { .limits = *limits };
        return readPageSizeLog2().transform([&](uint8_t pageSizeLog2) {
            return EncodedLimits { .limits = *limits, .pageSizeLog2 = pageSizeLog2 };
        });
    }

    std::expected<uint8_t, DecodeError> ModuleDecoder::readPageSizeLog2()
    {
        auto const offset = _reader.offset();
        auto const exponent = _reader.readU32();
        if (!exponent)
            return std::unexpected { exponent.error() };
        if (*exponent >= PageSizeLog2Limit)
            return std::unexpected { DecodeError { DecodeErrorCode::InvalidCustomPageSize, offset } };
        return static_cast<uint8_t>(*exponent);
    }

    template <typename ReadBound>
    std::expected<Limits, DecodeError> ModuleDecoder::readBounds(LimitsFlagRow const& row,
                                                                 ReadBound readBound)
    {
        auto const read = [&] {
            return readBound().transform_error([](DecodeError error) {
                if (error.code == DecodeErrorCode::IntegerTooLarge)
                    error.code = DecodeErrorCode::IntegerTooLargeForLimits;
                return error;
            });
        };
        auto const min = read();
        if (!min)
            return std::unexpected { min.error() };
        auto limits = Limits { .min = *min, .max = std::nullopt, .addressType = row.addressType };
        if (row.bound == LimitsBound::MinimumAndMaximum)
        {
            auto const max = read();
            if (!max)
                return std::unexpected { max.error() };
            limits.max = *max;
        }
        return limits;
    }

    std::expected<TableType, DecodeError> ModuleDecoder::readTableType()
    {
        auto const element = readRefType();
        if (!element)
            return std::unexpected { element.error() };
        return readLimits(LimitsOf::Table).transform([&](EncodedLimits const& encoded) {
            return TableType { .element = *element, .limits = encoded.limits };
        });
    }

    std::expected<MemoryType, DecodeError> ModuleDecoder::readMemoryType()
    {
        return readLimits(LimitsOf::Memory).transform([](EncodedLimits const& encoded) {
            return MemoryType { .limits = encoded.limits, .pageSizeLog2 = encoded.pageSizeLog2 };
        });
    }

    std::expected<GlobalType, DecodeError> ModuleDecoder::readGlobalType()
    {
        auto const type = readValueType();
        if (!type)
            return std::unexpected { type.error() };
        auto const mutabilityOffset = _reader.offset();
        auto const mutability = readByte();
        if (!mutability)
            return std::unexpected { mutability.error() };
        if (*mutability > 0x01)
            return std::unexpected { DecodeError { DecodeErrorCode::MalformedMutability, mutabilityOffset } };
        return GlobalType { .type = *type,
                            .mutability = *mutability == 0x01 ? Mutability::Var : Mutability::Const };
    }

    std::expected<ExternKind, DecodeError> ModuleDecoder::readExternKind(DecodeErrorCode malformed)
    {
        auto const kindOffset = _reader.offset();
        return readByte().and_then([&](uint8_t byte) -> std::expected<ExternKind, DecodeError> {
            if (std::size_t { byte } >= ExternKinds.size())
                return std::unexpected { DecodeError { malformed, kindOffset } };
            return ExternKinds[byte];
        });
    }

    std::expected<std::string, DecodeError> ModuleDecoder::readName()
    {
        return _reader.readName().transform([](std::string_view name) { return std::string { name }; });
    }

    std::expected<ConstExpr, DecodeError> ModuleDecoder::readConstExpr()
    {
        auto expression = ConstExpr {};
        _constBrTableLabels.clear();
        _constSelectTypes.clear();
        return readExpression(CodeSink { .body = expression,
                                         .brTableLabels = _constBrTableLabels,
                                         .selectTypes = _constSelectTypes })
            .transform([&] { return std::move(expression); });
    }

    std::optional<uint8_t> ModuleDecoder::peekByte() const
    {
        auto probe = _reader;
        auto const byte = probe.readByte();
        if (!byte)
            return std::nullopt;
        return std::to_integer<uint8_t>(*byte);
    }

    std::expected<void, DecodeError> ModuleDecoder::expectZeroByte()
    {
        // A zero byte, not a zero LEB128: the reserved memory index before multi-memory.
        auto const byteOffset = _reader.offset();
        return readByte().and_then([byteOffset](uint8_t byte) -> std::expected<void, DecodeError> {
            if (byte != 0x00)
                return std::unexpected { DecodeError { DecodeErrorCode::ZeroByteExpected, byteOffset } };
            return {};
        });
    }

    std::expected<void, DecodeError> ModuleDecoder::readIndex(uint32_t& index)
    {
        return _reader.readU32().transform([&index](uint32_t value) { index = value; });
    }

    std::expected<void, DecodeError> ModuleDecoder::readMemoryIndex(uint32_t& index)
    {
        if (_features.has(MemoryIndexFeature))
            return readIndex(index);
        index = 0;
        return expectZeroByte();
    }

    std::expected<void, DecodeError> ModuleDecoder::countImport(uint32_t& count,
                                                                uint32_t limit,
                                                                std::size_t offset)
    {
        if (count >= limit)
            return std::unexpected { DecodeError { DecodeErrorCode::ImplementationLimitExceeded, offset } };
        ++count;
        return {};
    }

    std::expected<void, DecodeError> ModuleDecoder::readExpression(CodeSink sink)
    {
        // Reads instructions up to the end that closes the expression, as the interpreter's
        // instr_block and end_ do, but with an explicit stack of open blocks instead of recursion,
        // so that nesting depth costs heap, never native stack. A sequence stops at else or end;
        // only an if without an else yet continues at else, every other sequence expects end there.
        auto const elseCode = opcodeInfo(Opcode::Else).code;
        auto const endCode = opcodeInfo(Opcode::End).code;
        auto open = std::vector<OpenBlock> {};
        while (true)
        {
            auto const index = static_cast<uint32_t>(sink.body.size());
            auto const next = peekByte();
            if (next == elseCode && !open.empty() && sink.body[open.back().opener].opcode == Opcode::If
                && !open.back().elseIndex)
            {
                std::ignore = readByte();
                open.back().elseIndex = index;
                sink.body.push_back(Instruction { .opcode = Opcode::Else });
                continue;
            }
            if (!next || next == elseCode || next == endCode)
            {
                auto const endOffset = _reader.offset();
                auto const byte = readByte();
                if (!byte)
                    return std::unexpected { byte.error() };
                if (*byte != endCode)
                    return std::unexpected { DecodeError { DecodeErrorCode::EndOpcodeExpected, endOffset } };
                sink.body.push_back(Instruction { .opcode = Opcode::End });
                if (open.empty())
                    return {};
                auto const block = open.back();
                open.pop_back();
                auto& opener = sink.body[block.opener];
                opener.endIndex = index;
                if (opener.opcode == Opcode::If)
                    opener.elseIndex = block.elseIndex.value_or(index);
                if (block.elseIndex)
                    sink.body[*block.elseIndex].endIndex = index;
                continue;
            }

            auto const instruction = readInstruction(*next, sink);
            if (!instruction)
                return std::unexpected { instruction.error() };
            if (opcodeInfo(instruction->opcode).immediate == ImmediateKind::BlockType)
                open.push_back(OpenBlock { .opener = index, .elseIndex = std::nullopt });
            sink.body.push_back(*instruction);
        }
    }

    std::expected<Instruction, DecodeError> ModuleDecoder::readInstruction(uint8_t byte, CodeSink sink)
    {
        auto const opcodeOffset = _reader.offset();
        std::ignore = readByte(); // byte, peeked
        auto prefix = NoPrefix;
        auto code = uint32_t { byte };
        if (OpcodeByteRoles[byte] == OpcodeByteRole::Prefix)
        {
            auto const subOpcode = _reader.readU32();
            if (!subOpcode)
                return std::unexpected { subOpcode.error() };
            prefix = byte;
            code = *subOpcode;
        }
        auto const opcode = lookupOpcode(prefix, code);
        // An instruction of a disabled proposal is as unknown as one of no proposal at all.
        if (!opcode || !enabled(opcodeInfo(*opcode).proposal))
            return std::unexpected { DecodeError {
                DecodeErrorCode::IllegalOpcode, opcodeOffset, illegalOpcodeSubject(prefix, code) } };

        auto instruction = Instruction { .opcode = *opcode };
        return readImmediates(instruction, sink).transform([&] { return instruction; });
    }

    std::expected<void, DecodeError> ModuleDecoder::readImmediates(Instruction& instruction, CodeSink sink)
    {
        // One case per immediate shape, never per opcode: the opcode table names the shape.
        switch (opcodeInfo(instruction.opcode).immediate)
        {
            case ImmediateKind::None: return {};
            case ImmediateKind::BlockType: return readBlockType(instruction);
            case ImmediateKind::LabelIndex:
            case ImmediateKind::FunctionIndex:
            case ImmediateKind::LocalIndex:
            case ImmediateKind::GlobalIndex:
            case ImmediateKind::TableIndex:
            case ImmediateKind::ElemIndex:
            case ImmediateKind::DataIndex: return readIndex(instruction.a);
            case ImmediateKind::CallIndirect:
                // Before reference types the table index is the reserved zero byte, as memory
                // indices are before multi-memory; an overlong zero is malformed there.
                if (!_features.has(Feature::ReferenceTypes))
                    return readIndex(instruction.a).and_then([this] { return expectZeroByte(); });
                return readIndex(instruction.a).and_then([&] { return readIndex(instruction.b); });
            case ImmediateKind::TableCopy:
            case ImmediateKind::TableInit:
                return readIndex(instruction.a).and_then([&] { return readIndex(instruction.b); });
            case ImmediateKind::LabelTable: {
                auto const offset = sink.brTableLabels.size();
                return readVector(_limits.maxBrTableSize,
                                  sink.brTableLabels,
                                  [this](uint32_t) { return _reader.readU32(); })
                    .and_then([this] { return _reader.readU32(); })
                    .transform([&](uint32_t defaultLabel) {
                        sink.brTableLabels.push_back(defaultLabel);
                        instruction.a = static_cast<uint32_t>(offset);
                        instruction.b = static_cast<uint32_t>(sink.brTableLabels.size() - offset);
                    });
            }
            case ImmediateKind::SelectTypes: {
                auto const offset = sink.selectTypes.size();
                return readVector(Unlimited, sink.selectTypes, [this](uint32_t) { return readValueType(); })
                    .transform([&] {
                        instruction.a = static_cast<uint32_t>(offset);
                        instruction.b = static_cast<uint32_t>(sink.selectTypes.size() - offset);
                    });
            }
            case ImmediateKind::MemArg: return readMemArg(instruction);
            // The shared memarg reader first, so every rule of a memory argument holds here too.
            case ImmediateKind::MemArgLane:
                return readMemArg(instruction).and_then([&] { return readLane(instruction); });
            case ImmediateKind::LaneIndex: return readLane(instruction);
            case ImmediateKind::V128Const:
            case ImmediateKind::Shuffle: return readV128Literal(instruction);
            case ImmediateKind::MemoryIndex: return readMemoryIndex(instruction.a);
            case ImmediateKind::MemoryCopy:
                return readMemoryIndex(instruction.a).and_then([&] {
                    return readMemoryIndex(instruction.b);
                });
            case ImmediateKind::MemoryInit:
                return readIndex(instruction.a).and_then([&] { return readMemoryIndex(instruction.b); });
            case ImmediateKind::I32Const:
                return _reader.readS32().transform(
                    [&](int32_t value) { instruction.c = static_cast<uint32_t>(value); });
            case ImmediateKind::I64Const:
                return _reader.readS64().transform(
                    [&](int64_t value) { instruction.c = static_cast<uint64_t>(value); });
            case ImmediateKind::F32Const:
                return _reader.readFixedU32().transform([&](uint32_t bits) { instruction.c = bits; });
            case ImmediateKind::F64Const:
                return _reader.readFixedU64().transform([&](uint64_t bits) { instruction.c = bits; });
            case ImmediateKind::HeapType: break; // read below the switch
        }
        return readRefType().transform([&](ValueType const& type) { instruction.type = type; });
    }

    std::expected<void, DecodeError> ModuleDecoder::readBlockType(Instruction& instruction)
    {
        // The interpreter tries three readings in turn, each from the same start: a non-negative
        // s33 type index (multi-value), the empty type 0x40, and a value type. Only the last one's
        // error is reported, so a negative s33 that names no value type is a malformed one.
        if (_features.has(Feature::MultiValue))
        {
            auto probe = _reader;
            if (auto const index = probe.readS33(); index && *index >= 0)
            {
                _reader = probe;
                instruction.blockKind = BlockType::Kind::TypeIndex;
                instruction.a = static_cast<uint32_t>(*index);
                return {};
            }
        }
        if (peekByte() == EmptyBlockType)
        {
            std::ignore = readByte();
            instruction.blockKind = BlockType::Kind::Empty;
            return {};
        }
        return readValueType().transform([&](ValueType const& type) {
            instruction.blockKind = BlockType::Kind::Value;
            instruction.type = type;
        });
    }

    std::expected<void, DecodeError> ModuleDecoder::readMemArg(Instruction& instruction)
    {
        auto const& grammar = _features.has(MemoryIndexFeature) ? MemArgGrammar3 : MemArgGrammar2;
        auto const flagsOffset = _reader.offset();
        auto const flags = _reader.readU32();
        if (!flags)
            return std::unexpected { flags.error() };
        if (*flags >= grammar.flagsLimit)
            return std::unexpected { DecodeError {
                DecodeErrorCode::MalformedMemopFlags,
                grammar.errorAt == FlagsErrorPosition::FirstByte ? flagsOffset : _reader.offset() - 1 } };
        instruction.a = *flags & ~grammar.memoryIndexBit;
        instruction.b = 0;
        if ((*flags & grammar.memoryIndexBit) != 0)
            if (auto const memory = readIndex(instruction.b); !memory)
                return memory;
        // A memory64 offset is a u64; the validator bounds it by the memory's address type.
        if (_features.has(Feature::Memory64))
            return _reader.readU64().transform([&](uint64_t offset) { instruction.c = offset; });
        return _reader.readU32().transform([&](uint32_t offset) { instruction.c = offset; });
    }

    std::expected<void, DecodeError> ModuleDecoder::readV128Literal(Instruction& instruction)
    {
        return _reader.readBytes(16).transform([&](std::span<std::byte const> bytes) {
            auto& literal = _module.v128Literals.emplace_back();
            std::ranges::copy(bytes, literal.begin());
            instruction.a = static_cast<uint32_t>(_module.v128Literals.size() - 1);
        });
    }

    std::expected<void, DecodeError> ModuleDecoder::readLane(Instruction& instruction)
    {
        return readByte().transform([&](uint8_t lane) { instruction.lane = lane; });
    }

    std::expected<Import, DecodeError> ModuleDecoder::readImport()
    {
        auto const importOffset = _reader.offset();
        auto module = readName();
        if (!module)
            return std::unexpected { module.error() };
        auto name = readName();
        if (!name)
            return std::unexpected { name.error() };
        auto const kind = readExternKind(DecodeErrorCode::MalformedImportKind);
        if (!kind)
            return std::unexpected { kind.error() };
        auto import = Import { .module = std::move(*module), .name = std::move(*name), .kind = *kind };
        auto described = std::expected<void, DecodeError> {};
        switch (*kind)
        {
            case ExternKind::Func:
                described =
                    _reader.readU32().transform([&](uint32_t typeIndex) { import.typeIndex = typeIndex; });
                break;
            case ExternKind::Table:
                described = countImport(_importedTables, _limits.maxTables, importOffset)
                                .and_then([this] { return readTableType(); })
                                .transform([&](TableType const& table) { import.table = table; });
                break;
            case ExternKind::Memory:
                described = countImport(_importedMemories, _limits.maxMemories, importOffset)
                                .and_then([this] { return readMemoryType(); })
                                .transform([&](MemoryType const& memory) { import.memory = memory; });
                break;
            case ExternKind::Global:
                described =
                    readGlobalType().transform([&](GlobalType const& global) { import.global = global; });
                break;
            case ExternKind::Tag: break; // uncovered(tag-import): readExternKind decodes no tags in P1
        }
        return described.transform([&] { return std::move(import); });
    }

    std::expected<Global, DecodeError> ModuleDecoder::readGlobal()
    {
        auto const type = readGlobalType();
        if (!type)
            return std::unexpected { type.error() };
        return readConstExpr().transform(
            [&](ConstExpr&& init) { return Global { .type = *type, .init = std::move(init) }; });
    }

    std::expected<Export, DecodeError> ModuleDecoder::readExport()
    {
        auto name = readName();
        if (!name)
            return std::unexpected { name.error() };
        auto const kind = readExternKind(DecodeErrorCode::MalformedExportKind);
        if (!kind)
            return std::unexpected { kind.error() };
        return _reader.readU32().transform([&](uint32_t index) {
            return Export { .name = std::move(*name), .kind = *kind, .index = index };
        });
    }

    std::expected<void, DecodeError> ModuleDecoder::typeSection()
    {
        return readVector(_limits.maxTypes, _module.types, [this](uint32_t) { return readFuncType(); });
    }

    std::expected<void, DecodeError> ModuleDecoder::importSection()
    {
        return readVector(_limits.maxImports, _module.imports, [this](uint32_t) { return readImport(); });
    }

    std::expected<void, DecodeError> ModuleDecoder::functionSection()
    {
        return readVector(
            _limits.maxFunctions, _module.functions, [this](uint32_t) { return _reader.readU32(); });
    }

    std::expected<void, DecodeError> ModuleDecoder::tableSection()
    {
        // countImport keeps the imports within the limit, so what is left of it cannot underflow.
        return readVector(_limits.maxTables - _importedTables, _module.tables, [this](uint32_t) {
            return readTableType();
        });
    }

    std::expected<void, DecodeError> ModuleDecoder::memorySection()
    {
        // Bounded by what the imports left of maxMemories; the validator allows one before multi-memory.
        return readVector(_limits.maxMemories - _importedMemories, _module.memories, [this](uint32_t) {
            return readMemoryType();
        });
    }

    std::expected<void, DecodeError> ModuleDecoder::globalSection()
    {
        return readVector(_limits.maxGlobals, _module.globals, [this](uint32_t) { return readGlobal(); });
    }

    std::expected<void, DecodeError> ModuleDecoder::exportSection()
    {
        return readVector(_limits.maxExports, _module.exports, [this](uint32_t) { return readExport(); });
    }

    std::expected<void, DecodeError> ModuleDecoder::startSection()
    {
        return _reader.readU32().transform([this](uint32_t index) { _module.start = index; });
    }

    std::expected<ElementSegment, DecodeError> ModuleDecoder::readElementSegment()
    {
        auto const flagsOffset = _reader.offset();
        auto const flags = _reader.readU32();
        if (!flags)
            return std::unexpected { flags.error() };
        if (*flags >= ElementForms.size() || !enabled(ElementForms[*flags].required))
            return std::unexpected { DecodeError { DecodeErrorCode::MalformedElementSegmentKind,
                                                   flagsOffset } };
        auto const& form = ElementForms[*flags];

        auto segment = ElementSegment {};
        segment.mode = form.mode;
        if (form.mode == SegmentMode::Active)
        {
            if (form.table == IndexEncoding::Explicit)
            {
                auto const table = _reader.readU32();
                if (!table)
                    return std::unexpected { table.error() };
                segment.table = *table;
            }
            auto offset = readConstExpr();
            if (!offset)
                return std::unexpected { offset.error() };
            segment.offset = std::move(*offset);
        }
        switch (form.type)
        {
            case ElementTypeEncoding::Implicit: break;
            case ElementTypeEncoding::ElemKind: {
                auto const kindOffset = _reader.offset();
                auto const kind = readByte();
                if (!kind)
                    return std::unexpected { kind.error() };
                if (*kind != 0x00)
                    return std::unexpected { DecodeError { DecodeErrorCode::MalformedElementKind,
                                                           kindOffset } };
                break;
            }
            case ElementTypeEncoding::RefType: {
                auto const type = readRefType();
                if (!type)
                    return std::unexpected { type.error() };
                segment.type = *type;
                break;
            }
        }

        auto items = std::expected<void, DecodeError> {};
        switch (form.items)
        {
            case ItemEncoding::Expressions:
                items = readVector(_limits.maxElementItems,
                                   segment.items.emplace<std::vector<ConstExpr>>(),
                                   [this](uint32_t) { return readConstExpr(); });
                break;
            case ItemEncoding::FunctionIndices:
                items = readVector(_limits.maxElementItems,
                                   segment.items.emplace<std::vector<uint32_t>>(),
                                   [this](uint32_t) { return _reader.readU32(); });
                break;
        }
        return items.transform([&] { return std::move(segment); });
    }

    std::expected<void, DecodeError> ModuleDecoder::elementSection()
    {
        return readVector(
            _limits.maxElementSegments, _module.elements, [this](uint32_t) { return readElementSegment(); });
    }

    std::expected<void, DecodeError> ModuleDecoder::dataCountSection()
    {
        return _reader.readU32().transform([this](uint32_t count) { _module.dataCount = count; });
    }

    std::expected<ModuleDecoder::Locals, DecodeError> ModuleDecoder::readLocals()
    {
        // Every run is read before the total is checked, as the interpreter does. The runs are
        // kept, never expanded (see localType).
        auto const localsOffset = _reader.offset();
        auto runs = std::vector<LocalRun> {};
        auto const read =
            readVector(Unlimited, runs, [this](uint32_t) -> std::expected<LocalRun, DecodeError> {
                auto const count = _reader.readU32();
                if (!count)
                    return std::unexpected { count.error() };
                return readValueType().transform(
                    [&](ValueType const& type) { return LocalRun { .count = *count, .type = type }; });
            });
        if (!read)
            return std::unexpected { read.error() };
        auto total = uint64_t { 0 };
        for (auto const& run: runs)
            total += run.count;
        if (total > _limits.maxLocals)
            return std::unexpected { DecodeError { DecodeErrorCode::TooManyLocals, localsOffset } };

        return Locals { .runs = std::move(runs), .count = static_cast<uint32_t>(total) };
    }

    std::expected<Function, DecodeError> ModuleDecoder::readFunction(uint32_t index)
    {
        auto const sizeOffset = _reader.offset();
        auto const size = _reader.readLength();
        if (!size)
            return std::unexpected { size.error() };
        if (*size > _limits.maxFunctionBodySize)
            return std::unexpected { DecodeError { DecodeErrorCode::ImplementationLimitExceeded,
                                                   sizeOffset } };
        auto const start = _reader.offset();
        auto const end = start + *size;

        auto locals = readLocals();
        if (!locals)
            return std::unexpected { locals.error() };
        auto function = Function {
            .typeIndex = index < _module.functions.size() ? _module.functions[index] : 0,
            .locals = std::move(locals->runs),
            .localCount = locals->count,
            .body = {},
            .bodyOffset = _reader.offset(),
            .brTableLabels = {},
            .selectTypes = {},
        };
        // The body is read from the module's stream, not from its declared bytes: one whose end
        // lies elsewhere reads on, or stops short, and then mismatches its size, as in the
        // interpreter.
        auto const body = readExpression(CodeSink { .body = function.body,
                                                    .brTableLabels = function.brTableLabels,
                                                    .selectTypes = function.selectTypes });
        if (!body)
            return std::unexpected { body.error() };
        if (_reader.offset() != end)
            return std::unexpected { DecodeError { DecodeErrorCode::SectionSizeMismatch, start } };
        return function;
    }

    std::expected<void, DecodeError> ModuleDecoder::codeSection()
    {
        return readVector(
            _limits.maxFunctions, _module.code, [this](uint32_t index) { return readFunction(index); });
    }

    std::expected<DataSegment, DecodeError> ModuleDecoder::readDataSegment()
    {
        auto const flagsOffset = _reader.offset();
        auto const flags = _reader.readU32();
        if (!flags)
            return std::unexpected { flags.error() };
        if (*flags >= DataForms.size() || !enabled(DataForms[*flags].required))
            return std::unexpected { DecodeError { DecodeErrorCode::MalformedDataSegmentKind, flagsOffset } };
        auto const& form = DataForms[*flags];

        auto segment = DataSegment {};
        segment.mode = form.mode;
        if (form.mode == SegmentMode::Active)
        {
            if (form.memory == IndexEncoding::Explicit)
            {
                auto const memory = _reader.readU32();
                if (!memory)
                    return std::unexpected { memory.error() };
                segment.memory = *memory;
            }
            auto offset = readConstExpr();
            if (!offset)
                return std::unexpected { offset.error() };
            segment.offset = std::move(*offset);
        }
        auto const size = _reader.readLength();
        if (!size)
            return std::unexpected { size.error() };
        auto const bytes = _reader.readBytes(*size);
        if (!bytes)
            return std::unexpected { bytes.error() };
        segment.bytes.assign(bytes->begin(), bytes->end());
        return segment;
    }

    std::expected<void, DecodeError> ModuleDecoder::dataSection()
    {
        return readVector(
            _limits.maxDataSegments, _module.data, [this](uint32_t) { return readDataSegment(); });
    }

} // namespace

std::expected<Module, DecodeError> decodeModule(std::span<std::byte const> bytes,
                                                FeatureSet const& features,
                                                DecodeLimits const& limits)
{
    // The one place a FeatureSet enters the engine; every later part takes the set decoding used.
    if (auto const closed = features.checkPrerequisites(); !closed)
        return std::unexpected { DecodeError {
            DecodeErrorCode::MissingFeaturePrerequisite, 0, prerequisiteSubject(closed.error()) } };
    return ModuleDecoder { bytes, features, limits }.decode();
}

} // namespace isoline::binary
