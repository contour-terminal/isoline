// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>

namespace isoline::text
{

/// An instruction whose OpcodeTable text is internal: the binary format tells it apart from another
/// instruction that the text format spells alike.
struct InternalTextName
{
    binary::Opcode opcode; ///< The instruction.
    std::string_view text; ///< How the text format spells it.
};

/// Every instruction whose OpcodeTable text the text format does not spell, with the text it does.
/// opcodeNamed() never finds their table text; textName() gives the text here.
///
/// Typed select's table text is "select_t"; the text format writes it `select (result t)`: `select`
/// followed by a `(result …)` list reads as typed select.
inline constexpr auto InternalTextNames = std::to_array<InternalTextName>({
    { .opcode = binary::Opcode::SelectTyped, .text = "select" },
});

/// An index space of the text format: what an identifier binds into, and what an index is looked up in.
enum class IndexSpace : uint8_t
{
    Type,
    Function,
    Table,
    Memory,
    Global,
    Tag,
    Element,
    Data,
    Local, ///< A function's parameters and locals: one space per function.
    Label, ///< The blocks around an instruction, innermost first: one space per instruction.
};

/// The last IndexSpace enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastIndexSpace = IndexSpace::Label;

/// Which field of binary::Instruction an index immediate goes to.
enum class InstructionField : uint8_t
{
    A, ///< binary::Instruction::a.
    B, ///< binary::Instruction::b.
};

/// One index an instruction takes: the space it is looked up in, and the field it goes to.
struct IndexOperand
{
    IndexSpace space = IndexSpace::Type;          ///< Where it is looked up.
    InstructionField field = InstructionField::A; ///< Where it goes.
};

/// Which of an instruction's indices the text format may leave out, each left out one being 0.
enum class IndexArity : uint8_t
{
    One,         ///< `x`: the one index is required.
    OptionalOne, ///< `x?`: the one index may be left out.
    NoneOrTwo,   ///< `(x y)?`: both indices, or neither.
    OneOrTwo,    ///< `x? y`: both indices, or the second one alone.
};

/// The text grammar of an immediate kind made of indices only (spec §6.5).
struct IndexImmediate
{
    binary::ImmediateKind kind;           ///< The immediate kind.
    IndexArity arity;                     ///< Which indices may be left out.
    std::array<IndexOperand, 2> operands; ///< The indices in text order; One and OptionalOne use the first.
};

/// Every immediate kind the text format writes as indices only. Each other kind has a grammar of its own.
inline constexpr auto IndexImmediates = std::to_array<IndexImmediate>({
    { .kind = binary::ImmediateKind::LabelIndex,
      .arity = IndexArity::One,
      .operands = { { { .space = IndexSpace::Label } } } },
    { .kind = binary::ImmediateKind::FunctionIndex,
      .arity = IndexArity::One,
      .operands = { { { .space = IndexSpace::Function } } } },
    { .kind = binary::ImmediateKind::TypeIndex,
      .arity = IndexArity::One,
      .operands = { { { .space = IndexSpace::Type } } } },
    { .kind = binary::ImmediateKind::TagIndex,
      .arity = IndexArity::One,
      .operands = { { { .space = IndexSpace::Tag } } } },
    { .kind = binary::ImmediateKind::LocalIndex,
      .arity = IndexArity::One,
      .operands = { { { .space = IndexSpace::Local } } } },
    { .kind = binary::ImmediateKind::GlobalIndex,
      .arity = IndexArity::One,
      .operands = { { { .space = IndexSpace::Global } } } },
    { .kind = binary::ImmediateKind::ElemIndex,
      .arity = IndexArity::One,
      .operands = { { { .space = IndexSpace::Element } } } },
    { .kind = binary::ImmediateKind::DataIndex,
      .arity = IndexArity::One,
      .operands = { { { .space = IndexSpace::Data } } } },
    { .kind = binary::ImmediateKind::TableIndex,
      .arity = IndexArity::OptionalOne,
      .operands = { { { .space = IndexSpace::Table } } } },
    { .kind = binary::ImmediateKind::MemoryIndex,
      .arity = IndexArity::OptionalOne,
      .operands = { { { .space = IndexSpace::Memory } } } },
    // The destination first, then the source.
    { .kind = binary::ImmediateKind::TableCopy,
      .arity = IndexArity::NoneOrTwo,
      .operands = { { { .space = IndexSpace::Table, .field = InstructionField::A },
                      { .space = IndexSpace::Table, .field = InstructionField::B } } } },
    { .kind = binary::ImmediateKind::MemoryCopy,
      .arity = IndexArity::NoneOrTwo,
      .operands = { { { .space = IndexSpace::Memory, .field = InstructionField::A },
                      { .space = IndexSpace::Memory, .field = InstructionField::B } } } },
    // The text format writes the table or memory first; the binary format, the segment.
    { .kind = binary::ImmediateKind::TableInit,
      .arity = IndexArity::OneOrTwo,
      .operands = { { { .space = IndexSpace::Table, .field = InstructionField::B },
                      { .space = IndexSpace::Element, .field = InstructionField::A } } } },
    { .kind = binary::ImmediateKind::MemoryInit,
      .arity = IndexArity::OneOrTwo,
      .operands = { { { .space = IndexSpace::Memory, .field = InstructionField::B },
                      { .space = IndexSpace::Data, .field = InstructionField::A } } } },
});

/// The text grammar of an immediate kind made of indices only.
/// @param kind The immediate kind.
/// @return Its IndexImmediates row, or nullptr when @p kind has a grammar of its own.
[[nodiscard]] IndexImmediate const* indexImmediateOf(binary::ImmediateKind kind) noexcept;

/// What the lanes of a `v128.const` shape are written as.
enum class LaneNumber : uint8_t
{
    Integer, ///< Integer literals, signed or not, of the lane's width.
    Float,   ///< Float literals of the lane's width.
};

/// One shape `v128.const` takes its lanes in (spec §6.5.8).
struct LaneShape
{
    std::string_view keyword; ///< The shape's keyword: `i8x16` and so on.
    LaneNumber number;        ///< What its lanes are written as.
    uint8_t laneBytes;        ///< The bytes of one lane: 1, 2, 4 or 8.

