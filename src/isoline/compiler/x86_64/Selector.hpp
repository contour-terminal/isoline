// SPDX-License-Identifier: Apache-2.0
#pragma once

// x86-64 instruction selection, driven by one table: Selection.def's rows, each {name, root operation,
// pattern, requires, cost, emitter}, grouped by root operation in Operation order and tried in table order
// (P5 Decision "Instruction selection: table-driven maximal munch"). A row applies when the target has the
// CPU feature it requires and its pattern matches the instruction's operands; its emitter is data: an
// EmitterKind and the forms, condition and parameter it is given, interpreted by the selector, so that rows
// of one shape (every ALU operation, every comparison) share one emitter. Every operation has a baseline row
// (no feature, pattern Any) last among its rows, which every other row falls back to.
//
// Selection walks each block bottom-up to decide what each instruction folds: a single-use load later folded
// into its user's memory operand, across only Pure and MayTrap instructions (P5 Decision "Effects are a
// column"); a single-use comparison into its branch or select; a constant into an immediate. It then emits
// each block top-down, in reverse postorder so that every value is defined before it is read, splitting
// critical edges as it goes (R6): no block is added after selection.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/Target.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/x86_64/Encoder.hpp>
#include <isoline/compiler/x86_64/Lowering.hpp>

namespace isoline::compiler::x86_64
{

/// What a row asks of its root instruction's operands beyond their types: the Pattern column.
enum class Pattern : uint8_t
{
    Any,               ///< Nothing: every operand in a register.
    RhsConst32,        ///< The second operand a constant that fits a sign-extended 32-bit immediate.
    RhsSingleUseLoad,  ///< The second operand a full-width load of one use, in the block, foldable here.
    AddressAddConst,   ///< An add of a 32-bit constant, as lea: the result in a register of its own.
    CompareIntoBranch, ///< A branch on an integer comparison of one use, in the block: cmp and jcc.
    CompareIntoSelect, ///< A select on an integer comparison of one use, in the block: cmp and cmovcc.
    /// The function's entry: the FunctionEntry poll first in the entry block, or the frame's charge first or
    /// right after it. Both run in the prologue, before the frame is allocated (enterFrame()).
    FunctionEntry,
};

/// The last Pattern enumerator.
inline constexpr auto LastPattern = Pattern::FunctionEntry;

/// What interprets a row: the shape of the code it emits.
enum class EmitterKind : uint8_t
{
    Nothing,           ///< Emits nothing: a removed instruction.
    Constant,          ///< An integer constant: the shortest mov.
    FloatConstant,     ///< A float constant: its bits through a General register.
    Binary,            ///< Two-address op of two registers: mov, then form.
    BinaryImmediate,   ///< Two-address op of a register and an immediate: form, or alternate for imm8.
    BinaryMemory,      ///< Two-address op of a register and the folded load's memory operand.
    AddImmediate,      ///< lea of a register plus a constant.
    MultiplyImmediate, ///< imul r, r, imm32.
    Shift,             ///< A shift by cl.
    ShiftImmediate,    ///< A shift by an immediate count, masked to the width.
    Compare,           ///< cmp, setcc, movzx.
    CompareImmediate,  ///< cmp with an immediate (form, alternate for imm8), setcc, movzx.
    TestZero,          ///< test, sete, movzx: eqz.
    FloatCompare,      ///< ucomis (operands in order or swapped), setcc of an above-or-equal-style condition.
    FloatEqual,        ///< ucomis, sete and setnp, and'd.
    FloatNotEqual,     ///< ucomis, setne and setp, or'd.
    Unary,             ///< One instruction of a Def and a Use: a bit count, a conversion, sqrt, a move.
    FloatBinary,       ///< Two-address float op of two registers.
    FloatBinaryMemory, ///< Two-address float op of a register and the folded load's memory operand.
    Rounding,          ///< roundss/roundsd with its mode (SSE4.1).
    RoundingStub,      ///< callStub(FloatRound) with its operation (baseline).
    Sequence,          ///< A lowering sequence (Lowering.hpp), by its Sequence.
    Select,            ///< test, cmovne.
    CompareSelect,     ///< The folded comparison's cmp, cmovcc.
    CheckMemory,       ///< checkMemory32() or checkMemory64(), by the index's type.
    GuardAddress,      ///< guardAddress().
    InBoundsAddress,   ///< inBoundsAddress().
    Load,              ///< A load by LoadFormTable.
    Store,             ///< A store by StoreFormTable.
    MemoryBase,        ///< A memory's base through the instance's views.
    MemoryBound,       ///< A memory's size through the instance's views.
    CheckTable,        ///< checkTable() on the table's view.
    LoadTableElement,  ///< loadRef().
    StoreTableElement, ///< storeRef().
    TableSize,         ///< A table's size through its view.
    CheckDispatch,     ///< checkDispatch() on the store's dispatch view.
    LoadDispatchType,  ///< A dispatch row's TypeId.
    TypeId,            ///< A module type's TypeId.
    RefNull,           ///< A null Ref's word.
    RefFunc,           ///< A function's Ref word, from its address.
    RefIsNull,         ///< Whether a Ref's kind is Null.
    LoadGlobal,        ///< A global's cell read.
    StoreGlobal,       ///< A global's cell written.
    LoadContext,       ///< A context value: the step countdown, memory 0's base or bound.
    StoreContext,      ///< The step countdown stored.
    Poll,              ///< The Poll pseudo-instruction on a copy of the countdown.
    PollStored,        ///< The PollStored pseudo-instruction.
    EnterFrame,        ///< chargeFrame().
    LeaveFrame,        ///< releaseFrame().
    Call,              ///< callDirect().
    CallIndirect,      ///< callIndirect().
    CallImport,        ///< callImport().
    CallStub,          ///< callStub().
    TrapIf,            ///< test, a trap on non-zero.
    TrapIfSubject,     ///< test, a trap on non-zero naming the element index.
    Jump,              ///< jmp, its arguments the edge's.
    Branch,            ///< The Branch pseudo-instruction.
    CompareBranch,     ///< The CompareBranch pseudo-instruction on the folded comparison.
    Switch,            ///< The Switch pseudo-instruction.
    Return,            ///< The results to their registers or the result area; the Return pseudo-instruction.
    Trap,              ///< The Trap pseudo-instruction.
    EntryPoll,         ///< The FunctionEntry poll, run by the prologue: the Entry pseudo-instruction's.
    EntryCharge,       ///< The frame's charge, run by the prologue: the Entry pseudo-instruction's.
};

/// The lowering sequences a Sequence row emits.
enum class Sequence : uint8_t
{
    FloatMin,           ///< floatMin().
    FloatMax,           ///< floatMax().
    FloatAbs,           ///< floatSign(Abs).
    FloatNeg,           ///< floatSign(Neg).
    FloatCopysign,      ///< floatSign(Copysign).
    TruncateChecked,    ///< truncateChecked() of the operation's TruncationTable row.
    TruncateSaturating, ///< truncateSaturating() of the operation's TruncationTable row.
    ConvertU32,         ///< convertU32().
    ConvertU64,         ///< convertU64ToF32() or convertU64ToF64().
    PopcntBaseline,     ///< popcntBaseline().
    ClzBaseline,        ///< clzBaseline().
    CtzBaseline,        ///< ctzBaseline().
    DivSigned,          ///< divSigned().
    RemSigned,          ///< remSigned().
    DivUnsigned,        ///< divideUnsigned(Quotient).
    RemUnsigned,        ///< divideUnsigned(Remainder).
    CanonicalizeNan,    ///< canonicalizeNan().
};

/// The order a FloatCompare row compares its operands in.
enum class OperandOrder : uint8_t
{
    InOrder, ///< ucomis a, b.
    Swapped, ///< ucomis b, a: lt and le as gt and ge.
};

/// The Emitter column: what interprets a row, and the data it is given.
struct Emission
{
    EmitterKind kind = EmitterKind::Nothing;           ///< The emitter.
    Mnemonic form = Mnemonic::Ud2;                     ///< Its main instruction form.
    Mnemonic alternate = Mnemonic::Ud2;                ///< A second form: an imm8 variant.
    ConditionCode condition = ConditionCode::Overflow; ///< A comparison's condition.
    uint8_t parameter = 0; ///< A Sequence, an OperandOrder, a RoundingMode or a FloatRoundOperation.

