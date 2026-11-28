// SPDX-License-Identifier: Apache-2.0
#include "ModuleGenerator.hpp"

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

namespace isoline::fuzz
{

namespace
{

    using binary::ValueType;
    using Bytes = std::vector<uint8_t>;

    // ---------------------------------------------------------------------------------------------------------------
    // The entropy

    /// The fuzzer's bytes, read front to back; zero once they run out, which every choice reads as its first
    /// case.
    class Entropy
    {
      public:
        explicit Entropy(std::span<std::byte const> bytes): _bytes { bytes } {}

        /// Whether every byte has been read.
        [[nodiscard]] bool isExhausted() const noexcept { return _offset >= _bytes.size(); }

        /// The next byte, or zero.
        [[nodiscard]] uint8_t byte() noexcept
        {
            if (isExhausted())
                return 0;
            return std::to_integer<uint8_t>(_bytes[_offset++]);
        }

        /// A number below @p bound, from as many bytes as it needs; zero for a bound of 0 or 1.
        [[nodiscard]] uint32_t below(uint64_t bound) noexcept
        {
            if (bound <= 1)
                return 0;
            auto value = uint64_t { byte() };
            auto reach = uint64_t { 0x100 };
            while (reach < bound && reach < (uint64_t { 1 } << 32))
            {
                value = (value << 8U) | byte();
                reach <<= 8U;
            }
            return static_cast<uint32_t>(value % bound);
        }

        /// Whether a one-in-@p chances choice came up.
        [[nodiscard]] bool isOneIn(uint32_t chances) noexcept { return below(chances) == 0; }

        /// 64 bits.
        [[nodiscard]] uint64_t bits() noexcept
        {
            auto value = uint64_t { 0 };
            for ([[maybe_unused]] auto const index: std::views::iota(0, 8))
                value = (value << 8U) | byte();
            return value;
        }

        /// One element of @p choices.
        template <typename T, std::size_t Extent>
        [[nodiscard]] T const& pick(std::span<T const, Extent> choices) noexcept
        {
            return choices[below(choices.size())];
        }

      private:
        std::span<std::byte const> _bytes;
        std::size_t _offset = 0;
    };

    // ---------------------------------------------------------------------------------------------------------------
    // The module's layout: what every generated module defines, at fixed indices

    /// The value types the generator writes: every type a local, a global, a block or a function signature
    /// has.
    constexpr auto ValueTypes = std::to_array<ValueType>({
        binary::I32,
        binary::I64,
        binary::F32,
        binary::F64,
        binary::V128,
        binary::FuncRef,
        binary::ExternRef,
        binary::ExnRef,
    });

    /// The types an untyped select takes: numbers and vectors.
    constexpr auto SelectableTypes = std::to_array<ValueType>({
        binary::I32,
        binary::I64,
        binary::F32,
        binary::F64,
        binary::V128,
    });

    /// The reference types: what ref.null, ref.is_null and the br_on_* snippets take.
    constexpr auto ReferenceTypes =
        std::to_array<ValueType>({ binary::FuncRef, binary::ExternRef, binary::ExnRef });

    /// The fixed function types, at these indices of the type section; the random ones follow them.
    constexpr auto EmptyType = uint32_t { 0 };      ///< [] -> []: tag 0, and the program shapes' entries.
    constexpr auto TagPayloadType = uint32_t { 1 }; ///< [i32] -> []: tag 1, and the deep unwind's worker.
    constexpr auto HostType = uint32_t { 2 };       ///< [i32 i64] -> [i32]: the import `host`.
    constexpr auto ReenterType = uint32_t { 3 };    ///< [i32] -> [i32]: the import `reenter`.
    constexpr auto CaughtRefType = uint32_t { 4 };  ///< [] -> [i32 exnref]: the label of catch_ref of tag 1.
    constexpr auto FirstRandomType = uint32_t { 5 };

    /// The most random function types, and their most parameters and results.
    constexpr auto MaxRandomTypes = uint32_t { 4 };
    constexpr auto MaxParams = uint32_t { 3 };
    constexpr auto MaxResults = uint32_t { 2 };

    /// The function types of the fixed indices, in order.
    [[nodiscard]] std::vector<binary::FuncType> fixedTypes()
    {
        using binary::ExnRef;
        using binary::I32;
        using binary::I64;
        return {
            binary::FuncType {},
            binary::FuncType { .params = { I32 }, .results = {} },
            binary::FuncType { .params = { I32, I64 }, .results = { I32 } },
            binary::FuncType { .params = { I32 }, .results = { I32 } },
            binary::FuncType { .params = {}, .results = { I32, ExnRef } },
        };
    }

    /// The function types of the host imports, by import index.
    constexpr auto HostImportTypes = std::to_array<uint32_t>({ HostType, ReenterType });
    static_assert(HostImportTypes.size() == HostImports.size(), "every host import has its type");

    /// The tables: table 0 holds every function, table 1 is of host references.
    constexpr auto FunctionTable = uint32_t { 0 };
    /// Each table's element type, by table index.
    constexpr auto TableElements = std::to_array<ValueType>({ binary::FuncRef, binary::ExternRef });
    constexpr auto ExternTableSize = uint64_t { 4 };
    constexpr auto TableGrowth = uint64_t { 8 };

    /// One memory of the module: its address type, its pages and their size.
    struct MemoryRow
    {
        binary::AddressType addressType; ///< The type its addresses take.
        uint64_t minPages;               ///< Its initial size.
        uint64_t maxPages;               ///< The most it grows to.
        uint8_t pageSizeLog2;            ///< Its page size, log2 (custom page sizes).

        /// Its initial size in bytes.
        [[nodiscard]] constexpr uint64_t minBytes() const noexcept { return minPages << pageSizeLog2; }
    };

    /// The memories, by memory index: memory 0 is the cached one, memory 1 a 64-bit one, memory 2 one of
    /// 1-byte pages.
    constexpr auto Memories = std::to_array<MemoryRow>({
        { .addressType = binary::AddressType::I32,
          .minPages = 1,
          .maxPages = 4,
          .pageSizeLog2 = binary::DefaultPageSizeLog2 },
        { .addressType = binary::AddressType::I64,
          .minPages = 1,
          .maxPages = 2,
          .pageSizeLog2 = binary::DefaultPageSizeLog2 },
        { .addressType = binary::AddressType::I32, .minPages = 4'096, .maxPages = 65'536, .pageSizeLog2 = 0 },
    });

    /// The tags, by tag index: their function types.
    constexpr auto Tags = std::to_array<uint32_t>({ EmptyType, TagPayloadType });

    /// The element segments: 0 is active in table 0, 1 is passive; both list every function.
    constexpr auto ElementSegments = uint32_t { 2 };

    /// The data segments: 0 is active in memory 0, 1 is passive; each of at most MaxDataBytes.
    constexpr auto DataSegments = uint32_t { 2 };
    constexpr auto MaxDataBytes = uint32_t { 32 };

    /// The loop counters a random function declares: a loop is opened only while one is free.
    constexpr auto MaxLoopCounters = uint32_t { 32 };

    /// The most values the generator keeps on one block's stack before it drops one.
    constexpr auto MaxOperandHeight = std::size_t { 8 };

    /// The most labels of a br_table besides its default.
    constexpr auto MaxBranchTableLabels = uint32_t { 8 };

    /// The most catch clauses of a try_table.
    constexpr auto MaxCatchClauses = uint32_t { 3 };

    /// The longest encoding of one instruction the generator writes: a br_table of MaxBranchTableLabels
    /// labels and its default, each a u32 of at most 5 bytes, after its opcode and count.
    constexpr auto MaxInstructionBytes = uint64_t { 2 + ((MaxBranchTableLabels + 1) * 5) };
    static_assert(MaxInstructionBytes >= 2 + 16,
                  "v128.const and i8x16.shuffle: a prefixed opcode and 16 bytes");

    /// What closing an empty function body writes at most: its results, made.
    constexpr auto FunctionCloseInstructions = uint64_t { (2 * MaxResults) + 1 };

    /// The edge values of each number type, which a constant is one of a quarter of the time.
    constexpr auto I32Edges = std::to_array<uint64_t>({
        0,           1,           2,           7,           8,           15,     16,       31,
        32,          33,          63,          64,          0x7F,        0x80,   0xFF,     0x100,
        0x7FFF,      0x8000,      0xFFF0,      0xFFF8,      0xFFFC,      0xFFFF, 0x1'0000, 0x1'0001,
        0x7FFF'FFFF, 0x8000'0000, 0x8000'0001, 0xFFFF'FFFE, 0xFFFF'FFFF,
    });
    constexpr auto I64Edges = std::to_array<uint64_t>({
        0,
        1,
        63,
        64,
        0xFFFF,
        0x1'0000,
        0x7FFF'FFFF,
        0x8000'0000,
        0xFFFF'FFFF,
        0x1'0000'0000,
        0x7FFF'FFFF'FFFF'FFFF,
        0x8000'0000'0000'0000,
        0x8000'0000'0000'0001,
        0xFFFF'FFFF'FFFF'FFFE,
        0xFFFF'FFFF'FFFF'FFFF,
    });
    /// Float edges as their bits: zeros, ones, infinities, quiet and signalling NaNs of either sign, the
    /// smallest subnormal, and the bounds of the conversions to integers.
    constexpr auto F32Edges = std::to_array<uint64_t>({
        0x0000'0000,
        0x8000'0000,
        0x3F80'0000,
        0xBF80'0000,
        0x7F80'0000,
        0xFF80'0000,
        0x7FC0'0000,
        0xFFC0'0000,
        0x7FA0'0000,
        0xFFA0'0001,
        0x0000'0001,
        0x4F00'0000,
        0xCF00'0000,
        0x5F00'0000,
        0xDF00'0000,
        0x7F7F'FFFF,
    });
    constexpr auto F64Edges = std::to_array<uint64_t>({
        0x0000'0000'0000'0000,
        0x8000'0000'0000'0000,
        0x3FF0'0000'0000'0000,
        0xBFF0'0000'0000'0000,
        0x7FF0'0000'0000'0000,
        0xFFF0'0000'0000'0000,
        0x7FF8'0000'0000'0000,
        0xFFF8'0000'0000'0000,
        0x7FF4'0000'0000'0000,
        0xFFF4'0000'0000'0001,
        0x0000'0000'0000'0001,
        0x41E0'0000'0000'0000,
        0xC1E0'0000'0000'0000,
        0x43E0'0000'0000'0000,
        0xC3E0'0000'0000'0000,
        0x7FEF'FFFF'FFFF'FFFF,
    });

