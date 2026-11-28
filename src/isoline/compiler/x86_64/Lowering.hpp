// SPDX-License-Identifier: Apache-2.0
#pragma once

// x86-64's lowering sequences: the instruction sequences selection rows and pseudo-instructions stand for.
//
// Two kinds, by when they run:
//  - Selection-time sequences append machine instructions over virtual registers to a block (a Writer): the
//    exact float lowerings (min, max, the truncations and unsigned conversions, canonicalization), the bit
//    counts' baselines, division, and every guard (checkMemory32/64, guardAddress, checkTable,
//    checkDispatch). A trap is a TrapIf pseudo-instruction: a branch to a trap site the encoder places out of
//    line, so no sequence adds a block. Where a sequence's flags must reach a later instruction (a compare
//    and its cmov), only moves may come between, and every move the register allocator inserts (mov, movsd,
//    push and pop) leaves the flags as they are.
//  - Encode-time sequences expand a pseudo-instruction once its registers and the frame are known (an
//    Assembler): the prologue (enterFrame) and epilogue (leaveFrame), the poll fast path and its slow path,
//    trap sites, calls with their host-call path, and the jump table.
//
// Every instruction either kind emits is a row of the encoding table (Encodings.def), which the llvm-mc
// oracle checks.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/Target.hpp>
#include <isoline/compiler/x86_64/Encoder.hpp>
#include <isoline/compiler/x86_64/Instructions.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::compiler::x86_64
{

/// The stack a prologue keeps free below its frame: machine::StubReserveBytes, the target-independent row.
inline constexpr auto StubReserveBytes = machine::StubReserveBytes;

/// The page a frame larger than one is probed by: one touch per page, top down, as Windows' guard pages
/// require of every OS row.
inline constexpr auto ProbePageBytes = uint64_t { 4096 };

/// The IR type of a WebAssembly value type the compiler compiles (Require: not a v128, which checkModule()
/// refuses): what selection gives a signature's values, and what an entry adapter passes a cell as.
[[nodiscard]] intermediate::Type typeOf(binary::ValueType const& type) noexcept;

/// The width of a float a sequence works on.
enum class FloatWidth : uint8_t
{
    Single, ///< F32: the ss forms.
    Double, ///< F64: the sd forms.
};

/// The width of an integer a sequence works on.
enum class IntegerWidth : uint8_t
{
    Bits32, ///< I32.
    Bits64, ///< I64.
};

/// Whether an integer conversion reads its bits as signed.
enum class Signedness : uint8_t
{
    Unsigned, ///< Unsigned.
    Signed,   ///< Two's complement.
};

/// What a float-to-integer truncation does with a value it cannot represent.
enum class Saturation : uint8_t
{
    Traps,     ///< NaN traps InvalidConversionToInteger; out of range traps IntegerOverflow.
    Saturates, ///< NaN is 0; out of range is the nearest bound (the non-trapping conversions).
};

/// One float-to-integer truncation: the IR operation and its shape. The rows of TruncationTable.
struct TruncationRow
{
    intermediate::Operation operation; ///< The IR operation.
    FloatWidth source;                 ///< The float it reads.
    IntegerWidth result;               ///< The integer it produces.
    Signedness signedness;             ///< Whether the integer is signed.
    Saturation saturation;             ///< Whether it traps or saturates.
};

/// Every truncation of the IR, the trapping ones then the saturating ones, in Operations.def's order.
inline constexpr auto TruncationTable = std::to_array<TruncationRow>({
    { intermediate::Operation::I32TruncF32S,
      FloatWidth::Single,
      IntegerWidth::Bits32,
      Signedness::Signed,
      Saturation::Traps },
    { intermediate::Operation::I32TruncF32U,
      FloatWidth::Single,
      IntegerWidth::Bits32,
      Signedness::Unsigned,
      Saturation::Traps },
    { intermediate::Operation::I32TruncF64S,
      FloatWidth::Double,
      IntegerWidth::Bits32,
      Signedness::Signed,
      Saturation::Traps },
    { intermediate::Operation::I32TruncF64U,
      FloatWidth::Double,
      IntegerWidth::Bits32,
      Signedness::Unsigned,
      Saturation::Traps },
    { intermediate::Operation::I64TruncF32S,
      FloatWidth::Single,
      IntegerWidth::Bits64,
      Signedness::Signed,
      Saturation::Traps },
    { intermediate::Operation::I64TruncF32U,
      FloatWidth::Single,
      IntegerWidth::Bits64,
      Signedness::Unsigned,
      Saturation::Traps },
    { intermediate::Operation::I64TruncF64S,
      FloatWidth::Double,
      IntegerWidth::Bits64,
      Signedness::Signed,
      Saturation::Traps },
    { intermediate::Operation::I64TruncF64U,
      FloatWidth::Double,
      IntegerWidth::Bits64,
      Signedness::Unsigned,
      Saturation::Traps },
    { intermediate::Operation::I32TruncSatF32S,
      FloatWidth::Single,
      IntegerWidth::Bits32,
      Signedness::Signed,
      Saturation::Saturates },
    { intermediate::Operation::I32TruncSatF32U,
      FloatWidth::Single,
      IntegerWidth::Bits32,
      Signedness::Unsigned,
      Saturation::Saturates },
    { intermediate::Operation::I32TruncSatF64S,
      FloatWidth::Double,
      IntegerWidth::Bits32,
      Signedness::Signed,
      Saturation::Saturates },
    { intermediate::Operation::I32TruncSatF64U,
      FloatWidth::Double,
      IntegerWidth::Bits32,
      Signedness::Unsigned,
      Saturation::Saturates },
    { intermediate::Operation::I64TruncSatF32S,
      FloatWidth::Single,
      IntegerWidth::Bits64,
      Signedness::Signed,
      Saturation::Saturates },
    { intermediate::Operation::I64TruncSatF32U,
      FloatWidth::Single,
      IntegerWidth::Bits64,
      Signedness::Unsigned,
      Saturation::Saturates },
    { intermediate::Operation::I64TruncSatF64S,
      FloatWidth::Double,
      IntegerWidth::Bits64,
      Signedness::Signed,
      Saturation::Saturates },
    { intermediate::Operation::I64TruncSatF64U,
      FloatWidth::Double,
      IntegerWidth::Bits64,
      Signedness::Unsigned,
      Saturation::Saturates },
});

/// The row of @p operation in TruncationTable; none for an operation that is no truncation.
[[nodiscard]] constexpr std::optional<TruncationRow> truncationOf(intermediate::Operation operation) noexcept
{
    for (auto const& row: TruncationTable)
        if (row.operation == operation)
            return row;
    return std::nullopt;
}

/// How a value of one IR type moves between its register and memory, at its own width: a row of
/// ValueFormTable. A Ref is one word in a register but two in a RawValue cell (refWordFromCell(),
/// refWordToCell()).
struct ValueFormRow
{
    intermediate::Type type; ///< The type; equal to the row's index.
    Mnemonic load;           ///< Its register from memory.
    Mnemonic store;          ///< Its register to memory.
};

/// Every IR type's forms, in Type order.
inline constexpr auto ValueFormTable = std::to_array<ValueFormRow>({
    { .type = intermediate::Type::I32, .load = Mnemonic::Mov32RM, .store = Mnemonic::Mov32MR },
    { .type = intermediate::Type::I64, .load = Mnemonic::Mov64RM, .store = Mnemonic::Mov64MR },
    { .type = intermediate::Type::F32, .load = Mnemonic::MovssRM, .store = Mnemonic::MovssMR },
    { .type = intermediate::Type::F64, .load = Mnemonic::MovsdRM, .store = Mnemonic::MovsdMR },
    { .type = intermediate::Type::Ref, .load = Mnemonic::Mov64RM, .store = Mnemonic::Mov64MR },
    { .type = intermediate::Type::Pointer, .load = Mnemonic::Mov64RM, .store = Mnemonic::Mov64MR },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ValueFormTable)
            if (static_cast<std::size_t>(row.type) != index++)
                return false;
        return index == static_cast<std::size_t>(intermediate::LastType) + 1;
    }(),
    "ValueFormTable must hold one row per Type, at the index of its enumerator");