    /// Whether both emit alike.
    [[nodiscard]] constexpr bool operator==(Emission const&) const noexcept = default;
};

/// The spelling of Selection.def's Emitter column.
namespace emitters
{
    using enum Mnemonic;

    /// Nothing at all.
    constexpr Emission nothing() noexcept
    {
        return Emission { .kind = EmitterKind::Nothing };
    }

    /// An emitter of @p kind with nothing but its kind.
    constexpr Emission special(EmitterKind kind) noexcept
    {
        return Emission { .kind = kind };
    }

    /// An emitter of @p kind with the form @p form.
    constexpr Emission of(EmitterKind kind, Mnemonic form) noexcept
    {
        return Emission { .kind = kind, .form = form };
    }

    /// BinaryImmediate of @p form, and @p imm8 for a constant that fits a byte.
    constexpr Emission binaryImmediate(Mnemonic form, Mnemonic imm8) noexcept
    {
        return Emission { .kind = EmitterKind::BinaryImmediate, .form = form, .alternate = imm8 };
    }

    /// Compare of @p form on @p condition.
    constexpr Emission compare(Mnemonic form, ConditionCode condition) noexcept
    {
        return Emission { .kind = EmitterKind::Compare, .form = form, .condition = condition };
    }

    /// CompareImmediate of @p form, @p imm8 for a byte, on @p condition.
    constexpr Emission compareImmediate(Mnemonic form, Mnemonic imm8, ConditionCode condition) noexcept
    {
        return Emission {
            .kind = EmitterKind::CompareImmediate, .form = form, .alternate = imm8, .condition = condition
        };
    }

