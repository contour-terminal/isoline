// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/InstructionReader.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/text/FloatLiteral.hpp>
#include <isoline/text/Instructions.hpp>
#include <isoline/text/IntegerLiteral.hpp>
#include <isoline/text/Keywords.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/LiteralError.hpp>
#include <isoline/text/ModuleReader.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

namespace
{
    using binary::CatchClause;
    using binary::ImmediateKind;
    using binary::Instruction;
    using binary::Opcode;

    /// Reads a literal's text as the bits of one constant.
    using LiteralParser = std::expected<uint64_t, LiteralError> (*)(std::string_view text,
                                                                    TextLimits const& limits);

    /// How a `t.const` instruction reads its literal.
    struct ConstantRow
    {
        ImmediateKind kind;  ///< The constant's immediate kind.
        LiteralParser parse; ///< How its literal is read: its bits, an integer zero-extended.
    };

    /// Every constant immediate kind, and how the text format writes its value (spec §6.3.1, §6.3.2): an
    /// integer may be written signed or not.
    constexpr auto ConstantRows = std::to_array<ConstantRow>({
        { .kind = ImmediateKind::I32Const,
          .parse =
              [](std::string_view text, TextLimits const& limits) {
                  return parseInteger(text, IntegerWidth::I32, IntegerSyntax::Either, limits);
              } },
        { .kind = ImmediateKind::I64Const,
          .parse =
              [](std::string_view text, TextLimits const& limits) {
                  return parseInteger(text, IntegerWidth::I64, IntegerSyntax::Either, limits);
              } },
        { .kind = ImmediateKind::F32Const,
          .parse =
              [](std::string_view text, TextLimits const& limits) {
                  return parseF32(text, limits).transform([](uint32_t bits) { return uint64_t { bits }; });
              } },
        { .kind = ImmediateKind::F64Const,
          .parse = [](std::string_view text, TextLimits const& limits) { return parseF64(text, limits); } },
    });

    /// The bits of one lane of a `v128.const`, as its shape writes it.
    /// @param shape The shape.
    /// @param text The lane's literal.
    /// @param limits The caps on reading text.
    /// @return The lane's bits, an integer truncated to the lane's width.
    std::expected<uint64_t, LiteralError> parseLane(LaneShape const& shape,
                                                    std::string_view text,
                                                    TextLimits const& limits)
    {
        switch (shape.number)
        {
            case LaneNumber::Integer:
                return parseInteger(
                    text, static_cast<IntegerWidth>(shape.laneBytes * 8U), IntegerSyntax::Either, limits);
            case LaneNumber::Float: break;
        }
        if (shape.laneBytes == sizeof(uint32_t))
            return parseF32(text, limits).transform([](uint32_t bits) { return uint64_t { bits }; });
        return parseF64(text, limits);
    }

    /// The keywords of the lists an instruction takes as immediates rather than as folded operands: a
    /// type use's, and typed select's `(result …)`.
    constexpr auto TypeUseKeywords = std::to_array<std::string_view>({ "type", "param", "result" });

    /// Whether @p list is a folded block, loop, if or try_table, whose end its `)` makes.
    /// @param tree The tree.
    /// @param list A list of @p tree.
    bool foldsBlock(SExpressionTree const& tree, NodeId list)
    {
        auto const opcode = opcodeNamed(headKeyword(tree, list));
        if (!opcode)
            return false;
        auto const kind = binary::opcodeInfo(*opcode).immediate;
        return kind == ImmediateKind::BlockType || kind == ImmediateKind::TryTable;
    }

    /// What reading an instruction sequence can hold at most.
    struct SequenceBound
    {
        std::size_t instructions = 1; ///< The most instructions it can make, its final end included.
        std::size_t depth = 0;        ///< The deepest nesting of its lists, annotations aside.
    };

    /// The most instructions @p nodes can make: one per keyword that names an instruction, one per
    /// folded block, whose `)` makes its end, and the final end; exact for a valid sequence, as every
    /// instruction is one of those, and annotations make none. And how deep its lists nest, which bounds
    /// the unfolding work stack.
    /// @param tree The tree.
    /// @param nodes The sequence.
    /// @return The bounds.
    SequenceBound boundOf(SExpressionTree const& tree, InstructionNodes const& nodes)
    {
        auto bound = SequenceBound {};
        if (nodes.first == NoNode)
            return bound;
        auto const all = tree.nodes();
        auto const end = tree.subtreeEnd(nodes.extent == NodeExtent::One ? nodes.first : nodes.list);
        auto openEnds = std::vector<uint32_t> {}; // The end offsets of the lists around the node.
        auto node = nodes.first;
        while (node < end)
        {
            auto const& entry = all[node];
            if (entry.kind == NodeKind::Annotation)
            {
                node = tree.subtreeEnd(node);
                continue;
            }
            while (!openEnds.empty() && openEnds.back() <= entry.offset)
                openEnds.pop_back();
            auto makesInstruction = false;
            if (entry.kind == NodeKind::List)
            {
                makesInstruction = foldsBlock(tree, node);
                openEnds.push_back(entry.offset + entry.length);
                bound.depth = std::max(bound.depth, openEnds.size());
            }
            else
                makesInstruction =
                    entry.tokenKind == TokenKind::Keyword && opcodeNamed(tree.textOf(node)).has_value();
            bound.instructions += makesInstruction ? 1 : 0;
            ++node;
        }
        return bound;
    }
} // namespace