    /// The number of lanes in a v128.
    /// @return 16 / laneBytes.
    [[nodiscard]] constexpr std::size_t laneCount() const noexcept { return 16U / laneBytes; }
};

/// Every shape `v128.const` takes its lanes in.
inline constexpr auto LaneShapes = std::to_array<LaneShape>({
    { .keyword = "i8x16", .number = LaneNumber::Integer, .laneBytes = 1 },
    { .keyword = "i16x8", .number = LaneNumber::Integer, .laneBytes = 2 },
    { .keyword = "i32x4", .number = LaneNumber::Integer, .laneBytes = 4 },
    { .keyword = "i64x2", .number = LaneNumber::Integer, .laneBytes = 8 },
    { .keyword = "f32x4", .number = LaneNumber::Float, .laneBytes = 4 },
    { .keyword = "f64x2", .number = LaneNumber::Float, .laneBytes = 8 },
});

/// The keyword of one kind of try_table catch clause.
struct CatchKeyword
{
    std::string_view keyword; ///< The clause's keyword: `catch` and so on.
    binary::CatchKind kind;   ///< The clause it reads as; binary::catchesByTag() tells if it names a tag.
};

/// Every kind of try_table catch clause: `(catch x l)`, `(catch_ref x l)`, `(catch_all l)` and
/// `(catch_all_ref l)`.
inline constexpr auto CatchKeywords = std::to_array<CatchKeyword>({
    { .keyword = "catch", .kind = binary::CatchKind::Catch },
    { .keyword = "catch_ref", .kind = binary::CatchKind::CatchRef },
    { .keyword = "catch_all", .kind = binary::CatchKind::CatchAll },
    { .keyword = "catch_all_ref", .kind = binary::CatchKind::CatchAllRef },
});

/// One kind of entity an import or export names: its keyword and its index space.
struct ExternRow
{
    std::string_view keyword; ///< The descriptor's keyword: `(func …)`, `(table …)` and so on.
    binary::ExternKind kind;  ///< What it names.
    IndexSpace space;         ///< The index space it binds into and is looked up in.
};

/// Every kind of entity an import or export names.
inline constexpr auto ExternRows = std::to_array<ExternRow>({
    { .keyword = "func", .kind = binary::ExternKind::Func, .space = IndexSpace::Function },
    { .keyword = "table", .kind = binary::ExternKind::Table, .space = IndexSpace::Table },
    { .keyword = "memory", .kind = binary::ExternKind::Memory, .space = IndexSpace::Memory },
    { .keyword = "global", .kind = binary::ExternKind::Global, .space = IndexSpace::Global },
    { .keyword = "tag", .kind = binary::ExternKind::Tag, .space = IndexSpace::Tag },
});

/// The memory-argument keyword of an offset, followed by its `nat` (spec §6.5.5).
inline constexpr auto OffsetPrefix = std::string_view { "offset=" };

/// The memory-argument keyword of an alignment, followed by its `nat`, a power of two.
inline constexpr auto AlignmentPrefix = std::string_view { "align=" };

/// The instruction a text-format name names, by binary search over OpcodeTable's texts sorted at
/// compile time.
/// @param text The instruction's name, as the text format spells it.
/// @return The instruction, or nullopt when no OpcodeTable row outside InternalTextNames has that text.
[[nodiscard]] std::optional<binary::Opcode> opcodeNamed(std::string_view text) noexcept;

/// The name the text format spells an instruction with: the inverse of opcodeNamed().
/// @param opcode The instruction.
/// @return Its InternalTextNames text when it has one, or else its OpcodeTable text.
[[nodiscard]] std::string_view textName(binary::Opcode opcode) noexcept;

} // namespace isoline::text