/// The forms of @p type.
[[nodiscard]] constexpr ValueFormRow const& valueFormOf(intermediate::Type type) noexcept
{
    return ValueFormTable[static_cast<std::size_t>(type)];
}

/// The registers a Ref conversion names: the Ref's word and two temporaries (which may be one register at
/// encode time, where nothing but the conversion is live in them).
enum class RefRegister : uint8_t
{
    Word,   ///< The Ref's word: kind | heap.kind << 8 | address << 32.
    First,  ///< A temporary.
    Second, ///< A temporary.
};

/// The Ref word of a RawValue cell, {kind | heap.kind << 8, address}, whose words are at @p low and @p high:
/// one routine for selection (over virtual registers) and encoding (over physical ones). @p emit takes a form
/// and its operands; @p reg names a RefRegister in a role.
template <typename Emit, typename Reg>
void refWordFromCell(Emit const& emit,
                     Reg const& reg,
                     machine::MachineOperand low,
                     machine::MachineOperand high)
{
    using machine::OperandRole;
    emit(Mnemonic::Movzx32M16, { reg(RefRegister::Word, OperandRole::Def), low });
    emit(Mnemonic::Mov32RM, { reg(RefRegister::First, OperandRole::Def), high });
    emit(Mnemonic::Shl64RI8,
         { reg(RefRegister::First, OperandRole::UseDef),
           machine::Immediate { .value = intermediate::RefWordAddressShift } });
    emit(Mnemonic::Or64RR,
         { reg(RefRegister::Word, OperandRole::UseDef), reg(RefRegister::First, OperandRole::Use) });
}

/// A Ref word written into a RawValue cell whose words are at @p low and @p high: refWordFromCell()'s
/// inverse.
template <typename Emit, typename Reg>
void refWordToCell(Emit const& emit,
                   Reg const& reg,
                   machine::MachineOperand low,
                   machine::MachineOperand high)
{
    using machine::OperandRole;
    emit(Mnemonic::Movzx32R16,
         { reg(RefRegister::First, OperandRole::Def), reg(RefRegister::Word, OperandRole::Use) });
    emit(Mnemonic::Mov64MR, { low, reg(RefRegister::First, OperandRole::Use) });
    emit(Mnemonic::Mov64RR,
         { reg(RefRegister::Second, OperandRole::Def), reg(RefRegister::Word, OperandRole::Use) });
    emit(Mnemonic::Shr64RI8,
         { reg(RefRegister::Second, OperandRole::UseDef),
           machine::Immediate { .value = intermediate::RefWordAddressShift } });
    emit(Mnemonic::Mov64MR, { high, reg(RefRegister::Second, OperandRole::Use) });
}