Outcome InstructionReader::read(InstructionNodes nodes)
{
    // Each nested list takes at most one frame, and the sequence itself one more.
    auto const bound = boundOf(_tree, nodes);
    _code.body.reserve(_code.body.size() + bound.instructions);
    _offsets.reserve(bound.instructions);
    _frames.reserve(bound.depth + 1);
    auto const list = nodes.extent == NodeExtent::One ? nodes.first : nodes.list;
    if (nodes.extent == NodeExtent::One)
    {
        if (auto const folded = startFolded(nodes.first); !folded)
            return folded;
    }
    else
        _frames.push_back(Frame { .kind = FrameKind::Body, .list = list, .next = nodes.first });
    while (!_frames.empty())
        if (auto const stepped = step(); !stepped)
            return stepped;
    emit(Instruction { .opcode = Opcode::End }, endOf(list));
    return {};
}

Outcome InstructionReader::step()
{
    auto& frame = _frames.back();
    auto cursor = Cursor { _tree, frame.list, frame.next };
    switch (frame.kind)
    {
        case FrameKind::Body:
        case FrameKind::BlockBody:
        case FrameKind::Part: return stepSequence(frame, cursor);
        case FrameKind::Operands: {
            if (cursor.atEnd())
            {
                auto const done = frame;
                _frames.pop_back();
                return emitDeferred(done);
            }
            // The reference's `plain_instr expr*`: after the immediates, operands are folded only.
            auto const node = cursor.take();
            frame.next = cursor.peek();
            if (!_fields.isList(node))
                return std::unexpected { _fields.unexpected(node) };
            return startFolded(node);
        }
        case FrameKind::IfCondition:
        case FrameKind::IfThen:
        case FrameKind::IfElse: break;
    }
    return stepIf(frame, cursor);
}

Outcome InstructionReader::stepSequence(Frame& frame, Cursor& cursor)
{
    if (cursor.atEnd())
        return finishSequence(frame, cursor);
    if (_fields.isList(cursor.peek()))
    {
        auto const node = cursor.take();
        frame.next = cursor.peek();
        return startFolded(node);
    }
    auto const read = readInstruction(cursor, frame.base);
    frame.next = cursor.peek();
    return read;
}

Outcome InstructionReader::finishSequence(Frame frame, Cursor const& cursor)
{
    // A flat block still open misses its `end`, which the reference expects before the list's `)`.
    if (_blocks.size() != frame.base)
        return std::unexpected { FieldReader::missing(cursor) };
    _frames.pop_back();
    if (frame.kind == FrameKind::BlockBody)
        closeBlock(endOf(frame.list));
    return {};
}

Outcome InstructionReader::stepIf(Frame& frame, Cursor& cursor)
{
    // The reference's `if_`: condition operands, `(then instr*)`, and an optional `(else instr*)`.
    // The if's label binds in its parts only, so its block opens at `(then`, after the condition.
    switch (frame.kind)
    {
        case FrameKind::IfCondition: {
            auto const node = _fields.required(cursor);
            if (!node)
                return std::unexpected { node.error() };
            frame.next = cursor.peek();
            if (!_fields.isList(*node))
                return std::unexpected { _fields.unexpected(*node) };
            if (!_fields.isListOf(*node, "then"))
                return startFolded(*node);
            auto const pending = frame;
            frame.kind = FrameKind::IfThen;
            if (auto const emitted = emitDeferred(pending); !emitted)
                return emitted;
            _frames.push_back(Frame { .kind = FrameKind::Part,
                                      .list = *node,
                                      .next = _fields.bodyOf(*node).peek(),
                                      .base = static_cast<uint32_t>(_blocks.size()) });
            return {};
        }
        case FrameKind::IfThen:
            if (_fields.isListOf(cursor.peek(), "else"))
            {
                auto const node = cursor.take();
                frame.next = cursor.peek();
                frame.kind = FrameKind::IfElse;
                _blocks.back().part = BlockPart::Else;
                emit(Instruction { .opcode = Opcode::Else }, startOf(node));
                _frames.push_back(Frame { .kind = FrameKind::Part,
                                          .list = node,
                                          .next = _fields.bodyOf(node).peek(),
                                          .base = static_cast<uint32_t>(_blocks.size()) });
                return {};
            }
            break;
        case FrameKind::IfElse:
        case FrameKind::Body:
        case FrameKind::BlockBody:
        case FrameKind::Part:
        case FrameKind::Operands: break;
    }
    // An if without an else after its then, or after its else: it ends. step() steps only if frames here.
    Require(frame.kind == FrameKind::IfThen || frame.kind == FrameKind::IfElse);
    if (auto const end = _fields.expectEnd(cursor); !end)
        return end;
    auto const list = frame.list;
    _frames.pop_back();
    closeBlock(endOf(list));
    return {};
}