    /// The lanes of a SIMD lane instruction, by the shape its name starts with.
    struct LaneShapeRow
    {
        std::string_view prefix; ///< The shape: "i8x16.", ….
        uint32_t lanes;          ///< Its lanes.
    };
    constexpr auto LaneShapes = std::to_array<LaneShapeRow>({
        { .prefix = "i8x16.", .lanes = 16 },
        { .prefix = "i16x8.", .lanes = 8 },
        { .prefix = "i32x4.", .lanes = 4 },
        { .prefix = "i64x2.", .lanes = 2 },
        { .prefix = "f32x4.", .lanes = 4 },
        { .prefix = "f64x2.", .lanes = 2 },
    });

    /// The lanes @p row's lane index chooses from.
    [[nodiscard]] uint32_t lanesOf(binary::OpcodeInfo const& row)
    {
        if (auto const* access = binary::memoryAccessOf(row.opcode))
            return 16 / access->width;
        auto const shape = std::ranges::find_if(
            LaneShapes, [&](LaneShapeRow const& shape) { return row.text.starts_with(shape.prefix); });
        Require(shape != LaneShapes.end());
        return shape->lanes;
    }

    /// What a generated module's functions are: their types, by function index, imports first.
    struct Layout
    {
        std::vector<binary::FuncType> types; ///< The type section.
        std::vector<uint32_t> functionTypes; ///< Each function's type index, by function index.

        [[nodiscard]] binary::FuncType const& typeOf(uint32_t function) const
        {
            return types[functionTypes[function]];
        }
        [[nodiscard]] uint32_t functionCount() const { return static_cast<uint32_t>(functionTypes.size()); }
    };

    /// The number types: what Wasmtime's command line passes as arguments.
    constexpr auto NumberTypes =
        std::to_array<ValueType>({ binary::I32, binary::I64, binary::F32, binary::F64 });

    /// What a GeneratorMode makes: the host imports, and the types a random function takes and returns.
    struct ModeRow
    {
        GeneratorMode mode;
        std::span<HostImport const> imports;
        std::span<ValueType const> params;
        std::span<ValueType const> results;
    };

    /// Every GeneratorMode, in enumerator order.
    constexpr auto ModeRows = std::to_array<ModeRow>({
        { .mode = GeneratorMode::Harness,
          .imports = HostImports,
          .params = ValueTypes,
          .results = ValueTypes },
        { .mode = GeneratorMode::Oracle, .imports = {}, .params = NumberTypes, .results = SelectableTypes },
    });