/// A register operand naming physical register @p reg, read.
[[nodiscard]] constexpr machine::MachineOperand physicalOperand(Gpr reg) noexcept
{
    return machine::RegisterOperand { .name = machine::RegisterName::of(physical(reg)),
                                      .role = machine::OperandRole::Use };
}

/// @copydoc physicalOperand(Gpr)
[[nodiscard]] constexpr machine::MachineOperand physicalOperand(Xmm reg) noexcept
{
    return machine::RegisterOperand { .name = machine::RegisterName::of(physical(reg)),
                                      .role = machine::OperandRole::Use };
}

/// A memory operand [@p base + @p displacement] on a physical register.
[[nodiscard]] constexpr machine::MachineOperand at(Gpr base, int64_t displacement = 0) noexcept
{
    return machine::MemoryOperand { .base = machine::RegisterName::of(physical(base)),
                                    .displacement = displacement };
}

/// An Immediate operand of @p value.
[[nodiscard]] constexpr machine::MachineOperand immediate(int64_t value) noexcept
{
    return machine::Immediate { .value = value };
}

/// A condition operand: @p condition as an Immediate.
[[nodiscard]] constexpr machine::MachineOperand conditionOperand(ConditionCode condition) noexcept
{
    return machine::Immediate { .value = static_cast<int64_t>(condition) };
}

/// The condition true exactly when @p condition is false.
[[nodiscard]] constexpr ConditionCode inverse(ConditionCode condition) noexcept
{
    return static_cast<ConditionCode>(static_cast<uint8_t>(condition) ^ 1U);
}

/// Appends machine instructions to a block of a function during selection, over virtual registers.
class Writer
{
  public:
    /// A writer appending to @p block of @p function.
    Writer(machine::MachineFunction& function, machine::BlockId block) noexcept:
        _function { &function }, _block { block }
    {
    }

    /// The function written to.
    [[nodiscard]] machine::MachineFunction& function() const noexcept { return *_function; }

    /// The block appended to.
    [[nodiscard]] machine::BlockId block() const noexcept { return _block; }

    /// Appends to @p block from now on.
    void moveTo(machine::BlockId block) noexcept { _block = block; }

    /// A new General virtual register.
    [[nodiscard]] machine::VirtualRegister general() const;

    /// A new Float virtual register.
    [[nodiscard]] machine::VirtualRegister vector() const;

    /// A new virtual register of @p registerClass.
    [[nodiscard]] machine::VirtualRegister create(machine::RegisterClass registerClass) const;

    /// @p reg, read.
    [[nodiscard]] machine::MachineOperand use(machine::VirtualRegister reg) const;

    /// @p reg, written.
    [[nodiscard]] machine::MachineOperand def(machine::VirtualRegister reg) const;

    /// @p reg, read and written (a two-address form).
    [[nodiscard]] machine::MachineOperand useDef(machine::VirtualRegister reg) const;

    /// @p reg, written before the instruction's uses are read.
    [[nodiscard]] machine::MachineOperand early(machine::VirtualRegister reg) const;

    /// A memory operand [@p base + @p displacement] on a virtual register.
    [[nodiscard]] machine::MachineOperand at(machine::VirtualRegister base, int64_t displacement = 0) const;

    /// A memory operand [@p base + @p index × @p scale + @p displacement] on virtual registers.
    [[nodiscard]] machine::MachineOperand indexed(machine::VirtualRegister base,
                                                  machine::VirtualRegister index,
                                                  uint8_t scale,
                                                  int64_t displacement = 0) const;

    /// Appends an instruction of @p mnemonic: its explicit operands, then its implicit ones
    /// (Instructions.hpp).
    void emit(Mnemonic mnemonic, std::initializer_list<machine::MachineOperand> operands) const;

    /// Appends a pseudo-instruction with @p operands.
    void emit(Pseudo pseudo, std::span<machine::MachineOperand const> operands) const;

    /// @copydoc emit(Pseudo, std::span<machine::MachineOperand const>) const
    void emit(Pseudo pseudo, std::initializer_list<machine::MachineOperand> operands) const;

    /// Appends a pseudo-instruction with @p operands.
    /// @return Its index: what a call site is recorded by.
    [[nodiscard]] uint32_t append(Pseudo pseudo, std::span<machine::MachineOperand const> operands) const;

  private:
    machine::MachineFunction* _function;
    machine::BlockId _block;
};

/// What the call and poll sequences need of the target beyond the function: its conventions.
struct CallConventions
{
    machine::ConventionRow const& internal; ///< Between compiled functions.
    machine::ConventionRow const& host;     ///< The runtime stubs'.
};

// --- Selection-time sequences -----------------------------------------------------------------------------

/// @p value in a new General register: mov r32, imm32 or a 64-bit form, the shortest that holds it.
/// @param width The register's width; a 32-bit value is zero-extended.
[[nodiscard]] machine::VirtualRegister materializeInteger(Writer const& writer,
                                                          IntegerWidth width,
                                                          uint64_t value);

/// A float of @p bits in a new Float register, through a General one.
[[nodiscard]] machine::VirtualRegister materializeFloat(Writer const& writer,
                                                        FloatWidth width,
                                                        uint64_t bits);