Outcome InstructionReader::startFolded(NodeId list)
{
    Require(_fields.isList(list)); // Every caller refuses an atom where a folded instruction may stand.
    auto cursor = Cursor { _tree, list };
    auto const head = _fields.required(cursor);
    if (!head)
        return std::unexpected { head.error() };
    auto opcode = _fields.isAtom(*head, TokenKind::Keyword) ? opcodeNamed(_tree.textOf(*head)) : std::nullopt;
    // `else` and `end` are no instructions of their own: a folded block has neither.
    if (!opcode || *opcode == Opcode::Else || *opcode == Opcode::End)
        return std::unexpected { _fields.unexpected(*head) };
    if (*opcode == Opcode::Select && _fields.isListOf(cursor.peek(), "result"))
        opcode = Opcode::SelectTyped;

    if (*opcode == Opcode::If)
    {
        skipImmediates(cursor);
        _frames.push_back(
            Frame { .kind = FrameKind::IfCondition, .opcode = *opcode, .list = list, .next = cursor.peek() });
        return {};
    }
    switch (binary::opcodeInfo(*opcode).immediate)
    {
        case ImmediateKind::BlockType:
        case ImmediateKind::TryTable: {
            // A folded block's own instructions follow its immediates, so it is emitted at once.
            auto instruction = Instruction { .opcode = *opcode };
            if (auto const immediates = readImmediates(cursor, *head, instruction); !immediates)
                return immediates;
            emit(instruction, startOf(list));
            _frames.push_back(Frame { .kind = FrameKind::BlockBody,
                                      .opcode = *opcode,
                                      .list = list,
                                      .next = cursor.peek(),
                                      .base = static_cast<uint32_t>(_blocks.size()) });
            return {};
        }
        default: break;
    }
    skipImmediates(cursor);
    _frames.push_back(
        Frame { .kind = FrameKind::Operands, .opcode = *opcode, .list = list, .next = cursor.peek() });
    return {};
}

Outcome InstructionReader::emitDeferred(Frame const& frame)
{
    auto cursor = Cursor { _tree, frame.list };
    auto const head = cursor.take();
    auto instruction = Instruction { .opcode = frame.opcode };
    if (auto const immediates = readImmediates(cursor, head, instruction); !immediates)
        return immediates;
    // The immediates must be exactly what skipImmediates() stepped over: what follows is an operand.
    if (!cursor.atEnd() && (!_fields.isList(cursor.peek()) || isTypeUseList(cursor.peek())))
        return std::unexpected { _fields.unexpected(cursor.peek()) };
    emit(instruction, startOf(frame.list));
    return {};
}

void InstructionReader::skipImmediates(Cursor& cursor) const noexcept
{
    // Immediates are atoms and the lists of a type use or typed select; operands are any other list.
    while (!cursor.atEnd() && (!_fields.isList(cursor.peek()) || isTypeUseList(cursor.peek())))
        cursor.take();
}

bool InstructionReader::isTypeUseList(NodeId node) const noexcept
{
    return std::ranges::find(TypeUseKeywords, headKeyword(_tree, node)) != TypeUseKeywords.end();
}

void InstructionReader::emit(Instruction const& instruction, uint32_t offset)
{
    _code.body.push_back(instruction);
    _offsets.push_back(offset);
}