    /// A random function type of @p mode's.
    [[nodiscard]] binary::FuncType randomType(Entropy& entropy, ModeRow const& mode)
    {
        auto type = binary::FuncType {};
        for ([[maybe_unused]] auto const index:
             std::views::iota(uint32_t { 0 }, entropy.below(MaxParams + 1)))
            type.params.push_back(entropy.pick(mode.params));
        for ([[maybe_unused]] auto const index:
             std::views::iota(uint32_t { 0 }, entropy.below(MaxResults + 1)))
            type.results.push_back(entropy.pick(mode.results));
        return type;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Instruction encodings

    /// The bytes of @p opcode: its prefix, if any, then its code.
    [[nodiscard]] Bytes opcodeBytes(binary::Opcode opcode)
    {
        auto const& row = binary::opcodeInfo(opcode);
        if (row.prefix == binary::NoPrefix)
            return { static_cast<uint8_t>(row.code) };
        auto bytes = Bytes { row.prefix };
        std::ranges::copy(binary::testing::encodeU32(row.code), std::back_inserter(bytes));
        return bytes;
    }

    /// The little-endian bytes of the low @p count bytes of @p bits.
    [[nodiscard]] Bytes littleEndian(uint64_t bits, std::size_t count)
    {
        auto bytes = Bytes {};
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
            bytes.push_back(static_cast<uint8_t>((bits >> (8 * index)) & 0xFFU));
        return bytes;
    }

    /// The bytes of a u32 immediate.
    [[nodiscard]] Bytes u32(uint32_t value)
    {
        return binary::testing::encodeU32(value);
    }

    /// The value type a number or vector kind is.
    [[nodiscard]] ValueType valueTypeOf(binary::ValueKind kind)
    {
        return ValueType { .kind = kind };
    }

    /// The value types of @p kinds.
    [[nodiscard]] std::vector<ValueType> valueTypesOf(std::span<binary::ValueKind const> kinds)
    {
        auto types = std::vector<ValueType> {};
        for (auto const kind: kinds)
            types.push_back(valueTypeOf(kind));
        return types;
    }

    /// The address type of memory @p memory, as a value type.
    [[nodiscard]] ValueType addressOf(uint32_t memory)
    {
        return binary::addressValueType(Memories[memory].addressType);
    }

    /// The element type of table @p table.
    [[nodiscard]] ValueType elementOf(uint32_t table)
    {
        return TableElements[table];
    }

    /// @p first, then @p rest.
    [[nodiscard]] std::vector<ValueType> concatenated(std::span<ValueType const> first,
                                                      std::initializer_list<ValueType> rest)
    {
        auto types = std::vector<ValueType> { first.begin(), first.end() };
        types.insert(types.end(), rest);
        return types;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The body writer

    /// What a block is.
    enum class FrameKind : uint8_t
    {
        Function,
        Block,
        Loop,
        If,
        TryTable,
    };

    /// Whether the code that follows in a block runs.
    enum class Reachability : uint8_t
    {
        Reachable,
        Unreachable, ///< After an unconditional transfer: the block is closed next.
    };

    /// Which branch of an if the writer is in.
    enum class IfBranch : uint8_t
    {
        Then,
        Else,
    };

    /// One open block, and the types of its operand stack.
    struct Frame
    {
        FrameKind kind = FrameKind::Block;
        std::vector<ValueType> params;
        std::vector<ValueType> results;
        std::vector<ValueType> stack; ///< Its operands, bottom first.
        Reachability reach = Reachability::Reachable;
        IfBranch branch = IfBranch::Then;
        uint32_t counter = 0; ///< A loop's counter local.
    };

    /// What an operand is used as: a value, or an address, a count or a table index, which a constant made
    /// for it is biased toward, so that most accesses and bulk operations do not trap at once.
    enum class Operand : uint8_t
    {
        Value,   ///< Any value of its type.
        Address, ///< An address: mostly within memory's first page, now and then at its end.
        Count,   ///< A length of a bulk operation: mostly short.
        Index,   ///< An index into a table: mostly within it.
    };

    /// The most a Count operand's constant is, and an Index operand's.
    constexpr auto MaxCount = uint32_t { 64 };
    constexpr auto MaxIndex = uint32_t { 16 };

    /// Whether an emitter wrote its instruction, or found it does not fit where the writer is.
    enum class Emission : uint8_t
    {
        Skipped,
        Written,
    };

    /// A block type: its encoding, and its params and results.
    struct BlockSignature
    {
        Bytes encoding;
        std::vector<ValueType> params;
        std::vector<ValueType> results;
    };

    /// Whether a block type may take params: a loop's may not, so that its counted back edge takes none.
    enum class BlockParams : uint8_t
    {
        None,
        Allowed,
    };

    /// One kind of catch clause.
    enum class CatchKind : uint8_t
    {
        Catch = 0x00,
        CatchRef = 0x01,
        CatchAll = 0x02,
        CatchAllRef = 0x03,
    };

    /// A catch clause: its kind and, for catch and catch_ref, its tag.
    struct CatchClause
    {
        CatchKind kind;
        uint32_t tag;
    };

    /// The handler of a try_table: the block around it whose label its clauses branch to, and the clauses
    /// that branch there with exactly its label's types.
    struct HandlerShape
    {
        std::vector<ValueType> label;
        std::vector<CatchClause> clauses;
    };

    /// Every handler shape: by the values a clause hands its label.
    [[nodiscard]] std::span<HandlerShape const> handlerShapes()
    {
        static auto const shapes = std::vector<HandlerShape> {
            { .label = {},
              .clauses = { { .kind = CatchKind::Catch, .tag = 0 },
                           { .kind = CatchKind::CatchAll, .tag = 0 } } },
            { .label = { binary::ExnRef },
              .clauses = { { .kind = CatchKind::CatchRef, .tag = 0 },
                           { .kind = CatchKind::CatchAllRef, .tag = 0 } } },
            { .label = { binary::I32 }, .clauses = { { .kind = CatchKind::Catch, .tag = 1 } } },
            { .label = { binary::I32, binary::ExnRef },
              .clauses = { { .kind = CatchKind::CatchRef, .tag = 1 } } },
        };
        return shapes;
    }

    /// The instruction groups a step picks from first, and how often.
    enum class RowGroup : uint8_t
    {
        Numeric,
        Vector,
        Memory,
        Variable,
        Control,
        Calls,
        Exceptions,
        Tables,
        References,
    };

    /// How often a step picks each group, by RowGroup.
    constexpr auto GroupWeights = std::to_array<uint32_t>({ 30, 18, 12, 14, 14, 5, 3, 2, 2 });
    constexpr auto GroupCount = GroupWeights.size();

    /// Writes one function's body.
    class FunctionWriter
    {
      public:
        using Emitter = Emission (FunctionWriter::*)(binary::OpcodeInfo const&);

        FunctionWriter(Entropy& entropy,
                       Layout const& layout,
                       GeneratorLimits const& limits,
                       uint32_t typeIndex):
            _entropy { entropy },
            _layout { layout },
            _limits { limits },
            _type { layout.types[typeIndex] },
            _counters { std::min(limits.maxNesting, MaxLoopCounters) },
            _locals { concatenated(_type.params, {}) },
            _randomLocals { static_cast<uint32_t>(_type.params.size() + ValueTypes.size()) }
        {
            _locals.insert(_locals.end(), ValueTypes.begin(), ValueTypes.end());
            _frames.push_back(Frame { .kind = FrameKind::Function,
                                      .params = {},
                                      .results = _type.results,
                                      .stack = {},
                                      .reach = Reachability::Reachable,
                                      .branch = IfBranch::Then,
                                      .counter = 0 });
        }

        /// The declared locals: one of each value type, then the loop counters.
        [[nodiscard]] std::vector<ValueType> declaredLocals() const
        {
            auto locals = std::vector<ValueType> { ValueTypes.begin(), ValueTypes.end() };
            locals.insert(locals.end(), _counters, binary::I32);
            return locals;
        }

        /// Writes the body: steps while the entropy lasts and the budget holds, then closes every block.
        /// @return The body, without its final end.
        [[nodiscard]] Bytes write();

        // The emitters, one per kind of row (rowCandidates()).
        Emission emitFixed(binary::OpcodeInfo const& row);
        Emission emitAccess(binary::OpcodeInfo const& row);
        Emission emitUnreachable(binary::OpcodeInfo const& row);
        Emission emitBlock(binary::OpcodeInfo const& row);
        Emission emitLoop(binary::OpcodeInfo const& row);
        Emission emitIf(binary::OpcodeInfo const& row);
        Emission emitElse(binary::OpcodeInfo const& row);
        Emission emitEnd(binary::OpcodeInfo const& row);
        Emission emitTryTable(binary::OpcodeInfo const& row);
        Emission emitThrow(binary::OpcodeInfo const& row);
        Emission emitThrowRef(binary::OpcodeInfo const& row);
        Emission emitBr(binary::OpcodeInfo const& row);
        Emission emitBrIf(binary::OpcodeInfo const& row);
        Emission emitBrTable(binary::OpcodeInfo const& row);
        Emission emitReturn(binary::OpcodeInfo const& row);
        Emission emitCall(binary::OpcodeInfo const& row);
        Emission emitCallIndirect(binary::OpcodeInfo const& row);
        Emission emitCallRef(binary::OpcodeInfo const& row);
        Emission emitDrop(binary::OpcodeInfo const& row);
        Emission emitSelect(binary::OpcodeInfo const& row);
        Emission emitLocal(binary::OpcodeInfo const& row);
        Emission emitGlobal(binary::OpcodeInfo const& row);
        Emission emitTable(binary::OpcodeInfo const& row);
        Emission emitMemory(binary::OpcodeInfo const& row);
        Emission emitReference(binary::OpcodeInfo const& row);
        Emission emitBranchOnNull(binary::OpcodeInfo const& row);

      private:
        /// What a step may roll back to: everything but the entropy, which stays consumed.
        struct Checkpoint
        {
            std::size_t bodyBytes;
            uint64_t written;
            std::vector<Frame> frames;
            uint32_t loops;
        };

        void step();
        void close();
        void fixStack(std::span<ValueType const> results);
        void prepare(std::span<ValueType const> types);
        void prepare(std::span<ValueType const> types, std::span<Operand const> roles, uint32_t memory = 0);
        void produce(ValueType type);
        void produce(ValueType type, Operand role, uint32_t memory);
        void instruction(std::initializer_list<Bytes> parts);
        [[nodiscard]] uint64_t reserve() const noexcept;
        [[nodiscard]] Frame& top() noexcept { return _frames.back(); }
        [[nodiscard]] std::size_t depth() const noexcept { return _frames.size() - 1; }
        [[nodiscard]] bool isTopLevel() const noexcept { return _frames.size() == 1; }
        [[nodiscard]] bool isTerminatorSkipped() noexcept;
        void pushAll(std::span<ValueType const> types);
        void popAll(std::size_t count);
        void markUnreachable() { top().reach = Reachability::Unreachable; }
        [[nodiscard]] BlockSignature blockSignature(BlockParams params);
        [[nodiscard]] std::size_t branchTarget();
        [[nodiscard]] std::optional<std::size_t> frameLabelled(std::span<ValueType const> types);
        [[nodiscard]] uint32_t labelIndexOf(std::size_t frame) const noexcept;
        [[nodiscard]] std::vector<ValueType> labelTypes(std::size_t frame) const;
        [[nodiscard]] uint32_t localOf(ValueType type);
        [[nodiscard]] uint32_t globalOf(ValueType type) const;
        [[nodiscard]] ValueType topOr(std::span<ValueType const> choices);
        [[nodiscard]] uint64_t number(std::span<uint64_t const> edges);
        [[nodiscard]] Bytes numberImmediate(binary::OpcodeInfo const& row);
        [[nodiscard]] Bytes vectorBytes();
        [[nodiscard]] Bytes constantOf(ValueType type);
        [[nodiscard]] std::optional<uint32_t> tailCallee();
        [[nodiscard]] std::optional<uint32_t> tailType();

        Entropy& _entropy;
        Layout const& _layout;
        GeneratorLimits _limits;
        binary::FuncType _type;
        uint32_t _counters;
        std::vector<ValueType> _locals;
        uint32_t _randomLocals = 0;
        Bytes _body;
        uint64_t _written = 0;
        std::vector<Frame> _frames;
        uint32_t _loops = 0;
    };

    /// One instruction a step may pick: its row and what writes it.
    struct Candidate
    {
        binary::OpcodeInfo const* row;
        FunctionWriter::Emitter emit;
    };

    /// A special row: an instruction SignatureTable does not type, its group, and its emitter.
    struct SpecialRow
    {
        binary::Opcode opcode;
        RowGroup group;
        FunctionWriter::Emitter emit;
    };

    /// Every special row that is no memory access: control, parametric, variable, table, memory, reference
    /// and exception instructions. A row of the cut found neither here nor in SignatureTable nor
    /// MemoryAccessTable stops the generator (rowCandidates()), so a new instruction lands with its row.
    [[nodiscard]] std::span<SpecialRow const> specialRows()
    {
        using enum binary::Opcode;
        using W = FunctionWriter;
        static auto const rows = std::to_array<SpecialRow>({
            { .opcode = Unreachable, .group = RowGroup::Control, .emit = &W::emitUnreachable },
            { .opcode = Block, .group = RowGroup::Control, .emit = &W::emitBlock },
            { .opcode = Loop, .group = RowGroup::Control, .emit = &W::emitLoop },
            { .opcode = If, .group = RowGroup::Control, .emit = &W::emitIf },
            { .opcode = Else, .group = RowGroup::Control, .emit = &W::emitElse },
            { .opcode = End, .group = RowGroup::Control, .emit = &W::emitEnd },
            { .opcode = Br, .group = RowGroup::Control, .emit = &W::emitBr },
            { .opcode = BrIf, .group = RowGroup::Control, .emit = &W::emitBrIf },
            { .opcode = BrTable, .group = RowGroup::Control, .emit = &W::emitBrTable },
            { .opcode = Return, .group = RowGroup::Control, .emit = &W::emitReturn },
            { .opcode = BrOnNull, .group = RowGroup::Control, .emit = &W::emitBranchOnNull },
            { .opcode = BrOnNonNull, .group = RowGroup::Control, .emit = &W::emitBranchOnNull },
            { .opcode = Call, .group = RowGroup::Calls, .emit = &W::emitCall },
            { .opcode = ReturnCall, .group = RowGroup::Calls, .emit = &W::emitCall },
            { .opcode = CallIndirect, .group = RowGroup::Calls, .emit = &W::emitCallIndirect },
            { .opcode = ReturnCallIndirect, .group = RowGroup::Calls, .emit = &W::emitCallIndirect },
            { .opcode = CallRef, .group = RowGroup::Calls, .emit = &W::emitCallRef },
            { .opcode = ReturnCallRef, .group = RowGroup::Calls, .emit = &W::emitCallRef },
            { .opcode = TryTable, .group = RowGroup::Exceptions, .emit = &W::emitTryTable },
            { .opcode = Throw, .group = RowGroup::Exceptions, .emit = &W::emitThrow },
            { .opcode = ThrowRef, .group = RowGroup::Exceptions, .emit = &W::emitThrowRef },
            { .opcode = Drop, .group = RowGroup::Variable, .emit = &W::emitDrop },
            { .opcode = Select, .group = RowGroup::Variable, .emit = &W::emitSelect },
            { .opcode = SelectTyped, .group = RowGroup::Variable, .emit = &W::emitSelect },
            { .opcode = LocalGet, .group = RowGroup::Variable, .emit = &W::emitLocal },
            { .opcode = LocalSet, .group = RowGroup::Variable, .emit = &W::emitLocal },
            { .opcode = LocalTee, .group = RowGroup::Variable, .emit = &W::emitLocal },
            { .opcode = GlobalGet, .group = RowGroup::Variable, .emit = &W::emitGlobal },
            { .opcode = GlobalSet, .group = RowGroup::Variable, .emit = &W::emitGlobal },
            { .opcode = TableGet, .group = RowGroup::Tables, .emit = &W::emitTable },
            { .opcode = TableSet, .group = RowGroup::Tables, .emit = &W::emitTable },
            { .opcode = TableInit, .group = RowGroup::Tables, .emit = &W::emitTable },
            { .opcode = ElemDrop, .group = RowGroup::Tables, .emit = &W::emitTable },
            { .opcode = TableCopy, .group = RowGroup::Tables, .emit = &W::emitTable },
            { .opcode = TableGrow, .group = RowGroup::Tables, .emit = &W::emitTable },
            { .opcode = TableSize, .group = RowGroup::Tables, .emit = &W::emitTable },
            { .opcode = TableFill, .group = RowGroup::Tables, .emit = &W::emitTable },
            { .opcode = MemorySize, .group = RowGroup::Memory, .emit = &W::emitMemory },
            { .opcode = MemoryGrow, .group = RowGroup::Memory, .emit = &W::emitMemory },
            { .opcode = MemoryInit, .group = RowGroup::Memory, .emit = &W::emitMemory },
            { .opcode = DataDrop, .group = RowGroup::Memory, .emit = &W::emitMemory },
            { .opcode = MemoryCopy, .group = RowGroup::Memory, .emit = &W::emitMemory },
            { .opcode = MemoryFill, .group = RowGroup::Memory, .emit = &W::emitMemory },
            { .opcode = RefNull, .group = RowGroup::References, .emit = &W::emitReference },
            { .opcode = RefIsNull, .group = RowGroup::References, .emit = &W::emitReference },
            { .opcode = RefFunc, .group = RowGroup::References, .emit = &W::emitReference },
            { .opcode = RefAsNonNull, .group = RowGroup::References, .emit = &W::emitReference },
        });
        return rows;
    }

    /// Whether a fixed-signature row is a vector instruction: it pops or pushes a v128.
    [[nodiscard]] RowGroup groupOfSignature(binary::OpcodeInfo const& row)
    {
        auto const& signature = binary::signatureInfo(row.signature);
        auto const isVector = [](binary::ValueKind kind) {
            return kind == binary::ValueKind::V128;
        };
        return std::ranges::any_of(signature.paramKinds(), isVector)
                       || std::ranges::any_of(signature.resultKinds(), isVector)
                   ? RowGroup::Vector
                   : RowGroup::Numeric;
    }

    /// The comparisons of two numbers to an i32, of every number type: what a fused br_if is made of.
    [[nodiscard]] std::span<binary::OpcodeInfo const* const> comparisons()
    {
        static auto const rows = [] {
            using enum binary::OpcodeSignature;
            constexpr auto Signatures = std::to_array({ I32I32ToI32, I64I64ToI32, F32F32ToI32, F64F64ToI32 });
            constexpr auto Relations =
                std::to_array<std::string_view>({ "eq", "ne", "lt", "gt", "le", "ge" });
            auto found = std::vector<binary::OpcodeInfo const*> {};
            for (auto const& row: binary::OpcodeTable)
            {
                auto const relation = row.text.substr(row.text.find('.') + 1);
                if (std::ranges::find(Signatures, row.signature) != Signatures.end()
                    && std::ranges::any_of(
                        Relations, [&](std::string_view prefix) { return relation.starts_with(prefix); }))
                    found.push_back(&row);
            }
            return found;
        }();
        return rows;
    }

    /// Every row of the cut, by group: what a step picks from.
    [[nodiscard]] std::array<std::vector<Candidate>, GroupCount> const& rowCandidates()
    {
        static auto const candidates = [] {
            auto groups = std::array<std::vector<Candidate>, GroupCount> {};
            for (auto const& row: binary::OpcodeTable)
            {
                if (!GeneratorFeatures.includes(row.required))
                    continue;
                if (row.signature != binary::OpcodeSignature::Special)
                {
                    groups[std::to_underlying(groupOfSignature(row))].push_back(
                        { .row = &row, .emit = &FunctionWriter::emitFixed });
                    continue;
                }
                if (binary::memoryAccessOf(row.opcode) != nullptr)
                {
                    groups[std::to_underlying(RowGroup::Memory)].push_back(
                        { .row = &row, .emit = &FunctionWriter::emitAccess });
                    continue;
                }
                auto const special = std::ranges::find(specialRows(), row.opcode, &SpecialRow::opcode);
                Require(special != specialRows().end());
                groups[std::to_underlying(special->group)].push_back({ .row = &row, .emit = special->emit });
            }
            return groups;
        }();
        return candidates;
    }

    /// The group a step picks from: by GroupWeights.
    [[nodiscard]] RowGroup pickGroup(Entropy& entropy)
    {
        static constexpr auto Total = [] {
            auto total = uint32_t { 0 };
            for (auto const weight: GroupWeights)
                total += weight;
            return total;
        }();
        auto point = entropy.below(Total);
        for (auto const group: std::views::iota(std::size_t { 0 }, GroupCount))
        {
            if (point < GroupWeights[group])
                return static_cast<RowGroup>(group);
            point -= GroupWeights[group];
        }
        return RowGroup::Numeric;
    }

    Bytes FunctionWriter::write()
    {
        while (!_frames.empty() && !_entropy.isExhausted())
        {
            auto checkpoint = Checkpoint {
                .bodyBytes = _body.size(), .written = _written, .frames = _frames, .loops = _loops
            };
            step();
            if (_written + reserve() > _limits.maxBodyInstructions)
            {
                _body.resize(checkpoint.bodyBytes);
                _written = checkpoint.written;
                _frames = std::move(checkpoint.frames);
                _loops = checkpoint.loops;
                break;
            }
        }
        while (!_frames.empty())
            close();
        Guarantee(_written + 1 <= maxFunctionInstructions(_limits));
        return std::move(_body);
    }

    void FunctionWriter::step()
    {
        if (top().reach == Reachability::Unreachable)
        {
            close();
            return;
        }
        if (top().stack.size() >= MaxOperandHeight)
        {
            instruction({ { 0x1A } });
            popAll(1);
            return;
        }
        auto const& group = rowCandidates()[std::to_underlying(pickGroup(_entropy))];
        if (group.empty())
            return;
        auto const& candidate = _entropy.pick(std::span { group });
        [[maybe_unused]] auto const emission = (this->*candidate.emit)(*candidate.row);
    }

    uint64_t FunctionWriter::reserve() const noexcept
    {
        // Closing a block drops its stack and makes its results, after an else and the else branch's results
        // for an if, after the back edge for a loop; its results then land on the block around it.
        auto cost = uint64_t { 0 };
        for (auto const& frame: _frames)
        {
            cost += frame.stack.size() + (2 * frame.results.size()) + 1;
            if (frame.kind == FrameKind::If && frame.branch == IfBranch::Then)
                cost += 1 + frame.params.size() + frame.results.size();
            if (frame.kind == FrameKind::Loop)
                cost += 2;
        }
        return cost;
    }

    void FunctionWriter::close()
    {
        auto& frame = top();
        if (frame.kind == FrameKind::If && frame.branch == IfBranch::Then && frame.results != frame.params)
        {
            // An if whose results are not its params needs an else, reachable or not.
            fixStack(frame.results);
            instruction({ { 0x05 } });
            frame.stack = frame.params;
            frame.reach = Reachability::Reachable;
            frame.branch = IfBranch::Else;
        }
        if (frame.kind == FrameKind::Loop && frame.reach == Reachability::Reachable)
        {
            // The back edge: local.get, br_if 0, while the counter, which the head decrements, is not zero.
            instruction({ { 0x20 }, u32(frame.counter) });
            instruction({ { 0x0D, 0x00 } });
        }
        auto const results = frame.results;
        fixStack(results);
        if (frame.kind == FrameKind::Loop)
            --_loops;
        auto const kind = frame.kind;
        _frames.pop_back();
        if (kind == FrameKind::Function)
            return; // build() appends the function's end
        instruction({ { 0x0B } });
        pushAll(results);
    }

    void FunctionWriter::fixStack(std::span<ValueType const> results)
    {
        auto& frame = top();
        if (frame.reach == Reachability::Unreachable || std::ranges::equal(frame.stack, results))
            return;
        for ([[maybe_unused]] auto const& value: frame.stack)
            instruction({ { 0x1A } });
        frame.stack.clear();
        for (auto const& type: results)
            produce(type);
    }

    void FunctionWriter::prepare(std::span<ValueType const> types)
    {
        auto const& stack = top().stack;
        if (stack.size() >= types.size() && std::ranges::equal(std::span { stack }.last(types.size()), types))
            return;
        for (auto const& type: types)
            produce(type);
    }

    void FunctionWriter::prepare(std::span<ValueType const> types,
                                 std::span<Operand const> roles,
                                 uint32_t memory)
    {
        Require(types.size() == roles.size());
        auto const& stack = top().stack;
        if (stack.size() >= types.size() && std::ranges::equal(std::span { stack }.last(types.size()), types))
            return;
        for (auto const index: std::views::iota(std::size_t { 0 }, types.size()))
            produce(types[index], roles[index], memory);
    }

    void FunctionWriter::instruction(std::initializer_list<Bytes> parts)
    {
        for (auto const& part: parts)
            _body.insert(_body.end(), part.begin(), part.end());
        ++_written;
    }

    void FunctionWriter::pushAll(std::span<ValueType const> types)
    {
        top().stack.insert(top().stack.end(), types.begin(), types.end());
    }

    void FunctionWriter::popAll(std::size_t count)
    {
        auto& stack = top().stack;
        Require(stack.size() >= count);
        stack.resize(stack.size() - count);
    }

    bool FunctionWriter::isTerminatorSkipped() noexcept
    {
        // An unconditional transfer ends its block; at the top level it ends the function, so it is rarer
        // there.
        return isTopLevel() && !_entropy.isOneIn(8);
    }

    uint32_t FunctionWriter::localOf(ValueType type)
    {
        auto candidates = std::vector<uint32_t> {};
        for (auto const index: std::views::iota(uint32_t { 0 }, _randomLocals))
            if (_locals[index] == type)
                candidates.push_back(index);
        Require(!candidates.empty());
        return _entropy.pick(std::span<uint32_t const> { candidates });
    }

    uint32_t FunctionWriter::globalOf(ValueType type) const
    {
        auto const global = std::ranges::find(ValueTypes, type);
        Require(global != ValueTypes.end());
        return static_cast<uint32_t>(std::distance(ValueTypes.begin(), global));
    }

    ValueType FunctionWriter::topOr(std::span<ValueType const> choices)
    {
        auto const& stack = top().stack;
        if (!stack.empty() && std::ranges::find(choices, stack.back()) != choices.end())
            return stack.back();
        return _entropy.pick(choices);
    }

    uint64_t FunctionWriter::number(std::span<uint64_t const> edges)
    {
        switch (_entropy.below(4))
        {
            case 0: return _entropy.below(256);
            case 1: return _entropy.pick(edges);
            case 2: return _entropy.below(16);
            default: return _entropy.bits();
        }
    }

    Bytes FunctionWriter::vectorBytes()
    {
        auto bytes = littleEndian(number(I64Edges), 8);
        auto const high = littleEndian(number(F64Edges), 8);
        bytes.insert(bytes.end(), high.begin(), high.end());
        return bytes;
    }

    Bytes FunctionWriter::numberImmediate(binary::OpcodeInfo const& row)
    {
        using binary::ImmediateKind;
        switch (row.immediate)
        {
            case ImmediateKind::None: return {};
            case ImmediateKind::I32Const:
                return binary::testing::encodeS64(
                    static_cast<int32_t>(static_cast<uint32_t>(number(I32Edges))));
            case ImmediateKind::I64Const:
                return binary::testing::encodeS64(static_cast<int64_t>(number(I64Edges)));
            case ImmediateKind::F32Const: return littleEndian(number(F32Edges), 4);
            case ImmediateKind::F64Const: return littleEndian(number(F64Edges), 8);
            case ImmediateKind::V128Const: return vectorBytes();
            case ImmediateKind::Shuffle: {
                auto lanes = Bytes {};
                for ([[maybe_unused]] auto const lane: std::views::iota(0, 16))
                    lanes.push_back(static_cast<uint8_t>(_entropy.below(32)));
                return lanes;
            }
            case ImmediateKind::LaneIndex: return { static_cast<uint8_t>(_entropy.below(lanesOf(row))) };
            default: break;
        }
        Require(false); // a fixed-signature row of another immediate needs its case here
        return {};
    }

    Bytes FunctionWriter::constantOf(ValueType type)
    {
        using enum binary::ValueKind;
        auto const constant = [&](binary::Opcode opcode) {
            auto bytes = opcodeBytes(opcode);
            auto const immediate = numberImmediate(binary::opcodeInfo(opcode));
            bytes.insert(bytes.end(), immediate.begin(), immediate.end());
            return bytes;
        };
        switch (type.kind)
        {
            case I32: return constant(binary::Opcode::I32Const);
            case I64: return constant(binary::Opcode::I64Const);
            case F32: return constant(binary::Opcode::F32Const);
            case F64: return constant(binary::Opcode::F64Const);
            case V128: return constant(binary::Opcode::V128Const);
            case Ref: break;
        }
        if (type == binary::FuncRef && _entropy.isOneIn(2))
        {
            auto bytes = Bytes { 0xD2 };
            std::ranges::copy(u32(_entropy.below(_layout.functionCount())), std::back_inserter(bytes));
            return bytes;
        }
        auto bytes = Bytes { 0xD0 };
        std::ranges::copy(binary::testing::encodeHeapType(type.heap), std::back_inserter(bytes));
        return bytes;
    }

    void FunctionWriter::produce(ValueType type)
    {
        switch (_entropy.below(8))
        {
            case 0:
            case 1: instruction({ { 0x20 }, u32(localOf(type)) }); break;
            case 2: instruction({ { 0x23 }, u32(globalOf(type)) }); break;
            default: instruction({ constantOf(type) }); break;
        }
        top().stack.push_back(type);
    }

    void FunctionWriter::produce(ValueType type, Operand role, uint32_t memory)
    {
        auto const isInteger = type == binary::I32 || type == binary::I64;
        if (role == Operand::Value || !isInteger || _entropy.isOneIn(8))
        {
            produce(type);
            return;
        }
        auto const bytes = Memories[memory].minBytes();
        auto value = uint64_t { 0 };
        switch (role)
        {
            case Operand::Address:
                value = _entropy.isOneIn(8) ? bytes - _entropy.below(32) : _entropy.below(bytes - 16);
                break;
            case Operand::Count: value = _entropy.below(MaxCount + 1); break;
            case Operand::Index: value = _entropy.below(MaxIndex); break;
            case Operand::Value: break;
        }
        auto constant = Bytes { type == binary::I32 ? uint8_t { 0x41 } : uint8_t { 0x42 } };
        std::ranges::copy(binary::testing::encodeS64(static_cast<int64_t>(value)),
                          std::back_inserter(constant));
        instruction({ constant });
        top().stack.push_back(type);
    }

    BlockSignature FunctionWriter::blockSignature(BlockParams params)
    {
        switch (_entropy.below(4))
        {
            case 0: return { .encoding = { 0x40 }, .params = {}, .results = {} };
            case 1:
            case 2: {
                auto const type = _entropy.pick(std::span { ValueTypes });
                return { .encoding = binary::testing::encodeValueType(type),
                         .params = {},
                         .results = { type } };
            }
            default: break;
        }
        auto const index =
            static_cast<uint32_t>(CaughtRefType + _entropy.below(_layout.types.size() - CaughtRefType));
        auto const& type = _layout.types[index];
        if (params == BlockParams::None && !type.params.empty())
            return { .encoding = { 0x40 }, .params = {}, .results = {} };
        return { .encoding = binary::testing::encodeS64(index),
                 .params = type.params,
                 .results = type.results };
    }

    std::size_t FunctionWriter::branchTarget()
    {
        // Every block's label, a loop's too: its head tests its counter.
        return _entropy.below(_frames.size());
    }

    std::optional<std::size_t> FunctionWriter::frameLabelled(std::span<ValueType const> types)
    {
        auto frames = std::vector<std::size_t> {};
        for (auto const frame: std::views::iota(std::size_t { 0 }, _frames.size()))
            if (std::ranges::equal(labelTypes(frame), types))
                frames.push_back(frame);
        if (frames.empty())
            return std::nullopt;
        return _entropy.pick(std::span<std::size_t const> { frames });
    }

    uint32_t FunctionWriter::labelIndexOf(std::size_t frame) const noexcept
    {
        return static_cast<uint32_t>(_frames.size() - 1 - frame);
    }

    std::vector<ValueType> FunctionWriter::labelTypes(std::size_t frame) const
    {
        auto const& target = _frames[frame];
        return target.kind == FrameKind::Loop ? target.params : target.results;
    }

    std::optional<uint32_t> FunctionWriter::tailCallee()
    {
        auto callees = std::vector<uint32_t> {};
        for (auto const function: std::views::iota(uint32_t { 0 }, _layout.functionCount()))
            if (_layout.typeOf(function).results == _type.results)
                callees.push_back(function);
        if (callees.empty())
            return std::nullopt;
        return _entropy.pick(std::span<uint32_t const> { callees });
    }

    std::optional<uint32_t> FunctionWriter::tailType()
    {
        auto types = std::vector<uint32_t> {};
        for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(_layout.types.size())))
            if (_layout.types[index].results == _type.results)
                types.push_back(index);
        if (types.empty())
            return std::nullopt;
        return _entropy.pick(std::span<uint32_t const> { types });
    }