/// WebAssembly's min: NaN if either is NaN (an arithmetic NaN), -0 below +0. minss both ways, or'd for the
/// zeros, the NaN case selected from a + b on the parity flag of ucomiss a, b.
[[nodiscard]] machine::VirtualRegister floatMin(Writer const& writer,
                                                FloatWidth width,
                                                machine::VirtualRegister a,
                                                machine::VirtualRegister b);

/// WebAssembly's max: as floatMin(), maxss both ways and'd for the zeros.
[[nodiscard]] machine::VirtualRegister floatMax(Writer const& writer,
                                                FloatWidth width,
                                                machine::VirtualRegister a,
                                                machine::VirtualRegister b);

/// abs, neg or copysign of a float: its bits through a General register, exact for every NaN.
enum class SignOperation : uint8_t
{
    Abs,      ///< Clears the sign.
    Neg,      ///< Flips the sign.
    Copysign, ///< The first operand's magnitude, the second's sign.
};

/// @p operation on @p a (and @p b for Copysign).
[[nodiscard]] machine::VirtualRegister floatSign(Writer const& writer,
                                                 FloatWidth width,
                                                 SignOperation operation,
                                                 machine::VirtualRegister a,
                                                 std::optional<machine::VirtualRegister> b);

/// The rounding modes of roundss/roundsd, each with precision exceptions suppressed (bit 3) and MXCSR never
/// consulted (bit 2 clear): nearest-even, floor, ceil, trunc.
enum class RoundingMode : uint8_t
{
    Nearest = 0b1000,
    Floor = 0b1001,
    Ceil = 0b1010,
    Truncate = 0b1011,
};

/// ceil, floor, trunc or nearest with SSE4.1: one roundss/roundsd.
[[nodiscard]] machine::VirtualRegister nearestSse41(Writer const& writer,
                                                    FloatWidth width,
                                                    RoundingMode mode,
                                                    machine::VirtualRegister a);

/// A rounding without SSE4.1: callStub(FloatRound) with the operation and the value's bits; the rounded bits
/// back in a Float register.
[[nodiscard]] machine::VirtualRegister nearestBaseline(Writer const& writer,
                                                       CallConventions const& conventions,
                                                       FloatWidth width,
                                                       FloatRoundOperation operation,
                                                       machine::VirtualRegister a);

/// A trapping truncation (@p row's Saturation Traps): NaN traps InvalidConversionToInteger first, then a
/// value at or past either bound IntegerOverflow, then the conversion.
[[nodiscard]] machine::VirtualRegister truncateChecked(Writer const& writer,
                                                       TruncationRow const& row,
                                                       machine::VirtualRegister a);

/// A saturating truncation (@p row's Saturation Saturates): the conversion, then cmovs to the bounds and to
/// 0.
[[nodiscard]] machine::VirtualRegister truncateSaturating(Writer const& writer,
                                                          TruncationRow const& row,
                                                          machine::VirtualRegister a);

/// i32.trunc_f32_s and its kin to an I32: truncateChecked() of an I32 row.
[[nodiscard]] machine::VirtualRegister truncToI32Checked(Writer const& writer,
                                                         FloatWidth source,
                                                         Signedness signedness,
                                                         machine::VirtualRegister a);

/// i64.trunc_sat_f64_u: truncateSaturating() of its row.
[[nodiscard]] machine::VirtualRegister truncSatF64ToU64(Writer const& writer, machine::VirtualRegister a);

/// f32/f64.convert_i32_u: the zero-extended value converted as a signed 64-bit one.
[[nodiscard]] machine::VirtualRegister convertU32(Writer const& writer,
                                                  FloatWidth width,
                                                  machine::VirtualRegister a);

/// f32.convert_i64_u: the value converted directly when its sign bit is clear, else its half (its lowest bit
/// or'd back in, so the rounding stays exact) converted and doubled; the two selected on the sign.
[[nodiscard]] machine::VirtualRegister convertU64ToF32(Writer const& writer, machine::VirtualRegister a);

/// f64.convert_i64_u: as convertU64ToF32().
[[nodiscard]] machine::VirtualRegister convertU64ToF64(Writer const& writer, machine::VirtualRegister a);

/// popcnt without POPCNT: the bit-parallel sum.
[[nodiscard]] machine::VirtualRegister popcntBaseline(Writer const& writer,
                                                      IntegerWidth width,
                                                      machine::VirtualRegister a);

/// clz without LZCNT: bsr, 2·width − 1 on a zero source (cmove), xor width − 1.
[[nodiscard]] machine::VirtualRegister clzBaseline(Writer const& writer,
                                                   IntegerWidth width,
                                                   machine::VirtualRegister a);

/// ctz without BMI1: bsf, width on a zero source (cmove).
[[nodiscard]] machine::VirtualRegister ctzBaseline(Writer const& writer,
                                                   IntegerWidth width,
                                                   machine::VirtualRegister a);

/// div_s: a zero divisor traps IntegerDivideByZero, MIN / −1 IntegerOverflow, then cdq/cqo and idiv.
[[nodiscard]] machine::VirtualRegister divSigned(Writer const& writer,
                                                 IntegerWidth width,
                                                 machine::VirtualRegister a,
                                                 machine::VirtualRegister b);

