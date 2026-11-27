// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <isoline/binary/BranchHints.hpp>
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

/// What a try_table's catch clause catches, and what it hands its label.
enum class CatchKind : uint8_t
{
    Catch,       ///< catch: exceptions of its tag; the label gets their payload.
    CatchRef,    ///< catch_ref: exceptions of its tag; the label gets their payload and the exnref.
    CatchAll,    ///< catch_all: every exception; the label gets nothing.
    CatchAllRef, ///< catch_all_ref: every exception; the label gets the exnref.
};

/// One catch clause of a try_table.
struct CatchClause
{
    CatchKind kind = CatchKind::Catch; ///< What it catches, and what it hands the label.
    uint32_t tag = 0;                  ///< The tag it catches; Catch and CatchRef only.
    uint32_t label = 0;                ///< The label it branches to, counted from outside the try_table.

    bool operator==(CatchClause const&) const = default;
};

/// Whether a clause of @p kind catches only its tag's exceptions (catch, catch_ref), not every one.
/// @param kind The clause's kind.
/// @return Whether it names a tag.
[[nodiscard]] constexpr bool catchesByTag(CatchKind kind) noexcept
{
    return kind == CatchKind::Catch || kind == CatchKind::CatchRef;
}

/// Whether a clause of @p kind hands its label the caught exception's exnref (catch_ref,
/// catch_all_ref), after the payload if it hands that too.
/// @param kind The clause's kind.
/// @return Whether the label's last value is the exnref.
[[nodiscard]] constexpr bool handsReference(CatchKind kind) noexcept
{
    return kind == CatchKind::CatchRef || kind == CatchKind::CatchAllRef;
}

/// One decoded instruction: its opcode and its immediates, in fields shared by all opcodes.
///
/// Which fields an instruction uses follows from its opcode's ImmediateKind; the rest stay zero. An
/// instruction owns no heap memory: the immediates of variable length, br_table's labels, typed
/// select's types and try_table's catch clauses, live in side tables of the Function the instruction
/// belongs to (labelsOf, selectTypesOf and catchClausesOf), referenced by an offset and a count. A
/// body thus costs a fixed number of bytes per instruction, whatever counts its immediates claim.
struct Instruction
{
    Opcode opcode {}; ///< The instruction.

    /// For block, loop and if: the form of the block type (see blockType()).
    BlockType::Kind blockKind = BlockType::Kind::Empty;

    /// The lane index immediate of a SIMD lane instruction; zero for every other instruction.
    uint8_t lane = 0;

    /// The first immediate: the index for LabelIndex, FunctionIndex, LocalIndex, GlobalIndex,
    /// TableIndex, ElemIndex, MemoryIndex, DataIndex and TagIndex; the type index for CallIndirect and
    /// for a block type of the TypeIndex form (try_table's included); the destination for TableCopy and
    /// MemoryCopy; the segment for TableInit and MemoryInit; MemArg::align for MemArg and MemArgLane; the
    /// offset of the instruction's run in Function::brTableLabels for LabelTable, or in Function::selectTypes
    /// for SelectTypes; and the index of its 16 bytes in Module::v128Literals for V128Const and Shuffle.
    uint32_t a = 0;

    /// The second immediate: the table for CallIndirect and TableInit, the memory for MemoryInit,
    /// the source for TableCopy and MemoryCopy, MemArg::memory for MemArg and MemArgLane, and the length of
    /// the instruction's run for LabelTable (the default label included), SelectTypes and TryTable.
    uint32_t b = 0;

    /// For block, loop, if, else and try_table: the index in the function body of the matching end.
    uint32_t endIndex = 0;

    /// The wide immediate: the raw bits of I32Const, I64Const, F32Const and F64Const (an i32 or f32
    /// zero-extended), MemArg::offset for MemArg and MemArgLane, and the offset of a try_table's run
    /// in Function::catchClauses (a holds its block type's index).
    uint64_t c = 0;

    /// For if: the index of the matching else, or endIndex when there is none.
    uint32_t elseIndex = 0;

    /// The result type of a block, loop, if or try_table whose block type has the Value form; the
    /// reference type of ref.null (its heap type, as a nullable reference).
    ValueType type {};

    /// The block type of a block, loop, if or try_table, assembled from blockKind, type and a.
    /// @return The block type.
    [[nodiscard]] constexpr BlockType blockType() const noexcept
    {
        return BlockType { .kind = blockKind, .value = type, .typeIndex = a };
    }
};