    /// TestZero of @p form.
    constexpr Emission testZero(Mnemonic form) noexcept
    {
        return Emission { .kind = EmitterKind::TestZero, .form = form, .condition = ConditionCode::Equal };
    }

    /// FloatCompare of @p form on @p condition, the operands in @p order.
    constexpr Emission floatCompare(Mnemonic form, ConditionCode condition, OperandOrder order) noexcept
    {
        return Emission { .kind = EmitterKind::FloatCompare,
                          .form = form,
                          .condition = condition,
                          .parameter = static_cast<uint8_t>(order) };
    }

    /// Rounding of @p form in @p mode.
    constexpr Emission rounding(Mnemonic form, RoundingMode mode) noexcept
    {
        return Emission { .kind = EmitterKind::Rounding,
                          .form = form,
                          .parameter = static_cast<uint8_t>(mode) };
    }

    /// RoundingStub of @p operation.
    constexpr Emission roundingStub(FloatRoundOperation operation) noexcept
    {
        return Emission { .kind = EmitterKind::RoundingStub, .parameter = static_cast<uint8_t>(operation) };
    }

    /// The lowering sequence @p sequence.
    constexpr Emission sequence(Sequence sequence) noexcept
    {
        return Emission { .kind = EmitterKind::Sequence, .parameter = static_cast<uint8_t>(sequence) };
    }
} // namespace emitters

/// The Requires column's spelling: a CPU feature, or Baseline for none.
namespace requirements
{
    inline constexpr auto Baseline = std::optional<machine::CpuFeature> {};       ///< Every x86-64.
    inline constexpr auto Sse41 = std::optional { machine::CpuFeature::Sse41 };   ///< SSE4.1.
    inline constexpr auto Popcnt = std::optional { machine::CpuFeature::Popcnt }; ///< POPCNT.
    inline constexpr auto Lzcnt = std::optional { machine::CpuFeature::Lzcnt };   ///< LZCNT.
    inline constexpr auto Bmi1 = std::optional { machine::CpuFeature::Bmi1 };     ///< BMI1.
} // namespace requirements

/// A row of the selection table: one enumerator per row of Selection.def, in its order.
enum class SelectionRowId : uint16_t
{
#define ISOLINE_SELECTION(name, root, pattern, requirement, cost, emitter) name,
#include <isoline/compiler/x86_64/Selection.def>
#undef ISOLINE_SELECTION
};

/// One row of the selection table.
struct SelectionRow
{
    SelectionRowId row;                             ///< The row; equal to its index.
    std::string_view name;                          ///< Its name.
    intermediate::Operation root;                   ///< The operation it selects.
    Pattern pattern;                                ///< What it asks of the operands.
    std::optional<machine::CpuFeature> requirement; ///< The CPU feature it needs; none for every x86-64.
    uint8_t cost;                                   ///< The instructions it emits, about: the statistics'.
    Emission emitter;                               ///< What emits it.
};

/// Every selection row, in Selection.def's order.
inline constexpr auto SelectionTable = std::to_array<SelectionRow>({
#define ISOLINE_SELECTION(rowName, rowRoot, rowPattern, rowRequires, rowCost, rowEmitter) \
    SelectionRow { .row = SelectionRowId::rowName,                                        \
                   .name = #rowName,                                                      \
                   .root = intermediate::Operation::rowRoot,                              \
                   .pattern = Pattern::rowPattern,                                        \
                   .requirement = requirements::rowRequires,                              \
                   .cost = (rowCost),                                                     \
                   .emitter = [] {                                                        \
                       using namespace emitters;                                          \
                       using enum EmitterKind;                                            \
                       using enum ConditionCode;                                          \
                       using enum x86_64::Sequence;                                       \
                       using enum OperandOrder;                                           \
                       using enum RoundingMode;                                           \
                       using enum FloatRoundOperation;                                    \
                       return rowEmitter;                                                 \
                   }() },
#include <isoline/compiler/x86_64/Selection.def>
#undef ISOLINE_SELECTION
});

/// How many selection rows there are.
inline constexpr auto SelectionRowCount = SelectionTable.size();

/// Where an operation's rows lie in SelectionTable: [begin, end).
struct RowRange
{
    uint16_t begin = 0; ///< Its first row.
    uint16_t end = 0;   ///< One past its last.
};

/// Per Operation, its rows.
inline constexpr auto RowsByOperation = [] {
    auto ranges = std::array<RowRange, intermediate::OperationCount> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, SelectionTable.size()))
    {
        auto& range = ranges[static_cast<std::size_t>(SelectionTable[index].root)];
        if (range.begin == range.end)
            range.begin = static_cast<uint16_t>(index);
        range.end = static_cast<uint16_t>(index + 1);
    }
    return ranges;
}();