/// rem_s: a zero divisor traps; a divisor of −1 is replaced by 1 (cmove), so MIN % −1 is 0 and idiv never
/// faults; then cdq/cqo and idiv.
[[nodiscard]] machine::VirtualRegister remSigned(Writer const& writer,
                                                 IntegerWidth width,
                                                 machine::VirtualRegister a,
                                                 machine::VirtualRegister b);

/// Which of a division's results a sequence returns.
enum class DivisionResult : uint8_t
{
    Quotient,  ///< rax.
    Remainder, ///< rdx.
};

/// div_u or rem_u: a zero divisor traps, then rdx cleared and div.
[[nodiscard]] machine::VirtualRegister divideUnsigned(Writer const& writer,
                                                      IntegerWidth width,
                                                      DivisionResult result,
                                                      machine::VirtualRegister a,
                                                      machine::VirtualRegister b);

/// The canonical NaN of its width for a NaN (cmovp on ucomiss a, a), every other value as it is.
[[nodiscard]] machine::VirtualRegister canonicalizeNan(Writer const& writer,
                                                       FloatWidth width,
                                                       machine::VirtualRegister a);

/// One checkMemory's operands and immediates.
struct MemoryCheck
{
    machine::VirtualRegister index {};            ///< The index, the memory's address type.
    machine::VirtualRegister base {};             ///< The memory's base.
    machine::VirtualRegister bound {};            ///< The memory's size in bytes.
    uint64_t offset = 0;                          ///< The access's static offset.
    uint64_t size = 0;                            ///< The access's width in bytes.
    SpectreGuard spectre = SpectreGuard::Enabled; ///< Whether the pointer is clamped.
};

/// checkMemory of a 32-bit memory: the index zero-extended, end = index + offset + size (no overflow in 64
/// bits), `cmp end, bound; ja trap`, then under SpectreGuard::Enabled `cmova` replacing the effective pointer
/// base + index + offset by 0. An offset past INT32_MAX takes mov imm64 + add instead of lea.
/// @return The guarded pointer.
[[nodiscard]] machine::VirtualRegister checkMemory32(Writer const& writer, MemoryCheck const& check);

/// checkMemory of a 64-bit memory: end = index + (offset + size) with the carry folded in (cmovb to all
/// ones), so one `cmp end, bound; ja trap` and one `cmova` cover both the overflow and the bound.
/// @return The guarded pointer.
[[nodiscard]] machine::VirtualRegister checkMemory64(Writer const& writer, MemoryCheck const& check);

/// guardAddress: @p address + @p offset, kept 0 when @p address is 0 (test, cmove) under
/// SpectreGuard::Enabled.
[[nodiscard]] machine::VirtualRegister guardAddress(Writer const& writer,
                                                    machine::VirtualRegister address,
                                                    uint64_t offset,
                                                    SpectreGuard spectre);

/// inBoundsAddress: @p base + @p offset: lea when the offset fits a signed 32-bit displacement, else mov
/// imm64 and add.
[[nodiscard]] machine::VirtualRegister inBoundsAddress(Writer const& writer,
                                                       machine::VirtualRegister base,
                                                       uint64_t offset);

/// One checkTable's operands: the index and the table's view, loaded.
struct TableCheck
{
    machine::VirtualRegister index {};                      ///< The index.
    intermediate::Type indexType = intermediate::Type::I32; ///< I32 for a 32-bit table, I64 for a 64-bit one.
    machine::VirtualRegister data {};                       ///< TableView::data.
    machine::VirtualRegister size {};                       ///< TableView::size.
    runtime::TrapCode code =
        runtime::TrapCode::Unreachable; ///< The trap: UndefinedElement names the index as its subject.
    SpectreGuard spectre = SpectreGuard::Enabled; ///< Whether the pointer is clamped.
};

/// checkTable: `cmp index, size; jae trap`, the element's address data + index × sizeof(Ref), and under
/// SpectreGuard::Enabled `cmovae` replacing it by 0.
[[nodiscard]] machine::VirtualRegister checkTable(Writer const& writer, TableCheck const& check);

/// One checkDispatch's operands.
struct DispatchCheck
{
    machine::VirtualRegister reference {}; ///< The non-null Ref's word.
    machine::VirtualRegister index {};     ///< The element index it came from: the trap's subject.
    intermediate::Type indexType = intermediate::Type::I32;  ///< The index's type.
    machine::VirtualRegister view {};                        ///< The store's DispatchView.
    runtime::TrapCode code = runtime::TrapCode::Unreachable; ///< The trap.
    SpectreGuard spectre = SpectreGuard::Enabled; ///< Whether the address and the pointer are clamped.
};

/// checkDispatch: the reference's address against the view's rows (`jae trap`, naming the element index),
/// then its row's address through its chunk. Under SpectreGuard::Enabled the address is clamped to 0 before
/// the chunk is loaded, and the row's address replaced by 0 where it traps.
[[nodiscard]] machine::VirtualRegister checkDispatch(Writer const& writer, DispatchCheck const& check);

/// The dispatch row of function address @p address (a valid one) in @p view: its chunk, then the row in it.
[[nodiscard]] machine::VirtualRegister dispatchRowOf(Writer const& writer,
                                                     machine::VirtualRegister view,
                                                     machine::VirtualRegister address);

