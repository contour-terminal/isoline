// SPDX-License-Identifier: Apache-2.0
#pragma once

// x86-64's instruction encoder, driven by one table. Each row of Encodings.def is an instruction form: its
// prefix, REX.W, opcode, ModRM use and immediate width, the kinds and roles of its operands, and its Intel
// syntax. encode() interprets a row for the operands it is given, with no code of any one instruction: REX
// W/R/X/B (and a REX of its own for spl, bpl, sil and dil), ModRM and SIB with their special cases (rsp and
// r12 bases take a SIB, rbp and r13 bases a displacement), disp8 or disp32, rip-relative operands and rel32
// targets as CodeBuffer fixups, and the immediate. What it refuses, it refuses by EncodeError, never by
// emitting something else.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/Registers.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>

namespace isoline::compiler::x86_64
{

/// An instruction form of the encoding table: one enumerator per row of Encodings.def, in its order.
enum class Mnemonic : uint8_t
{
#define ISOLINE_ENCODING(mnemonic, form, prefix, rexW, opcode, modRm, immediate, roles, text) mnemonic,
#include <isoline/compiler/x86_64/Encodings.def>
#undef ISOLINE_ENCODING
};

/// Where an explicit operand goes in the encoding.
enum class Field : uint8_t
{
    ModRmReg,       ///< ModRM.reg (REX.R): a register.
    ModRmRm,        ///< ModRM.rm (REX.B, with SIB and REX.X for memory): a register or a memory operand.
    OpcodeRegister, ///< The opcode's low three bits (REX.B): a register.
    Immediate,      ///< The immediate field.
    Relative,       ///< A rel32 to a label.
    Condition,      ///< A condition code, added to the opcode's last byte.
};

/// Where a form places its explicit operands: the Form column of Encodings.def.
enum class Form : uint8_t
{
    ZeroOperands,            ///< No explicit operand.
    RegisterModRm,           ///< ModRM.reg, ModRM.rm (Intel's RM).
    ModRmRegister,           ///< ModRM.rm, ModRM.reg (Intel's MR).
    ModRmOnly,               ///< ModRM.rm, an opcode extension in ModRM.reg (Intel's M).
    ModRmImmediate,          ///< ModRM.rm, the immediate (Intel's MI).
    RegisterModRmImmediate,  ///< ModRM.reg, ModRM.rm, the immediate (Intel's RMI).
    OpcodeRegister,          ///< The opcode's low bits (Intel's O).
    OpcodeRegisterImmediate, ///< The opcode's low bits, the immediate (Intel's OI).
    Relative,                ///< A rel32 (Intel's D).
    ConditionRelative,       ///< A condition code, a rel32 (jcc).
    ConditionRegisterModRm,  ///< A condition code, ModRM.reg, ModRM.rm (cmovcc).
    ConditionModRm,          ///< A condition code, ModRM.rm (setcc).
};

/// The most explicit operands an instruction form has.
inline constexpr auto MaximumExplicitOperands = std::size_t { 3 };

/// One Form's fields, by explicit operand.
struct FormRow
{
    Form form;                                            ///< The form; equal to the row's index.
    std::array<Field, MaximumExplicitOperands> fields {}; ///< Its operands' fields; the first count used.
    uint8_t count = 0;                                    ///< How many explicit operands it has.
};

/// Every Form's fields, in enumerator order.
inline constexpr auto FormTable = std::to_array<FormRow>({
    { .form = Form::ZeroOperands, .fields = {}, .count = 0 },
    { .form = Form::RegisterModRm, .fields = { Field::ModRmReg, Field::ModRmRm }, .count = 2 },
    { .form = Form::ModRmRegister, .fields = { Field::ModRmRm, Field::ModRmReg }, .count = 2 },
    { .form = Form::ModRmOnly, .fields = { Field::ModRmRm }, .count = 1 },
    { .form = Form::ModRmImmediate, .fields = { Field::ModRmRm, Field::Immediate }, .count = 2 },
    { .form = Form::RegisterModRmImmediate,
      .fields = { Field::ModRmReg, Field::ModRmRm, Field::Immediate },
      .count = 3 },
    { .form = Form::OpcodeRegister, .fields = { Field::OpcodeRegister }, .count = 1 },
    { .form = Form::OpcodeRegisterImmediate,
      .fields = { Field::OpcodeRegister, Field::Immediate },
      .count = 2 },
    { .form = Form::Relative, .fields = { Field::Relative }, .count = 1 },
    { .form = Form::ConditionRelative, .fields = { Field::Condition, Field::Relative }, .count = 2 },
    { .form = Form::ConditionRegisterModRm,
      .fields = { Field::Condition, Field::ModRmReg, Field::ModRmRm },
      .count = 3 },
    { .form = Form::ConditionModRm, .fields = { Field::Condition, Field::ModRmRm }, .count = 2 },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: FormTable)
        {
            if (static_cast<std::size_t>(row.form) != index || row.count > MaximumExplicitOperands)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(Form::ConditionModRm) + 1;
    }(),
    "FormTable must hold one row per Form, at the index of its enumerator");

/// The row of @p form.
[[nodiscard]] constexpr FormRow const& formRow(Form form) noexcept
{
    return FormTable[static_cast<std::size_t>(form)];
}

/// The prefix a row emits before REX: the Prefix column.
enum class Prefix : uint8_t
{
    None,  ///< None.
    Hex66, ///< 0x66: the operand-size override, or SSE's mandatory 66.
    HexF2, ///< 0xF2: SSE's scalar-double prefix.
    HexF3, ///< 0xF3: SSE's scalar-single prefix, and lzcnt's, tzcnt's and popcnt's.
};

/// The REX.W bit a row sets: the RexW column.
enum class RexW : uint8_t
{
    W0, ///< The default operand size (32 bits; 64 for push, pop, jmp and call).
    W1, ///< A 64-bit operand size.
};

/// What a row's ModRM.reg holds: the ModRm column.
enum class ModRm : uint8_t
{
    None,     ///< The form has no ModRM byte.
    Register, ///< An explicit operand (/r).
    Digit0,   ///< The opcode extension /0.
    Digit1,   ///< The opcode extension /1.
    Digit2,   ///< The opcode extension /2.
    Digit3,   ///< The opcode extension /3.
    Digit4,   ///< The opcode extension /4.
    Digit5,   ///< The opcode extension /5.
    Digit6,   ///< The opcode extension /6.
    Digit7,   ///< The opcode extension /7.
};

/// The width of a row's immediate field: the Immediate column.
enum class Immediate : uint8_t
{
    None,       ///< No immediate.
    Bits8,      ///< One byte.
    Bits32,     ///< Four bytes.
    Bits64,     ///< Eight bytes (movabs).
    Relative32, ///< A rel32 to a label.
};

/// A row's opcode: one to three bytes, escapes included.
struct OpcodeBytes
{
    std::array<uint8_t, 3> bytes {}; ///< The bytes; the first count used.
    uint8_t count = 0;               ///< How many.
};

/// An opcode of @p bytes: what the Opcode column's `(0x0f, 0xaf)` expands to.
template <typename... Bytes>
[[nodiscard]] constexpr OpcodeBytes opcodeBytes(Bytes... bytes) noexcept
{
    static_assert(sizeof...(Bytes) >= 1 && sizeof...(Bytes) <= 3, "an opcode has one to three bytes");
    return OpcodeBytes { .bytes = { static_cast<uint8_t>(bytes)... }, .count = sizeof...(Bytes) };
}

/// What one operand of an instruction form is.
enum class OperandKind : uint8_t
{
    General8,   ///< An 8-bit General register (al … r15b; spl, bpl, sil and dil take a REX).
    General16,  ///< A 16-bit General register.
    General32,  ///< A 32-bit General register.
    General64,  ///< A 64-bit General register.
    Vector,     ///< An XMM register.
    Memory8,    ///< A byte in memory.
    Memory16,   ///< A word in memory.
    Memory32,   ///< A doubleword in memory.
    Memory64,   ///< A quadword in memory.
    Memory128,  ///< Sixteen bytes in memory.
    Address,    ///< An address, not accessed (lea).
    Signed8,    ///< An 8-bit immediate, sign-extended.
    Unsigned8,  ///< An 8-bit immediate, unsigned (a shift count).
    Signed32,   ///< A 32-bit immediate, sign-extended to 64 bits.
    Unsigned32, ///< A 32-bit immediate of a 32-bit operation.
    Signed64,   ///< A 64-bit immediate.
    Rounding,   ///< roundss's 8-bit rounding mode; bit 2 (use MXCSR) is never set.
    Condition,  ///< A condition code (ConditionCode).
    Label,      ///< A branch or call target.
};

/// Whether an operand is one of the explicit operands encode() takes, or a fixed register the encoding
/// names without one.
enum class Placement : uint8_t
{
    Explicit, ///< In the operands encode() takes, in the form's order.
    Implicit, ///< Not an operand: the fixed register the instruction reads or writes.
};

/// One operand of an instruction form: its kind, what the instruction does with it, and for an implicit one
/// the register.
struct OperandSlot
{
    OperandKind kind = OperandKind::General64;             ///< What it is.
    machine::OperandRole role = machine::OperandRole::Use; ///< Read, written, or both.
    Placement placement = Placement::Explicit;             ///< Explicit or implicit.
    std::optional<Gpr> fixed {};                           ///< An implicit operand's register.
};

/// The spelling of Encodings.def's Roles column.
namespace roles
{
    inline constexpr auto G8 = OperandKind::General8;         ///< An 8-bit General register.
    inline constexpr auto G16 = OperandKind::General16;       ///< A 16-bit General register.
    inline constexpr auto G32 = OperandKind::General32;       ///< A 32-bit General register.
    inline constexpr auto G64 = OperandKind::General64;       ///< A 64-bit General register.
    inline constexpr auto X = OperandKind::Vector;            ///< An XMM register.
    inline constexpr auto M8 = OperandKind::Memory8;          ///< A byte in memory.
    inline constexpr auto M16 = OperandKind::Memory16;        ///< A word in memory.
    inline constexpr auto M32 = OperandKind::Memory32;        ///< A doubleword in memory.
    inline constexpr auto M64 = OperandKind::Memory64;        ///< A quadword in memory.
    inline constexpr auto M128 = OperandKind::Memory128;      ///< Sixteen bytes in memory.
    inline constexpr auto Address = OperandKind::Address;     ///< An address (lea).
    inline constexpr auto S8 = OperandKind::Signed8;          ///< A sign-extended 8-bit immediate.
    inline constexpr auto U8 = OperandKind::Unsigned8;        ///< An unsigned 8-bit immediate.
    inline constexpr auto S32 = OperandKind::Signed32;        ///< A sign-extended 32-bit immediate.
    inline constexpr auto U32 = OperandKind::Unsigned32;      ///< A 32-bit operation's immediate.
    inline constexpr auto S64 = OperandKind::Signed64;        ///< A 64-bit immediate.
    inline constexpr auto Rounding = OperandKind::Rounding;   ///< A rounding mode.
    inline constexpr auto Condition = OperandKind::Condition; ///< A condition code.
    inline constexpr auto Label = OperandKind::Label;         ///< A branch or call target.