    Emission FunctionWriter::emitFixed(binary::OpcodeInfo const& row)
    {
        auto const& signature = binary::signatureInfo(row.signature);
        auto const params = valueTypesOf(signature.paramKinds());
        prepare(params);
        popAll(params.size());
        instruction({ opcodeBytes(row.opcode), numberImmediate(row) });
        pushAll(valueTypesOf(signature.resultKinds()));
        return Emission::Written;
    }

    Emission FunctionWriter::emitAccess(binary::OpcodeInfo const& row)
    {
        auto const& access = *binary::memoryAccessOf(row.opcode);
        // Memory 0, the cached one, three times in four.
        auto const memory = _entropy.isOneIn(4) ? 1 + _entropy.below(Memories.size() - 1) : uint32_t { 0 };
        auto const isLoad = access.access == binary::MemoryAccessKind::Load;
        auto params = std::vector<ValueType> { addressOf(memory) };
        auto roles = std::vector<Operand> { Operand::Address };
        if (!isLoad || access.shape == binary::VectorShape::Lane)
        {
            params.push_back(valueTypeOf(access.value));
            roles.push_back(Operand::Value);
        }
        prepare(params, roles, memory);
        popAll(params.size());
        // The alignment's bit 6 says a memory index follows it (multi-memory).
        constexpr auto MemoryIndexFlag = uint32_t { 0x40 };
        auto const bytes = Memories[memory].minBytes();
        auto const alignment = _entropy.below(access.naturalAlignment() + 1);
        auto memarg = u32(memory == 0 ? alignment : alignment | MemoryIndexFlag);
        if (memory != 0)
            std::ranges::copy(u32(memory), std::back_inserter(memarg));
        auto const offset =
            _entropy.isOneIn(16) ? bytes - _entropy.below(32) : uint64_t { _entropy.below(64) };
        std::ranges::copy(binary::testing::encodeU64(offset), std::back_inserter(memarg));
        if (row.immediate == binary::ImmediateKind::MemArgLane)
            memarg.push_back(static_cast<uint8_t>(_entropy.below(lanesOf(row))));
        instruction({ opcodeBytes(row.opcode), memarg });
        if (isLoad)
            top().stack.push_back(valueTypeOf(access.value));
        return Emission::Written;
    }