/// Where a table element's fields lie: the immediates of loadTableElement and storeTableElement.
struct RefOffsets
{
    int64_t kind = 0;          ///< Ref::kind.
    int64_t heapKind = 0;      ///< Ref::heap's kind.
    int64_t heapTypeIndex = 0; ///< Ref::heap's type index.
    int64_t address = 0;       ///< Ref::address.
};

/// A table element read field by field into a Ref's word: kind | heap.kind << 8 | address << 32.
[[nodiscard]] machine::VirtualRegister loadRef(Writer const& writer,
                                               machine::VirtualRegister pointer,
                                               RefOffsets const& offsets);

/// A Ref's word written into a table element field by field, its heap type index 0.
void storeRef(Writer const& writer,
              machine::VirtualRegister pointer,
              machine::VirtualRegister word,
              RefOffsets const& offsets);

/// The IR's enterFrame: the call depth (`sub 1; jb trap`) then the stack values (`sub cost; jb trap`), each
/// trapping CallStackExhausted, after the FunctionEntry poll as the reference orders them. The native stack
/// was checked by the prologue (enterFrame()), which must precede the poll: the poll's slow path is a call,
/// which leaves from the frame's bottom.
void chargeFrame(Writer const& writer, uint64_t frameCost);

/// The IR's leaveFrame: the depth and the stack values given back.
void releaseFrame(Writer const& writer, uint64_t frameCost);

/// A value passed to or returned from a call.
struct CallValue
{
    machine::VirtualRegister reg {};                   ///< Its register.
    intermediate::Type type = intermediate::Type::I64; ///< Its type.
};

/// Whether a call's operands end in its signature's types, as Immediates.
enum class TrailingTypes : uint8_t
{
    No,  ///< They do not.
    Yes, ///< The parameter count, each parameter's type, then each result's (CallDispatched's host path).
};

/// What a call sequence emits around its pseudo-instruction.
struct CallShape
{
    Pseudo pseudo;                                 ///< CallDirect, CallDispatched or CallStub.
    CallLayout layout;                             ///< Its operands' layout.
    machine::ConventionRow const& convention;      ///< Its convention.
    std::span<machine::MachineOperand const> lead; ///< Its leading operands.
    std::span<CallValue const> arguments;          ///< Its arguments.
    std::span<intermediate::Type const> results;   ///< Its results' types.
    TrailingTypes types = TrailingTypes::No;       ///< Whether its types follow as Immediates.
    /// The outgoing bytes the call needs at least, beyond its own arguments and results: a dispatched call's
    /// host-call path calls CallHost under the host convention from the same frame bottom.
    uint32_t minimumOutgoing = 0;
};

/// A call under @p shape: stack arguments stored to the outgoing area, the result area's pointer passed
/// when the convention returns through one, the pseudo-instruction with an argument at each register's
/// position and a result at each result register's, the call site recorded with the outgoing bytes it needs,
/// and the results in memory loaded after it.
/// @return The results' registers, in order.
std::pmr::vector<machine::VirtualRegister> emitCall(Writer const& writer, CallShape const& shape);

/// callDirect: a call of the module's function with code index @p codeIndex.
std::pmr::vector<machine::VirtualRegister> callDirect(Writer const& writer,
                                                      CallConventions const& conventions,
                                                      uint32_t codeIndex,
                                                      std::span<CallValue const> arguments,
                                                      std::span<intermediate::Type const> results);

/// callIndirect: a call through the dispatch row @p row (checkDispatch's), of function address
/// @p reference >> 32; then r14 restored.
std::pmr::vector<machine::VirtualRegister> callIndirect(Writer const& writer,
                                                        CallConventions const& conventions,
                                                        machine::VirtualRegister row,
                                                        machine::VirtualRegister reference,
                                                        std::span<CallValue const> arguments,
                                                        std::span<intermediate::Type const> results);

/// callImport: a call of imported function @p function, through its address in the instance's function
/// table and its dispatch row; then r14 restored.
std::pmr::vector<machine::VirtualRegister> callImport(Writer const& writer,
                                                      CallConventions const& conventions,
                                                      uint32_t function,
                                                      std::span<CallValue const> arguments,
                                                      std::span<intermediate::Type const> results);

/// callStub: runtime stub @p stub with the execution state, the instance context and @p operands (each a
/// 64-bit word), under the host convention; a status other than StubSucceeded leaves through the trap exit.
/// @return The stub's value.
machine::VirtualRegister callStub(Writer const& writer,
                                  CallConventions const& conventions,
                                  RuntimeStub stub,
                                  std::span<machine::VirtualRegister const> operands);

/// The outgoing bytes a stub call under @p host with @p operands operands needs: its stack arguments and,
/// for a convention that returns StubResult through memory, the result area.
[[nodiscard]] uint32_t stubOutgoingBytes(machine::ConventionRow const& host,
                                         std::size_t operands,
                                         std::pmr::memory_resource& memory);

// --- Encode-time sequences --------------------------------------------------------------------------------

/// One row an Assembler emitted, for the shape tests: its form, and its condition when it has one.
struct TraceEntry
{
    Mnemonic mnemonic = Mnemonic::Ud2;         ///< The form.
    std::optional<ConditionCode> condition {}; ///< A jcc's, cmovcc's or setcc's condition.
};