    /// An explicit operand of @p kind the instruction reads.
    constexpr OperandSlot use(OperandKind kind) noexcept
    {
        return OperandSlot { .kind = kind, .role = machine::OperandRole::Use };
    }

    /// An explicit operand of @p kind the instruction writes.
    constexpr OperandSlot def(OperandKind kind) noexcept
    {
        return OperandSlot { .kind = kind, .role = machine::OperandRole::Def };
    }

    /// An explicit operand of @p kind the instruction reads and writes (two-address).
    constexpr OperandSlot useDef(OperandKind kind) noexcept
    {
        return OperandSlot { .kind = kind, .role = machine::OperandRole::UseDef };
    }

    /// The register @p reg, which the instruction reads without naming it.
    constexpr OperandSlot implicitUse(Gpr reg) noexcept
    {
        return OperandSlot { .kind = OperandKind::General64,
                             .role = machine::OperandRole::Use,
                             .placement = Placement::Implicit,
                             .fixed = reg };
    }

    /// The register @p reg, which the instruction writes without naming it.
    constexpr OperandSlot implicitDef(Gpr reg) noexcept
    {
        return OperandSlot { .kind = OperandKind::General64,
                             .role = machine::OperandRole::Def,
                             .placement = Placement::Implicit,
                             .fixed = reg };
    }