std::vector<std::optional<uint32_t>> InstructionReader::instructionsAfter(
    std::span<uint32_t const> offsets) const
{
    struct Start
    {
        uint32_t offset = 0; ///< Where the instruction starts.
        uint32_t index = 0;  ///< Its index in the body.
    };
    // _offsets runs parallel to the instructions this reader appended, the body's last ones.
    auto const base = _code.body.size() - _offsets.size();
    auto starts = std::vector<Start> {};
    starts.reserve(_offsets.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, _offsets.size()))
    {
        auto const opcode = _code.body[base + index].opcode;
        if (opcode != Opcode::Else && opcode != Opcode::End)
            starts.push_back(
                Start { .offset = _offsets[index], .index = static_cast<uint32_t>(base + index) });
    }
    std::ranges::sort(starts, {}, &Start::offset);
    auto instructions = std::vector<std::optional<uint32_t>> {};
    instructions.reserve(offsets.size());
    for (auto const offset: offsets)
    {
        auto const found = std::ranges::lower_bound(starts, offset, {}, &Start::offset);
        instructions.push_back(found == starts.end() ? std::nullopt : std::optional { found->index });
    }
    return instructions;
}

uint32_t InstructionReader::startOf(NodeId node) const noexcept
{
    return _tree.nodes()[node].offset;
}

uint32_t InstructionReader::endOf(NodeId node) const noexcept
{
    auto const& entry = _tree.nodes()[node];
    return entry.offset + entry.length - 1;
}

Outcome InstructionReader::readInstruction(Cursor& cursor, uint32_t base)
{
    auto const node = cursor.take();
    auto opcode = _fields.isAtom(node, TokenKind::Keyword) ? opcodeNamed(_tree.textOf(node)) : std::nullopt;
    if (!opcode)
        return std::unexpected { _fields.unexpected(node) };
    if (*opcode == Opcode::Else)
        return readElse(cursor, node, base);
    if (*opcode == Opcode::End)
        return readEnd(cursor, node, base);
    // `select` with `(result …)` lists is typed select, which the text format spells alike.
    if (*opcode == Opcode::Select && _fields.isListOf(cursor.peek(), "result"))
        opcode = Opcode::SelectTyped;

    auto instruction = Instruction { .opcode = *opcode };
    if (auto const immediates = readImmediates(cursor, node, instruction); !immediates)
        return immediates;
    emit(instruction, startOf(node));
    return {};
}

Outcome InstructionReader::readImmediates(Cursor& cursor, NodeId node, Instruction& instruction)
{
    // One case per immediate shape, never per opcode, as the decoder and the encoder read them: the
    // opcode table names the shape, and Instructions.hpp's rows give the text grammar of the shapes that
    // are indices only.
    switch (auto const kind = binary::opcodeInfo(instruction.opcode).immediate)
    {
        case ImmediateKind::None: return {};
        case ImmediateKind::BlockType:
        case ImmediateKind::TryTable: return readBlock(cursor, node, instruction);
        case ImmediateKind::LabelIndex:
        case ImmediateKind::FunctionIndex:
        case ImmediateKind::TypeIndex:
        case ImmediateKind::TagIndex:
        case ImmediateKind::LocalIndex:
        case ImmediateKind::GlobalIndex:
        case ImmediateKind::TableIndex:
        case ImmediateKind::TableCopy:
        case ImmediateKind::TableInit:
        case ImmediateKind::ElemIndex:
        case ImmediateKind::MemoryIndex:
        case ImmediateKind::MemoryCopy:
        case ImmediateKind::MemoryInit:
        case ImmediateKind::DataIndex: {
            auto const* row = indexImmediateOf(kind);
            Require(row != nullptr); // Every kind of this case is a row of IndexImmediates.
            return readIndices(cursor, *row, instruction);
        }
        case ImmediateKind::LabelTable: return readLabelTable(cursor, instruction);
        case ImmediateKind::CallIndirect: return readCallIndirect(cursor, instruction);
        case ImmediateKind::SelectTypes: return readSelectTypes(cursor, instruction);
        case ImmediateKind::MemArg: return readMemArg(cursor, instruction);
        case ImmediateKind::MemArgLane: return readMemArgLane(cursor, instruction);
        case ImmediateKind::LaneIndex: return readLane(cursor, instruction);
        case ImmediateKind::I32Const:
        case ImmediateKind::I64Const:
        case ImmediateKind::F32Const:
        case ImmediateKind::F64Const: return readConstant(cursor, instruction);
        case ImmediateKind::V128Const: return readVector(cursor, node, instruction);
        case ImmediateKind::Shuffle: return readShuffle(cursor, node, instruction);
        case ImmediateKind::HeapType: break;
    }
    return readHeapType(cursor, instruction);
}