    Emission FunctionWriter::emitUnreachable(binary::OpcodeInfo const& /*row*/)
    {
        if (isTerminatorSkipped() || !_entropy.isOneIn(2))
            return Emission::Skipped;
        instruction({ { 0x00 } });
        markUnreachable();
        return Emission::Written;
    }

    Emission FunctionWriter::emitBlock(binary::OpcodeInfo const& /*row*/)
    {
        if (depth() >= _limits.maxNesting)
            return Emission::Skipped;
        auto signature = blockSignature(BlockParams::Allowed);
        prepare(signature.params);
        popAll(signature.params.size());
        instruction({ { 0x02 }, signature.encoding });
        auto stack = signature.params;
        _frames.push_back(Frame { .kind = FrameKind::Block,
                                  .params = std::move(signature.params),
                                  .results = std::move(signature.results),
                                  .stack = std::move(stack),
                                  .reach = Reachability::Reachable,
                                  .branch = IfBranch::Then,
                                  .counter = 0 });
        return Emission::Written;
    }

    Emission FunctionWriter::emitLoop(binary::OpcodeInfo const& /*row*/)
    {
        // block $exit, loop: the head leaves through $exit when the counter is zero, else decrements it, so
        // every branch to the loop, its own back edge or any other, is bounded by the counter.
        if (depth() + 2 > _limits.maxNesting || _loops >= _counters)
            return Emission::Skipped;
        auto signature = blockSignature(BlockParams::None);
        // The counter starts at 1 to 4 iterations, now and then up to 64.
        auto const iterations = 1 + (_entropy.isOneIn(8) ? _entropy.below(64) : _entropy.below(4));
        auto const counter = _randomLocals + _loops;
        instruction({ { 0x41 }, binary::testing::encodeS64(iterations) });
        instruction({ { 0x21 }, u32(counter) });
        instruction({ { 0x02, 0x40 } });
        _frames.push_back(Frame { .kind = FrameKind::Block,
                                  .params = {},
                                  .results = {},
                                  .stack = {},
                                  .reach = Reachability::Reachable,
                                  .branch = IfBranch::Then,
                                  .counter = 0 });
        instruction({ { 0x03 }, signature.encoding });
        instruction({ { 0x20 }, u32(counter) }); // local.get
        instruction({ { 0x45 } });               // i32.eqz
        instruction({ { 0x0D, 0x01 } });         // br_if $exit
        instruction({ { 0x20 }, u32(counter) }); // local.get
        instruction({ { 0x41, 0x01 } });         // i32.const 1
        instruction({ { 0x6B } });               // i32.sub
        instruction({ { 0x21 }, u32(counter) }); // local.set
        _frames.push_back(Frame { .kind = FrameKind::Loop,
                                  .params = {},
                                  .results = std::move(signature.results),
                                  .stack = {},
                                  .reach = Reachability::Reachable,
                                  .branch = IfBranch::Then,
                                  .counter = counter });
        ++_loops;
        return Emission::Written;
    }