    /// The register @p reg, which the instruction reads and writes without naming it.
    constexpr OperandSlot implicitUseDef(Gpr reg) noexcept
    {
        return OperandSlot { .kind = OperandKind::General64,
                             .role = machine::OperandRole::UseDef,
                             .placement = Placement::Implicit,
                             .fixed = reg };
    }

    /// The Roles column's operands as an array: what `(use(G64), …)` expands to.
    template <typename... Slots>
    constexpr std::array<OperandSlot, sizeof...(Slots)> slots(Slots... operands) noexcept
    {
        return { operands... };
    }

// Each row's operands, named after its mnemonic.
#define ISOLINE_ENCODING(                                                                           \
    rowMnemonic, rowForm, rowPrefix, rowRexW, rowOpcode, rowModRm, rowImmediate, rowRoles, rowText) \
    inline constexpr auto rowMnemonic = slots rowRoles;
#include <isoline/compiler/x86_64/Encodings.def>
#undef ISOLINE_ENCODING
} // namespace roles

/// One instruction form: a row of Encodings.def.
struct EncodingRow
{
    Mnemonic mnemonic;                  ///< The form; equal to the row's index.
    std::string_view name;              ///< The mnemonic's name.
    Form form;                          ///< Where its explicit operands go.
    Prefix prefix;                      ///< Its prefix before REX.
    RexW rexW;                          ///< Its REX.W.
    OpcodeBytes opcode;                 ///< Its opcode.
    ModRm modRm;                        ///< What its ModRM.reg holds.
    Immediate immediate;                ///< Its immediate field.
    std::span<OperandSlot const> slots; ///< Its operands: the explicit ones in the form's order, then the
                                        ///< implicit ones.
    std::string_view intelTemplate;     ///< Its Intel syntax, {n} naming the n-th explicit operand.
};

