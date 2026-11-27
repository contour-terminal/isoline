// SPDX-License-Identifier: Apache-2.0
#include <isoline/binary/Encoder.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/ByteWriter.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Encodings.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>

namespace isoline::binary
{

namespace
{
    /// The size every branch hint item declares: that of its one hint byte.
    constexpr auto BranchHintSize = uint32_t { 1 };

    /// Precondition (checked): @p heap has an encoding the decoder reads (AbstractHeapEncodings).
    void writeHeapType(Bytes& out, HeapType const& heap)
    {
        if (heap.kind == AbstractHeapType::Concrete)
        {
            writeSigned(out, heap.typeIndex);
            return;
        }
        auto const row = std::ranges::find(AbstractHeapEncodings, heap.kind, &HeapEncoding::kind);
        Require(row != AbstractHeapEncodings.end());
        writeByte(out, row->byte);
    }

    /// Writes a value type: the one byte that stands for it when there is one (a number, a vector, or a
    /// shorthand), otherwise a reference's prefix by its nullability, then its heap type.
    void writeValueType(Bytes& out, ValueType const& type)
    {
        auto const whole = std::ranges::find_if(ValueTypeEncodings, [&](TypeEncoding const& row) {
            return row.form == TypeForm::Complete && row.type == type;
        });
        if (whole != ValueTypeEncodings.end())
        {
            writeByte(out, whole->byte);
            return;
        }
        Require(type.kind == ValueKind::Ref);
        auto const prefix = std::ranges::find_if(ValueTypeEncodings, [&](TypeEncoding const& row) {
            return row.form == TypeForm::HeapFollows && row.type.nullable == type.nullable;
        });
        Require(prefix != ValueTypeEncodings.end());
        writeByte(out, prefix->byte);
        writeHeapType(out, type.heap);
    }

    void writeValueTypes(Bytes& out, std::span<ValueType const> types)
    {
        writeVector(out, types, [&](ValueType const& type) { writeValueType(out, type); });
    }

    /// Writes limits by the LimitsFlagTable row that announces their bounds, address type and page size.
    void writeLimits(Bytes& out, Limits const& limits, uint8_t pageSizeLog2)
    {
        auto const bound = limits.max ? LimitsBound::MinimumAndMaximum : LimitsBound::Minimum;
        auto const pageSize =
            pageSizeLog2 == DefaultPageSizeLog2 ? PageSizeField::Default : PageSizeField::Exponent;
        auto const row = std::ranges::find_if(LimitsFlagTable, [&](LimitsFlagRow const& candidate) {
            return candidate.bound == bound && candidate.addressType == limits.addressType
                   && candidate.pageSize == pageSize;
        });
        Require(row != LimitsFlagTable.end());
        writeByte(out, row->flags);
        writeUnsigned(out, limits.min);
        if (limits.max)
            writeUnsigned(out, *limits.max);
        if (pageSize == PageSizeField::Exponent)
            writeUnsigned(out, pageSizeLog2);
    }

    void writeTableType(Bytes& out, TableType const& type)
    {
        writeValueType(out, type.element);
        writeLimits(out, type.limits, DefaultPageSizeLog2);
    }

    void writeMemoryType(Bytes& out, MemoryType const& type)
    {
        writeLimits(out, type.limits, type.pageSizeLog2);
    }

    void writeGlobalType(Bytes& out, GlobalType const& type)
    {
        writeValueType(out, type.type);
        writeByte(out, type.mutability == Mutability::Var ? MutableGlobal : ImmutableGlobal);
    }

    void writeTagType(Bytes& out, uint32_t typeIndex)
    {
        writeByte(out, ExceptionAttribute);
        writeUnsigned(out, typeIndex);
    }

    /// The byte ExternKinds decodes to @p kind.
    [[nodiscard]] uint8_t externKindByte(ExternKind kind) noexcept
    {
        auto const row = std::ranges::find(ExternKinds, kind, &ExternKindRow::kind);
        return static_cast<uint8_t>(row - ExternKinds.begin());
    }

    /// The byte CatchKinds decodes to @p kind.
    [[nodiscard]] uint8_t catchKindByte(CatchKind kind) noexcept
    {
        return static_cast<uint8_t>(std::ranges::find(CatchKinds, kind) - CatchKinds.begin());
    }