Outcome InstructionReader::readIndices(Cursor& cursor, IndexImmediate const& row, Instruction& instruction)
{
    auto const assign = [&](IndexOperand const& operand, NodeId node) -> Outcome {
        return readIndex(operand.space, node).transform([&](uint32_t index) {
            (operand.field == InstructionField::A ? instruction.a : instruction.b) = index;
        });
    };
    auto const& [first, second] = row.operands;
    switch (row.arity)
    {
        case IndexArity::One:
            return _fields.required(cursor).and_then([&](NodeId node) { return assign(first, node); });
        case IndexArity::OptionalOne:
            if (!_fields.isIndex(cursor.peek()))
                return {};
            return assign(first, cursor.take());
        case IndexArity::NoneOrTwo:
            if (!_fields.isIndex(cursor.peek()))
                return {};
            if (auto const read = assign(first, cursor.take()); !read)
                return read;
            return _fields.required(cursor).and_then([&](NodeId node) { return assign(second, node); });
        case IndexArity::OneOrTwo: break;
    }
    // `x? y`: one index alone is the second.
    auto const node = _fields.required(cursor);
    if (!node)
        return std::unexpected { node.error() };
    if (!_fields.isIndex(cursor.peek()))
        return assign(second, *node);
    if (auto const read = assign(first, *node); !read)
        return read;
    return assign(second, cursor.take());
}

std::expected<uint32_t, TextError> InstructionReader::readIndex(IndexSpace space, NodeId node) const
{
    if (space == IndexSpace::Label)
        return resolveLabel(node);
    if (space == IndexSpace::Local)
        return _fields.lookup(_locals, space, node);
    return _fields.lookup(space, node);
}

Outcome InstructionReader::readLabelTable(Cursor& cursor, Instruction& instruction)
{
    auto const offset = _code.brTableLabels.size();
    auto const append = [&](NodeId node) -> Outcome {
        return resolveLabel(node).transform([&](uint32_t label) { _code.brTableLabels.push_back(label); });
    };
    // At least one label: the last one is the default.
    if (auto const first = _fields.required(cursor).and_then(append); !first)
        return first;
    while (_fields.isIndex(cursor.peek()))
        if (auto const label = append(cursor.take()); !label)
            return label;
    instruction.a = static_cast<uint32_t>(offset);
    instruction.b = static_cast<uint32_t>(_code.brTableLabels.size() - offset);
    return {};
}

Outcome InstructionReader::readCallIndirect(Cursor& cursor, Instruction& instruction)
{
    if (_fields.isIndex(cursor.peek()))
    {
        auto const table = _fields.lookup(IndexSpace::Table, cursor.take());
        if (!table)
            return std::unexpected { table.error() };
        instruction.b = *table;
    }
    auto const use = _fields.readTypeUse(cursor, ParamNames::Refused);
    if (!use)
        return std::unexpected { use.error() };
    return _fields.resolve(*use).transform([&](uint32_t type) { instruction.a = type; });
}

Outcome InstructionReader::readSelectTypes(Cursor& cursor, Instruction& instruction)
{
    auto const offset = _code.selectTypes.size();
    while (_fields.isListOf(cursor.peek(), "result"))
        if (auto const types = _fields.readValueTypes(cursor.take(), _code.selectTypes); !types)
            return types;
    instruction.a = static_cast<uint32_t>(offset);
    instruction.b = static_cast<uint32_t>(_code.selectTypes.size() - offset);
    return {};
}

Outcome InstructionReader::readMemArg(Cursor& cursor, Instruction& instruction) const
{
    if (_fields.isIndex(cursor.peek()))
    {
        auto const memory = _fields.lookup(IndexSpace::Memory, cursor.take());
        if (!memory)
            return std::unexpected { memory.error() };
        instruction.b = *memory;
    }
    return readOffsetAndAlignment(cursor, instruction);
}

Outcome InstructionReader::readMemArgLane(Cursor& cursor, Instruction& instruction) const
{
    // The reference's lane_imms: a leading `nat` is the memory index only when more immediates follow
    // it; alone, it is the lane.
    auto const isMemoryArgument = [&](NodeId node) {
        return _fields.isAtom(node, TokenKind::Keyword)
               && (_tree.textOf(node).starts_with(OffsetPrefix)
                   || _tree.textOf(node).starts_with(AlignmentPrefix));
    };
    auto const next = cursor.peekNext();
    auto const namesMemory =
        _fields.isAtom(cursor.peek(), TokenKind::Identifier)
        || (_fields.isNatural(cursor.peek()) && (_fields.isNatural(next) || isMemoryArgument(next)));
    if (namesMemory)
    {
        auto const memory = _fields.lookup(IndexSpace::Memory, cursor.take());
        if (!memory)
            return std::unexpected { memory.error() };
        instruction.b = *memory;
    }
    if (auto const memArg = readOffsetAndAlignment(cursor, instruction); !memArg)
        return memArg;
    return readLane(cursor, instruction);
}