/// Every instruction form, in Mnemonic order.
inline constexpr auto EncodingTable = std::to_array<EncodingRow>({
#define ISOLINE_ENCODING(                                                                           \
    rowMnemonic, rowForm, rowPrefix, rowRexW, rowOpcode, rowModRm, rowImmediate, rowRoles, rowText) \
    EncodingRow { .mnemonic = Mnemonic::rowMnemonic,                                                \
                  .name = #rowMnemonic,                                                             \
                  .form = Form::rowForm,                                                            \
                  .prefix = Prefix::rowPrefix,                                                      \
                  .rexW = RexW::rowRexW,                                                            \
                  .opcode = opcodeBytes rowOpcode,                                                  \
                  .modRm = ModRm::rowModRm,                                                         \
                  .immediate = Immediate::rowImmediate,                                             \
                  .slots = roles::rowMnemonic,                                                      \
                  .intelTemplate = (rowText) },
#include <isoline/compiler/x86_64/Encodings.def>
#undef ISOLINE_ENCODING
});

/// How many rows the encoding table has: one per Mnemonic.
inline constexpr auto MnemonicCount = EncodingTable.size();

/// The row of @p mnemonic.
[[nodiscard]] constexpr EncodingRow const& encodingRow(Mnemonic mnemonic) noexcept
{
    return EncodingTable[static_cast<std::size_t>(mnemonic)];
}

/// The explicit operands of @p row: the first formRow(row.form).count of its slots.
[[nodiscard]] constexpr std::span<OperandSlot const> explicitSlots(EncodingRow const& row) noexcept
{
    return row.slots.first(formRow(row.form).count);
}

/// Whether @p kind is a register.
[[nodiscard]] constexpr bool isRegisterKind(OperandKind kind) noexcept
{
    return kind == OperandKind::General8 || kind == OperandKind::General16 || kind == OperandKind::General32
           || kind == OperandKind::General64 || kind == OperandKind::Vector;
}