static_assert(std::is_trivially_copyable_v<Instruction>, "an Instruction must own no heap memory");

// Every instruction takes at least one byte of input, so 40 bytes bound a decoded body at 40 times
// its encoding: linear in what the guest supplies. 40 is what the members need once packed: the
// 8-byte wide immediate, a 12-byte ValueType, four 4-byte fields, the 2-byte opcode, the one-byte
// block kind and the lane byte. Adding a member must not grow it; give a rare, long immediate a side
// table instead, as v128.const's 16 bytes have (Module::v128Literals).
static_assert(sizeof(Instruction) <= 40, "an Instruction must stay compact");

/// A run of locals of one type, as the code section declares them.
struct LocalRun
{
    uint32_t count = 0; ///< How many locals the run declares.
    ValueType type;     ///< Their type.
};

/// An if or br_if of a function body, where a branch hint may attach.
struct BranchSite
{
    uint32_t instruction = 0; ///< Its index in Function::body.
    uint32_t offset = 0;      ///< Its byte offset, relative to the function's locals declaration.
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

    /// The catch clauses of every try_table in body, one run per try_table in body order; a
    /// try_table's Instruction::c and b give its run's offset and length.
    std::vector<CatchClause> catchClauses;

    /// Every if and br_if in body, in body order, which is also increasing offset order; what the
    /// branch hint section's offsets are resolved against.
    std::vector<BranchSite> branchSites;
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

/// The catch clauses of a try_table in @p function's body, in the order they are tried.
///
/// Precondition (checked): @p instruction is a try_table of @p function's body, so its run lies
/// within Function::catchClauses; any other instruction's b and c mean something else.
/// @param function The function whose body holds @p instruction.
/// @param instruction A try_table of that body.
/// @return Its run of Function::catchClauses.
[[nodiscard]] inline std::span<CatchClause const> catchClausesOf(Function const& function,
                                                                 Instruction const& instruction) noexcept
{
    Require(instruction.opcode == Opcode::TryTable);
    Require(instruction.c <= function.catchClauses.size()
            && instruction.b <= function.catchClauses.size() - instruction.c);
    return std::span { function.catchClauses }.subspan(static_cast<std::size_t>(instruction.c),
                                                       instruction.b);
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
/// br_table, a typed select or a try_table, so none has side tables: the decoder reads their immediates and
/// drops them, and their offsets and counts refer to nothing.
using ConstExpr = std::vector<Instruction>;

/// A defined tag: the type of the exceptions it marks, whose results are empty.
struct Tag
{
    uint32_t typeIndex = 0; ///< The index of its function type; its params are the payload's types.
};

/// An import: what it names, and the type the importer requires of it.
///
/// Its module name is held once per import entry, in Module::importModules, and named here by index:
/// a compact import group gives all its items one module name, which a copy per item would make cost
/// memory of the name's length times the item count (importModuleOf() reads it).
struct Import
{
    uint32_t moduleIndex = 0;           ///< The module name's index in Module::importModules.
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

/// A defined table: its type, and what its slots start with.
struct Table
{
    TableType type; ///< Its element type and size bounds.

    /// The expression every slot starts with, as typed function references' table section may give
    /// one; empty for a table without, whose slots start as the null of its element type.
    ConstExpr init;
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
    ValueType type = FuncRef;                ///< The reference type of its items; (ref func) for indices.

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

/// Where a custom section sits among the non-custom ones: after the one an enumerator names, or
/// before them all. The enumerators follow the order the binary format lists the sections in, the
/// tag section after the memory section and the data count section before the code section.
enum class SectionAnchor : uint8_t
{
    BeforeFirst, ///< Before every non-custom section.
    Type,        ///< After the type section.
    Import,      ///< After the import section.
    Function,    ///< After the function section.
    Table,       ///< After the table section.
    Memory,      ///< After the memory section.
    Tag,         ///< After the tag section.
    Global,      ///< After the global section.
    Export,      ///< After the export section.
    Start,       ///< After the start section.
    Element,     ///< After the element section.
    DataCount,   ///< After the data count section.
    Code,        ///< After the code section.
    Data,        ///< After the data section: at the module's end.
};

/// The last SectionAnchor enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastSectionAnchor = SectionAnchor::Data;

/// A custom section, kept verbatim, with its place among the other sections.
struct CustomSection
{
    std::string name;             ///< The section's name.
    std::vector<std::byte> bytes; ///< Its payload after the name.

    /// The last non-custom section before it, in section order, or BeforeFirst. The decoder records
    /// the last section it read before this one, and the encoder writes the section right after that
    /// anchor's slot; within one slot, custom sections keep their order in Module::customs.
    SectionAnchor after = SectionAnchor::Data;
};

/// A decoded module: every section's contents, in index-space order.
struct Module
{
    std::vector<FuncType> types;            ///< The type section.
    std::vector<Import> imports;            ///< The import section.
    std::vector<std::string> importModules; ///< The import entries' module names (Import::moduleIndex).
    std::vector<uint32_t> functions;        ///< The function section: the type index per defined function.
    std::vector<Table> tables;              ///< The table section: defined tables.
    std::vector<MemoryType> memories;       ///< The memory section: defined memories.
    std::vector<Tag> tags;                  ///< The tag section: defined tags.
    std::vector<Global> globals;            ///< The global section: defined globals.
    std::vector<Export> exports;            ///< The export section.
    std::optional<uint32_t> start;          ///< The start section: the start function's index, if any.
    std::vector<ElementSegment> elements;   ///< The element section.
    std::optional<uint32_t> dataCount;      ///< The data count section, if present.
    std::vector<Function> code;             ///< The code section: one entry per defined function.
    std::vector<DataSegment> data;          ///< The data section.
    std::vector<CustomSection> customs;     ///< Every custom section, in module order.

    /// The 16 immediate bytes of every v128.const and i8x16.shuffle, in the order the decoder read
    /// them, constant expressions included; such an instruction's Instruction::a indexes this.
    /// Kept beside the code, as 16 bytes would more than double every Instruction.
    std::vector<std::array<std::byte, 16>> v128Literals;

    /// The branch hints of the `metadata.code.branch_hint` section, ordered by function, then by
    /// instruction; empty without the section, or when it was refused.
    std::vector<BranchHint> branchHints;

    /// The custom sections the decoder interpreted and refused; none of them fails the module.
    std::vector<CustomSectionError> customErrors;

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

/// The module name of one of @p module's imports.
///
/// Precondition (checked): @p import is one of @p module's, so its module index lies within
/// Module::importModules.
/// @param module The module that declares @p import.
/// @param import One of its imports.
/// @return The name of the module it imports from; valid as long as @p module.
[[nodiscard]] inline std::string_view importModuleOf(Module const& module, Import const& import) noexcept
{
    Require(import.moduleIndex < module.importModules.size());
    return module.importModules[import.moduleIndex];
}

/// Whether some function body of @p module names a data segment: holds a memory.init or a data.drop,
/// which need the data count section. Constant expressions do not count, as they are no code.
/// @param module The module whose code to search.
/// @return Whether any instruction of its code section takes a data segment index.
[[nodiscard]] inline bool codeNamesDataSegment(Module const& module) noexcept
{
    auto const namesDataSegment = [](Instruction const& instruction) {
        auto const immediate = opcodeInfo(instruction.opcode).immediate;
        return immediate == ImmediateKind::MemoryInit || immediate == ImmediateKind::DataIndex;
    };
    return std::ranges::any_of(module.code, [&](Function const& function) {
        return std::ranges::any_of(function.body, namesDataSegment);
    });
}

/// The 16 immediate bytes of a v128.const or i8x16.shuffle of @p module: a v128.const's vector,
/// lowest lane byte first, or a shuffle's 16 lane indices.
///
/// Precondition (checked): @p instruction is a v128.const or i8x16.shuffle of @p module, so its
/// index lies within Module::v128Literals; any other instruction's a means something else.
/// @param module The module whose code holds @p instruction.
/// @param instruction A v128.const or i8x16.shuffle.
/// @return Its entry of Module::v128Literals.
[[nodiscard]] inline std::span<std::byte const, 16> v128LiteralOf(Module const& module,
                                                                  Instruction const& instruction) noexcept
{
    Require(instruction.opcode == Opcode::V128Const || instruction.opcode == Opcode::I8x16Shuffle);
    Require(instruction.a < module.v128Literals.size());
    return std::span<std::byte const, 16> { module.v128Literals[instruction.a] };
}

} // namespace isoline::binary