Outcome InstructionReader::readOffsetAndAlignment(Cursor& cursor, Instruction& instruction) const
{
    auto const isPrefixed = [&](std::string_view prefix) {
        return _fields.isAtom(cursor.peek(), TokenKind::Keyword)
               && _tree.textOf(cursor.peek()).starts_with(prefix);
    };
    auto const* access = binary::memoryAccessOf(instruction.opcode);
    Require(access != nullptr); // Every MemArg and MemArgLane instruction is a row of MemoryAccessTable.
    instruction.a = access->naturalAlignment();
    // An offset is a u64 whatever the memory's address type; the validator refuses a too large one.
    if (isPrefixed(OffsetPrefix))
    {
        auto const offset = _fields.readNaturalAfter(cursor.take(), OffsetPrefix, IntegerWidth::I64);
        if (!offset)
            return std::unexpected { offset.error() };
        instruction.c = *offset;
    }
    if (isPrefixed(AlignmentPrefix))
    {
        auto const node = cursor.take();
        auto const alignment = _fields.readNaturalAfter(node, AlignmentPrefix, IntegerWidth::I64);
        if (!alignment)
            return std::unexpected { alignment.error() };
        if (!std::has_single_bit(*alignment))
            return std::unexpected { TextError { TextErrorCode::AlignmentNotPowerOfTwo,
                                                 _tree.positionOf(node) } };
        instruction.a = static_cast<uint32_t>(std::countr_zero(*alignment));
    }
    return {};
}

Outcome InstructionReader::readLane(Cursor& cursor, Instruction& instruction) const
{
    return _fields.required(cursor)
        .and_then([&](NodeId node) { return _fields.readNatural(node, IntegerWidth::I8); })
        .transform([&](uint64_t lane) { instruction.lane = static_cast<uint8_t>(lane); });
}

Outcome InstructionReader::readConstant(Cursor& cursor, Instruction& instruction) const
{
    auto const literal = _fields.required(cursor);
    if (!literal)
        return std::unexpected { literal.error() };
    if (!_fields.isNumber(*literal))
        return std::unexpected { _fields.unexpected(*literal) };
    auto const kind = binary::opcodeInfo(instruction.opcode).immediate;
    auto const row = std::ranges::find(ConstantRows, kind, &ConstantRow::kind);
    Require(row != ConstantRows.end()); // Every constant immediate kind is a row of ConstantRows.
    auto const value = row->parse(_tree.textOf(*literal), _fields.limits());
    if (value)
    {
        instruction.c = *value;
        return {};
    }
    // The reference's `num`: a number this constant cannot hold, a float for an integer included, is
    // out of range, at the literal.
    if (value.error() == LiteralError::TooLong)
        return std::unexpected { TextError { TextErrorCode::NumberTooLong, _tree.positionOf(*literal) } };
    return std::unexpected { TextError { TextErrorCode::ConstantOutOfRange, _tree.positionOf(*literal) } };
}

std::expected<std::size_t, TextError> InstructionReader::countNumbers(Cursor const& cursor) const
{
    // The reference reads a list of numbers here, and its lexer refuses a malformed one before that: a
    // reserved token, or a keyword it does not know, such as `nan:1`, is an unknown operator before any
    // lane is counted.
    auto probe = cursor;
    auto count = std::size_t { 0 };
    while (!probe.atEnd())
    {
        if (_fields.isAtom(probe.peek(), TokenKind::Reserved))
            return std::unexpected { _fields.unexpected(probe.peek()) };
        if (!_fields.isNumber(probe.peek()))
        {
            // Only an unknown keyword builds its error, whose position costs a scan of the source.
            if (_fields.isAtom(probe.peek(), TokenKind::Keyword)
                && !isKnownKeyword(_tree.textOf(probe.peek())))
                if (auto error = unexpectedKeyword(_tree, probe.peek());
                    error.code == TextErrorCode::UnknownOperator)
                    return std::unexpected { std::move(error) };
            break;
        }
        probe.take();
        ++count;
    }
    return count;
}