/// Whether @p kind is a memory operand or an address.
[[nodiscard]] constexpr bool isMemoryKind(OperandKind kind) noexcept
{
    return kind == OperandKind::Memory8 || kind == OperandKind::Memory16 || kind == OperandKind::Memory32
           || kind == OperandKind::Memory64 || kind == OperandKind::Memory128 || kind == OperandKind::Address;
}

/// The width of the immediate field @p kind takes; None for a kind that is no immediate.
[[nodiscard]] constexpr Immediate immediateWidthOf(OperandKind kind) noexcept
{
    switch (kind)
    {
        case OperandKind::Signed8:
        case OperandKind::Unsigned8:
        case OperandKind::Rounding: return Immediate::Bits8;
        case OperandKind::Signed32:
        case OperandKind::Unsigned32: return Immediate::Bits32;
        case OperandKind::Signed64: return Immediate::Bits64;
        case OperandKind::Label: return Immediate::Relative32;
        default: return Immediate::None;
    }
}

/// Whether @p kind may stand in @p field.
[[nodiscard]] constexpr bool fits(Field field, OperandKind kind) noexcept
{
    switch (field)
    {
        case Field::ModRmReg: return isRegisterKind(kind);
        case Field::ModRmRm: return isRegisterKind(kind) || isMemoryKind(kind);
        case Field::OpcodeRegister: return isRegisterKind(kind) && kind != OperandKind::Vector;
        case Field::Immediate:
            return immediateWidthOf(kind) != Immediate::None
                   && immediateWidthOf(kind) != Immediate::Relative32;
        case Field::Relative: return kind == OperandKind::Label;
        case Field::Condition: return kind == OperandKind::Condition;
    }
    return false; // uncovered(field-without-case)
}

/// Whether @p row's columns agree with each other: its slots fit its form's fields and come before its
/// implicit ones, which each name a register; its ModRm column matches whether the form has ModRM.reg; its
/// Immediate column is its immediate slot's width; its opcode has one to three bytes; and its template
/// names each explicit operand exactly once.
[[nodiscard]] constexpr bool isWellFormed(EncodingRow const& row) noexcept
{
    auto const& form = formRow(row.form);
    if (row.slots.size() < form.count || row.opcode.count == 0 || row.opcode.count > 3)
        return false;
    auto hasRegisterField = false;
    auto hasRmField = false;
    auto immediate = Immediate::None;
    for (auto const index: std::views::iota(std::size_t { 0 }, row.slots.size()))
    {
        auto const& slot = row.slots[index];
        if (index < form.count)
        {
            auto const field = form.fields[index];
            if (slot.placement != Placement::Explicit || slot.fixed.has_value() || !fits(field, slot.kind))
                return false;
            hasRegisterField = hasRegisterField || field == Field::ModRmReg;
            hasRmField = hasRmField || field == Field::ModRmRm;
            if (field == Field::Immediate || field == Field::Relative)
                immediate = immediateWidthOf(slot.kind);
        }
        else if (slot.placement != Placement::Implicit || !slot.fixed.has_value())
            return false;
    }
    // ModRM.reg holds an operand, or an opcode extension when only ModRM.rm does; no ModRM otherwise.
    auto modRmAgrees = row.modRm == ModRm::None;
    if (hasRegisterField)
        modRmAgrees = row.modRm == ModRm::Register;
    else if (hasRmField)
        modRmAgrees = row.modRm != ModRm::None && row.modRm != ModRm::Register;
    if (!modRmAgrees || immediate != row.immediate)
        return false;
    // Every {n} names an explicit operand, and each is named once.
    auto named = std::array<uint8_t, MaximumExplicitOperands> {};
    auto const& text = row.intelTemplate;
    for (auto const position: std::views::iota(std::size_t { 0 }, text.size()))
    {
        if (text[position] != '{')
            continue;
        if (position + 2 >= text.size() || text[position + 2] != '}')
            return false;
        auto const operand = static_cast<std::size_t>(text[position + 1] - '0');
        if (operand >= form.count)
            return false;
        ++named[operand];
    }
    for (auto const operand: std::views::iota(std::size_t { 0 }, std::size_t { form.count }))
        if (named[operand] != 1)
            return false;
    return !text.empty();
}

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: EncodingTable)
        {
            if (static_cast<std::size_t>(row.mnemonic) != index || !isWellFormed(row))
                return false;
            ++index;
        }
        return index == MnemonicCount;
    }(),
    "EncodingTable must hold one well-formed row per Mnemonic, at the index of its enumerator");