    /// The type an element segment's items have when its form leaves the type implicit.
    [[nodiscard]] constexpr ValueType implicitElementType(ItemEncoding items) noexcept
    {
        return items == ItemEncoding::FunctionIndices ? NonNullFuncRef : FuncRef;
    }

    /// Whether @p form can express @p segment, whose items are encoded as @p items.
    [[nodiscard]] bool expresses(ElementForm const& form,
                                 ElementSegment const& segment,
                                 ItemEncoding items) noexcept
    {
        if (form.mode != segment.mode || form.items != items)
            return false;
        if (form.mode == SegmentMode::Active && form.table == IndexEncoding::Implicit && segment.table != 0)
            return false;
        switch (form.type)
        {
            case ElementTypeEncoding::Implicit: return segment.type == implicitElementType(items);
            case ElementTypeEncoding::ElemKind: return segment.type == NonNullFuncRef;
            case ElementTypeEncoding::RefType: break;
        }
        return true;
    }

    /// Whether @p form can express @p segment.
    [[nodiscard]] bool expresses(DataForm const& form, DataSegment const& segment) noexcept
    {
        return form.mode == segment.mode
               && (form.mode != SegmentMode::Active || form.memory == IndexEncoding::Explicit
                   || segment.memory == 0);
    }

    /// Whether @p module has entries in the field @p Field, so that its section is written.
    template <auto Field>
    [[nodiscard]] bool hasEntries(Module const& module) noexcept
    {
        return !(module.*Field).empty();
    }

    /// Whether @p module sets the optional field @p Field, so that its section is written.
    template <auto Field>
    [[nodiscard]] bool isSet(Module const& module) noexcept
    {
        return (module.*Field).has_value();
    }

    /// Encodes one module front to back, as decodeModule reads it.
    class ModuleEncoder
    {
      public:
        /// @param module The module to encode.
        /// @param maxBytes The most bytes its encoding may take.
        ModuleEncoder(Module const& module, std::size_t maxBytes):
            _module { module },
            _maxBytes { maxBytes },
            _importedFunctions { static_cast<uint32_t>(module.importedCount(ExternKind::Func)) },
            _hints { module.branchHints }
        {
            std::ranges::stable_sort(_hints, [](BranchHint const& a, BranchHint const& b) {
                return std::pair { a.function, a.instruction } < std::pair { b.function, b.instruction };
            });
            _hintOffsets.resize(_hints.size());
        }

        /// Encodes the module.
        /// @return Its bytes, or ModuleTooLarge once they would exceed the limit.
        [[nodiscard]] std::expected<Bytes, EncodeError> encode();

        // The section writers, one per row of SectionEncodings (SectionWriters).
        void typeSection(Bytes& out) const;
        void importSection(Bytes& out) const;
        void functionSection(Bytes& out) const;
        void tableSection(Bytes& out) const;
        void memorySection(Bytes& out) const;
        void tagSection(Bytes& out) const;
        void globalSection(Bytes& out) const;
        void exportSection(Bytes& out) const;
        void startSection(Bytes& out) const;
        void elementSection(Bytes& out) const;
        void dataCountSection(Bytes& out) const;
        void codeSection(Bytes& out) const;
        void dataSection(Bytes& out) const;

        /// The payload of the branch hint section Module::branchHints make, or nothing without hints.
        [[nodiscard]] std::optional<Bytes> branchHintSection() const;

        /// The payload of the branch hint section, encoding the hinted functions' bodies only, one at a
        /// time, for their offsets: what encode() synthesizes, holding no more than one body.
        [[nodiscard]] Bytes branchHintsOnly();

