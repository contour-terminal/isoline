// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>

namespace isoline::binary
{

/// A load's or store's memory argument.
struct MemArg
{
    uint32_t align = 0;  ///< The alignment exponent: the access is aligned to 2^align bytes.
    uint64_t offset = 0; ///< The static offset added to the dynamic address.
    uint32_t memory = 0; ///< The memory index.
};

/// The type of a block, loop or if: no result, one value type, or a function type by index.
struct BlockType
{
    /// Which of the three forms a block type takes.
    enum class Kind : uint8_t
    {
        Empty,     ///< [] -> [].
        Value,     ///< [] -> [value].
        TypeIndex, ///< The function type at typeIndex.
    };

    Kind kind = Kind::Empty; ///< The form.
    ValueType value {};      ///< The result type, when kind is Value.
    uint32_t typeIndex = 0;  ///< The function type's index, when kind is TypeIndex.
};

/// One decoded instruction: its opcode and its immediates, in fields shared by all opcodes.
///
/// Which fields an instruction uses follows from its opcode's ImmediateKind; the rest stay zero. An
/// instruction owns no heap memory: the immediates of variable length, br_table's labels and typed
/// select's types, live in side tables of the Function the instruction belongs to (labelsOf and
/// selectTypesOf), referenced by an offset in a and a count in b. A body thus costs a fixed number
/// of bytes per instruction, whatever counts its immediates claim.
struct Instruction
{
    Opcode opcode {}; ///< The instruction.

    /// For block, loop and if: the form of the block type (see blockType()).
    BlockType::Kind blockKind = BlockType::Kind::Empty;

    /// The first immediate: the index for LabelIndex, FunctionIndex, LocalIndex, GlobalIndex,
    /// TableIndex, ElemIndex, MemoryIndex and DataIndex; the type index for CallIndirect and for a
    /// block type of the TypeIndex form; the destination for TableCopy and MemoryCopy; the segment
    /// for TableInit and MemoryInit; MemArg::align for MemArg; and the offset of the instruction's
    /// run in Function::brTableLabels for LabelTable, or in Function::selectTypes for SelectTypes.
    uint32_t a = 0;

    /// The second immediate: the table for CallIndirect and TableInit, the memory for MemoryInit,
    /// the source for TableCopy and MemoryCopy, MemArg::memory for MemArg, and the length of the
    /// instruction's run for LabelTable (the default label included) and SelectTypes.
    uint32_t b = 0;

    /// For block, loop, if and else: the index in the function body of the matching end.
    uint32_t endIndex = 0;

    /// The wide immediate: the raw bits of I32Const, I64Const, F32Const and F64Const (an i32 or f32
    /// zero-extended), and MemArg::offset for MemArg.
    uint64_t c = 0;

    /// For if: the index of the matching else, or endIndex when there is none.
    uint32_t elseIndex = 0;

    /// The result type of a block, loop or if whose block type has the Value form; the reference
    /// type of ref.null (its heap type, as a nullable reference).
    ValueType type {};

    /// The block type of a block, loop or if, assembled from blockKind, type and a.
    /// @return The block type.
    [[nodiscard]] constexpr BlockType blockType() const noexcept
    {
        return BlockType { .kind = blockKind, .value = type, .typeIndex = a };
    }
};

static_assert(std::is_trivially_copyable_v<Instruction>, "an Instruction must own no heap memory");

// Every instruction takes at least one byte of input, so 40 bytes bound a decoded body at 40 times
// its encoding: linear in what the guest supplies. 40 is what the members need once packed: the
// 8-byte wide immediate, a 12-byte ValueType, four 4-byte fields and the two one-byte enums. Adding
// a member must not grow it; give a rare, long immediate a side table instead.
static_assert(sizeof(Instruction) <= 40, "an Instruction must stay compact");

/// A run of locals of one type, as the code section declares them.
struct LocalRun
{
    uint32_t count = 0; ///< How many locals the run declares.
    ValueType type;     ///< Their type.
};

/// A defined function's code.
///
/// The decoder reads each body in place, as the code section lists it: its locals, then its
/// instructions, with block structure resolved (Instruction::endIndex and elseIndex).
struct Function
{
    uint32_t typeIndex = 0;        ///< The index of its function type.
    std::vector<LocalRun> locals;  ///< Its declared locals, as the runs the binary declares them in.
    uint32_t localCount = 0;       ///< The number of declared locals over all runs; params excluded.
    std::vector<Instruction> body; ///< Its instructions, ending with Opcode::End.
    std::size_t bodyOffset = 0;    ///< The module offset of its body's first instruction.

    /// The labels of every br_table in body, one run per br_table in body order, each with its
    /// default label last; a br_table's Instruction::a and b give its run's offset and length.
    std::vector<uint32_t> brTableLabels;