Outcome InstructionReader::readVector(Cursor& cursor, NodeId node, Instruction& instruction)
{
    auto const shapeNode = _fields.required(cursor);
    if (!shapeNode)
        return std::unexpected { shapeNode.error() };
    auto const shape = _fields.isAtom(*shapeNode, TokenKind::Keyword)
                           ? std::ranges::find(LaneShapes, _tree.textOf(*shapeNode), &LaneShape::keyword)
                           : LaneShapes.end();
    if (shape == LaneShapes.end())
        return std::unexpected { _fields.unexpected(*shapeNode) };
    auto const count = countNumbers(cursor);
    if (!count)
        return std::unexpected { count.error() };
    // The reference's `vec` reports both the lane count and a lane's range at the instruction.
    if (*count != shape->laneCount())
        return std::unexpected { TextError { TextErrorCode::WrongLaneCount, _tree.positionOf(node) } };

    auto bytes = std::array<std::byte, 16> {};
    for (auto const lane: std::views::iota(std::size_t { 0 }, shape->laneCount()))
    {
        auto const literal = cursor.take();
        auto const bits = parseLane(*shape, _tree.textOf(literal), _fields.limits());
        if (!bits)
            return std::unexpected {
                bits.error() == LiteralError::TooLong
                    ? TextError { TextErrorCode::NumberTooLong, _tree.positionOf(literal) }
                    : TextError { TextErrorCode::ConstantOutOfRange, _tree.positionOf(node) }
            };
        // Little endian: the lowest lane byte first.
        for (auto const byte: std::views::iota(std::size_t { 0 }, std::size_t { shape->laneBytes }))
            bytes.at((lane * shape->laneBytes) + byte) = static_cast<std::byte>(*bits >> (8U * byte));
    }
    instruction.a = _fields.addV128Literal(bytes);
    return {};
}

Outcome InstructionReader::readShuffle(Cursor& cursor, NodeId node, Instruction& instruction)
{
    auto const count = countNumbers(cursor);
    if (!count)
        return std::unexpected { count.error() };
    auto bytes = std::array<std::byte, 16> {};
    // The reference's shuffle_lit reports the lane count and each lane, a nat8, at the instruction.
    if (*count != bytes.size())
        return std::unexpected { TextError { TextErrorCode::InvalidLaneLength, _tree.positionOf(node) } };
    for (auto& byte: bytes)
    {
        auto const literal = cursor.take();
        auto const lane =
            parseInteger(_tree.textOf(literal), IntegerWidth::I8, IntegerSyntax::Unsigned, _fields.limits());
        if (!lane)
            return std::unexpected {
                lane.error() == LiteralError::TooLong
                    ? TextError { TextErrorCode::NumberTooLong, _tree.positionOf(literal) }
                    : TextError { TextErrorCode::ConstantOutOfRange, _tree.positionOf(node), "i8" }
            };
        byte = static_cast<std::byte>(*lane);
    }
    instruction.a = _fields.addV128Literal(bytes);
    return {};
}

Outcome InstructionReader::readHeapType(Cursor& cursor, Instruction& instruction) const
{
    return _fields.required(cursor)
        .and_then([&](NodeId node) { return _fields.readHeapType(node); })
        .transform([&](binary::HeapType const& heap) {
            instruction.type = binary::ValueType { .kind = binary::ValueKind::Ref,
                                                   .nullable = binary::Nullable::Yes,
                                                   .heap = heap };
        });
}

// -------------------------------------------------------------------------------------------------
// Blocks and labels

Outcome InstructionReader::readBlock(Cursor& cursor, NodeId node, Instruction& instruction)
{
    auto const label = _fields.isAtom(cursor.peek(), TokenKind::Identifier) ? cursor.take() : NoNode;
    // `block $l? (@name "…")?`: a name follows the label, or the keyword of a block without one.
    if (auto const namedAfter = label != NoNode ? label : node; _fields.isNamedAfter(namedAfter))
        _labelAnnotations.push_back(LabelAnnotation { .index = _blockCount, .namedAfter = namedAfter });
    if (auto const type = readBlockType(cursor, instruction); !type)
        return type;
    // A try_table's catch clauses branch to labels outside it: they are read before its own label binds.
    if (binary::opcodeInfo(instruction.opcode).immediate == ImmediateKind::TryTable)
        if (auto const clauses = readCatchClauses(cursor, instruction); !clauses)
            return clauses;
    return openBlock(instruction.opcode, label);
}

Outcome InstructionReader::readBlockType(Cursor& cursor, Instruction& instruction)
{
    auto const use = _fields.readTypeUse(cursor, ParamNames::Refused);
    if (!use)
        return std::unexpected { use.error() };
    auto const& [params, results] = use->inlineType;
    // No parameters and at most one result, without `(type x)`, is the short form (parser.mly's block).
    if (use->explicitNode == NoNode && params.empty() && results.size() <= 1)
    {
        instruction.blockKind =
            results.empty() ? binary::BlockType::Kind::Empty : binary::BlockType::Kind::Value;
        if (!results.empty())
            instruction.type = results.front();
        return {};
    }
    return _fields.resolve(*use).transform([&](uint32_t type) {
        instruction.blockKind = binary::BlockType::Kind::TypeIndex;
        instruction.a = type;
    });
}