      private:
        /// Encodes the code section's payload into _code, recording the offset of every hinted instruction.
        void encodeCode();
        void writeFunction(Bytes& out,
                           Function const& function,
                           uint32_t functionIndex,
                           std::size_t& nextHint);
        /// Whether @p pending more bytes, beyond what the module holds so far, still fit its limit.
        [[nodiscard]] bool fits(std::size_t pending) const noexcept;
        void writeSection(uint8_t id, Bytes const& payload);
        void writeCustom(std::string_view name, std::span<std::byte const> payload);
        void writeCustoms(SectionAnchor anchor);
        void writeSynthesized(SynthesizedPlacement placement);
        void writeConstExpr(Bytes& out, ConstExpr const& expression) const;
        /// Writes @p instruction: its opcode by its OpcodeTable row, then its immediates.
        /// @param function The function whose side tables its runs index; null in a constant expression.
        void writeInstruction(Bytes& out, Instruction const& instruction, Function const* function) const;
        void writeImmediates(Bytes& out, Instruction const& instruction, Function const* function) const;
        void writeBlockType(Bytes& out, Instruction const& instruction) const;
        void writeMemArg(Bytes& out, Instruction const& instruction) const;
        void writeImport(Bytes& out, Import const& import) const;
        void writeElementSegment(Bytes& out, ElementSegment const& segment) const;
        void writeDataSegment(Bytes& out, DataSegment const& segment) const;

        Module const& _module;
        std::size_t _maxBytes;    ///< The most bytes the encoding may take.
        bool _isTooLarge = false; ///< Whether a section did not fit _maxBytes, which stops encoding.
        uint32_t _importedFunctions;
        std::vector<BranchHint> _hints;     ///< Module::branchHints, by function, then by instruction.
        std::vector<uint32_t> _hintOffsets; ///< The offset of each of _hints' instruction, once encoded.
        Bytes _code;                        ///< The code section's payload, encoded first for the offsets.
        Bytes _body;                        ///< The function body being encoded.
        Bytes _section;                     ///< The section being encoded.
        Bytes _out;                         ///< The module.
    };

    /// One non-custom section's writer, and when it is written.
    struct SectionWriter
    {
        SectionAnchor anchor;                           ///< The section, as in SectionEncodings.
        bool (*isWritten)(Module const&) noexcept;      ///< Whether the module has the section.
        void (ModuleEncoder::*write)(Bytes& out) const; ///< Writes its payload.
    };

    /// The writer of every row of SectionEncodings, in its order.
    constexpr auto SectionWriters = std::to_array<SectionWriter>({
        { SectionAnchor::Type, &hasEntries<&Module::types>, &ModuleEncoder::typeSection },
        { SectionAnchor::Import, &hasEntries<&Module::imports>, &ModuleEncoder::importSection },
        { SectionAnchor::Function, &hasEntries<&Module::functions>, &ModuleEncoder::functionSection },
        { SectionAnchor::Table, &hasEntries<&Module::tables>, &ModuleEncoder::tableSection },
        { SectionAnchor::Memory, &hasEntries<&Module::memories>, &ModuleEncoder::memorySection },
        { SectionAnchor::Tag, &hasEntries<&Module::tags>, &ModuleEncoder::tagSection },
        { SectionAnchor::Global, &hasEntries<&Module::globals>, &ModuleEncoder::globalSection },
        { SectionAnchor::Export, &hasEntries<&Module::exports>, &ModuleEncoder::exportSection },
        { SectionAnchor::Start, &isSet<&Module::start>, &ModuleEncoder::startSection },
        { SectionAnchor::Element, &hasEntries<&Module::elements>, &ModuleEncoder::elementSection },
        { SectionAnchor::DataCount, &isSet<&Module::dataCount>, &ModuleEncoder::dataCountSection },
        { SectionAnchor::Code, &hasEntries<&Module::code>, &ModuleEncoder::codeSection },
        { SectionAnchor::Data, &hasEntries<&Module::data>, &ModuleEncoder::dataSection },
    });

    static_assert(
        [] {
            if (SectionWriters.size() != SectionEncodings.size())
                return false;
            for (auto const index: std::views::iota(std::size_t { 0 }, SectionWriters.size()))
                if (SectionWriters[index].anchor != SectionEncodings[index].anchor)
                    return false;
            return true;
        }(),
        "SectionWriters holds one writer per row of SectionEncodings, in its order");

    /// The writer of the section @p anchor names; BeforeFirst names none, and a value past
    /// LastSectionAnchor is no anchor (checked: the table is never read out of its bounds).
    [[nodiscard]] SectionWriter const& sectionWriterOf(SectionAnchor anchor) noexcept
    {
        Require(anchor != SectionAnchor::BeforeFirst);
        Require(anchor <= LastSectionAnchor);
        return SectionWriters[std::to_underlying(anchor) - 1];
    }