/// x86-64's condition codes, by their encoding: what a Condition operand's Immediate holds.
enum class ConditionCode : uint8_t
{
    Overflow,       ///< o: OF.
    NoOverflow,     ///< no: !OF.
    Below,          ///< b: CF (unsigned <).
    AboveOrEqual,   ///< ae: !CF (unsigned >=).
    Equal,          ///< e: ZF.
    NotEqual,       ///< ne: !ZF.
    BelowOrEqual,   ///< be: CF or ZF (unsigned <=).
    Above,          ///< a: !CF and !ZF (unsigned >).
    Sign,           ///< s: SF.
    NoSign,         ///< ns: !SF.
    Parity,         ///< p: PF (unordered, after ucomis).
    NoParity,       ///< np: !PF.
    Less,           ///< l: SF != OF (signed <).
    GreaterOrEqual, ///< ge: SF == OF (signed >=).
    LessOrEqual,    ///< le: ZF or SF != OF (signed <=).
    Greater,        ///< g: !ZF and SF == OF (signed >).
};

/// How many condition codes there are.
inline constexpr auto ConditionCodeCount = std::size_t { 16 };

/// Why encode() refused an instruction.
enum class EncodeError : uint8_t
{
    OperandMismatch,        ///< An operand's count, kind or register class is not its row's.
    UnallocatedRegister,    ///< A register operand names a virtual register.
    DisplacementOutOfRange, ///< A memory operand's displacement does not fit a signed 32-bit field.
    ImmediateOutOfRange,    ///< An immediate does not fit its field.
    InvalidScale,           ///< An index's scale is not 1, 2, 4 or 8.
    IndexIsStackPointer,    ///< A memory operand indexes by rsp, which SIB cannot encode.
    AbsoluteAddress,        ///< A memory operand has neither base nor index: no code is position-dependent.
    IndexExtensionUnsupported, ///< A memory operand widens its index, which x86-64 addressing cannot.
    RoundingUsesMxcsr,         ///< A rounding immediate sets bit 2: it would defer to MXCSR.
    UnknownLabel,              ///< A label or constant operand names no label of the given ones.
};

/// The name of @p error, for diagnostics.
[[nodiscard]] std::string_view describe(EncodeError error) noexcept;

/// Where encode() finds the labels a label or constant operand names.
struct EncodeLabels
{
    std::span<machine::CodeLabel const> blocks {};    ///< Per BlockId: its label (rel32 targets).
    std::span<machine::CodeLabel const> constants {}; ///< Per constant-pool slot: its label (rip-relative
                                                      ///< operands, a jump table among them).
};

/// Encodes one instruction of form @p mnemonic into @p buffer.
///
/// A memory operand is a MemoryOperand (base + index × scale + displacement), or a ConstantOperand, which
/// encodes as [rip + its label]; a Label operand is a LabelOperand, a rel32 to its block's label. Both are
/// fixups the buffer resolves later. Registers are physical, numbered as Registers.hpp numbers them; a
/// condition code and every immediate is an Immediate. Nothing is emitted when the instruction is refused.
/// @param mnemonic The form.
/// @param operands Its explicit operands, in its form's order.
/// @param labels The labels label and constant operands name.
/// @param buffer Where the bytes and fixups go.
/// @return Nothing; or why the instruction cannot be encoded.
[[nodiscard]] std::expected<void, EncodeError> encode(Mnemonic mnemonic,
                                                      std::span<machine::MachineOperand const> operands,
                                                      EncodeLabels const& labels,
                                                      machine::CodeBuffer& buffer);

} // namespace isoline::compiler::x86_64