Outcome InstructionReader::readCatchClauses(Cursor& cursor, Instruction& instruction)
{
    auto const offset = _code.catchClauses.size();
    while (_fields.isList(cursor.peek()))
    {
        auto const row =
            std::ranges::find(CatchKeywords, headKeyword(_tree, cursor.peek()), &CatchKeyword::keyword);
        if (row == CatchKeywords.end())
            break;
        auto inner = _fields.bodyOf(cursor.take());
        auto clause = CatchClause { .kind = row->kind, .tag = 0, .label = 0 };
        if (binary::catchesByTag(row->kind))
        {
            auto const tag = _fields.required(inner).and_then(
                [&](NodeId node) { return _fields.lookup(IndexSpace::Tag, node); });
            if (!tag)
                return std::unexpected { tag.error() };
            clause.tag = *tag;
        }
        auto const label = _fields.required(inner).and_then([&](NodeId node) { return resolveLabel(node); });
        if (!label)
            return std::unexpected { label.error() };
        clause.label = *label;
        if (auto const end = _fields.expectEnd(inner); !end)
            return end;
        _code.catchClauses.push_back(clause);
    }
    instruction.c = offset;
    instruction.b = static_cast<uint32_t>(_code.catchClauses.size() - offset);
    return {};
}

Outcome InstructionReader::openBlock(Opcode opener, NodeId label)
{
    auto block = OpenBlock { .opener = opener, .part = BlockPart::Body, .label = nullptr };
    if (label != NoNode)
    {
        auto name = _fields.identifierOf(label);
        if (!name)
            return std::unexpected { std::move(name.error()) };
        _labelNames.push_back(binary::NameAssoc { .index = _blockCount, .name = *name });
        auto& entry = *_labels.try_emplace(std::move(*name)).first;
        entry.second.push_back(static_cast<uint32_t>(_blocks.size()));
        block.label = &entry;
    }
    ++_blockCount;
    _blocks.push_back(block);
    return {};
}

Outcome InstructionReader::readElse(Cursor& cursor, NodeId node, uint32_t base)
{
    // Only a flat if opened in this sequence takes a flat else: a folded block's parts have none.
    if (_blocks.size() <= base || _blocks.back().opener != Opcode::If
        || _blocks.back().part == BlockPart::Else)
        return std::unexpected { _fields.unexpected(node) };
    if (auto const label = checkEndLabel(cursor, _blocks.back()); !label)
        return label;
    _blocks.back().part = BlockPart::Else;
    emit(Instruction { .opcode = Opcode::Else }, startOf(node));
    return {};
}

Outcome InstructionReader::readEnd(Cursor& cursor, NodeId node, uint32_t base)
{
    // Only a flat block opened in this sequence takes a flat end: a folded one ends at its `)`.
    if (_blocks.size() <= base)
        return std::unexpected { _fields.unexpected(node) };
    if (auto const label = checkEndLabel(cursor, _blocks.back()); !label)
        return label;
    closeBlock(startOf(node));
    return {};
}

void InstructionReader::closeBlock(uint32_t offset)
{
    if (auto* const label = _blocks.back().label; label != nullptr)
        label->second.pop_back();
    _blocks.pop_back();
    emit(Instruction { .opcode = Opcode::End }, offset);
}

Outcome InstructionReader::checkEndLabel(Cursor& cursor, OpenBlock const& block) const
{
    // `else $l` and `end $l` repeat the block's label; one that names another is a mismatch.
    if (!_fields.isAtom(cursor.peek(), TokenKind::Identifier))
        return {};
    auto const node = cursor.take();
    auto const name = _fields.identifierOf(node);
    if (!name)
        return std::unexpected { name.error() };
    if (block.label == nullptr || block.label->first != *name)
        return std::unexpected { TextError { TextErrorCode::MismatchingLabel, _tree.positionOf(node) } };
    return {};
}

std::expected<uint32_t, TextError> InstructionReader::resolveLabel(NodeId node) const
{
    // A numeric label is kept as written: the validator refuses one beyond the blocks around it.
    if (!_fields.isAtom(node, TokenKind::Identifier))
        return _fields.readNatural(node, IntegerWidth::I32).transform([](uint64_t label) {
            return static_cast<uint32_t>(label);
        });
    auto const name = _fields.identifierOf(node);
    if (!name)
        return std::unexpected { name.error() };
    auto const found = _labels.find(*name);
    if (found == _labels.end() || found->second.empty())
        return std::unexpected { TextError { TextErrorCode::UnknownLabel, _tree.positionOf(node) } };
    return static_cast<uint32_t>(_blocks.size() - 1 - found->second.back());
}

} // namespace isoline::text