    /// A custom section the encoder builds from a module's fields: its name, and its payload's builder.
    struct Synthesizer
    {
        std::string_view name;                                ///< A SynthesizedSections row's name.
        std::optional<Bytes> (ModuleEncoder::*build)() const; ///< Builds the payload, if any.
    };

    /// The SynthesizedSections rows the encoder builds; the name section is built by the text reader.
    constexpr auto Synthesizers = std::to_array<Synthesizer>({
        { .name = BranchHintSectionName, .build = &ModuleEncoder::branchHintSection },
    });

    std::expected<Bytes, EncodeError> ModuleEncoder::encode()
    {
        writeFixed(_out, ModuleMagic, sizeof(ModuleMagic));
        writeFixed(_out, BinaryVersion, sizeof(BinaryVersion));
        encodeCode();
        if (!fits(_code.size()))
            return std::unexpected { EncodeError::ModuleTooLarge };
        writeCustoms(SectionAnchor::BeforeFirst);
        for (auto const index: std::views::iota(std::size_t { 0 }, SectionEncodings.size()))
        {
            if (_isTooLarge)
                return std::unexpected { EncodeError::ModuleTooLarge };
            auto const& encoding = SectionEncodings[index];
            auto const& writer = SectionWriters[index];
            if (encoding.anchor == SectionAnchor::Code)
                writeSynthesized(SynthesizedPlacement::BeforeCode);
            if (writer.isWritten(_module))
            {
                _section.clear();
                (this->*writer.write)(_section);
                writeSection(encoding.id, _section);
            }
            writeCustoms(encoding.anchor);
        }
        writeSynthesized(SynthesizedPlacement::Last);
        if (_isTooLarge)
            return std::unexpected { EncodeError::ModuleTooLarge };
        return std::move(_out);
    }

    bool ModuleEncoder::fits(std::size_t pending) const noexcept
    {
        return pending <= _maxBytes && _out.size() <= _maxBytes - pending;
    }

    void ModuleEncoder::writeSection(uint8_t id, Bytes const& payload)
    {
        // The section's id, then its length: a LEB128 of seven bits per byte, at least one byte.
        auto const headerBytes =
            1
            + std::max(std::size_t { 1 }, (static_cast<std::size_t>(std::bit_width(payload.size())) + 6) / 7);
        // encode() writes no section after one was refused, so only this one's size can refuse it.
        if (!fits(payload.size() + headerBytes))
        {
            _isTooLarge = true;
            return;
        }
        writeByte(_out, id);
        writeLength(_out, payload.size());
        writeBytes(_out, payload);
    }

    void ModuleEncoder::writeCustom(std::string_view name, std::span<std::byte const> payload)
    {
        _section.clear();
        writeName(_section, name);
        writeBytes(_section, payload);
        writeSection(CustomSectionId, _section);
    }

    void ModuleEncoder::writeCustoms(SectionAnchor anchor)
    {
        for (auto const& custom: _module.customs)
            if (custom.after == anchor)
                writeCustom(custom.name, custom.bytes);
    }

    void ModuleEncoder::writeSynthesized(SynthesizedPlacement placement)
    {
        for (auto const& section: SynthesizedSections)
        {
            if (section.placement != placement
                || std::ranges::find(_module.customs, section.name, &CustomSection::name)
                       != _module.customs.end())
                continue;
            auto const synthesizer = std::ranges::find(Synthesizers, section.name, &Synthesizer::name);
            if (synthesizer == Synthesizers.end())
                continue;
            if (auto const payload = (this->*synthesizer->build)())
                writeCustom(section.name, *payload);
        }
    }

    void ModuleEncoder::encodeCode()
    {
        auto nextHint = std::size_t { 0 };
        // Every hint names a defined function: none an imported one, which sort first.
        Require(_hints.empty() || _hints.front().function >= _importedFunctions);
        writeLength(_code, _module.code.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, _module.code.size()))
        {
            // Past the limit the encoding is refused, so the rest is not written.
            if (!fits(_code.size()))
                return;
            writeFunction(
                _code, _module.code[index], _importedFunctions + static_cast<uint32_t>(index), nextHint);
        }
        // ... and none a function past the last defined one.
        Require(nextHint == _hints.size());
    }

