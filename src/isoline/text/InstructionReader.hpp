// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/text/FieldReader.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>

namespace isoline::text
{

/// Reads the instructions of one function body or constant expression (spec §6.5) into a
/// binary::Function: its body, ending with Opcode::End, and the side tables of br_table, typed select
/// and try_table.
///
/// Every instruction is read by its opcode's binary::ImmediateKind, in one exhaustive switch, through
/// the text grammar rows of Instructions.hpp. Flat block structure (`block … end`) takes no
/// parenthesized level, so it reads at any depth. Folded instructions (spec §6.5.13) are unfolded by an
/// explicit work stack, never by recursion: a folded operator's operands are emitted first, then the
/// operator, whose immediates are read when it is emitted, so implicit types, side tables and v128
/// literals follow the body's order, as wasm-tools assigns them. Labels resolve through one stack of
/// depths per label name, in an ordered map: each label reference is O(log n) in the names, whatever
/// the nesting.
///
/// The body is reserved once, for the most instructions its nodes can make, so reading a body never
/// holds a vector grown to twice what it uses.
class InstructionReader
{
  public:
    /// A block whose label an `@name` annotation may name.
    struct LabelAnnotation
    {
        uint32_t index = 0;         ///< The block's label index (takeLabelNames()).
        NodeId namedAfter = NoNode; ///< The node its `@name` annotations follow: its label, or its keyword.
    };

    /// A reader that appends to @p code.
    /// @param fields The module's field reader, for its identifiers, types and literals.
    /// @param locals The identifiers of the function's parameters and locals; none for a constant
    ///               expression.
    /// @param code Receives the instructions and side tables; outlives the reader.
    InstructionReader(FieldReader& fields, Symbols const& locals, binary::Function& code) noexcept:
        _fields { fields }, _tree { fields.tree() }, _locals { locals }, _code { code }
    {
    }

    InstructionReader(InstructionReader const&) = delete;
    InstructionReader(InstructionReader&&) = delete;
    InstructionReader& operator=(InstructionReader const&) = delete;
    InstructionReader& operator=(InstructionReader&&) = delete;
    ~InstructionReader() = default;

    /// Reads an instruction sequence and appends Opcode::End after it.
    /// @param nodes The sequence.
    /// @return Nothing, or the first error.
    [[nodiscard]] Outcome read(InstructionNodes nodes);

    /// The names of the labels read, for the name section: each block's index counts every block,
    /// loop, if and try_table of the sequence in source order, named or not (as wasm-tools does).
    /// @return The names, in block order; the reader keeps none.
    [[nodiscard]] std::vector<binary::NameAssoc> takeLabelNames() noexcept { return std::move(_labelNames); }

    /// Where each instruction read is in the source, parallel to the body: a flat instruction's keyword,
    /// a folded operator's `(`, and the `)` of the list a synthesized `end` closes (a folded block's, or
    /// the sequence's own for its final end).
    /// @return One byte offset per instruction appended to the body.
    [[nodiscard]] std::span<uint32_t const> sourceOffsets() const noexcept { return _offsets; }

    /// The blocks read that an `@name` annotation directly follows, for the name section.
    /// @return The blocks, in block order; the reader keeps none.
    [[nodiscard]] std::vector<LabelAnnotation> takeLabelAnnotations() noexcept
    {
        return std::move(_labelAnnotations);
    }

    /// The instruction each of some source offsets precedes: the first one read, in source order, that
    /// starts at or after the offset, an else or end aside (they are no instructions of the reference's
    /// AST), as handler_branch_hint.ml's get_inst_idx finds the instruction a branch hint names. A
    /// folded instruction starts at its `(`. O((n + k) log n) for n instructions and k offsets.
    /// @param offsets The byte offsets.
    /// @return For each offset, the index of its instruction in the body, or nothing when none follows.
    [[nodiscard]] std::vector<std::optional<uint32_t>> instructionsAfter(
        std::span<uint32_t const> offsets) const;

  private:
    /// The labels of one name: the depth of each open block that binds it, innermost last.
    using LabelEntry = std::pair<std::string const, std::vector<uint32_t>>;

    /// Which part of a block the reader is in: an if's then part, until its else.
    enum class BlockPart : uint8_t
    {
        Body, ///< A block's, loop's or try_table's body, or an if's then part.
        Else, ///< An if's else part.
    };

    /// A block whose `end` is still to come.
    struct OpenBlock
    {
        binary::Opcode opener = binary::Opcode::Block; ///< Its block, loop, if or try_table.
        BlockPart part = BlockPart::Body;              ///< The part being read.
        LabelEntry* label = nullptr;                   ///< The entry of its label name; null without one.
    };

    /// What one level of the unfolding work stack is reading.
    enum class FrameKind : uint8_t
    {
        Body,        ///< The sequence read() was given.
        BlockBody,   ///< A folded block's, loop's or try_table's instructions; its end closes the block.
        Part,        ///< A folded if's `(then …)` or `(else …)` instructions.
        Operands,    ///< A folded operator's operands; the operator follows them.
        IfCondition, ///< A folded if's condition operands, up to its `(then …)`.
        IfThen,      ///< A folded if after its then part: an `(else …)`, or its end.
        IfElse,      ///< A folded if after its else part: its end.
    };