/// Emits encoding rows into a code buffer, recording the first refusal (a compiler bug) and, when given a
/// trace, each row emitted, for the shape tests.
class Assembler
{
  public:
    /// An assembler into @p buffer, branching to @p blockLabels.
    /// @param buffer The module's code buffer.
    /// @param blockLabels Per block, its label.
    /// @param trace Where each emitted row is recorded; null for none.
    Assembler(machine::CodeBuffer& buffer,
              std::span<machine::CodeLabel const> blockLabels,
              std::pmr::vector<TraceEntry>* trace) noexcept:
        _buffer { &buffer }, _blockLabels { blockLabels }, _trace { trace }
    {
    }

    /// Emits @p mnemonic with its explicit @p operands; a ConstantOperand names @p constants' labels.
    void emit(Mnemonic mnemonic,
              std::span<machine::MachineOperand const> operands,
              std::span<machine::CodeLabel const> constants = {});

    /// @copydoc emit(Mnemonic, std::span<machine::MachineOperand const>, std::span<machine::CodeLabel const>)
    void emit(Mnemonic mnemonic, std::initializer_list<machine::MachineOperand> operands);

    /// A jump of @p mnemonic (Jmp or Jcc with @p condition) to @p label, which need be no block's.
    void jumpTo(machine::CodeLabel label, std::optional<ConditionCode> condition = std::nullopt);

    /// A call (rel32) of @p label.
    void callTo(machine::CodeLabel label);

    /// A new label.
    [[nodiscard]] machine::CodeLabel label() { return _buffer->createLabel(); }

    /// Binds @p label here.
    void bind(machine::CodeLabel label) { _buffer->bind(label); }

    /// The buffer.
    [[nodiscard]] machine::CodeBuffer& buffer() const noexcept { return *_buffer; }

    /// The label of @p block (Require: one of the function's).
    [[nodiscard]] machine::CodeLabel blockLabel(machine::BlockId block) const noexcept;

    /// The first refusal; none while every row encoded.
    [[nodiscard]] std::optional<EncodeError> failure() const noexcept { return _failure; }

  private:
    void record(Mnemonic mnemonic,
                std::expected<void, EncodeError> const& encoded,
                std::optional<ConditionCode> condition = std::nullopt);

    machine::CodeBuffer* _buffer;
    std::span<machine::CodeLabel const> _blockLabels;
    std::pmr::vector<TraceEntry>* _trace;
    std::optional<EncodeError> _failure {};
};

/// A path the encoder places after a function's body: its label and what it does.
struct OutOfLinePath
{
    /// What kind of path.
    enum class Kind : uint8_t
    {
        TrapSubject, ///< A trap site that also stores a subject register.
        PollSlow,    ///< A poll's slow path.
        HostCall,    ///< A dispatched call's host-call path.
        JumpTable,   ///< A switch's jump table.
        EntryPoll,   ///< The FunctionEntry poll's slow path, before the frame.
    };

    Kind kind = Kind::TrapSubject;                   ///< Which.
    machine::CodeLabel label {};                     ///< Where it starts.
    machine::CodeLabel resume {};                    ///< Where it returns to (PollSlow, HostCall).
    uint32_t instruction = 0;                        ///< The instruction it belongs to.
    runtime::TrapCode code {};                       ///< TrapSubject: its trap.
    std::optional<machine::PhysicalRegister> reg {}; ///< TrapSubject: the subject; PollSlow: the countdown.
};

/// What the encode-time sequences of one function share.
class FunctionEmitter
{
  public:
    /// An emitter for @p function with @p frame.
    FunctionEmitter(Assembler& assembler,
                    machine::MachineFunction const& function,
                    machine::FrameLayout const& frame,
                    CallConventions const& conventions,
                    machine::ModuleLabels const& labels,
                    std::pmr::memory_resource& memory);

    /// The assembler.
    [[nodiscard]] Assembler& assembler() const noexcept { return *_assembler; }

    /// The frame.
    [[nodiscard]] machine::FrameLayout const& frame() const noexcept { return *_frame; }

    /// The label of a trap site of @p code without a subject, shared within the function.
    [[nodiscard]] machine::CodeLabel trapSiteOf(runtime::TrapCode code);

    /// The label every failed stub branches to: straight to the trap exit.
    [[nodiscard]] machine::CodeLabel pendingExit() const noexcept { return _pendingExit; }

    /// Defers @p path until after the body.
    void defer(OutOfLinePath const& path) { _deferred.push_back(path); }

    /// The prologue (enterFrame()).
    void prologue();

    /// Expands the pseudo-instruction at @p instruction, whose next block in the layout is @p next.
    void expand(uint32_t instruction, std::optional<machine::BlockId> next);

    /// Emits every deferred path and trap site, after the body.
    void finish();

    /// The function.
    [[nodiscard]] machine::MachineFunction const& function() const noexcept { return *_function; }

    /// The conventions.
    [[nodiscard]] CallConventions const& conventions() const noexcept { return _conventions; }

    /// The module's labels.
    [[nodiscard]] machine::ModuleLabels const& labels() const noexcept { return *_labels; }

    /// What the temporaries allocate from.
    [[nodiscard]] std::pmr::memory_resource& memory() const noexcept { return *_memory; }

  private:
    Assembler* _assembler;
    machine::MachineFunction const* _function;
    machine::FrameLayout const* _frame;
    CallConventions _conventions;
    machine::ModuleLabels const* _labels;
    std::pmr::memory_resource* _memory;
    std::pmr::vector<std::pair<runtime::TrapCode, machine::CodeLabel>> _trapSites;
    std::pmr::vector<OutOfLinePath> _deferred;
    machine::CodeLabel _pendingExit;
};