    void ModuleEncoder::writeFunction(Bytes& out,
                                      Function const& function,
                                      uint32_t functionIndex,
                                      std::size_t& nextHint)
    {
        // Branch hint offsets count from the locals declaration, where _body starts.
        _body.clear();
        writeVector(_body, std::span { function.locals }, [&](LocalRun const& run) {
            writeUnsigned(_body, run.count);
            writeValueType(_body, run.type);
        });
        for (auto const index: std::views::iota(std::size_t { 0 }, function.body.size()))
        {
            while (nextHint < _hints.size() && _hints[nextHint].function == functionIndex
                   && _hints[nextHint].instruction == index)
                _hintOffsets[nextHint++] = static_cast<uint32_t>(_body.size());
            writeInstruction(_body, function.body[index], &function);
        }
        // Every hint of this function names one of its instructions.
        Require(nextHint == _hints.size() || _hints[nextHint].function != functionIndex);
        writeLength(out, _body.size());
        writeBytes(out, _body);
    }

    Bytes ModuleEncoder::branchHintsOnly()
    {
        auto nextHint = std::size_t { 0 };
        auto scratch = Bytes {};
        while (nextHint < _hints.size())
        {
            auto const function = _hints[nextHint].function;
            // Every hint names a defined function.
            Require(function >= _importedFunctions && function - _importedFunctions < _module.code.size());
            scratch.clear();
            writeFunction(scratch, _module.code[function - _importedFunctions], function, nextHint);
        }
        return branchHintSection().value_or(Bytes {});
    }