    Emission FunctionWriter::emitIf(binary::OpcodeInfo const& /*row*/)
    {
        if (depth() >= _limits.maxNesting)
            return Emission::Skipped;
        auto signature = blockSignature(BlockParams::Allowed);
        prepare(concatenated(signature.params, { binary::I32 }));
        popAll(signature.params.size() + 1);
        instruction({ { 0x04 }, signature.encoding });
        auto stack = signature.params;
        _frames.push_back(Frame { .kind = FrameKind::If,
                                  .params = std::move(signature.params),
                                  .results = std::move(signature.results),
                                  .stack = std::move(stack),
                                  .reach = Reachability::Reachable,
                                  .branch = IfBranch::Then,
                                  .counter = 0 });
        return Emission::Written;
    }

    Emission FunctionWriter::emitElse(binary::OpcodeInfo const& /*row*/)
    {
        if (top().kind != FrameKind::If || top().branch != IfBranch::Then)
            return Emission::Skipped;
        auto const results = top().results;
        fixStack(results);
        instruction({ { 0x05 } });
        auto& frame = top();
        frame.stack = frame.params;
        frame.branch = IfBranch::Else;
        return Emission::Written;
    }

    Emission FunctionWriter::emitEnd(binary::OpcodeInfo const& /*row*/)
    {
        if (isTopLevel())
            return Emission::Skipped;
        close();
        return Emission::Written;
    }

    Emission FunctionWriter::emitTryTable(binary::OpcodeInfo const& /*row*/)
    {
        if (depth() + 2 > _limits.maxNesting)
            return Emission::Skipped;
        // A block whose label the clauses branch to, then the try_table inside it.
        auto const& handler = _entropy.pick(handlerShapes());
        auto label = Bytes { 0x40 };
        if (handler.label.size() == 1)
            label = binary::testing::encodeValueType(handler.label.front());
        else if (handler.label.size() == 2)
            label = binary::testing::encodeS64(CaughtRefType);
        instruction({ { 0x02 }, label });
        _frames.push_back(Frame { .kind = FrameKind::Block,
                                  .params = {},
                                  .results = handler.label,
                                  .stack = {},
                                  .reach = Reachability::Reachable,
                                  .branch = IfBranch::Then,
                                  .counter = 0 });
        auto signature = blockSignature(BlockParams::None);
        auto const count = 1 + _entropy.below(MaxCatchClauses);
        auto clauses = u32(count);
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        {
            auto const& clause = _entropy.pick(std::span { handler.clauses });
            clauses.push_back(static_cast<uint8_t>(clause.kind));
            if (clause.kind == CatchKind::Catch || clause.kind == CatchKind::CatchRef)
                std::ranges::copy(u32(clause.tag), std::back_inserter(clauses));
            // A label of the clause's types: the block around, or another, a loop's among them. Catch labels
            // skip the try_table's own, so the block around is label 0.
            auto const target = frameLabelled(handler.label);
            Require(target.has_value());
            std::ranges::copy(u32(labelIndexOf(*target)), std::back_inserter(clauses));
        }
        instruction({ { 0x1F }, signature.encoding, clauses });
        _frames.push_back(Frame { .kind = FrameKind::TryTable,
                                  .params = {},
                                  .results = std::move(signature.results),
                                  .stack = {},
                                  .reach = Reachability::Reachable,
                                  .branch = IfBranch::Then,
                                  .counter = 0 });
        return Emission::Written;
    }

    Emission FunctionWriter::emitThrow(binary::OpcodeInfo const& /*row*/)
    {
        if (isTerminatorSkipped())
            return Emission::Skipped;
        auto const tag = _entropy.below(Tags.size());
        auto const params = _layout.types[Tags[tag]].params;
        prepare(params);
        instruction({ { 0x08 }, u32(tag) });
        markUnreachable();
        return Emission::Written;
    }

    Emission FunctionWriter::emitThrowRef(binary::OpcodeInfo const& /*row*/)
    {
        if (isTerminatorSkipped())
            return Emission::Skipped;
        prepare(std::array { binary::ExnRef });
        instruction({ { 0x0A } });
        markUnreachable();
        return Emission::Written;
    }

    Emission FunctionWriter::emitBr(binary::OpcodeInfo const& /*row*/)
    {
        if (isTerminatorSkipped())
            return Emission::Skipped;
        auto const target = branchTarget();
        prepare(labelTypes(target));
        instruction({ { 0x0C }, u32(labelIndexOf(target)) });
        markUnreachable();
        return Emission::Written;
    }

    Emission FunctionWriter::emitBrIf(binary::OpcodeInfo const& /*row*/)
    {
        auto const target = branchTarget();
        auto const types = labelTypes(target);
        if (_entropy.isOneIn(2))
        {
            // A comparison right before the br_if, which the interpreter fuses into one branch.
            auto const& compare = *_entropy.pick(comparisons());
            auto const operands = valueTypesOf(binary::signatureInfo(compare.signature).paramKinds());
            auto stack = concatenated(types, {});
            stack.insert(stack.end(), operands.begin(), operands.end());
            prepare(stack);
            popAll(operands.size());
            instruction({ opcodeBytes(compare.opcode) });
        }
        else
        {
            prepare(concatenated(types, { binary::I32 }));
            popAll(1);
        }
        instruction({ { 0x0D }, u32(labelIndexOf(target)) });
        return Emission::Written;
    }

    Emission FunctionWriter::emitBrTable(binary::OpcodeInfo const& /*row*/)
    {
        if (isTerminatorSkipped())
            return Emission::Skipped;
        auto const fallback = branchTarget();
        auto const types = labelTypes(fallback);
        auto matching = std::vector<std::size_t> {};
        for (auto const frame: std::views::iota(std::size_t { 0 }, _frames.size()))
            if (labelTypes(frame) == types)
                matching.push_back(frame);
        auto const count = _entropy.below(MaxBranchTableLabels + 1);
        auto labels = u32(count);
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
            std::ranges::copy(u32(labelIndexOf(_entropy.pick(std::span<std::size_t const> { matching }))),
                              std::back_inserter(labels));
        std::ranges::copy(u32(labelIndexOf(fallback)), std::back_inserter(labels));
        prepare(concatenated(types, { binary::I32 }));
        instruction({ { 0x0E }, labels });
        markUnreachable();
        return Emission::Written;
    }

    Emission FunctionWriter::emitReturn(binary::OpcodeInfo const& /*row*/)
    {
        if (isTerminatorSkipped())
            return Emission::Skipped;
        prepare(_type.results);
        instruction({ { 0x0F } });
        markUnreachable();
        return Emission::Written;
    }

    Emission FunctionWriter::emitCall(binary::OpcodeInfo const& row)
    {
        auto const isTail = row.opcode == binary::Opcode::ReturnCall;
        if (isTail && isTerminatorSkipped())
            return Emission::Skipped;
        auto const callee = isTail ? tailCallee() : std::optional { _entropy.below(_layout.functionCount()) };
        if (!callee)
            return Emission::Skipped;
        auto const type = _layout.typeOf(*callee);
        prepare(type.params);
        popAll(type.params.size());
        instruction({ opcodeBytes(row.opcode), u32(*callee) });
        if (isTail)
            markUnreachable();
        else
            pushAll(type.results);
        return Emission::Written;
    }

    Emission FunctionWriter::emitCallIndirect(binary::OpcodeInfo const& row)
    {
        auto const isTail = row.opcode == binary::Opcode::ReturnCallIndirect;
        if (isTail && isTerminatorSkipped())
            return Emission::Skipped;
        auto const typeIndex = isTail ? tailType() : std::optional { _entropy.below(_layout.types.size()) };
        if (!typeIndex)
            return Emission::Skipped;
        auto const type = _layout.types[*typeIndex];
        auto roles = std::vector<Operand>(type.params.size(), Operand::Value);
        roles.push_back(Operand::Index);
        prepare(concatenated(type.params, { binary::I32 }), roles);
        popAll(type.params.size() + 1);
        instruction({ opcodeBytes(row.opcode), u32(*typeIndex), u32(FunctionTable) });
        if (isTail)
            markUnreachable();
        else
            pushAll(type.results);
        return Emission::Written;
    }

    Emission FunctionWriter::emitCallRef(binary::OpcodeInfo const& row)
    {
        auto const isTail = row.opcode == binary::Opcode::ReturnCallRef;
        if (isTail && isTerminatorSkipped())
            return Emission::Skipped;
        auto const callee = isTail ? tailCallee() : std::optional { _entropy.below(_layout.functionCount()) };
        if (!callee)
            return Emission::Skipped;
        auto const typeIndex = _layout.functionTypes[*callee];
        auto const type = _layout.types[typeIndex];
        prepare(type.params);
        popAll(type.params.size());
        // The callee as a typed reference: ref.func, or now and then the null of its type, which traps.
        if (_entropy.isOneIn(8))
            instruction({ { 0xD0 },
                          binary::testing::encodeHeapType(binary::HeapType {
                              .kind = binary::AbstractHeapType::Concrete, .typeIndex = typeIndex }) });
        else
            instruction({ { 0xD2 }, u32(*callee) });
        instruction({ opcodeBytes(row.opcode), u32(typeIndex) });
        if (isTail)
            markUnreachable();
        else
            pushAll(type.results);
        return Emission::Written;
    }

    Emission FunctionWriter::emitDrop(binary::OpcodeInfo const& /*row*/)
    {
        if (top().stack.empty())
            return Emission::Skipped;
        instruction({ { 0x1A } });
        popAll(1);
        return Emission::Written;
    }

    Emission FunctionWriter::emitSelect(binary::OpcodeInfo const& row)
    {
        auto const isTyped = row.opcode == binary::Opcode::SelectTyped;
        auto const type = isTyped ? topOr(ValueTypes) : topOr(SelectableTypes);
        prepare(std::array { type, type, binary::I32 });
        popAll(3);
        if (isTyped)
            instruction({ { 0x1C, 0x01 }, binary::testing::encodeValueType(type) });
        else
            instruction({ { 0x1B } });
        top().stack.push_back(type);
        return Emission::Written;
    }