/// Allocates @p bytes of stack below rsp: one sub, or past ProbePageBytes a loop that moves rsp a page at a
/// time and touches each page, top down, as Windows' guard pages require of every OS row; then the rest.
/// @param counter The loop's count: a register free here, which it clobbers.
void allocateStack(Assembler& assembler, uint64_t bytes, Gpr counter);

/// The prologue: push rbp and mov rbp, rsp; the entry's steps the Entry pseudo-instruction asks for, in the
/// reference's order (the FunctionEntry poll, the call depth, the stack values); the native stack check
/// `rsp − (frameBytes + StubReserveBytes)` against ExecutionState::nativeStackLimit, trapping
/// CallStackExhausted, before the frame is allocated; then
/// the frame (one sub, or a probe loop a page at a time when it exceeds ProbePageBytes); then the
/// callee-saved registers saved at the top of it.
void enterFrame(FunctionEmitter& emitter);

/// The stack a function's entry takes before its frame is allocated, at most: what its caller pushed (the
/// return address), the saved frame pointer, and the entry poll's slow path (entrySlowPath()) with the
/// arguments it keeps across the host call. With StubStackBudgetBytes for the stub itself it must fit
/// StubReserveBytes, which every caller's native check keeps free below its own frame.
[[nodiscard]] uint64_t entryPathBytes(CallConventions const& conventions, std::pmr::memory_resource& memory);

/// The stack a runtime stub other than a host call may take below its caller on x86-64: its row of
/// machine::StubStackBudgetTable, which StackBounds.stubReserveCoversEveryStub measures every stub against by
/// stack painting.
inline constexpr auto StubStackBudgetBytes = machine::stubStackBudgetBytes(machine::Architecture::X64);

/// The FunctionEntry poll's slow path, before the frame: every argument register of the internal convention
/// kept on the reserve, callStubNow(PollSlow), a failed status to the trap exit, the arguments restored, back
/// to the prologue.
void entrySlowPath(FunctionEmitter& emitter, OutOfLinePath const& path);

/// The epilogue: the callee-saved registers restored, mov rsp, rbp, pop rbp, ret.
void leaveFrame(FunctionEmitter& emitter);

/// The poll fast path (P5 Decision "Poll fast path"): load the interrupt flag's address, cmp byte [flag], 0,
/// jne slow, test steps, steps, je slow, dec steps; the slow path (pollSlowPath()) out of line.
/// @param steps The countdown's register; none for pollStored, whose countdown is in the execution state.
void poll(FunctionEmitter& emitter, uint32_t instruction, std::optional<machine::PhysicalRegister> steps);

/// A poll's slow path: the countdown stored, callStub(PollSlow) with the poll site, a failed status to the
/// trap exit, the countdown reloaded, back to the fast path's end.
void pollSlowPath(FunctionEmitter& emitter, OutOfLinePath const& path);

/// A trap site: PendingAbrupt's kind, code and subject presence written in one store, the subject's bits
/// when it has one, then a jump to the trap exit. The step countdown is already stored: every trap site is
/// preceded by the IR's storeContext (VerifierErrorCode::CountdownNotStored), and a prologue's trap comes
/// before the function changed it.
void trapSite(FunctionEmitter& emitter,
              runtime::TrapCode code,
              std::optional<machine::PhysicalRegister> subject);

/// The jump table of a switch: a cmp and jae to the fallback, or under SpectreGuard::Enabled a cmovae of the
/// index to the fallback's entry (the last of count + 1), then lea of the table, movsxd of the entry, add and
/// jmp; the table (one TableEntry32 per entry) deferred after the body.
void switchTable(FunctionEmitter& emitter, uint32_t instruction);

/// A dispatched call: the row's code null takes the host-call path (hostCallPath()) out of line; else r14
/// from the row's context and a call of its code.
void callDispatched(FunctionEmitter& emitter, uint32_t instruction);

/// The host-call path of a dispatched call: its arguments marshalled into ExecutionState::hostScratch as
/// RawValues, callStub(CallHost) with the function address, a failed status to the trap exit, the results
/// read back into their registers or the result area, back to after the call.
void hostCallPath(FunctionEmitter& emitter, OutOfLinePath const& path);

/// Encodes @p function, allocated, with @p frame into @p buffer: what X64Target::encode() runs. The blocks
/// in linearize()'s layout, a jump to the next block left out; before each instruction the moves the
/// allocator inserted (a slot-to-slot one as push and pop of the memory operands); each pseudo-instruction
/// expanded; the out-of-line paths after the body.
/// @param trace Where each emitted row is recorded, for the shape tests; null for none.
/// @return Nothing; or InternalVerificationFailed (pass "encoder") for an instruction no row encodes.
[[nodiscard]] std::expected<void, CompileError> encodeFunction(machine::MachineFunction const& function,
                                                               machine::FrameLayout const& frame,
                                                               CallConventions const& conventions,
                                                               machine::ModuleLabels const& labels,
                                                               machine::CodeBuffer& buffer,
                                                               std::pmr::vector<TraceEntry>* trace,
                                                               std::pmr::memory_resource& memory);

} // namespace isoline::compiler::x86_64