    std::optional<Bytes> ModuleEncoder::branchHintSection() const
    {
        if (_hints.empty())
            return std::nullopt;
        // vec(funcidx vec(offset size hint)): one entry per hinted function, in increasing order.
        auto const startsGroup = [this](std::size_t index) {
            return index == 0 || _hints[index].function != _hints[index - 1].function;
        };
        auto groupSizes = std::vector<std::size_t> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, _hints.size()))
        {
            if (startsGroup(index))
                groupSizes.push_back(0);
            ++groupSizes.back();
        }
        auto payload = Bytes {};
        writeLength(payload, groupSizes.size());
        auto groupSize = groupSizes.begin();
        for (auto const index: std::views::iota(std::size_t { 0 }, _hints.size()))
        {
            if (startsGroup(index))
            {
                writeUnsigned(payload, _hints[index].function);
                writeLength(payload, *groupSize++);
            }
            writeUnsigned(payload, _hintOffsets[index]);
            writeUnsigned(payload, BranchHintSize);
            // BranchLikelihood's enumerators are the hint bytes.
            writeByte(payload, std::to_underlying(_hints[index].likelihood));
        }
        return payload;
    }

    void ModuleEncoder::writeConstExpr(Bytes& out, ConstExpr const& expression) const
    {
        for (auto const& instruction: expression)
            writeInstruction(out, instruction, nullptr);
    }

    void ModuleEncoder::writeInstruction(Bytes& out,
                                         Instruction const& instruction,
                                         Function const* function) const
    {
        auto const& row = opcodeInfo(instruction.opcode);
        if (row.prefix == NoPrefix)
            writeByte(out, static_cast<uint8_t>(row.code));
        else
        {
            writeByte(out, row.prefix);
            writeUnsigned(out, row.code);
        }
        writeImmediates(out, instruction, function);
    }

    void ModuleEncoder::writeImmediates(Bytes& out,
                                        Instruction const& instruction,
                                        Function const* function) const
    {
        // One case per immediate shape, never per opcode: the opcode table names the shape. A constant
        // expression's side-table runs were dropped by the decoder, and are written empty.
        switch (opcodeInfo(instruction.opcode).immediate)
        {
            case ImmediateKind::None: return;
            case ImmediateKind::BlockType: writeBlockType(out, instruction); return;
            case ImmediateKind::LabelIndex:
            case ImmediateKind::FunctionIndex:
            case ImmediateKind::LocalIndex:
            case ImmediateKind::GlobalIndex:
            case ImmediateKind::TableIndex:
            case ImmediateKind::ElemIndex:
            case ImmediateKind::DataIndex:
            case ImmediateKind::TypeIndex:
            case ImmediateKind::TagIndex:
            case ImmediateKind::MemoryIndex: writeUnsigned(out, instruction.a); return;
            case ImmediateKind::CallIndirect:
            case ImmediateKind::TableCopy:
            case ImmediateKind::TableInit:
            case ImmediateKind::MemoryCopy:
            case ImmediateKind::MemoryInit:
                writeUnsigned(out, instruction.a);
                writeUnsigned(out, instruction.b);
                return;
            case ImmediateKind::LabelTable: {
                if (function == nullptr)
                {
                    writeLength(out, 0);
                    writeUnsigned(out, 0);
                    return;
                }
                auto const labels = labelsOf(*function, instruction);
                writeVector(
                    out, labels.first(labels.size() - 1), [&](uint32_t label) { writeUnsigned(out, label); });
                writeUnsigned(out, labels.back());
                return;
            }
            case ImmediateKind::SelectTypes:
                writeValueTypes(out,
                                function == nullptr ? std::span<ValueType const> {}
                                                    : selectTypesOf(*function, instruction));
                return;
            case ImmediateKind::TryTable:
                writeBlockType(out, instruction);
                writeVector(out,
                            function == nullptr ? std::span<CatchClause const> {}
                                                : catchClausesOf(*function, instruction),
                            [&](CatchClause const& clause) {
                                writeByte(out, catchKindByte(clause.kind));
                                if (catchesByTag(clause.kind))
                                    writeUnsigned(out, clause.tag);
                                writeUnsigned(out, clause.label);
                            });
                return;
            case ImmediateKind::MemArg: writeMemArg(out, instruction); return;
            case ImmediateKind::MemArgLane:
                writeMemArg(out, instruction);
                writeByte(out, instruction.lane);
                return;
            case ImmediateKind::LaneIndex: writeByte(out, instruction.lane); return;
            case ImmediateKind::V128Const:
            case ImmediateKind::Shuffle: writeBytes(out, v128LiteralOf(_module, instruction)); return;
            case ImmediateKind::I32Const:
                writeSigned(out, static_cast<int32_t>(static_cast<uint32_t>(instruction.c)));
                return;
            case ImmediateKind::I64Const: writeSigned(out, static_cast<int64_t>(instruction.c)); return;
            case ImmediateKind::F32Const: writeFixed(out, instruction.c, sizeof(uint32_t)); return;
            case ImmediateKind::F64Const: writeFixed(out, instruction.c, sizeof(uint64_t)); return;
            case ImmediateKind::HeapType: writeHeapType(out, instruction.type.heap); return;
        }
    }

    void ModuleEncoder::writeBlockType(Bytes& out, Instruction const& instruction) const
    {
        switch (instruction.blockKind)
        {
            case BlockType::Kind::Empty: writeByte(out, EmptyBlockType); return;
            case BlockType::Kind::Value: writeValueType(out, instruction.type); return;
            case BlockType::Kind::TypeIndex: writeSigned(out, instruction.a); return;
        }
    }

    void ModuleEncoder::writeMemArg(Bytes& out, Instruction const& instruction) const
    {
        // The memory index flag only for a memory other than 0, so that 2.0's grammar reads memory 0.
        auto const namesMemory = instruction.b != 0;
        writeUnsigned(out, instruction.a | (namesMemory ? MemArgMemoryIndexFlag : 0U));
        if (namesMemory)
            writeUnsigned(out, instruction.b);
        writeUnsigned(out, instruction.c);
    }

    void ModuleEncoder::writeImport(Bytes& out, Import const& import) const
    {
        writeName(out, importModuleOf(_module, import));
        writeName(out, import.name);
        writeByte(out, externKindByte(import.kind));
        switch (import.kind)
        {
            case ExternKind::Func: writeUnsigned(out, import.typeIndex); return;
            case ExternKind::Table: writeTableType(out, import.table); return;
            case ExternKind::Memory: writeMemoryType(out, import.memory); return;
            case ExternKind::Global: writeGlobalType(out, import.global); return;
            case ExternKind::Tag: writeTagType(out, import.typeIndex); return;
        }
    }

    void ModuleEncoder::writeElementSegment(Bytes& out, ElementSegment const& segment) const
    {
        auto const* indices = std::get_if<std::vector<uint32_t>>(&segment.items);
        auto const items = indices ? ItemEncoding::FunctionIndices : ItemEncoding::Expressions;
        // The first form that expresses the segment is the one of the smallest flags.
        auto const form = std::ranges::find_if(
            ElementForms, [&](ElementForm const& candidate) { return expresses(candidate, segment, items); });
        Require(form != ElementForms.end());
        writeUnsigned(out, static_cast<uint64_t>(form - ElementForms.begin()));
        if (form->mode == SegmentMode::Active)
        {
            if (form->table == IndexEncoding::Explicit)
                writeUnsigned(out, segment.table);
            writeConstExpr(out, segment.offset);
        }
        switch (form->type)
        {
            case ElementTypeEncoding::Implicit: break;
            case ElementTypeEncoding::ElemKind: writeByte(out, FuncElementKind); break;
            case ElementTypeEncoding::RefType: writeValueType(out, segment.type); break;
        }
        if (indices)
            writeVector(out, std::span { *indices }, [&](uint32_t index) { writeUnsigned(out, index); });
        else
            writeVector(out,
                        std::span { std::get<std::vector<ConstExpr>>(segment.items) },
                        [&](ConstExpr const& item) { writeConstExpr(out, item); });
    }

    void ModuleEncoder::writeDataSegment(Bytes& out, DataSegment const& segment) const
    {
        auto const form = std::ranges::find_if(
            DataForms, [&](DataForm const& candidate) { return expresses(candidate, segment); });
        Require(form != DataForms.end());
        writeUnsigned(out, static_cast<uint64_t>(form - DataForms.begin()));
        if (form->mode == SegmentMode::Active)
        {
            if (form->memory == IndexEncoding::Explicit)
                writeUnsigned(out, segment.memory);
            writeConstExpr(out, segment.offset);
        }
        writeLength(out, segment.bytes.size());
        writeBytes(out, segment.bytes);
    }

    void ModuleEncoder::typeSection(Bytes& out) const
    {
        writeVector(out, std::span { _module.types }, [&](FuncType const& type) {
            writeByte(out, FuncTypeForm);
            writeValueTypes(out, type.params);
            writeValueTypes(out, type.results);
        });
    }

    void ModuleEncoder::importSection(Bytes& out) const
    {
        // One plain entry per import, whatever compact grouping it was read from: each repeats its module
        // name, so the section can be far larger than the module, and stops once it no longer fits.
        writeLength(out, _module.imports.size());
        for (auto const& import: _module.imports)
        {
            if (!fits(out.size()))
                return;
            writeImport(out, import);
        }
    }

    void ModuleEncoder::functionSection(Bytes& out) const
    {
        writeVector(
            out, std::span { _module.functions }, [&](uint32_t typeIndex) { writeUnsigned(out, typeIndex); });
    }

    void ModuleEncoder::tableSection(Bytes& out) const
    {
        writeVector(out, std::span { _module.tables }, [&](Table const& table) {
            if (table.init.empty())
            {
                writeTableType(out, table.type);
                return;
            }
            writeByte(out, TableWithInitializer);
            writeByte(out, ReservedZeroByte);
            writeTableType(out, table.type);
            writeConstExpr(out, table.init);
        });
    }

    void ModuleEncoder::memorySection(Bytes& out) const
    {
        writeVector(
            out, std::span { _module.memories }, [&](MemoryType const& type) { writeMemoryType(out, type); });
    }

    void ModuleEncoder::tagSection(Bytes& out) const
    {
        writeVector(
            out, std::span { _module.tags }, [&](Tag const& tag) { writeTagType(out, tag.typeIndex); });
    }

    void ModuleEncoder::globalSection(Bytes& out) const
    {
        writeVector(out, std::span { _module.globals }, [&](Global const& global) {
            writeGlobalType(out, global.type);
            writeConstExpr(out, global.init);
        });
    }

    void ModuleEncoder::exportSection(Bytes& out) const
    {
        writeVector(out, std::span { _module.exports }, [&](Export const& entry) {
            writeName(out, entry.name);
            writeByte(out, externKindByte(entry.kind));
            writeUnsigned(out, entry.index);
        });
    }

    void ModuleEncoder::startSection(Bytes& out) const
    {
        writeUnsigned(out, *_module.start);
    }

    void ModuleEncoder::elementSection(Bytes& out) const
    {
        writeVector(out, std::span { _module.elements }, [&](ElementSegment const& segment) {
            writeElementSegment(out, segment);
        });
    }

    void ModuleEncoder::dataCountSection(Bytes& out) const
    {
        writeUnsigned(out, *_module.dataCount);
    }

    void ModuleEncoder::codeSection(Bytes& out) const
    {
        writeBytes(out, _code);
    }

    void ModuleEncoder::dataSection(Bytes& out) const
    {
        writeVector(out, std::span { _module.data }, [&](DataSegment const& segment) {
            writeDataSegment(out, segment);
        });
    }

    /// @p runs with the runs of no locals dropped and adjacent runs of one type merged.
    [[nodiscard]] std::vector<LocalRun> mergedRuns(std::vector<LocalRun> const& runs)
    {
        auto merged = std::vector<LocalRun> {};
        for (auto const& run: runs)
        {
            if (run.count == 0)
                continue;
            if (!merged.empty() && merged.back().type == run.type
                && run.count <= std::numeric_limits<uint32_t>::max() - merged.back().count)
                merged.back().count += run.count;
            else
                merged.push_back(run);
        }
        return merged;
    }
} // namespace