    Emission FunctionWriter::emitLocal(binary::OpcodeInfo const& row)
    {
        if (row.opcode == binary::Opcode::LocalGet)
        {
            auto const local = _entropy.below(_randomLocals);
            instruction({ { 0x20 }, u32(local) });
            top().stack.push_back(_locals[local]);
            return Emission::Written;
        }
        auto const local = top().stack.empty() ? _entropy.below(_randomLocals) : localOf(top().stack.back());
        prepare(std::array { _locals[local] });
        instruction({ opcodeBytes(row.opcode), u32(local) });
        if (row.opcode == binary::Opcode::LocalSet)
            popAll(1);
        return Emission::Written;
    }

    Emission FunctionWriter::emitGlobal(binary::OpcodeInfo const& row)
    {
        if (row.opcode == binary::Opcode::GlobalGet)
        {
            auto const global = _entropy.below(ValueTypes.size());
            instruction({ { 0x23 }, u32(global) });
            top().stack.push_back(ValueTypes[global]);
            return Emission::Written;
        }
        auto const type = topOr(ValueTypes);
        prepare(std::array { type });
        popAll(1);
        instruction({ { 0x24 }, u32(globalOf(type)) });
        return Emission::Written;
    }

    Emission FunctionWriter::emitTable(binary::OpcodeInfo const& row)
    {
        using enum binary::Opcode;
        using binary::I32;
        auto const table = _entropy.below(TableElements.size());
        auto const element = elementOf(table);
        auto const opcode = opcodeBytes(row.opcode);
        switch (row.opcode)
        {
            case TableGet:
                prepare(std::array { I32 }, std::array { Operand::Index });
                popAll(1);
                instruction({ opcode, u32(table) });
                top().stack.push_back(element);
                break;
            case TableSet:
                prepare(std::array { I32, element }, std::array { Operand::Index, Operand::Value });
                popAll(2);
                instruction({ opcode, u32(table) });
                break;
            case TableInit:
                prepare(std::array { I32, I32, I32 },
                        std::array { Operand::Index, Operand::Index, Operand::Count });
                popAll(3);
                instruction({ opcode, u32(_entropy.below(ElementSegments)), u32(FunctionTable) });
                break;
            case ElemDrop: instruction({ opcode, u32(_entropy.below(ElementSegments)) }); break;
            case TableCopy:
                prepare(std::array { I32, I32, I32 },
                        std::array { Operand::Index, Operand::Index, Operand::Count });
                popAll(3);
                instruction({ opcode, u32(table), u32(table) });
                break;
            case TableGrow:
                prepare(std::array { element, I32 }, std::array { Operand::Value, Operand::Count });
                popAll(2);
                instruction({ opcode, u32(table) });
                top().stack.push_back(I32);
                break;
            case TableSize:
                instruction({ opcode, u32(table) });
                top().stack.push_back(I32);
                break;
            case TableFill:
                prepare(std::array { I32, element, I32 },
                        std::array { Operand::Index, Operand::Value, Operand::Count });
                popAll(3);
                instruction({ opcode, u32(table) });
                break;
            default: Require(false); // specialRows() names this emitter for table instructions only
        }
        return Emission::Written;
    }

    Emission FunctionWriter::emitMemory(binary::OpcodeInfo const& row)
    {
        using enum binary::Opcode;
        using binary::I32;
        auto const memory = _entropy.below(Memories.size());
        auto const address = addressOf(memory);
        auto const opcode = opcodeBytes(row.opcode);
        switch (row.opcode)
        {
            case MemorySize:
                instruction({ opcode, u32(memory) });
                top().stack.push_back(address);
                break;
            case MemoryGrow:
                prepare(std::array { address }, std::array { Operand::Count }, memory);
                instruction({ opcode, u32(memory) });
                break;
            case MemoryInit:
                prepare(std::array { address, I32, I32 },
                        std::array { Operand::Address, Operand::Count, Operand::Count },
                        memory);
                popAll(3);
                instruction({ opcode, u32(_entropy.below(DataSegments)), u32(memory) });
                break;
            case DataDrop: instruction({ opcode, u32(_entropy.below(DataSegments)) }); break;
            case MemoryCopy:
                prepare(std::array { address, address, address },
                        std::array { Operand::Address, Operand::Address, Operand::Count },
                        memory);
                popAll(3);
                instruction({ opcode, u32(memory), u32(memory) });
                break;
            case MemoryFill:
                prepare(std::array { address, I32, address },
                        std::array { Operand::Address, Operand::Value, Operand::Count },
                        memory);
                popAll(3);
                instruction({ opcode, u32(memory) });
                break;
            default: Require(false); // specialRows() names this emitter for memory instructions only
        }
        return Emission::Written;
    }

    Emission FunctionWriter::emitReference(binary::OpcodeInfo const& row)
    {
        using enum binary::Opcode;
        switch (row.opcode)
        {
            case RefNull: {
                auto const type = _entropy.pick(std::span { ReferenceTypes });
                instruction({ { 0xD0 }, binary::testing::encodeHeapType(type.heap) });
                top().stack.push_back(type);
                break;
            }
            case RefIsNull:
                prepare(std::array { topOr(ReferenceTypes) });
                popAll(1);
                instruction({ { 0xD1 } });
                top().stack.push_back(binary::I32);
                break;
            case RefFunc:
                instruction({ { 0xD2 }, u32(_entropy.below(_layout.functionCount())) });
                top().stack.push_back(binary::FuncRef);
                break;
            case RefAsNonNull:
                // The non-null reference is kept as its nullable type, which every use of it accepts.
                prepare(std::array { topOr(ReferenceTypes) });
                instruction({ { 0xD4 } });
                break;
            default: Require(false); // specialRows() names this emitter for reference instructions only
        }
        return Emission::Written;
    }

    Emission FunctionWriter::emitBranchOnNull(binary::OpcodeInfo const& row)
    {
        // A block of its own, whose label it branches to: the null leaves it (br_on_null), or the non-null
        // reference does, as the block's result, and the null falls through to a null result
        // (br_on_non_null).
        if (depth() >= _limits.maxNesting)
            return Emission::Skipped;
        auto const isOnNull = row.opcode == binary::Opcode::BrOnNull;
        if (isOnNull && _entropy.isOneIn(2))
        {
            // Or to an enclosing label of no values, a loop's among them; the non-null reference stays.
            if (auto const target = frameLabelled({}))
            {
                prepare(std::array { topOr(ReferenceTypes) });
                instruction({ opcodeBytes(row.opcode), u32(labelIndexOf(*target)) });
                return Emission::Written;
            }
        }
        auto const type = _entropy.pick(std::span { ReferenceTypes });
        instruction({ { 0x02 }, isOnNull ? Bytes { 0x40 } : binary::testing::encodeValueType(type) });
        produce(type);
        popAll(1);
        instruction({ opcodeBytes(row.opcode), u32(0) });
        if (isOnNull)
            instruction({ { 0x1A } });
        else
            instruction({ { 0xD0 }, binary::testing::encodeHeapType(type.heap) });
        instruction({ { 0x0B } });
        if (!isOnNull)
            top().stack.push_back(type);
        return Emission::Written;
    }

    // -----------------------------------------------------------------------------------------------------------
    // Program shapes: functions that reach the hard edges no random body does

    /// What a defined function's body is.
    enum class FunctionBody : uint8_t
    {
        Random,         ///< Written by FunctionWriter.
        UnwindEntry,    ///< Calls the unwind worker after it with a depth of hundreds of frames.
        UnwindWorker,   ///< Recurses inside eight blocks, and throws at the bottom: one unwind past them all.
        ExceptionChurn, ///< Throws and catches more exceptions than the store holds, which sweeps.
    };

    /// A program shape: the bodies of its consecutive functions, the most instructions and the deepest
    /// nesting any of them has.
    struct ProgramShape
    {
        std::vector<FunctionBody> bodies;
        uint32_t instructions;
        uint32_t nesting;
    };

    /// The labels the unwind worker nests its recursion in, and the least frames it recurses: with the frame
    /// itself, 9 items an unwind counts per frame, past runtime::UnwindPollItems (4'096) in 600 frames, and
    /// within differential::FuzzLimits' call depth (1'000) at the most, 855.
    constexpr auto UnwindLabels = uint32_t { 8 };
    constexpr auto UnwindLeastDepth = uint32_t { 600 };
    constexpr auto UnwindDepthSpread = uint32_t { 256 };

    /// The exceptions the churn throws: twice the 64 differential::FuzzLimits lets the store hold.
    constexpr auto ChurnThrows = int64_t { 128 };

    [[nodiscard]] std::span<ProgramShape const> programShapes()
    {
        static auto const shapes = std::vector<ProgramShape> {
            { .bodies = { FunctionBody::UnwindEntry, FunctionBody::UnwindWorker },
              .instructions = 5 + 4 + (2 * UnwindLabels),
              .nesting = UnwindLabels },
            { .bodies = { FunctionBody::ExceptionChurn }, .instructions = 14, .nesting = 3 },
        };
        return shapes;
    }

    /// The type of a function of @p body; a random one's is chosen elsewhere.
    [[nodiscard]] uint32_t shapeTypeOf(FunctionBody body)
    {
        return body == FunctionBody::UnwindWorker ? TagPayloadType : EmptyType;
    }

    /// A function's declared locals and its body without the final end.
    struct WrittenFunction
    {
        std::vector<ValueType> locals;
        Bytes body;
    };

    /// The unwind's entry: i32.const depth, call worker.
    [[nodiscard]] WrittenFunction unwindEntry(Entropy& entropy, uint32_t worker)
    {
        auto body = Bytes { 0x41 };
        std::ranges::copy(binary::testing::encodeS64(UnwindLeastDepth + entropy.below(UnwindDepthSpread)),
                          std::back_inserter(body));
        body.push_back(0x10);
        std::ranges::copy(u32(worker), std::back_inserter(body));
        return { .locals = {}, .body = std::move(body) };
    }

    /// The unwind's worker, (param i32): at 0 it throws tag 0, else it calls itself with one less inside
    /// UnwindLabels blocks.
    [[nodiscard]] WrittenFunction unwindWorker(uint32_t self)
    {
        auto body = Bytes {
            0x20, 0x00, 0x45, 0x04, 0x40, 0x08, 0x00, 0x0B
        }; // local.get 0, i32.eqz, if, throw 0, end
        for ([[maybe_unused]] auto const label: std::views::iota(uint32_t { 0 }, UnwindLabels))
            body.insert(body.end(), { 0x02, 0x40 });
        body.insert(body.end(),
                    { 0x20, 0x00, 0x41, 0x01, 0x6B, 0x10 }); // local.get 0, i32.const 1, i32.sub, call
        std::ranges::copy(u32(self), std::back_inserter(body));
        body.insert(body.end(), UnwindLabels, 0x0B);
        return { .locals = {}, .body = std::move(body) };
    }