    /// The operand types of every typed select in body, one run per instruction in body order; a
    /// typed select's Instruction::a and b give its run's offset and length.
    std::vector<ValueType> selectTypes;
};

/// The labels of a br_table in @p function's body, its default label last.
///
/// Precondition (checked): @p instruction is a br_table of @p function's body, so its run lies
/// within Function::brTableLabels and holds at least the default label; any other instruction's a and b mean
/// something else.
/// @param function The function whose body holds @p instruction.
/// @param instruction A br_table of that body.
/// @return Its run of Function::brTableLabels.
[[nodiscard]] inline std::span<uint32_t const> labelsOf(Function const& function,
                                                        Instruction const& instruction) noexcept
{
    Require(instruction.opcode == Opcode::BrTable);
    Require(instruction.b >= 1); // br_table always has its default label
    Require(std::size_t { instruction.a } + instruction.b <= function.brTableLabels.size());
    return std::span { function.brTableLabels }.subspan(instruction.a, instruction.b);
}

/// The operand types of a typed select in @p function's body.
///
/// Precondition (checked): @p instruction is a typed select of @p function's body, so its run lies
/// within Function::selectTypes; any other instruction's a and b mean something else.
/// @param function The function whose body holds @p instruction.
/// @param instruction A typed select of that body.
/// @return Its run of Function::selectTypes.
[[nodiscard]] inline std::span<ValueType const> selectTypesOf(Function const& function,
                                                              Instruction const& instruction) noexcept
{
    Require(instruction.opcode == Opcode::SelectTyped);
    Require(std::size_t { instruction.a } + instruction.b <= function.selectTypes.size());
    return std::span { function.selectTypes }.subspan(instruction.a, instruction.b);
}

/// The type of one of @p function's declared locals.
///
/// Locals are kept as runs, never expanded: a few input bytes can declare tens of thousands of
/// locals, and expanding them per function would let a small module claim gigabytes. Parameters
/// are not in the runs; a caller offsets local indices by its type's parameter count first.
/// @param function The function whose locals to look up.
/// @param index The index among its declared locals, 0 being the first after the parameters.
/// @return The local's type, or nullopt when index is not below localCount.
[[nodiscard]] inline std::optional<ValueType> localType(Function const& function, uint32_t index) noexcept
{
    for (auto const& run: function.locals)
    {
        if (index < run.count)
            return run.type;
        index -= run.count;
    }
    return std::nullopt;
}

/// A constant expression: a short instruction sequence ending with Opcode::End.
///
/// The decoder reads it as it reads a function body, so any instruction decodes here; which ones a
/// constant expression may hold is the validator's to check. No constant expression may hold a
/// br_table or a typed select, so none has side tables: the decoder reads their immediates and
/// drops them, and their offsets and counts refer to nothing.
using ConstExpr = std::vector<Instruction>;

/// An import: what it names, and the type the importer requires of it.
struct Import
{
    std::string module;                 ///< The module name.
    std::string name;                   ///< The field name.
    ExternKind kind = ExternKind::Func; ///< What is imported; selects the field below that applies.
    uint32_t typeIndex = 0;             ///< Func (and Tag): the function type's index.
    TableType table {};                 ///< Table: the table's type.
    MemoryType memory {};               ///< Memory: the memory's type.
    GlobalType global {};               ///< Global: the global's type.
};

/// An export: a name for one of the module's entities.
struct Export
{
    std::string name;                   ///< The exported name.
    ExternKind kind = ExternKind::Func; ///< The index space index refers to.
    uint32_t index = 0;                 ///< The entity's index in that space.
};

/// A defined global.
struct Global
{
    GlobalType type; ///< Its type.
    ConstExpr init;  ///< Its initializer.
};

/// How a segment is used.
enum class SegmentMode : uint8_t
{
    Passive,     ///< Copied on demand by table.init or memory.init.
    Active,      ///< Copied into its table or memory at instantiation.
    Declarative, ///< Only declares functions referenceable by ref.func (element segments only).
};

/// An element segment.
struct ElementSegment
{
    SegmentMode mode = SegmentMode::Passive; ///< How it is used.
    uint32_t table = 0;                      ///< Active: the table it initializes.
    ConstExpr offset;                        ///< Active: the table offset it starts at.
    ValueType type = FuncRef;                ///< The reference type of its items.

    /// Its items, as the binary encodes them: function indices, each standing for the expression
    /// (ref.func index), or constant expressions. Indices are kept as indices, since expanding
    /// each one-byte index into an expression would multiply the segment's size by the hundreds.
    std::variant<std::vector<uint32_t>, std::vector<ConstExpr>> items;
};

/// A data segment.
struct DataSegment
{
    SegmentMode mode = SegmentMode::Passive; ///< How it is used; never Declarative.
    uint32_t memory = 0;                     ///< Active: the memory it initializes.
    ConstExpr offset;                        ///< Active: the memory offset it starts at.
    std::vector<std::byte> bytes;            ///< Its contents.
};

/// A custom section, kept verbatim.
struct CustomSection
{
    std::string name;             ///< The section's name.
    std::vector<std::byte> bytes; ///< Its payload after the name.
};

/// A decoded module: every section's contents, in index-space order.
struct Module
{
    std::vector<FuncType> types;          ///< The type section.
    std::vector<Import> imports;          ///< The import section.
    std::vector<uint32_t> functions;      ///< The function section: the type index per defined function.
    std::vector<TableType> tables;        ///< The table section: defined tables.
    std::vector<MemoryType> memories;     ///< The memory section: defined memories.
    std::vector<Global> globals;          ///< The global section: defined globals.
    std::vector<Export> exports;          ///< The export section.
    std::optional<uint32_t> start;        ///< The start section: the start function's index, if any.
    std::vector<ElementSegment> elements; ///< The element section.
    std::optional<uint32_t> dataCount;    ///< The data count section, if present.
    std::vector<Function> code;           ///< The code section: one entry per defined function.
    std::vector<DataSegment> data;        ///< The data section.
    std::vector<CustomSection> customs;   ///< Every custom section, in module order.

    /// How many imports of @p kind the module has; they precede the defined entities of that kind
    /// in its index space.
    /// @param kind The kind of entity to count.
    /// @return The number of imports of that kind.
    [[nodiscard]] std::size_t importedCount(ExternKind kind) const noexcept
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(imports, [kind](Import const& entry) { return entry.kind == kind; }));
    }
};

} // namespace isoline::binary