/// Whether @p table is in order: one row per enumerator at its index; rows grouped by root operation in
/// Operation order; and every operation's last row its baseline, needing no feature and matching Any.
/// @param table The rows; SelectionTable by default.
/// @param ranges Per operation, its rows in @p table; RowsByOperation by default.
[[nodiscard]] constexpr bool isSelectionTableComplete(
    std::span<SelectionRow const> table = SelectionTable,
    std::span<RowRange const> ranges = RowsByOperation) noexcept
{
    auto index = std::size_t { 0 };
    auto previous = std::size_t { 0 };
    for (auto const& row: table)
    {
        auto const root = static_cast<std::size_t>(row.root);
        if (static_cast<std::size_t>(row.row) != index++ || root < previous)
            return false;
        previous = root;
    }
    for (auto const& range: ranges)
    {
        if (range.begin == range.end)
            return false;
        auto const& baseline = table[range.end - 1];
        if (baseline.requirement.has_value() || baseline.pattern != Pattern::Any)
            return false;
    }
    return true;
}

static_assert(
    isSelectionTableComplete(),
    "SelectionTable must hold its rows in Operation order, every operation's last row its baseline");

/// How often each row was selected, by one selector.
struct SelectionStatistics
{
    std::array<uint64_t, SelectionRowCount> hits {}; ///< Per row, its selections.

    /// How often @p row was selected.
    [[nodiscard]] uint64_t hitsOf(SelectionRowId row) const noexcept
    {
        return hits[static_cast<std::size_t>(row)];
    }
};

/// The instruction selector for one target description: selects functions one at a time, counting the rows it
/// selects. Configured at construction; not shared between threads (its statistics are its own).
class Selector
{
  public:
    /// A selector generating code for @p features under @p conventions.
    Selector(machine::CpuFeatures features, CallConventions conventions) noexcept:
        _features { features }, _conventions { conventions }
    {
    }

    /// Selects @p function.
    /// @param function A verified, optimized function.
    /// @param context The compile's context: its configuration (the Spectre guard), the module's imported
    ///                functions, the cancellation and the arena.
    /// @return The machine function, its critical edges split; or Cancelled.
    [[nodiscard]] std::expected<machine::MachineFunction, CompileError> select(
        intermediate::Function const& function, machine::SelectionContext const& context);

    /// The rows selected so far.
    [[nodiscard]] SelectionStatistics const& statistics() const noexcept { return _statistics; }

  private:
    machine::CpuFeatures _features;
    CallConventions _conventions;
    SelectionStatistics _statistics {};
};

/// The name of @p row.
[[nodiscard]] constexpr std::string_view nameOf(SelectionRowId row) noexcept
{
    return SelectionTable[static_cast<std::size_t>(row)].name;
}

} // namespace isoline::compiler::x86_64