    /// One level of the unfolding work stack: a list, and the node of it to read next. Kept small, as
    /// folded instructions nest as deep as the source's lists.
    struct Frame
    {
        FrameKind kind = FrameKind::Body;            ///< What the level reads.
        binary::Opcode opcode = binary::Opcode::Nop; ///< Operands, IfCondition: the deferred operator.
        NodeId list = NoNode;                        ///< The list read.
        NodeId next = NoNode;                        ///< Its next node to read, or NoNode at its end.
        uint32_t base = 0;                           ///< Sequences: the blocks open when it began.
    };

    [[nodiscard]] Outcome step();
    [[nodiscard]] Outcome stepSequence(Frame& frame, Cursor& cursor);
    [[nodiscard]] Outcome stepIf(Frame& frame, Cursor& cursor);
    [[nodiscard]] Outcome finishSequence(Frame frame, Cursor const& cursor);
    [[nodiscard]] Outcome startFolded(NodeId list);
    [[nodiscard]] Outcome emitDeferred(Frame const& frame);
    [[nodiscard]] Outcome readInstruction(Cursor& cursor, uint32_t base);
    void skipImmediates(Cursor& cursor) const noexcept;
    [[nodiscard]] bool isTypeUseList(NodeId node) const noexcept;
    void emit(binary::Instruction const& instruction, uint32_t offset);
    [[nodiscard]] uint32_t startOf(NodeId node) const noexcept;
    [[nodiscard]] uint32_t endOf(NodeId node) const noexcept;
    [[nodiscard]] Outcome readImmediates(Cursor& cursor, NodeId node, binary::Instruction& instruction);
    [[nodiscard]] Outcome readIndices(Cursor& cursor,
                                      IndexImmediate const& row,
                                      binary::Instruction& instruction);
    [[nodiscard]] std::expected<uint32_t, TextError> readIndex(IndexSpace space, NodeId node) const;
    [[nodiscard]] Outcome readLabelTable(Cursor& cursor, binary::Instruction& instruction);
    [[nodiscard]] Outcome readCallIndirect(Cursor& cursor, binary::Instruction& instruction);
    [[nodiscard]] Outcome readSelectTypes(Cursor& cursor, binary::Instruction& instruction);
    [[nodiscard]] Outcome readMemArg(Cursor& cursor, binary::Instruction& instruction) const;
    [[nodiscard]] Outcome readMemArgLane(Cursor& cursor, binary::Instruction& instruction) const;
    [[nodiscard]] Outcome readOffsetAndAlignment(Cursor& cursor, binary::Instruction& instruction) const;
    [[nodiscard]] Outcome readLane(Cursor& cursor, binary::Instruction& instruction) const;
    [[nodiscard]] Outcome readConstant(Cursor& cursor, binary::Instruction& instruction) const;
    [[nodiscard]] Outcome readVector(Cursor& cursor, NodeId node, binary::Instruction& instruction);
    [[nodiscard]] Outcome readShuffle(Cursor& cursor, NodeId node, binary::Instruction& instruction);
    [[nodiscard]] std::expected<std::size_t, TextError> countNumbers(Cursor const& cursor) const;
    [[nodiscard]] Outcome readHeapType(Cursor& cursor, binary::Instruction& instruction) const;

    // Blocks and labels.
    [[nodiscard]] Outcome readBlock(Cursor& cursor, NodeId node, binary::Instruction& instruction);
    [[nodiscard]] Outcome readBlockType(Cursor& cursor, binary::Instruction& instruction);
    [[nodiscard]] Outcome readCatchClauses(Cursor& cursor, binary::Instruction& instruction);
    [[nodiscard]] Outcome openBlock(binary::Opcode opener, NodeId label);
    [[nodiscard]] Outcome readElse(Cursor& cursor, NodeId node, uint32_t base);
    [[nodiscard]] Outcome readEnd(Cursor& cursor, NodeId node, uint32_t base);
    void closeBlock(uint32_t offset);
    [[nodiscard]] Outcome checkEndLabel(Cursor& cursor, OpenBlock const& block) const;
    [[nodiscard]] std::expected<uint32_t, TextError> resolveLabel(NodeId node) const;

    FieldReader& _fields;           ///< The module's field reader.
    SExpressionTree const& _tree;   ///< The tree the instructions are in.
    Symbols const& _locals;         ///< The function's parameters and locals.
    binary::Function& _code;        ///< Where the instructions go.
    std::vector<OpenBlock> _blocks; ///< The blocks open around the next instruction, innermost last.
    std::vector<Frame> _frames;     ///< The unfolding work stack, innermost last.
    std::vector<uint32_t> _offsets; ///< The source offset of each instruction appended to the body.

    /// Each label name's depths. Its entries never move: OpenBlock points at them.
    std::map<std::string, std::vector<uint32_t>, std::less<>> _labels;
    uint32_t _blockCount = 0;                       ///< The blocks opened so far: the next one's label index.
    std::vector<binary::NameAssoc> _labelNames;     ///< The names of the labels read.
    std::vector<LabelAnnotation> _labelAnnotations; ///< The blocks an `@name` annotation follows.
};

} // namespace isoline::text