std::vector<std::byte> encodeModule(Module const& module)
{
    auto bytes = ModuleEncoder { module, std::numeric_limits<std::size_t>::max() }.encode();
    Require(bytes.has_value());
    return std::move(*bytes);
}

std::expected<std::vector<std::byte>, EncodeError> encodeModule(Module const& module, std::size_t maxBytes)
{
    return ModuleEncoder { module, maxBytes }.encode();
}

std::vector<std::byte> encodeBranchHints(Module const& module)
{
    return ModuleEncoder { module, std::numeric_limits<std::size_t>::max() }.branchHintsOnly();
}

SectionAnchor writtenAnchor(Module const& module, SectionAnchor anchor) noexcept
{
    while (anchor != SectionAnchor::BeforeFirst && !sectionWriterOf(anchor).isWritten(module))
        anchor = static_cast<SectionAnchor>(std::to_underlying(anchor) - 1);
    return anchor;
}

Module normalize(Module module)
{
    if (module.dataCount && *module.dataCount == module.data.size() && !codeNamesDataSegment(module))
        module.dataCount.reset();
    for (auto& function: module.code)
        function.locals = mergedRuns(function.locals);
    // A branch hint section the decoder read whole is what Module::branchHints hold, at the offsets of
    // the encoding it came from: it is dropped, and encodeModule writes it again from the hints, at the
    // offsets their instructions take now, before the code section wherever the raw one sat. One the
    // decoder refused stays verbatim: hints are advisory, and a refused section has no meaning to write
    // again, though its stale offsets may name other instructions once the code is re-encoded.
    auto const hintSectionRefused =
        std::ranges::any_of(module.customErrors, [](CustomSectionError const& error) {
            return customSectionOf(error.code) == BranchHintSectionName;
        });
    if (!module.branchHints.empty() && !hintSectionRefused)
        std::erase_if(module.customs,
                      [](CustomSection const& custom) { return custom.name == BranchHintSectionName; });
    // A name section is advisory: one the codec reads is written again canonically, one it refuses
    // stays verbatim.
    for (auto& custom: module.customs)
        if (custom.name == NameSectionName)
            if (auto const names = decodeNameSection(custom.bytes))
                custom.bytes = encodeNameSection(*names);
    for (auto& custom: module.customs)
        custom.after = writtenAnchor(module, custom.after);
    std::ranges::stable_sort(module.customs, {}, &CustomSection::after);
    return module;
}

std::expected<std::vector<std::byte>, DecodeError> canonicalize(std::span<std::byte const> bytes,
                                                                FeatureSet const& features)
{
    return decodeModule(bytes, features).transform([](Module&& module) {
        return encodeModule(normalize(std::move(module)));
    });
}

} // namespace isoline::binary