    /// The churn: a loop of ChurnThrows iterations, each a throw of tag 0 caught by a catch_all, which keeps
    /// no reference, so that each exception is garbage once caught.
    [[nodiscard]] WrittenFunction exceptionChurn()
    {
        auto body = Bytes { 0x41 };
        std::ranges::copy(binary::testing::encodeS64(ChurnThrows), std::back_inserter(body));
        body.insert(body.end(),
                    {
                        0x21, 0x00,                   // local.set 0
                        0x03, 0x40,                   // loop
                        0x02, 0x40,                   // block
                        0x1F, 0x40, 0x01, 0x02, 0x00, // try_table (catch_all 0)
                        0x08, 0x00,                   // throw 0
                        0x0B, 0x0B,                   // end, end
                        0x20, 0x00, 0x41, 0x01, 0x6B, // local.get 0, i32.const 1, i32.sub
                        0x22, 0x00, 0x0D, 0x00,       // local.tee 0, br_if 0
                        0x0B,                         // end
                    });
        return { .locals = { binary::I32 }, .body = std::move(body) };
    }

    // -----------------------------------------------------------------------------------------------------------
    // The module

    /// A global's zero, as its initializer.
    [[nodiscard]] Bytes zeroOf(ValueType type)
    {
        switch (type.kind)
        {
            case binary::ValueKind::I32: return { 0x41, 0x00 };
            case binary::ValueKind::I64: return { 0x42, 0x00 };
            case binary::ValueKind::F32: return { 0x43, 0x00, 0x00, 0x00, 0x00 };
            case binary::ValueKind::F64: return { 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            case binary::ValueKind::V128: {
                auto bytes = opcodeBytes(binary::Opcode::V128Const);
                bytes.insert(bytes.end(), 16, 0x00);
                return bytes;
            }
            case binary::ValueKind::Ref: break;
        }
        auto bytes = Bytes { 0xD0 };
        std::ranges::copy(binary::testing::encodeHeapType(type.heap), std::back_inserter(bytes));
        return bytes;
    }

    /// Up to MaxDataBytes bytes of the entropy.
    [[nodiscard]] Bytes dataBytes(Entropy& entropy)
    {
        auto bytes = Bytes {};
        for ([[maybe_unused]] auto const index:
             std::views::iota(uint32_t { 0 }, entropy.below(MaxDataBytes + 1)))
            bytes.push_back(entropy.byte());
        return bytes;
    }

    /// An element segment's function indices: every function.
    [[nodiscard]] Bytes everyFunction(uint32_t count)
    {
        auto bytes = u32(count);
        for (auto const function: std::views::iota(uint32_t { 0 }, count))
            std::ranges::copy(u32(function), std::back_inserter(bytes));
        return bytes;
    }

    /// The product of @p left and @p right, or the largest uint64_t when it does not fit.
    [[nodiscard]] constexpr uint64_t saturatingProduct(uint64_t left, uint64_t right) noexcept
    {
        if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)
            return std::numeric_limits<uint64_t>::max();
        return left * right;
    }

    /// The sum of @p left and @p right, or the largest uint64_t when it does not fit.
    [[nodiscard]] constexpr uint64_t saturatingSum(uint64_t left, uint64_t right) noexcept
    {
        return left > std::numeric_limits<uint64_t>::max() - right ? std::numeric_limits<uint64_t>::max()
                                                                   : left + right;
    }

    /// What maxModuleBytes() allows for everything but the functions: the header, every section's id and
    /// size, the fixed and random types, the imports, tables, memories, globals, tags, both data segments,
    /// and the imports' entries in both element segments. Each is far below its share.
    constexpr auto FixedModuleBytes = uint64_t { 2'048 };

    /// What maxModuleBytes() allows per function besides its body's instructions: its type index, its export,
    /// its entries in both element segments, its body's size and its locals' runs.
    constexpr auto PerFunctionBytes = uint64_t { 160 };

} // namespace

uint64_t maxFunctionInstructions(GeneratorLimits const& limits) noexcept
{
    // Closing the body may write FunctionCloseInstructions past an empty budget; the + 1 is the final end.
    return std::max(uint64_t { limits.maxBodyInstructions }, FunctionCloseInstructions) + 1;
}

uint64_t maxModuleBytes(GeneratorLimits const& limits) noexcept
{
    auto const perFunction = saturatingSum(
        PerFunctionBytes, saturatingProduct(maxFunctionInstructions(limits), MaxInstructionBytes));
    return saturatingSum(FixedModuleBytes, saturatingProduct(limits.maxFunctions, perFunction));
}

std::vector<std::byte> generateModule(std::span<std::byte const> entropyBytes, GeneratorLimits const& limits)
{
    return generateModule(entropyBytes, limits, GeneratorMode::Harness);
}

std::vector<std::byte> generateModule(std::span<std::byte const> entropyBytes,
                                      GeneratorLimits const& limits,
                                      GeneratorMode modeName)
{
    auto const& mode = ModeRows[std::to_underlying(modeName)];
    Require(mode.mode == modeName);
    auto entropy = Entropy { entropyBytes };
    auto layout = Layout { .types = fixedTypes(), .functionTypes = {} };
    for ([[maybe_unused]] auto const index:
         std::views::iota(uint32_t { 0 }, 1 + entropy.below(MaxRandomTypes)))
        layout.types.push_back(randomType(entropy, mode));
    for (auto const index: std::views::iota(std::size_t { 0 }, mode.imports.size()))
        layout.functionTypes.push_back(HostImportTypes[index]);

    // The defined functions: random ones, and now and then a program shape's.
    auto const defined = limits.maxFunctions == 0 ? uint32_t { 0 } : 1 + entropy.below(limits.maxFunctions);
    auto bodies = std::vector<FunctionBody> {};
    while (bodies.size() < defined)
    {
        if (entropy.isOneIn(6))
        {
            auto const& shape = entropy.pick(programShapes());
            if (bodies.size() + shape.bodies.size() <= defined
                && shape.instructions <= limits.maxBodyInstructions && shape.nesting <= limits.maxNesting)
            {
                for (auto const body: shape.bodies)
                {
                    bodies.push_back(body);
                    layout.functionTypes.push_back(shapeTypeOf(body));
                }
                continue;
            }
        }
        bodies.push_back(FunctionBody::Random);
        layout.functionTypes.push_back(
            static_cast<uint32_t>(FirstRandomType + entropy.below(layout.types.size() - FirstRandomType)));
    }
    auto const activeData = dataBytes(entropy);
    auto const passiveData = dataBytes(entropy);

    auto builder = binary::testing::ModuleBuilder {};
    for (auto const& type: layout.types)
        builder.type(type);
    for (auto const index: std::views::iota(std::size_t { 0 }, mode.imports.size()))
        builder.importFunction(mode.imports[index].module, mode.imports[index].name, HostImportTypes[index]);
    for (auto const index: std::views::iota(std::size_t { 0 }, bodies.size()))
    {
        auto const function = static_cast<uint32_t>(mode.imports.size() + index);
        auto const typeIndex = layout.functionTypes[function];
        auto written = WrittenFunction {};
        switch (bodies[index])
        {
            case FunctionBody::Random: {
                auto writer = FunctionWriter { entropy, layout, limits, typeIndex };
                written = WrittenFunction { .locals = writer.declaredLocals(), .body = writer.write() };
                break;
            }
            case FunctionBody::UnwindEntry: written = unwindEntry(entropy, function + 1); break;
            case FunctionBody::UnwindWorker: written = unwindWorker(function); break;
            case FunctionBody::ExceptionChurn: written = exceptionChurn(); break;
        }
        builder.function(typeIndex, std::move(written.locals), std::move(written.body));
        builder.exportFunction("f" + std::to_string(function), function);
    }

    auto const functions = layout.functionCount();
    builder.table(binary::TableType { .element = binary::FuncRef,
                                      .limits = { .min = functions,
                                                  .max = functions + TableGrowth,
                                                  .addressType = binary::AddressType::I32 } });
    builder.table(binary::TableType { .element = binary::ExternRef,
                                      .limits = { .min = ExternTableSize,
                                                  .max = ExternTableSize + TableGrowth,
                                                  .addressType = binary::AddressType::I32 } });
    for (auto const& memory: Memories)
        builder.memory(binary::MemoryType {
            .limits = { .min = memory.minPages, .max = memory.maxPages, .addressType = memory.addressType },
            .pageSizeLog2 = memory.pageSizeLog2 });
    for (auto const& type: ValueTypes)
        builder.global(binary::GlobalType { .type = type, .mutability = binary::Mutability::Var },
                       zeroOf(type));
    for (auto const tag: Tags)
        builder.tag(tag);

    // Element segment 0: active, every function into table 0 from 0; 1: passive, the same functions.
    auto active = Bytes { 0x00, 0x41, 0x00, 0x0B };
    std::ranges::copy(everyFunction(functions), std::back_inserter(active));
    builder.element(std::move(active));
    auto passive = Bytes { 0x01, 0x00 };
    std::ranges::copy(everyFunction(functions), std::back_inserter(passive));
    builder.element(std::move(passive));

    // Data segment 0: active in memory 0 at a small offset; 1: passive.
    builder.dataCount(DataSegments);
    auto activeSegment = Bytes { 0x00, 0x41, static_cast<uint8_t>(entropy.below(64)), 0x0B };
    std::ranges::copy(u32(static_cast<uint32_t>(activeData.size())), std::back_inserter(activeSegment));
    activeSegment.insert(activeSegment.end(), activeData.begin(), activeData.end());
    builder.data(std::move(activeSegment));
    auto passiveSegment = Bytes { 0x01 };
    std::ranges::copy(u32(static_cast<uint32_t>(passiveData.size())), std::back_inserter(passiveSegment));
    passiveSegment.insert(passiveSegment.end(), passiveData.begin(), passiveData.end());
    builder.data(std::move(passiveSegment));
    return builder.build();
}

} // namespace isoline::fuzz
