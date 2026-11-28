// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/x86_64/Lowering.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/machine/Linearize.hpp>
#include <isoline/runtime/Store.hpp>

namespace isoline::compiler::x86_64
{

using intermediate::Type;
using machine::CodeLabel;
using machine::ConventionRow;
using machine::MachineOperand;
using machine::OperandRole;
using machine::PhysicalRegister;
using machine::RegisterClass;
using machine::RegisterName;
using machine::RegisterOperand;
using machine::VirtualRegister;
using runtime::TrapCode;

namespace
{
    /// Which register files a call's argument positions cover.
    enum class ArgumentFiles : uint8_t
    {
        General,         ///< General only: a stub, whose operands are all words.
        GeneralAndFloat, ///< General, then Float: a Wasm call.
    };

    constexpr auto Int32Maximum = uint64_t { std::numeric_limits<int32_t>::max() };

    /// The 32- or 64-bit form, by @p width.
    [[nodiscard]] constexpr Mnemonic pick(IntegerWidth width, Mnemonic bits32, Mnemonic bits64) noexcept
    {
        return width == IntegerWidth::Bits32 ? bits32 : bits64;
    }

    /// The ss or sd form, by @p width.
    [[nodiscard]] constexpr Mnemonic pick(FloatWidth width, Mnemonic single, Mnemonic doubled) noexcept
    {
        return width == FloatWidth::Single ? single : doubled;
    }

    /// The integer width of a float's bits.
    [[nodiscard]] constexpr IntegerWidth bitsOf(FloatWidth width) noexcept
    {
        return width == FloatWidth::Single ? IntegerWidth::Bits32 : IntegerWidth::Bits64;
    }

    /// The bits of @p value as a float of @p width (every value used is exact in both).
    [[nodiscard]] uint64_t floatBits(FloatWidth width, double value) noexcept
    {
        if (width == FloatWidth::Single)
            return std::bit_cast<uint32_t>(static_cast<float>(value));
        return std::bit_cast<uint64_t>(value);
    }

    /// The byte offset of @p field, as a displacement.
    template <typename Field>
    [[nodiscard]] constexpr int64_t offsetOf(Field field) noexcept
    {
        return static_cast<int64_t>(layoutOf(field).offset);
    }

    /// A physical register operand of @p role.
    [[nodiscard]] constexpr MachineOperand physicalAs(PhysicalRegister reg, OperandRole role) noexcept
    {
        return RegisterOperand { .name = RegisterName::of(reg), .role = role };
    }

    /// The physical register of @p operand (Require: a register operand).
    [[nodiscard]] PhysicalRegister registerOf(MachineOperand const& operand) noexcept
    {
        auto const* reg = std::get_if<RegisterOperand>(&operand);
        Require(reg != nullptr && reg->name.kind == machine::RegisterKind::Physical);
        return reg->name.physicalRegister();
    }

    /// @p operand's Immediate (Require: one).
    [[nodiscard]] int64_t immediateOf(MachineOperand const& operand) noexcept
    {
        auto const* value = std::get_if<machine::Immediate>(&operand);
        Require(value != nullptr);
        return value->value;
    }

    /// The register operand of physical @p reg.
    [[nodiscard]] constexpr MachineOperand operandOf(PhysicalRegister reg) noexcept
    {
        return physicalAs(reg, OperandRole::Use);
    }

    /// A copy of @p a in a new General register: mov r32 (zero-extending) or mov r64.
    VirtualRegister copyInteger(Writer const& writer, IntegerWidth width, VirtualRegister a)
    {
        auto const copy = writer.general();
        writer.emit(pick(width, Mnemonic::Mov32RR, Mnemonic::Mov64RR), { writer.def(copy), writer.use(a) });
        return copy;
    }

    /// A copy of @p a in a new Float register.
    VirtualRegister copyFloat(Writer const& writer, VirtualRegister a)
    {
        auto const copy = writer.vector();
        writer.emit(Mnemonic::MovapsRR, { writer.def(copy), writer.use(a) });
        return copy;
    }

    /// The bits of float @p a in a new General register.
    VirtualRegister bitsInGeneral(Writer const& writer, FloatWidth width, VirtualRegister a)
    {
        auto const bits = writer.general();
        writer.emit(pick(width, Mnemonic::MovdRX, Mnemonic::MovqRX), { writer.def(bits), writer.use(a) });
        return bits;
    }

    /// A float of the bits in General @p bits, in a new Float register.
    VirtualRegister bitsInFloat(Writer const& writer, FloatWidth width, VirtualRegister bits)
    {
        auto const value = writer.vector();
        writer.emit(pick(width, Mnemonic::MovdXR, Mnemonic::MovqXR), { writer.def(value), writer.use(bits) });
        return value;
    }

    /// ucomiss/ucomisd @p a, @p b.
    void compareFloats(Writer const& writer, FloatWidth width, VirtualRegister a, VirtualRegister b)
    {
        writer.emit(pick(width, Mnemonic::UcomissRR, Mnemonic::UcomisdRR), { writer.use(a), writer.use(b) });
    }

    /// cmovcc @p into, @p from at @p width: @p into becomes @p from where @p condition holds.
    void conditionalMove(Writer const& writer,
                         IntegerWidth width,
                         ConditionCode condition,
                         VirtualRegister into,
                         VirtualRegister from)
    {
        writer.emit(pick(width, Mnemonic::Cmovcc32RR, Mnemonic::Cmovcc64RR),
                    { conditionOperand(condition), writer.useDef(into), writer.use(from) });
    }

    /// A branch to a trap site of @p code on @p condition.
    void trapIf(Writer const& writer, ConditionCode condition, TrapCode code)
    {
        writer.emit(
            Pseudo::TrapIf,
            { conditionOperand(condition), immediate(static_cast<int64_t>(std::to_underlying(code))) });
    }

    /// @p target += @p value at 64 bits: an imm32 add, or mov imm64 and add.
    void addConstant(Writer const& writer, VirtualRegister target, uint64_t value)
    {
        if (value <= Int32Maximum)
        {
            writer.emit(Mnemonic::Add64RI, { writer.useDef(target), immediate(static_cast<int64_t>(value)) });
            return;
        }
        auto const constant = materializeInteger(writer, IntegerWidth::Bits64, value);
        writer.emit(Mnemonic::Add64RR, { writer.useDef(target), writer.use(constant) });
    }

    /// @p base + @p index + @p offset in a new register: one lea when the offset fits a displacement, else
    /// mov, add and an imm64 add.
    VirtualRegister effectiveAddress(Writer const& writer,
                                     VirtualRegister base,
                                     std::optional<VirtualRegister> index,
                                     uint64_t offset)
    {
        auto const address = writer.general();
        if (offset <= Int32Maximum)
        {
            auto const operand = index ? writer.indexed(base, *index, 1, static_cast<int64_t>(offset))
                                       : writer.at(base, static_cast<int64_t>(offset));
            writer.emit(Mnemonic::Lea64RM, { writer.def(address), operand });
            return address;
        }
        writer.emit(Mnemonic::Mov64RR, { writer.def(address), writer.use(base) });
        if (index)
            writer.emit(Mnemonic::Add64RR, { writer.useDef(address), writer.use(*index) });
        addConstant(writer, address, offset);
        return address;
    }

    /// The register files' clobber order under @p row, for the call layouts.
    [[nodiscard]] std::size_t clobberCountOf(ConventionRow const& row) noexcept
    {
        return detail::clobberCount(row);
    }
} // namespace

// --- Writer -----------------------------------------------------------------------------------------------

VirtualRegister Writer::general() const
{
    return _function->createVirtualRegister(RegisterClass::General);
}

VirtualRegister Writer::vector() const
{
    return _function->createVirtualRegister(RegisterClass::Float);
}

VirtualRegister Writer::create(RegisterClass registerClass) const
{
    return _function->createVirtualRegister(registerClass);
}

MachineOperand Writer::use(VirtualRegister reg) const
{
    return RegisterOperand { .name = _function->name(reg), .role = OperandRole::Use };
}

MachineOperand Writer::def(VirtualRegister reg) const
{
    return RegisterOperand { .name = _function->name(reg), .role = OperandRole::Def };
}

MachineOperand Writer::useDef(VirtualRegister reg) const
{
    return RegisterOperand { .name = _function->name(reg), .role = OperandRole::UseDef };
}

MachineOperand Writer::early(VirtualRegister reg) const
{
    return RegisterOperand { .name = _function->name(reg), .role = OperandRole::Early };
}

MachineOperand Writer::at(VirtualRegister base, int64_t displacement) const
{
    return machine::MemoryOperand { .base = _function->name(base), .displacement = displacement };
}

MachineOperand Writer::indexed(VirtualRegister base,
                               VirtualRegister index,
                               uint8_t scale,
                               int64_t displacement) const
{
    return machine::MemoryOperand { .base = _function->name(base),
                                    .index = _function->name(index),
                                    .scale = scale,
                                    .displacement = displacement };
}

void Writer::emit(Mnemonic mnemonic, std::initializer_list<MachineOperand> operands) const
{
    Require(operands.size() == constraintsOf(mnemonic).size());
    std::ignore =
        _function->append(_block, opcodeOf(mnemonic), std::span { operands.begin(), operands.size() });
}

uint32_t Writer::append(Pseudo pseudo, std::span<MachineOperand const> operands) const
{
    return _function->append(_block, opcodeOf(pseudo), operands);
}

void Writer::emit(Pseudo pseudo, std::span<MachineOperand const> operands) const
{
    std::ignore = append(pseudo, operands);
}

void Writer::emit(Pseudo pseudo, std::initializer_list<MachineOperand> operands) const
{
    emit(pseudo, std::span { operands.begin(), operands.size() });
}

// --- Selection-time sequences -----------------------------------------------------------------------------

VirtualRegister materializeInteger(Writer const& writer, IntegerWidth width, uint64_t value)
{
    auto const reg = writer.general();
    if (width == IntegerWidth::Bits32 || value <= std::numeric_limits<uint32_t>::max())
        writer.emit(Mnemonic::Mov32RI,
                    { writer.def(reg),
                      immediate(static_cast<int64_t>(value & std::numeric_limits<uint32_t>::max())) });
    else if (auto const signedValue = static_cast<int64_t>(value);
             signedValue >= std::numeric_limits<int32_t>::min() && signedValue < 0)
        writer.emit(Mnemonic::Mov64RI32, { writer.def(reg), immediate(signedValue) });
    else
        writer.emit(Mnemonic::Mov64RI, { writer.def(reg), immediate(static_cast<int64_t>(value)) });
    return reg;
}

VirtualRegister materializeFloat(Writer const& writer, FloatWidth width, uint64_t bits)
{
    return bitsInFloat(writer, width, materializeInteger(writer, bitsOf(width), bits));
}

namespace
{
    /// floatMin() and floatMax(): @p minMax both ways, combined by @p combine, a + b on unordered.
    VirtualRegister minOrMax(Writer const& writer,
                             FloatWidth width,
                             Mnemonic minMax,
                             Mnemonic combine,
                             VirtualRegister a,
                             VirtualRegister b)
    {
        auto const first = copyFloat(writer, a);
        writer.emit(minMax, { writer.useDef(first), writer.use(b) });
        auto const second = copyFloat(writer, b);
        writer.emit(minMax, { writer.useDef(second), writer.use(a) });
        writer.emit(combine, { writer.useDef(first), writer.use(second) });
        auto const sum = copyFloat(writer, a);
        writer.emit(pick(width, Mnemonic::AddssRR, Mnemonic::AddsdRR), { writer.useDef(sum), writer.use(b) });
        auto const result = bitsInGeneral(writer, width, first);
        auto const nan = bitsInGeneral(writer, width, sum);
        compareFloats(writer, width, a, b);
        conditionalMove(writer, bitsOf(width), ConditionCode::Parity, result, nan);
        return bitsInFloat(writer, width, result);
    }
} // namespace

VirtualRegister floatMin(Writer const& writer, FloatWidth width, VirtualRegister a, VirtualRegister b)
{
    return minOrMax(writer, width, pick(width, Mnemonic::MinssRR, Mnemonic::MinsdRR), Mnemonic::OrpsRR, a, b);
}

VirtualRegister floatMax(Writer const& writer, FloatWidth width, VirtualRegister a, VirtualRegister b)
{
    return minOrMax(
        writer, width, pick(width, Mnemonic::MaxssRR, Mnemonic::MaxsdRR), Mnemonic::AndpsRR, a, b);
}

VirtualRegister floatSign(Writer const& writer,
                          FloatWidth width,
                          SignOperation operation,
                          VirtualRegister a,
                          std::optional<VirtualRegister> b)
{
    auto const bits = bitsInGeneral(writer, width, a);
    // The magnitude: and 0x7fffffff, or for 64 bits a shift out and back of the sign.
    auto const clearSign = [&](VirtualRegister reg) {
        if (width == FloatWidth::Single)
            writer.emit(Mnemonic::And32RI, { writer.useDef(reg), immediate(0x7fff'ffff) });
        else
        {
            writer.emit(Mnemonic::Shl64RI8, { writer.useDef(reg), immediate(1) });
            writer.emit(Mnemonic::Shr64RI8, { writer.useDef(reg), immediate(1) });
        }
    };
    switch (operation)
    {
        case SignOperation::Abs: clearSign(bits); break;
        case SignOperation::Neg:
            if (width == FloatWidth::Single)
                writer.emit(Mnemonic::Xor32RI, { writer.useDef(bits), immediate(0x8000'0000) });
            else
            {
                auto const sign = materializeInteger(writer, IntegerWidth::Bits64, uint64_t { 1 } << 63U);
                writer.emit(Mnemonic::Xor64RR, { writer.useDef(bits), writer.use(sign) });
            }
            break;
        case SignOperation::Copysign: {
            Require(b.has_value());
            auto const sign = bitsInGeneral(writer, width, *b);
            clearSign(bits);
            if (width == FloatWidth::Single)
            {
                writer.emit(Mnemonic::And32RI, { writer.useDef(sign), immediate(0x8000'0000) });
                writer.emit(Mnemonic::Or32RR, { writer.useDef(bits), writer.use(sign) });
            }
            else
            {
                writer.emit(Mnemonic::Shr64RI8, { writer.useDef(sign), immediate(63) });
                writer.emit(Mnemonic::Shl64RI8, { writer.useDef(sign), immediate(63) });
                writer.emit(Mnemonic::Or64RR, { writer.useDef(bits), writer.use(sign) });
            }
        }
        break;
    }
    return bitsInFloat(writer, width, bits);
}

VirtualRegister nearestSse41(Writer const& writer, FloatWidth width, RoundingMode mode, VirtualRegister a)
{
    auto const result = writer.vector();
    writer.emit(pick(width, Mnemonic::RoundssRRI, Mnemonic::RoundsdRRI),
                { writer.def(result), writer.use(a), immediate(std::to_underlying(mode)) });
    return result;
}

VirtualRegister nearestBaseline(Writer const& writer,
                                CallConventions const& conventions,
                                FloatWidth width,
                                FloatRoundOperation operation,
                                VirtualRegister a)
{
    auto const bits = bitsInGeneral(writer, width, a);
    auto const selected = materializeInteger(writer, IntegerWidth::Bits32, std::to_underlying(operation));
    auto const operands = std::to_array({ selected, bits });
    auto const rounded = callStub(writer, conventions, RuntimeStub::FloatRound, operands);
    return bitsInFloat(writer, width, rounded);
}

namespace
{
    /// The bound a truncation's value must stay below: 2^31, 2^63, 2^32 or 2^64.
    [[nodiscard]] double upperBoundOf(TruncationRow const& row) noexcept
    {
        auto const bits = row.result == IntegerWidth::Bits32 ? 32 : 64;
        auto const exponent = row.signedness == Signedness::Signed ? bits - 1 : bits;
        return std::ldexp(1.0, exponent);
    }

    /// The bound a trapping truncation's value must stay above, and the condition of the trap at it: below
    /// −2^31 (single) or at or below −2^31 − 1 (double) for a signed 32-bit result, below −2^63 for a signed
    /// 64-bit one, at or below −1 for an unsigned one.
    [[nodiscard]] std::pair<double, ConditionCode> lowerBoundOf(TruncationRow const& row) noexcept
    {
        if (row.signedness == Signedness::Unsigned)
            return { -1.0, ConditionCode::BelowOrEqual };
        if (row.result == IntegerWidth::Bits64)
            return { -std::ldexp(1.0, 63), ConditionCode::Below };
        if (row.source == FloatWidth::Double)
            return { -std::ldexp(1.0, 31) - 1.0, ConditionCode::BelowOrEqual };
        return { -std::ldexp(1.0, 31), ConditionCode::Below };
    }

    /// cvttss2si/cvttsd2si of @p a at @p width.
    VirtualRegister convertTruncating(Writer const& writer,
                                      FloatWidth source,
                                      IntegerWidth width,
                                      VirtualRegister a)
    {
        auto const result = writer.general();
        auto const mnemonic = source == FloatWidth::Single
                                  ? pick(width, Mnemonic::Cvttss2si32, Mnemonic::Cvttss2si64)
                                  : pick(width, Mnemonic::Cvttsd2si32, Mnemonic::Cvttsd2si64);
        writer.emit(mnemonic, { writer.def(result), writer.use(a) });
        return result;
    }

    /// The conversion of a value in range: signed directly; an unsigned 32-bit one as a signed 64-bit one;
    /// an unsigned 64-bit one directly below 2^63, else of a − 2^63 with the sign bit flipped back.
    VirtualRegister convertInRange(Writer const& writer, TruncationRow const& row, VirtualRegister a)
    {
        if (row.signedness == Signedness::Signed)
            return convertTruncating(writer, row.source, row.result, a);
        if (row.result == IntegerWidth::Bits32)
            return convertTruncating(writer, row.source, IntegerWidth::Bits64, a);
        auto const half = materializeFloat(writer, row.source, floatBits(row.source, std::ldexp(1.0, 63)));
        auto const low = convertTruncating(writer, row.source, IntegerWidth::Bits64, a);
        auto const reduced = copyFloat(writer, a);
        writer.emit(pick(row.source, Mnemonic::SubssRR, Mnemonic::SubsdRR),
                    { writer.useDef(reduced), writer.use(half) });
        auto const high = convertTruncating(writer, row.source, IntegerWidth::Bits64, reduced);
        auto const sign = materializeInteger(writer, IntegerWidth::Bits64, uint64_t { 1 } << 63U);
        writer.emit(Mnemonic::Xor64RR, { writer.useDef(high), writer.use(sign) });
        compareFloats(writer, row.source, a, half);
        conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::AboveOrEqual, low, high);
        return low;
    }
} // namespace

VirtualRegister truncateChecked(Writer const& writer, TruncationRow const& row, VirtualRegister a)
{
    Require(row.saturation == Saturation::Traps);
    compareFloats(writer, row.source, a, a);
    trapIf(writer, ConditionCode::Parity, TrapCode::InvalidConversionToInteger);
    auto const upper = materializeFloat(writer, row.source, floatBits(row.source, upperBoundOf(row)));
    compareFloats(writer, row.source, a, upper);
    trapIf(writer, ConditionCode::AboveOrEqual, TrapCode::IntegerOverflow);
    auto const [lowerBound, below] = lowerBoundOf(row);
    auto const lower = materializeFloat(writer, row.source, floatBits(row.source, lowerBound));
    compareFloats(writer, row.source, a, lower);
    trapIf(writer, below, TrapCode::IntegerOverflow);
    return convertInRange(writer, row, a);
}

VirtualRegister truncateSaturating(Writer const& writer, TruncationRow const& row, VirtualRegister a)
{
    Require(row.saturation == Saturation::Saturates);
    auto const result = convertInRange(writer, row, a);
    auto const upper = materializeFloat(writer, row.source, floatBits(row.source, upperBoundOf(row)));
    if (row.signedness == Signedness::Signed)
    {
        // Past the top: the largest value; NaN: 0 (cvtt gave the smallest, right for the bottom).
        auto const largest = materializeInteger(
            writer, row.result, row.result == IntegerWidth::Bits32 ? 0x7fff'ffffU : 0x7fff'ffff'ffff'ffffU);
        auto const zero = materializeInteger(writer, row.result, 0);
        compareFloats(writer, row.source, a, upper);
        conditionalMove(writer, row.result, ConditionCode::AboveOrEqual, result, largest);
        compareFloats(writer, row.source, a, a);
        conditionalMove(writer, row.result, ConditionCode::Parity, result, zero);
        return result;
    }
    // Past the top: all ones of the width; below 0 or NaN (CF set by both): 0.
    auto const largest = row.result == IntegerWidth::Bits32
                             ? materializeInteger(writer, IntegerWidth::Bits32, 0xffff'ffffU)
                             : materializeInteger(writer, IntegerWidth::Bits64, ~uint64_t { 0 });
    auto const zero = materializeInteger(writer, IntegerWidth::Bits64, 0);
    auto const floatZero = materializeFloat(writer, row.source, 0);
    compareFloats(writer, row.source, a, upper);
    conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::AboveOrEqual, result, largest);
    compareFloats(writer, row.source, a, floatZero);
    conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::Below, result, zero);
    return result;
}

VirtualRegister truncToI32Checked(Writer const& writer,
                                  FloatWidth source,
                                  Signedness signedness,
                                  VirtualRegister a)
{
    return truncateChecked(writer,
                           TruncationRow { .operation = intermediate::Operation::Removed,
                                           .source = source,
                                           .result = IntegerWidth::Bits32,
                                           .signedness = signedness,
                                           .saturation = Saturation::Traps },
                           a);
}

VirtualRegister truncSatF64ToU64(Writer const& writer, VirtualRegister a)
{
    auto const row = truncationOf(intermediate::Operation::I64TruncSatF64U);
    Require(row.has_value());
    return truncateSaturating(writer, *row, a);
}

VirtualRegister convertU32(Writer const& writer, FloatWidth width, VirtualRegister a)
{
    auto const extended = copyInteger(writer, IntegerWidth::Bits32, a);
    auto const result = writer.vector();
    writer.emit(pick(width, Mnemonic::Cvtsi2ss64, Mnemonic::Cvtsi2sd64),
                { writer.def(result), writer.use(extended) });
    return result;
}

namespace
{
    /// convertU64ToF32() and convertU64ToF64().
    VirtualRegister convertU64(Writer const& writer, FloatWidth width, VirtualRegister a)
    {
        auto const convert = pick(width, Mnemonic::Cvtsi2ss64, Mnemonic::Cvtsi2sd64);
        auto const half = copyInteger(writer, IntegerWidth::Bits64, a);
        writer.emit(Mnemonic::Shr64RI8, { writer.useDef(half), immediate(1) });
        auto const lowest = copyInteger(writer, IntegerWidth::Bits64, a);
        writer.emit(Mnemonic::And64RI8, { writer.useDef(lowest), immediate(1) });
        writer.emit(Mnemonic::Or64RR, { writer.useDef(half), writer.use(lowest) });
        auto const doubled = writer.vector();
        writer.emit(convert, { writer.def(doubled), writer.use(half) });
        writer.emit(pick(width, Mnemonic::AddssRR, Mnemonic::AddsdRR),
                    { writer.useDef(doubled), writer.use(doubled) });
        auto const direct = writer.vector();
        writer.emit(convert, { writer.def(direct), writer.use(a) });
        auto const large = bitsInGeneral(writer, width, doubled);
        auto const result = bitsInGeneral(writer, width, direct);
        writer.emit(Mnemonic::Test64RR, { writer.use(a), writer.use(a) });
        conditionalMove(writer, bitsOf(width), ConditionCode::Sign, result, large);
        return bitsInFloat(writer, width, result);
    }
} // namespace

VirtualRegister convertU64ToF32(Writer const& writer, VirtualRegister a)
{
    return convertU64(writer, FloatWidth::Single, a);
}

VirtualRegister convertU64ToF64(Writer const& writer, VirtualRegister a)
{
    return convertU64(writer, FloatWidth::Double, a);
}

VirtualRegister popcntBaseline(Writer const& writer, IntegerWidth width, VirtualRegister a)
{
    // x - ((x >> 1) & 0x55..), then pairs into nibbles, nibbles into bytes, and the bytes summed by a
    // multiply.
    auto const masked = [&](VirtualRegister reg, uint64_t mask) {
        if (width == IntegerWidth::Bits32)
            writer.emit(Mnemonic::And32RI,
                        { writer.useDef(reg), immediate(static_cast<int64_t>(mask & 0xffff'ffffU)) });
        else
        {
            auto const constant = materializeInteger(writer, width, mask);
            writer.emit(Mnemonic::And64RR, { writer.useDef(reg), writer.use(constant) });
        }
    };
    auto const shiftRight = pick(width, Mnemonic::Shr32RI8, Mnemonic::Shr64RI8);
    auto const pairs = copyInteger(writer, width, a);
    writer.emit(shiftRight, { writer.useDef(pairs), immediate(1) });
    masked(pairs, 0x5555'5555'5555'5555U);
    auto const value = copyInteger(writer, width, a);
    writer.emit(pick(width, Mnemonic::Sub32RR, Mnemonic::Sub64RR),
                { writer.useDef(value), writer.use(pairs) });
    auto const nibbles = copyInteger(writer, width, value);
    writer.emit(shiftRight, { writer.useDef(nibbles), immediate(2) });
    masked(nibbles, 0x3333'3333'3333'3333U);
    masked(value, 0x3333'3333'3333'3333U);
    auto const add = pick(width, Mnemonic::Add32RR, Mnemonic::Add64RR);
    writer.emit(add, { writer.useDef(value), writer.use(nibbles) });
    auto const bytes = copyInteger(writer, width, value);
    writer.emit(shiftRight, { writer.useDef(bytes), immediate(4) });
    writer.emit(add, { writer.useDef(value), writer.use(bytes) });
    masked(value, 0x0f0f'0f0f'0f0f'0f0fU);
    auto const result = writer.general();
    if (width == IntegerWidth::Bits32)
        writer.emit(Mnemonic::Imul32RRI, { writer.def(result), writer.use(value), immediate(0x0101'0101) });
    else
    {
        auto const ones = materializeInteger(writer, width, 0x0101'0101'0101'0101U);
        writer.emit(Mnemonic::Mov64RR, { writer.def(result), writer.use(value) });
        writer.emit(Mnemonic::Imul64RR, { writer.useDef(result), writer.use(ones) });
    }
    writer.emit(shiftRight, { writer.useDef(result), immediate(width == IntegerWidth::Bits32 ? 24 : 56) });
    return result;
}

VirtualRegister clzBaseline(Writer const& writer, IntegerWidth width, VirtualRegister a)
{
    auto const bits = width == IntegerWidth::Bits32 ? 32U : 64U;
    auto const zeroCase = materializeInteger(writer, width, (2 * bits) - 1);
    auto const result = writer.general();
    writer.emit(pick(width, Mnemonic::Bsr32RR, Mnemonic::Bsr64RR), { writer.def(result), writer.use(a) });
    conditionalMove(writer, width, ConditionCode::Equal, result, zeroCase);
    writer.emit(pick(width, Mnemonic::Xor32RI8, Mnemonic::Xor64RI8),
                { writer.useDef(result), immediate(static_cast<int64_t>(bits - 1)) });
    return result;
}

VirtualRegister ctzBaseline(Writer const& writer, IntegerWidth width, VirtualRegister a)
{
    auto const zeroCase = materializeInteger(writer, width, width == IntegerWidth::Bits32 ? 32U : 64U);
    auto const result = writer.general();
    writer.emit(pick(width, Mnemonic::Bsf32RR, Mnemonic::Bsf64RR), { writer.def(result), writer.use(a) });
    conditionalMove(writer, width, ConditionCode::Equal, result, zeroCase);
    return result;
}

namespace
{
    /// test divisor, divisor; trap IntegerDivideByZero on zero.
    void trapOnZeroDivisor(Writer const& writer, IntegerWidth width, VirtualRegister divisor)
    {
        writer.emit(pick(width, Mnemonic::Test32RR, Mnemonic::Test64RR),
                    { writer.use(divisor), writer.use(divisor) });
        trapIf(writer, ConditionCode::Equal, TrapCode::IntegerDivideByZero);
    }

    /// The signed division of @p a by @p divisor: rax ← a, rdx ← its sign (cdq, cqo), idiv.
    /// @return {quotient, remainder}.
    std::pair<VirtualRegister, VirtualRegister> divide(Writer const& writer,
                                                       IntegerWidth width,
                                                       VirtualRegister a,
                                                       VirtualRegister divisor)
    {
        auto const quotient = copyInteger(writer, width, a);
        auto const remainder = writer.general();
        writer.emit(pick(width, Mnemonic::Cdq, Mnemonic::Cqo),
                    { writer.use(quotient), writer.def(remainder) });
        writer.emit(pick(width, Mnemonic::Idiv32R, Mnemonic::Idiv64R),
                    { writer.use(divisor), writer.useDef(quotient), writer.useDef(remainder) });
        return { quotient, remainder };
    }
} // namespace

VirtualRegister divSigned(Writer const& writer, IntegerWidth width, VirtualRegister a, VirtualRegister b)
{
    trapOnZeroDivisor(writer, width, b);
    // (a ^ MIN) | (b + 1) is zero exactly for MIN / −1, whose quotient overflows.
    auto const isMinimum = copyInteger(writer, width, a);
    if (width == IntegerWidth::Bits32)
        writer.emit(Mnemonic::Xor32RI, { writer.useDef(isMinimum), immediate(0x8000'0000) });
    else
    {
        auto const minimum = materializeInteger(writer, width, uint64_t { 1 } << 63U);
        writer.emit(Mnemonic::Xor64RR, { writer.useDef(isMinimum), writer.use(minimum) });
    }
    auto const isMinusOne = copyInteger(writer, width, b);
    writer.emit(pick(width, Mnemonic::Add32RI8, Mnemonic::Add64RI8),
                { writer.useDef(isMinusOne), immediate(1) });
    writer.emit(pick(width, Mnemonic::Or32RR, Mnemonic::Or64RR),
                { writer.useDef(isMinimum), writer.use(isMinusOne) });
    trapIf(writer, ConditionCode::Equal, TrapCode::IntegerOverflow);
    return divide(writer, width, a, b).first;
}

VirtualRegister remSigned(Writer const& writer, IntegerWidth width, VirtualRegister a, VirtualRegister b)
{
    trapOnZeroDivisor(writer, width, b);
    auto const divisor = copyInteger(writer, width, b);
    auto const one = materializeInteger(writer, width, 1);
    writer.emit(pick(width, Mnemonic::Cmp32RI8, Mnemonic::Cmp64RI8), { writer.use(b), immediate(-1) });
    conditionalMove(writer, width, ConditionCode::Equal, divisor, one);
    return divide(writer, width, a, divisor).second;
}

VirtualRegister divideUnsigned(
    Writer const& writer, IntegerWidth width, DivisionResult result, VirtualRegister a, VirtualRegister b)
{
    trapOnZeroDivisor(writer, width, b);
    auto const quotient = copyInteger(writer, width, a);
    auto const remainder = materializeInteger(writer, IntegerWidth::Bits32, 0);
    writer.emit(pick(width, Mnemonic::Div32R, Mnemonic::Div64R),
                { writer.use(b), writer.useDef(quotient), writer.useDef(remainder) });
    return result == DivisionResult::Quotient ? quotient : remainder;
}

VirtualRegister canonicalizeNan(Writer const& writer, FloatWidth width, VirtualRegister a)
{
    auto const bits = bitsInGeneral(writer, width, a);
    auto const canonical = materializeInteger(
        writer, bitsOf(width), width == FloatWidth::Single ? 0x7fc0'0000U : 0x7ff8'0000'0000'0000U);
    compareFloats(writer, width, a, a);
    conditionalMove(writer, bitsOf(width), ConditionCode::Parity, bits, canonical);
    return bitsInFloat(writer, width, bits);
}

VirtualRegister checkMemory32(Writer const& writer, MemoryCheck const& check)
{
    // An I32 index is its low 32 bits, zero-extended (Guards.hpp); index + offset + size < 2^34 never wraps.
    auto const index = copyInteger(writer, IntegerWidth::Bits32, check.index);
    auto const end = effectiveAddress(writer, index, std::nullopt, check.offset + check.size);
    auto const pointer = effectiveAddress(writer, check.base, index, check.offset);
    auto const zero = check.spectre == SpectreGuard::Enabled
                          ? std::optional { materializeInteger(writer, IntegerWidth::Bits64, 0) }
                          : std::nullopt;
    writer.emit(Mnemonic::Cmp64RR, { writer.use(end), writer.use(check.bound) });
    trapIf(writer, ConditionCode::Above, TrapCode::OutOfBoundsMemoryAccess);
    if (zero)
        conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::Above, pointer, *zero);
    return pointer;
}

VirtualRegister checkMemory64(Writer const& writer, MemoryCheck const& check)
{
    auto const pointer = effectiveAddress(writer, check.base, check.index, check.offset);
    // end = index + (offset + size), the carry folded in: a wrapped end becomes all ones, past every bound,
    // so the one compare is the trap's condition and the select's alike (ledger carry to T13 from T6).
    auto const end = copyInteger(writer, IntegerWidth::Bits64, check.index);
    // An offset + size past 2^64 − 1 (which translation makes an unconditional trap) saturates: every index
    // then carries or ends at all ones, past every bound.
    auto const reach = intermediate::staticEndOverflows(check.offset, check.size) ? ~uint64_t { 0 }
                                                                                  : check.offset + check.size;
    addConstant(writer, end, reach);
    auto const allOnes = materializeInteger(writer, IntegerWidth::Bits64, ~uint64_t { 0 });
    conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::Below, end, allOnes);
    auto const zero = check.spectre == SpectreGuard::Enabled
                          ? std::optional { materializeInteger(writer, IntegerWidth::Bits64, 0) }
                          : std::nullopt;
    writer.emit(Mnemonic::Cmp64RR, { writer.use(end), writer.use(check.bound) });
    trapIf(writer, ConditionCode::Above, TrapCode::OutOfBoundsMemoryAccess);
    if (zero)
        conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::Above, pointer, *zero);
    return pointer;
}

VirtualRegister guardAddress(Writer const& writer,
                             VirtualRegister address,
                             uint64_t offset,
                             SpectreGuard spectre)
{
    auto const moved = effectiveAddress(writer, address, std::nullopt, offset);
    if (spectre == SpectreGuard::Enabled)
    {
        writer.emit(Mnemonic::Test64RR, { writer.use(address), writer.use(address) });
        conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::Equal, moved, address);
    }
    return moved;
}

VirtualRegister inBoundsAddress(Writer const& writer, VirtualRegister base, uint64_t offset)
{
    return effectiveAddress(writer, base, std::nullopt, offset);
}

namespace
{
    /// The guarded index of an index of @p type: an I32's low 32 bits zero-extended, an I64 as it is.
    VirtualRegister guardedIndex(Writer const& writer, Type type, VirtualRegister index)
    {
        return type == Type::I32 ? copyInteger(writer, IntegerWidth::Bits32, index) : index;
    }

    /// A branch to a trap site of @p code on @p condition, naming @p subject when the code has one.
    void trapNaming(Writer const& writer, ConditionCode condition, TrapCode code, VirtualRegister subject)
    {
        if (code != TrapCode::UndefinedElement && code != TrapCode::UninitializedElement)
        {
            trapIf(writer, condition, code);
            return;
        }
        writer.emit(Pseudo::TrapIfSubject,
                    { conditionOperand(condition),
                      immediate(static_cast<int64_t>(std::to_underlying(code))),
                      writer.use(subject) });
    }
} // namespace

VirtualRegister checkTable(Writer const& writer, TableCheck const& check)
{
    static_assert(std::has_single_bit(RefBytes), "a table element is addressed by a shift");
    auto const index = guardedIndex(writer, check.indexType, check.index);
    auto const scaled = copyInteger(writer, IntegerWidth::Bits64, index);
    writer.emit(Mnemonic::Shl64RI8, { writer.useDef(scaled), immediate(std::countr_zero(RefBytes)) });
    auto const pointer = writer.general();
    writer.emit(Mnemonic::Lea64RM, { writer.def(pointer), writer.indexed(check.data, scaled, 1) });
    auto const zero = check.spectre == SpectreGuard::Enabled
                          ? std::optional { materializeInteger(writer, IntegerWidth::Bits64, 0) }
                          : std::nullopt;
    writer.emit(Mnemonic::Cmp64RR, { writer.use(index), writer.use(check.size) });
    trapNaming(writer, ConditionCode::AboveOrEqual, check.code, index);
    if (zero)
        conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::AboveOrEqual, pointer, *zero);
    return pointer;
}

VirtualRegister dispatchRowOf(Writer const& writer, VirtualRegister view, VirtualRegister address)
{
    static_assert(std::has_single_bit(runtime::DispatchChunkRows));
    auto const chunk = copyInteger(writer, IntegerWidth::Bits64, address);
    writer.emit(Mnemonic::Shr64RI8,
                { writer.useDef(chunk), immediate(std::countr_zero(runtime::DispatchChunkRows)) });
    auto const chunks = writer.general();
    writer.emit(Mnemonic::Mov64RM,
                { writer.def(chunks), writer.at(view, offsetOf(DispatchViewField::Chunks)) });
    auto const rows = writer.general();
    writer.emit(Mnemonic::Mov64RM, { writer.def(rows), writer.indexed(chunks, chunk, 8) });
    auto const row = copyInteger(writer, IntegerWidth::Bits64, address);
    writer.emit(Mnemonic::And64RI, { writer.useDef(row), immediate(runtime::DispatchChunkRows - 1) });
    auto const offset = writer.general();
    writer.emit(Mnemonic::Imul64RRI,
                { writer.def(offset), writer.use(row), immediate(sizeof(runtime::FunctionDispatch)) });
    auto const pointer = writer.general();
    writer.emit(Mnemonic::Lea64RM, { writer.def(pointer), writer.indexed(rows, offset, 1) });
    return pointer;
}

VirtualRegister checkDispatch(Writer const& writer, DispatchCheck const& check)
{
    auto const rows = writer.general();
    writer.emit(Mnemonic::Mov32RM,
                { writer.def(rows), writer.at(check.view, offsetOf(DispatchViewField::Rows)) });
    auto const address = copyInteger(writer, IntegerWidth::Bits64, check.reference);
    writer.emit(Mnemonic::Shr64RI8, { writer.useDef(address), immediate(intermediate::RefWordAddressShift) });
    auto const subject = guardedIndex(writer, check.indexType, check.index);
    auto const zero = check.spectre == SpectreGuard::Enabled
                          ? std::optional { materializeInteger(writer, IntegerWidth::Bits64, 0) }
                          : std::nullopt;
    writer.emit(Mnemonic::Cmp64RR, { writer.use(address), writer.use(rows) });
    trapNaming(writer, ConditionCode::AboveOrEqual, check.code, subject);
    if (!zero)
        return dispatchRowOf(writer, check.view, address);
    // Misspeculated past the trap, the chunk is loaded for address 0, and the row's address is 0.
    auto const clamped = copyInteger(writer, IntegerWidth::Bits64, address);
    conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::AboveOrEqual, clamped, *zero);
    auto const pointer = dispatchRowOf(writer, check.view, clamped);
    writer.emit(Mnemonic::Cmp64RR, { writer.use(address), writer.use(rows) });
    conditionalMove(writer, IntegerWidth::Bits64, ConditionCode::AboveOrEqual, pointer, *zero);
    return pointer;
}

VirtualRegister loadRef(Writer const& writer, VirtualRegister pointer, RefOffsets const& offsets)
{
    auto const word = writer.general();
    writer.emit(Mnemonic::Movzx32M8, { writer.def(word), writer.at(pointer, offsets.kind) });
    auto const heap = writer.general();
    writer.emit(Mnemonic::Movzx32M8, { writer.def(heap), writer.at(pointer, offsets.heapKind) });
    writer.emit(Mnemonic::Shl32RI8, { writer.useDef(heap), immediate(intermediate::RefWordHeapKindShift) });
    writer.emit(Mnemonic::Or32RR, { writer.useDef(word), writer.use(heap) });
    auto const address = writer.general();
    writer.emit(Mnemonic::Mov32RM, { writer.def(address), writer.at(pointer, offsets.address) });
    writer.emit(Mnemonic::Shl64RI8, { writer.useDef(address), immediate(intermediate::RefWordAddressShift) });
    writer.emit(Mnemonic::Or64RR, { writer.useDef(word), writer.use(address) });
    return word;
}

void storeRef(Writer const& writer, VirtualRegister pointer, VirtualRegister word, RefOffsets const& offsets)
{
    writer.emit(Mnemonic::Mov8MR, { writer.at(pointer, offsets.kind), writer.use(word) });
    auto const heap = copyInteger(writer, IntegerWidth::Bits64, word);
    writer.emit(Mnemonic::Shr64RI8, { writer.useDef(heap), immediate(intermediate::RefWordHeapKindShift) });
    writer.emit(Mnemonic::Mov8MR, { writer.at(pointer, offsets.heapKind), writer.use(heap) });
    writer.emit(Mnemonic::Mov32MI, { writer.at(pointer, offsets.heapTypeIndex), immediate(0) });
    auto const address = copyInteger(writer, IntegerWidth::Bits64, word);
    writer.emit(Mnemonic::Shr64RI8, { writer.useDef(address), immediate(intermediate::RefWordAddressShift) });
    writer.emit(Mnemonic::Mov32MR, { writer.at(pointer, offsets.address), writer.use(address) });
}

namespace
{
    /// What a borrow out of an execution-state count means.
    enum class Borrow : uint8_t
    {
        Impossible, ///< Nothing: the count only grows.
        Traps,      ///< The count ran out: CallStackExhausted.
    };

    /// The execution state's @p field at @p width: loaded, adjusted by @p adjust, trapped on a borrow when
    /// @p borrow traps, and stored back.
    template <typename Adjust>
    void adjustState(Writer const& writer,
                     ExecutionStateField field,
                     IntegerWidth width,
                     Adjust const& adjust,
                     Borrow borrow)
    {
        auto const value = writer.general();
        writer.emit(pick(width, Mnemonic::Mov32RM, Mnemonic::Mov64RM),
                    { writer.def(value), at(Gpr::R15, offsetOf(field)) });
        adjust(value);
        if (borrow == Borrow::Traps)
            trapIf(writer, ConditionCode::Below, TrapCode::CallStackExhausted);
        writer.emit(pick(width, Mnemonic::Mov32MR, Mnemonic::Mov64MR),
                    { at(Gpr::R15, offsetOf(field)), writer.use(value) });
    }
} // namespace

void chargeFrame(Writer const& writer, uint64_t frameCost)
{
    adjustState(
        writer,
        ExecutionStateField::CallDepthLeft,
        IntegerWidth::Bits32,
        [&](VirtualRegister depth) {
            writer.emit(Mnemonic::Sub32RI8, { writer.useDef(depth), immediate(1) });
        },
        Borrow::Traps);
    adjustState(
        writer,
        ExecutionStateField::StackValuesLeft,
        IntegerWidth::Bits64,
        [&](VirtualRegister values) {
            if (frameCost <= Int32Maximum)
                writer.emit(Mnemonic::Sub64RI,
                            { writer.useDef(values), immediate(static_cast<int64_t>(frameCost)) });
            else
            {
                auto const cost = materializeInteger(writer, IntegerWidth::Bits64, frameCost);
                writer.emit(Mnemonic::Sub64RR, { writer.useDef(values), writer.use(cost) });
            }
        },
        Borrow::Traps);
}

void releaseFrame(Writer const& writer, uint64_t frameCost)
{
    adjustState(
        writer,
        ExecutionStateField::CallDepthLeft,
        IntegerWidth::Bits32,
        [&](VirtualRegister depth) {
            writer.emit(Mnemonic::Add32RI8, { writer.useDef(depth), immediate(1) });
        },
        Borrow::Impossible);
    adjustState(
        writer,
        ExecutionStateField::StackValuesLeft,
        IntegerWidth::Bits64,
        [&](VirtualRegister values) { addConstant(writer, values, frameCost); },
        Borrow::Impossible);
}

// --- Calls ------------------------------------------------------------------------------------------------

namespace
{
    /// The position among @p row's argument registers (General, then Float when @p floats) of @p reg.
    [[nodiscard]] std::size_t argumentIndexOf(ConventionRow const& row, PhysicalRegister reg) noexcept
    {
        for (auto const [index, candidate]: core::views::enumerate(row.integerArguments))
            if (candidate == reg)
                return static_cast<std::size_t>(index);
        for (auto const [index, candidate]: core::views::enumerate(row.floatArguments))
            if (candidate == reg)
                return row.integerArguments.size() + static_cast<std::size_t>(index);
        Require(false); // an argument register is one of its row's
        return 0;       // uncovered(argument-register-of-another-row)
    }

    /// The store of @p reg to the outgoing area at @p offset: its 8-byte slot.
    void storeOutgoing(Writer const& writer, VirtualRegister reg, uint32_t offset)
    {
        auto const isFloat = writer.function().registerClass(reg) == RegisterClass::Float;
        writer.emit(isFloat ? Mnemonic::MovsdMR : Mnemonic::Mov64MR,
                    { at(Gpr::Rsp, offset), writer.use(reg) });
    }

    /// The load of a @p type value from [rsp + @p offset].
    VirtualRegister loadFromStack(Writer const& writer, Type type, uint32_t offset)
    {
        auto const reg = writer.create(machine::registerClassOf(type));
        auto const mnemonic = valueFormOf(type).load;
        writer.emit(mnemonic, { writer.def(reg), at(Gpr::Rsp, offset) });
        return reg;
    }

    /// The layout of a call of @p lead leading operands, with @p arguments' argument registers of @p files
    /// and @p clobbers' clobbers.
    [[nodiscard]] CallLayout callLayoutOf(std::size_t lead,
                                          ConventionRow const& arguments,
                                          ConventionRow const& clobbers,
                                          ArgumentFiles files) noexcept
    {
        auto const floats = files == ArgumentFiles::GeneralAndFloat ? arguments.floatArguments.size() : 0;
        return CallLayout { .lead = lead,
                            .arguments = arguments.integerArguments.size() + floats,
                            .clobbers = clobberCountOf(clobbers) };
    }
} // namespace

std::pmr::vector<VirtualRegister> emitCall(Writer const& writer, CallShape const& shape)
{
    auto& function = writer.function();
    auto& memory = function.memory();
    auto parameterTypes = std::pmr::vector<Type> { &memory };
    for (auto const& argument: shape.arguments)
        parameterTypes.push_back(argument.type);
    auto const assignment = machine::assignCall(shape.convention, parameterTypes, shape.results, memory);
    auto operands = std::pmr::vector<MachineOperand>(shape.layout.size(), immediate(0), &memory);
    std::ranges::copy(shape.lead, operands.begin());
    auto const place = [&](machine::ValueLocation const& location, VirtualRegister reg) {
        if (auto const* physicalRegister = std::get_if<PhysicalRegister>(&location))
            operands[shape.layout.argument(argumentIndexOf(shape.convention, *physicalRegister))] =
                writer.use(reg);
        else
            storeOutgoing(writer, reg, std::get<machine::StackOffset>(location).bytes);
    };
    // The hidden pointer to the result area, which lies above the stack arguments, comes first.
    auto const hidden = assignment.resultPointer.has_value() ? std::size_t { 1 } : 0;
    auto const resultArea = assignment.arguments.memoryBytes;
    for (auto const [index, argument]: core::views::enumerate(shape.arguments))
        place(assignment.arguments.locations[hidden + static_cast<std::size_t>(index)], argument.reg);
    if (hidden != 0)
    {
        auto const pointer = writer.general();
        writer.emit(Mnemonic::Lea64RM, { writer.def(pointer), at(Gpr::Rsp, resultArea) });
        place(*assignment.resultPointer, pointer);
    }
    // Every register the convention clobbers is a definition: a result's, or a physical clobber.
    auto results = std::pmr::vector<VirtualRegister> { &memory };
    auto inMemory = std::pmr::vector<std::pair<std::size_t, uint32_t>> { &memory };
    auto clobberIndex = std::size_t { 0 };
    machine::forEachCallerSaved(
        shape.convention, AllocatableGeneral, AllocatableFloat, [&](PhysicalRegister reg) {
            operands[shape.layout.clobber(clobberIndex++)] = physicalAs(reg, OperandRole::Def);
        });
    for (auto const [index, location]: core::views::enumerate(assignment.results.locations))
    {
        if (auto const* physicalRegister = std::get_if<PhysicalRegister>(&location))
        {
            auto const reg = writer.create(physicalRegister->registerClass);
            auto const position = clobberIndexOf(shape.convention, *physicalRegister);
            Require(position.has_value()); // a result register is one the call clobbers
            operands[shape.layout.clobber(*position)] = writer.def(reg);
            results.push_back(reg);
        }
        else
        {
            results.push_back(VirtualRegister {});
            inMemory.emplace_back(static_cast<std::size_t>(index),
                                  resultArea + std::get<machine::StackOffset>(location).bytes);
        }
    }
    if (shape.types == TrailingTypes::Yes)
    {
        operands.push_back(immediate(static_cast<int64_t>(shape.arguments.size())));
        for (auto const& argument: shape.arguments)
            operands.push_back(immediate(std::to_underlying(argument.type)));
        for (auto const type: shape.results)
            operands.push_back(immediate(std::to_underlying(type)));
    }
    auto const call = writer.append(shape.pseudo, operands);
    function.addCallSite(
        call,
        std::max(shape.minimumOutgoing, assignment.arguments.memoryBytes + assignment.results.memoryBytes));
    for (auto const& [index, offset]: inMemory)
        results[index] = loadFromStack(writer, shape.results[index], offset);
    return results;
}

std::pmr::vector<VirtualRegister> callDirect(Writer const& writer,
                                             CallConventions const& conventions,
                                             uint32_t codeIndex,
                                             std::span<CallValue const> arguments,
                                             std::span<Type const> results)
{
    auto const lead = std::array { immediate(codeIndex) };
    return emitCall(
        writer,
        CallShape { .pseudo = Pseudo::CallDirect,
                    .layout = callLayoutOf(
                        1, conventions.internal, conventions.internal, ArgumentFiles::GeneralAndFloat),
                    .convention = conventions.internal,
                    .lead = lead,
                    .arguments = arguments,
                    .results = results });
}

namespace
{
    /// A dispatched call of the row @p row, of function address @p address, then r14 restored.
    std::pmr::vector<VirtualRegister> callThroughRow(Writer const& writer,
                                                     CallConventions const& conventions,
                                                     VirtualRegister row,
                                                     VirtualRegister address,
                                                     std::span<CallValue const> arguments,
                                                     std::span<Type const> results)
    {
        auto const saved = writer.general();
        writer.emit(Mnemonic::Mov64RR, { writer.def(saved), physicalOperand(Gpr::R14) });
        auto const lead = std::array { writer.use(row), writer.use(address) };
        auto values = emitCall(
            writer,
            CallShape { .pseudo = Pseudo::CallDispatched,
                        .layout = callLayoutOf(
                            2, conventions.internal, conventions.internal, ArgumentFiles::GeneralAndFloat),
                        .convention = conventions.internal,
                        .lead = lead,
                        .arguments = arguments,
                        .results = results,
                        .types = TrailingTypes::Yes,
                        .minimumOutgoing =
                            stubOutgoingBytes(conventions.host, 1, writer.function().memory()) });
        writer.emit(Pseudo::RestoreInstanceContext, { writer.use(saved) });
        return values;
    }
} // namespace

std::pmr::vector<VirtualRegister> callIndirect(Writer const& writer,
                                               CallConventions const& conventions,
                                               VirtualRegister row,
                                               VirtualRegister reference,
                                               std::span<CallValue const> arguments,
                                               std::span<Type const> results)
{
    auto const address = copyInteger(writer, IntegerWidth::Bits64, reference);
    writer.emit(Mnemonic::Shr64RI8, { writer.useDef(address), immediate(intermediate::RefWordAddressShift) });
    return callThroughRow(writer, conventions, row, address, arguments, results);
}

std::pmr::vector<VirtualRegister> callImport(Writer const& writer,
                                             CallConventions const& conventions,
                                             uint32_t function,
                                             std::span<CallValue const> arguments,
                                             std::span<Type const> results)
{
    auto const functions = writer.general();
    writer.emit(Mnemonic::Mov64RM,
                { writer.def(functions), at(Gpr::R14, offsetOf(InstanceContextField::Functions)) });
    auto const address = writer.general();
    writer.emit(
        Mnemonic::Mov32RM,
        { writer.def(address),
          writer.at(functions, static_cast<int64_t>(uint64_t { function } * sizeof(runtime::FuncAddr))) });
    auto const view = writer.general();
    writer.emit(Mnemonic::Mov64RM,
                { writer.def(view), at(Gpr::R15, offsetOf(ExecutionStateField::Dispatch)) });
    auto const row = dispatchRowOf(writer, view, address);
    return callThroughRow(writer, conventions, row, address, arguments, results);
}

VirtualRegister callStub(Writer const& writer,
                         CallConventions const& conventions,
                         RuntimeStub stub,
                         std::span<VirtualRegister const> operands)
{
    auto& memory = writer.function().memory();
    auto arguments = std::pmr::vector<CallValue> { &memory };
    auto const state = writer.general();
    writer.emit(Mnemonic::Mov64RR, { writer.def(state), physicalOperand(Gpr::R15) });
    auto const context = writer.general();
    writer.emit(Mnemonic::Mov64RR, { writer.def(context), physicalOperand(Gpr::R14) });
    arguments.push_back(CallValue { .reg = state, .type = Type::Pointer });
    arguments.push_back(CallValue { .reg = context, .type = Type::Pointer });
    for (auto const operand: operands)
        arguments.push_back(CallValue { .reg = operand, .type = Type::I64 });
    auto const lead = std::array { immediate(std::to_underlying(stub)) };
    auto const resultTypes = std::array { Type::I64, Type::I64 };
    auto const results = emitCall(
        writer,
        CallShape { .pseudo = Pseudo::CallStub,
                    .layout = callLayoutOf(1, conventions.host, conventions.host, ArgumentFiles::General),
                    .convention = conventions.host,
                    .lead = lead,
                    .arguments = arguments,
                    .results = resultTypes });
    writer.emit(Mnemonic::Test64RR, { writer.use(results[0]), writer.use(results[0]) });
    writer.emit(Pseudo::TrapIfPending, { conditionOperand(ConditionCode::NotEqual) });
    return results[1];
}

uint32_t stubOutgoingBytes(ConventionRow const& host, std::size_t operands, std::pmr::memory_resource& memory)
{
    auto const parameters = std::pmr::vector<Type>(2 + operands, Type::I64, &memory);
    auto const results = std::array { Type::I64, Type::I64 };
    auto const assignment = machine::assignCall(host, parameters, results, memory);
    return assignment.arguments.memoryBytes + assignment.results.memoryBytes;
}

// --- Assembler --------------------------------------------------------------------------------------------

void Assembler::record(Mnemonic mnemonic,
                       std::expected<void, EncodeError> const& encoded,
                       std::optional<ConditionCode> condition)
{
    if (!encoded && !_failure)
        _failure = encoded.error();
    if (_trace != nullptr)
        _trace->push_back(TraceEntry { .mnemonic = mnemonic, .condition = condition });
}

CodeLabel Assembler::blockLabel(machine::BlockId block) const noexcept
{
    auto const index = std::to_underlying(block);
    Require(index < _blockLabels.size());
    return _blockLabels[index];
}

void Assembler::emit(Mnemonic mnemonic,
                     std::span<MachineOperand const> operands,
                     std::span<CodeLabel const> constants)
{
    auto const labels = EncodeLabels { .blocks = _blockLabels, .constants = constants };
    // A form whose first field is a condition names it in its first operand.
    auto condition = std::optional<ConditionCode> {};
    if (auto const& form = formRow(encodingRow(mnemonic).form);
        form.count != 0 && form.fields[0] == Field::Condition && !operands.empty())
        if (auto const* value = std::get_if<machine::Immediate>(operands.data()))
            condition = static_cast<ConditionCode>(value->value);
    record(mnemonic, encode(mnemonic, operands, labels, *_buffer), condition);
}

void Assembler::emit(Mnemonic mnemonic, std::initializer_list<MachineOperand> operands)
{
    emit(mnemonic, std::span { operands.begin(), operands.size() });
}

void Assembler::jumpTo(CodeLabel label, std::optional<ConditionCode> condition)
{
    // The label as the one block of a span of its own.
    auto const labels = EncodeLabels { .blocks = std::span { &label, 1 } };
    auto const target = MachineOperand { machine::LabelOperand { .block = machine::BlockId { 0 } } };
    if (condition)
    {
        auto const operands = std::array { conditionOperand(*condition), target };
        record(Mnemonic::Jcc, encode(Mnemonic::Jcc, operands, labels, *_buffer), condition);
        return;
    }
    auto const operands = std::array { target };
    record(Mnemonic::Jmp, encode(Mnemonic::Jmp, operands, labels, *_buffer));
}

void Assembler::callTo(CodeLabel label)
{
    auto const labels = EncodeLabels { .blocks = std::span { &label, 1 } };
    auto const operands =
        std::array { MachineOperand { machine::LabelOperand { .block = machine::BlockId { 0 } } } };
    record(Mnemonic::Call, encode(Mnemonic::Call, operands, labels, *_buffer));
}

// --- Encode-time sequences --------------------------------------------------------------------------------

FunctionEmitter::FunctionEmitter(Assembler& assembler,
                                 machine::MachineFunction const& function,
                                 machine::FrameLayout const& frame,
                                 CallConventions const& conventions,
                                 machine::ModuleLabels const& labels,
                                 std::pmr::memory_resource& memory):
    _assembler { &assembler },
    _function { &function },
    _frame { &frame },
    _conventions { conventions },
    _labels { &labels },
    _memory { &memory },
    _trapSites { &memory },
    _deferred { &memory },
    _pendingExit { assembler.label() }
{
}

CodeLabel FunctionEmitter::trapSiteOf(TrapCode code)
{
    for (auto const& [siteCode, label]: _trapSites)
        if (siteCode == code)
            return label;
    _trapSites.emplace_back(code, _assembler->label());
    return _trapSites.back().second;
}

void FunctionEmitter::prologue()
{
    enterFrame(*this);
}

namespace
{
    /// The bytes of the frame below the saved frame pointer: what the prologue allocates.
    [[nodiscard]] uint64_t bodyBytesOf(machine::FrameLayout const& frame) noexcept
    {
        auto const above = uint64_t { X64StackRule.returnAddressBytes } + X64StackRule.framePointerBytes;
        Require(frame.totalBytes >= above);
        return frame.totalBytes - above;
    }

    /// Visits each callee-saved General register @p frame saves with its slot's offset from the frame's
    /// bottom: the top of the frame body, just below the saved frame pointer, in register order.
    template <typename Visit>
    void forEachSavedRegister(machine::FrameLayout const& frame, Visit const& visit)
    {
        Require(frame.calleeSaved.countIn(RegisterClass::Float) == 0); // the internal convention saves no XMM
        auto slot = bodyBytesOf(frame);
        for (auto const& row: RegisterTable)
            if (row.reg.registerClass == RegisterClass::General && frame.calleeSaved.contains(row.reg))
            {
                slot -= X64StackRule.savedRegisterBytes[0];
                visit(row.reg, static_cast<int64_t>(slot));
            }
    }

    /// The operand of physical General @p reg.
    [[nodiscard]] constexpr MachineOperand gpr(Gpr reg) noexcept
    {
        return physicalOperand(reg);
    }

} // namespace

namespace
{
    /// A value a stub call made at encode time passes: a register's, or an immediate.
    struct StubArgument
    {
        std::optional<Gpr> reg {}; ///< The register holding it; none for an immediate.
        int64_t value = 0;         ///< The immediate.
    };

    /// A call of @p stub under the host convention with @p arguments (the state, the context, then the
    /// stub's operands), its status in rax, a failed one to the trap exit. Every register the host convention
    /// clobbers is free: the instruction this belongs to declared them.
    void callStubNow(FunctionEmitter& emitter, RuntimeStub stub, std::span<StubArgument const> arguments)
    {
        auto& assembler = emitter.assembler();
        auto const& host = emitter.conventions().host;
        auto const parameters = std::pmr::vector<Type>(arguments.size(), Type::I64, &emitter.memory());
        auto const results = std::array { Type::I64, Type::I64 };
        auto const assignment = machine::assignCall(host, parameters, results, emitter.memory());
        auto const hidden = assignment.resultPointer.has_value() ? std::size_t { 1 } : 0;
        auto const resultArea = assignment.arguments.memoryBytes;
        for (auto const [index, argument]: core::views::enumerate(arguments))
        {
            auto const& location = assignment.arguments.locations[hidden + static_cast<std::size_t>(index)];
            // Every encode-time stub takes at most three arguments, which every host convention passes in
            // registers, a hidden result pointer included.
            auto const* reg = std::get_if<PhysicalRegister>(&location);
            Require(reg != nullptr);
            if (argument.reg)
                assembler.emit(Mnemonic::Mov64RR, { operandOf(*reg), gpr(*argument.reg) });
            else
                assembler.emit(Mnemonic::Mov64RI, { operandOf(*reg), immediate(argument.value) });
        }
        if (hidden != 0)
        {
            auto const* pointer = std::get_if<PhysicalRegister>(&*assignment.resultPointer);
            Require(pointer != nullptr); // both host conventions pass it in a register
            assembler.emit(Mnemonic::Lea64RM, { operandOf(*pointer), at(Gpr::Rsp, resultArea) });
        }
        assembler.emit(Mnemonic::Mov64RM,
                       { gpr(Gpr::Rax), at(Gpr::R15, offsetOf(ExecutionStateField::Stubs)) });
        assembler.emit(
            Mnemonic::Call64M,
            { at(Gpr::Rax, static_cast<int64_t>(std::to_underlying(stub) * sizeof(void const*))) });
        if (hidden != 0)
            assembler.emit(Mnemonic::Mov64RM, { gpr(Gpr::Rax), at(Gpr::Rsp, resultArea) });
        assembler.emit(Mnemonic::Test64RR, { gpr(Gpr::Rax), gpr(Gpr::Rax) });
        assembler.jumpTo(emitter.pendingExit(), ConditionCode::NotEqual);
    }
} // namespace

namespace
{
    /// Whether the prologue runs one of the entry's steps.
    enum class EntryStep : uint8_t
    {
        Skipped, ///< It does not: the function's IR has none where the prologue runs it.
        Runs,    ///< It does.
    };

    /// What the Entry pseudo-instruction asks of the prologue.
    struct EntrySteps
    {
        EntryStep poll = EntryStep::Skipped;   ///< The FunctionEntry poll.
        EntryStep charge = EntryStep::Skipped; ///< The depth and the stack values.
        uint64_t frameCost = 0;                ///< The stack values charged.
    };

    /// The Entry pseudo-instruction of @p function, first in its entry block; none without one.
    [[nodiscard]] std::optional<EntrySteps> entryOf(machine::MachineFunction const& function) noexcept
    {
        auto const instructions = function.instructions(machine::BlockId { 0 });
        if (instructions.empty() || function.instruction(instructions[0]).opcode != opcodeOf(Pseudo::Entry))
            return std::nullopt;
        auto const operands = function.operands(instructions[0]);
        auto const stepOf = [&](std::size_t position) {
            return immediateOf(operands[position]) != 0 ? EntryStep::Runs : EntryStep::Skipped;
        };
        return EntrySteps { .poll = stepOf(0),
                            .charge = stepOf(1),
                            .frameCost = static_cast<uint64_t>(immediateOf(operands[2])) };
    }

    /// The FunctionEntry poll's fast path on the stored countdown, through rax and r11 (no argument register
    /// of the internal convention), its slow path (entrySlowPath()) out of line.
    void entryPoll(FunctionEmitter& emitter)
    {
        auto& assembler = emitter.assembler();
        auto const slow = assembler.label();
        auto const resume = assembler.label();
        auto const stepsLeft = at(Gpr::R15, offsetOf(ExecutionStateField::StepsLeft));
        assembler.emit(Mnemonic::Mov64RM,
                       { gpr(Gpr::Rax), at(Gpr::R15, offsetOf(ExecutionStateField::InterruptFlag)) });
        assembler.emit(Mnemonic::Cmp8MI, { at(Gpr::Rax), immediate(0) });
        assembler.jumpTo(slow, ConditionCode::NotEqual);
        assembler.emit(Mnemonic::Mov64RM, { gpr(Gpr::R11), stepsLeft });
        assembler.emit(Mnemonic::Test64RR, { gpr(Gpr::R11), gpr(Gpr::R11) });
        assembler.jumpTo(slow, ConditionCode::Equal);
        assembler.emit(Mnemonic::Dec64R, { gpr(Gpr::R11) });
        assembler.emit(Mnemonic::Mov64MR, { stepsLeft, gpr(Gpr::R11) });
        assembler.bind(resume);
        emitter.defer(
            OutOfLinePath { .kind = OutOfLinePath::Kind::EntryPoll, .label = slow, .resume = resume });
    }

    /// The call depth, then the stack values, each trapping CallStackExhausted at @p exhausted on a borrow,
    /// through r11 (and rax for a cost past INT32_MAX).
    void entryCharge(FunctionEmitter& emitter, uint64_t frameCost, CodeLabel exhausted)
    {
        auto& assembler = emitter.assembler();
        auto const depth = at(Gpr::R15, offsetOf(ExecutionStateField::CallDepthLeft));
        assembler.emit(Mnemonic::Mov32RM, { gpr(Gpr::R11), depth });
        assembler.emit(Mnemonic::Sub32RI8, { gpr(Gpr::R11), immediate(1) });
        assembler.jumpTo(exhausted, ConditionCode::Below);
        assembler.emit(Mnemonic::Mov32MR, { depth, gpr(Gpr::R11) });
        auto const values = at(Gpr::R15, offsetOf(ExecutionStateField::StackValuesLeft));
        assembler.emit(Mnemonic::Mov64RM, { gpr(Gpr::R11), values });
        if (frameCost <= Int32Maximum)
            assembler.emit(Mnemonic::Sub64RI, { gpr(Gpr::R11), immediate(static_cast<int64_t>(frameCost)) });
        else
        {
            assembler.emit(Mnemonic::Mov64RI, { gpr(Gpr::Rax), immediate(static_cast<int64_t>(frameCost)) });
            assembler.emit(Mnemonic::Sub64RR, { gpr(Gpr::R11), gpr(Gpr::Rax) });
        }
        assembler.jumpTo(exhausted, ConditionCode::Below);
        assembler.emit(Mnemonic::Mov64MR, { values, gpr(Gpr::R11) });
    }

    /// The bytes the entry poll's slow path takes below the saved frame pointer: the host's outgoing area for
    /// PollSlow, then every argument register of the internal convention, rounded to the stack's alignment.
    [[nodiscard]] uint64_t entrySaveBytes(CallConventions const& conventions,
                                          std::pmr::memory_resource& memory)
    {
        auto const& internal = conventions.internal;
        auto const saved =
            uint64_t { 8 } * (internal.integerArguments.size() + internal.floatArguments.size());
        auto const bytes = stubOutgoingBytes(conventions.host, 1, memory) + saved;
        auto const alignment = uint64_t { X64StackRule.alignment };
        return (bytes + alignment - 1) / alignment * alignment;
    }
} // namespace

uint64_t entryPathBytes(CallConventions const& conventions, std::pmr::memory_resource& memory)
{
    return uint64_t { X64StackRule.returnAddressBytes } + X64StackRule.framePointerBytes
           + entrySaveBytes(conventions, memory);
}

void entrySlowPath(FunctionEmitter& emitter, OutOfLinePath const& path)
{
    // Before the frame: the arguments are still in their registers, which the host call would clobber, so
    // they are kept on the reserve around it (16-aligned: the prologue pushed rbp onto a call's 8 bytes).
    auto& assembler = emitter.assembler();
    auto const& internal = emitter.conventions().internal;
    auto const bytes = entrySaveBytes(emitter.conventions(), emitter.memory());
    auto const outgoing =
        static_cast<int64_t>(stubOutgoingBytes(emitter.conventions().host, 1, emitter.memory()));
    auto const slotOf = [&](std::size_t index) {
        return at(Gpr::Rsp, outgoing + (8 * static_cast<int64_t>(index)));
    };
    assembler.bind(path.label);
    assembler.emit(Mnemonic::Sub64RI, { gpr(Gpr::Rsp), immediate(static_cast<int64_t>(bytes)) });
    auto const forEachArgument = [&](auto const& visit) {
        auto index = std::size_t { 0 };
        for (auto const reg: internal.integerArguments)
            visit(reg, index++);
        for (auto const reg: internal.floatArguments)
            visit(reg, index++);
    };
    forEachArgument([&](PhysicalRegister reg, std::size_t index) {
        assembler.emit(reg.registerClass == RegisterClass::Float ? Mnemonic::MovsdMR : Mnemonic::Mov64MR,
                       { slotOf(index), operandOf(reg) });
    });
    auto const arguments = std::to_array<StubArgument>(
        { { .reg = Gpr::R15 },
          { .reg = Gpr::R14 },
          { .value = static_cast<int64_t>(std::to_underlying(runtime::PollSite::FunctionEntry)) } });
    callStubNow(emitter, RuntimeStub::PollSlow, arguments);
    forEachArgument([&](PhysicalRegister reg, std::size_t index) {
        assembler.emit(reg.registerClass == RegisterClass::Float ? Mnemonic::MovsdRM : Mnemonic::Mov64RM,
                       { operandOf(reg), slotOf(index) });
    });
    assembler.emit(Mnemonic::Add64RI, { gpr(Gpr::Rsp), immediate(static_cast<int64_t>(bytes)) });
    assembler.jumpTo(path.resume);
}

void enterFrame(FunctionEmitter& emitter)
{
    auto& assembler = emitter.assembler();
    auto const body = bodyBytesOf(emitter.frame());
    assembler.emit(Mnemonic::Push64R, { gpr(Gpr::Rbp) });
    assembler.emit(Mnemonic::Mov64RR, { gpr(Gpr::Rbp), gpr(Gpr::Rsp) });
    auto const exhausted = emitter.trapSiteOf(TrapCode::CallStackExhausted);
    // The callee's entry in the reference's order: the FunctionEntry poll, the call depth, the stack values,
    // and only then the native stack (P5 Decision "Native stack limit"). Until the frame is allocated they
    // run on the StubReserveBytes every caller's native check kept free below its own frame
    // (entryPathBytes()).
    if (auto const entry = entryOf(emitter.function()))
    {
        if (entry->poll == EntryStep::Runs)
            entryPoll(emitter);
        if (entry->charge == EntryStep::Runs)
            entryCharge(emitter, entry->frameCost, exhausted);
    }
    // The native stack check, before anything is allocated: rsp − (body + reserve), a borrow included, must
    // stay at or above the limit. rax and r11 carry no argument of the internal convention.
    auto const reserve = body + StubReserveBytes;
    assembler.emit(Mnemonic::Mov64RR, { gpr(Gpr::Rax), gpr(Gpr::Rsp) });
    if (reserve <= Int32Maximum)
        assembler.emit(Mnemonic::Sub64RI, { gpr(Gpr::Rax), immediate(static_cast<int64_t>(reserve)) });
    else
    {
        assembler.emit(Mnemonic::Mov64RI, { gpr(Gpr::R11), immediate(static_cast<int64_t>(reserve)) });
        assembler.emit(Mnemonic::Sub64RR, { gpr(Gpr::Rax), gpr(Gpr::R11) });
    }
    assembler.jumpTo(exhausted, ConditionCode::Below);
    assembler.emit(Mnemonic::Cmp64RM,
                   { gpr(Gpr::Rax), at(Gpr::R15, offsetOf(ExecutionStateField::NativeStackLimit)) });
    assembler.jumpTo(exhausted, ConditionCode::Below);
    // The frame: a page at a time past one page, touching each, so no guard page is skipped.
    if (body > ProbePageBytes)
    {
        auto const pages = body / ProbePageBytes;
        auto const rest = body % ProbePageBytes;
        assembler.emit(Mnemonic::Mov64RI, { gpr(Gpr::R11), immediate(static_cast<int64_t>(pages)) });
        auto const probe = assembler.label();
        assembler.bind(probe);
        assembler.emit(Mnemonic::Sub64RI, { gpr(Gpr::Rsp), immediate(static_cast<int64_t>(ProbePageBytes)) });
        assembler.emit(Mnemonic::Test64MR, { at(Gpr::Rsp), gpr(Gpr::Rsp) });
        assembler.emit(Mnemonic::Dec64R, { gpr(Gpr::R11) });
        assembler.jumpTo(probe, ConditionCode::NotEqual);
        if (rest != 0)
            assembler.emit(Mnemonic::Sub64RI, { gpr(Gpr::Rsp), immediate(static_cast<int64_t>(rest)) });
    }
    else if (body != 0)
        assembler.emit(Mnemonic::Sub64RI, { gpr(Gpr::Rsp), immediate(static_cast<int64_t>(body)) });
    forEachSavedRegister(emitter.frame(), [&](PhysicalRegister reg, int64_t offset) {
        assembler.emit(Mnemonic::Mov64MR, { at(Gpr::Rsp, offset), operandOf(reg) });
    });
}

void leaveFrame(FunctionEmitter& emitter)
{
    auto& assembler = emitter.assembler();
    forEachSavedRegister(emitter.frame(), [&](PhysicalRegister reg, int64_t offset) {
        assembler.emit(Mnemonic::Mov64RM, { operandOf(reg), at(Gpr::Rsp, offset) });
    });
    assembler.emit(Mnemonic::Mov64RR, { gpr(Gpr::Rsp), gpr(Gpr::Rbp) });
    assembler.emit(Mnemonic::Pop64R, { gpr(Gpr::Rbp) });
    assembler.emit(Mnemonic::Ret, {});
}

void poll(FunctionEmitter& emitter, uint32_t instruction, std::optional<PhysicalRegister> steps)
{
    // rax and rcx are clobbered by every poll (its slow path calls the host), so the fast path may use them.
    auto& assembler = emitter.assembler();
    auto const slow = assembler.label();
    auto const resume = assembler.label();
    assembler.emit(Mnemonic::Mov64RM,
                   { gpr(Gpr::Rax), at(Gpr::R15, offsetOf(ExecutionStateField::InterruptFlag)) });
    assembler.emit(Mnemonic::Cmp8MI, { at(Gpr::Rax), immediate(0) });
    assembler.jumpTo(slow, ConditionCode::NotEqual);
    if (steps)
    {
        // Tested before it is decremented, so the countdown never wraps.
        assembler.emit(Mnemonic::Test64RR, { operandOf(*steps), operandOf(*steps) });
        assembler.jumpTo(slow, ConditionCode::Equal);
        assembler.emit(Mnemonic::Dec64R, { operandOf(*steps) });
    }
    else
    {
        auto const stepsLeft = at(Gpr::R15, offsetOf(ExecutionStateField::StepsLeft));
        assembler.emit(Mnemonic::Mov64RM, { gpr(Gpr::Rcx), stepsLeft });
        assembler.emit(Mnemonic::Test64RR, { gpr(Gpr::Rcx), gpr(Gpr::Rcx) });
        assembler.jumpTo(slow, ConditionCode::Equal);
        assembler.emit(Mnemonic::Dec64R, { gpr(Gpr::Rcx) });
        assembler.emit(Mnemonic::Mov64MR, { stepsLeft, gpr(Gpr::Rcx) });
    }
    assembler.bind(resume);
    emitter.defer(OutOfLinePath { .kind = OutOfLinePath::Kind::PollSlow,
                                  .label = slow,
                                  .resume = resume,
                                  .instruction = instruction,
                                  .reg = steps });
}

void pollSlowPath(FunctionEmitter& emitter, OutOfLinePath const& path)
{
    auto& assembler = emitter.assembler();
    auto const operands = emitter.function().operands(path.instruction);
    auto const site = immediateOf(operands[path.reg ? 1 : 0]);
    assembler.bind(path.label);
    auto const stepsLeft = at(Gpr::R15, offsetOf(ExecutionStateField::StepsLeft));
    if (path.reg)
        assembler.emit(Mnemonic::Mov64MR, { stepsLeft, operandOf(*path.reg) });
    auto const arguments =
        std::to_array<StubArgument>({ { .reg = Gpr::R15 }, { .reg = Gpr::R14 }, { .value = site } });
    callStubNow(emitter, RuntimeStub::PollSlow, arguments);
    if (path.reg)
        assembler.emit(Mnemonic::Mov64RM, { operandOf(*path.reg), stepsLeft });
    assembler.jumpTo(path.resume);
}

void trapSite(FunctionEmitter& emitter, TrapCode code, std::optional<PhysicalRegister> subject)
{
    auto& assembler = emitter.assembler();
    // kind, code and subject presence are one byte each within PendingAbrupt's first word: one store.
    constexpr auto Pending = offsetOf(ExecutionStateField::Pending);
    constexpr auto KindAt = layoutOf(PendingAbruptField::Kind).offset;
    constexpr auto CodeAt = layoutOf(PendingAbruptField::Code).offset;
    constexpr auto SubjectAt = layoutOf(PendingAbruptField::Subject).offset;
    static_assert(KindAt < 4 && CodeAt < 4 && SubjectAt < 4 && layoutOf(PendingAbruptField::Kind).size == 1
                      && layoutOf(PendingAbruptField::Code).size == 1
                      && layoutOf(PendingAbruptField::Subject).size == 1,
                  "a trap site writes kind, code and subject in one 32-bit store");
    auto const presence = subject ? SubjectPresence::Present : SubjectPresence::Absent;
    auto const word = (uint64_t { std::to_underlying(PendingKind::Trap) } << (8 * KindAt))
                      | (uint64_t { std::to_underlying(code) } << (8 * CodeAt))
                      | (uint64_t { std::to_underlying(presence) } << (8 * SubjectAt));
    assembler.emit(Mnemonic::Mov32MI, { at(Gpr::R15, Pending), immediate(static_cast<int64_t>(word)) });
    if (subject)
        assembler.emit(
            Mnemonic::Mov64MR,
            { at(Gpr::R15, Pending + offsetOf(PendingAbruptField::SubjectBits)), operandOf(*subject) });
    assembler.jumpTo(emitter.labels().trapExit);
}

void switchTable(FunctionEmitter& emitter, uint32_t instruction)
{
    auto& assembler = emitter.assembler();
    auto const operands = emitter.function().operands(instruction);
    auto const index = operands[0];
    auto const clamped = operands[1];
    auto const table = operands[2];
    auto const count = immediateOf(operands[3]);
    auto const spectre = static_cast<SpectreGuard>(immediateOf(operands[4]));
    auto const fallback = std::get<machine::LabelOperand>(operands[5]);
    // The index, zero-extended (an I32's high half is not its own).
    assembler.emit(Mnemonic::Mov32RR, { clamped, index });
    if (spectre == SpectreGuard::Enabled)
    {
        // Past the targets the index becomes the fallback's entry, the last of count + 1, before the load:
        // a mispredicted bound never loads past the table.
        assembler.emit(Mnemonic::Mov32RI, { table, immediate(count) });
        assembler.emit(Mnemonic::Cmp64RR, { clamped, table });
        assembler.emit(Mnemonic::Cmovcc64RR,
                       { conditionOperand(ConditionCode::AboveOrEqual), clamped, table });
    }
    else
    {
        assembler.emit(Mnemonic::Cmp32RI, { clamped, immediate(count) });
        assembler.emit(Mnemonic::Jcc, { conditionOperand(ConditionCode::AboveOrEqual), fallback });
    }
    auto const label = assembler.label();
    auto const constants = std::array { label };
    auto const tableAddress = std::array { table, MachineOperand { machine::ConstantOperand { .slot = 0 } } };
    assembler.emit(Mnemonic::Lea64RM, tableAddress, constants);
    auto const entry =
        MachineOperand { machine::MemoryOperand { .base = std::get<RegisterOperand>(table).name,
                                                  .index = std::get<RegisterOperand>(clamped).name,
                                                  .scale = 4 } };
    assembler.emit(Mnemonic::Movsxd64M32, { clamped, entry });
    assembler.emit(Mnemonic::Add64RR, { table, clamped });
    assembler.emit(Mnemonic::Jmp64R, { table });
    emitter.defer(
        OutOfLinePath { .kind = OutOfLinePath::Kind::JumpTable, .label = label, .instruction = instruction });
}

void callDispatched(FunctionEmitter& emitter, uint32_t instruction)
{
    // r10 holds the row and rax the function address (the instruction's fixed operands); r11 carries no
    // argument of the internal convention.
    auto& assembler = emitter.assembler();
    auto const host = assembler.label();
    auto const resume = assembler.label();
    assembler.emit(Mnemonic::Mov64RM, { gpr(Gpr::R11), at(Gpr::R10, offsetOf(FunctionDispatchField::Code)) });
    assembler.emit(Mnemonic::Test64RR, { gpr(Gpr::R11), gpr(Gpr::R11) });
    assembler.jumpTo(host, ConditionCode::Equal);
    assembler.emit(Mnemonic::Mov64RM,
                   { gpr(Gpr::R14), at(Gpr::R10, offsetOf(FunctionDispatchField::Context)) });
    assembler.emit(Mnemonic::Call64R, { gpr(Gpr::R11) });
    assembler.bind(resume);
    emitter.defer(OutOfLinePath {
        .kind = OutOfLinePath::Kind::HostCall, .label = host, .resume = resume, .instruction = instruction });
}

namespace
{
    /// A dispatched call's signature, from its trailing Immediates.
    struct Signature
    {
        std::pmr::vector<Type> parameters;
        std::pmr::vector<Type> results;
    };

    [[nodiscard]] Signature signatureOf(FunctionEmitter const& emitter, uint32_t instruction)
    {
        auto const operands = emitter.function().operands(instruction);
        auto const layout = callLayoutOf(2,
                                         emitter.conventions().internal,
                                         emitter.conventions().internal,
                                         ArgumentFiles::GeneralAndFloat);
        auto const trailing = operands.subspan(layout.size());
        Require(!trailing.empty());
        auto const parameterCount = static_cast<std::size_t>(immediateOf(trailing[0]));
        auto signature = Signature { .parameters = std::pmr::vector<Type> { &emitter.memory() },
                                     .results = std::pmr::vector<Type> { &emitter.memory() } };
        for (auto const [index, operand]: core::views::enumerate(trailing.subspan(1)))
            (static_cast<std::size_t>(index) < parameterCount ? signature.parameters : signature.results)
                .push_back(static_cast<Type>(immediateOf(operand)));
        return signature;
    }
} // namespace

void hostCallPath(FunctionEmitter& emitter, OutOfLinePath const& path)
{
    // Every register but r14, r15 and those the internal convention preserves is free here: the call
    // declared them clobbered. r11 holds the scratch; r10 and xmm15 carry a value read from the stack, rax a
    // stack Ref's word (the function address waits in xmm14 meanwhile), r9 a temporary once the call
    // returned.
    auto& assembler = emitter.assembler();
    auto const emit = [&](Mnemonic mnemonic, std::initializer_list<MachineOperand> operands) {
        assembler.emit(mnemonic, operands);
    };
    auto const signature = signatureOf(emitter, path.instruction);
    auto const assignment = machine::assignCall(
        emitter.conventions().internal, signature.parameters, signature.results, emitter.memory());
    auto const hidden = assignment.resultPointer.has_value() ? std::size_t { 1 } : 0;
    auto const resultArea = static_cast<int64_t>(assignment.arguments.memoryBytes);
    constexpr auto Cell = static_cast<int64_t>(sizeof(runtime::RawValue));
    constexpr auto Word = int64_t { 8 };
    auto const scratchOf = [](Type type) {
        if (type == Type::Ref)
            return physical(Gpr::Rax);
        return machine::registerClassOf(type) == RegisterClass::Float ? physical(Xmm::Xmm15)
                                                                      : physical(Gpr::R10);
    };
    assembler.bind(path.label);
    assembler.emit(Mnemonic::Mov64RM,
                   { gpr(Gpr::R11), at(Gpr::R15, offsetOf(ExecutionStateField::HostScratch)) });
    assembler.emit(Mnemonic::MovqXR, { physicalOperand(Xmm::Xmm14), gpr(Gpr::Rax) });
    for (auto const [index, type]: core::views::enumerate(signature.parameters))
    {
        auto const cell = static_cast<int64_t>(index) * Cell;
        auto const& location = assignment.arguments.locations[hidden + static_cast<std::size_t>(index)];
        auto const* reg = std::get_if<PhysicalRegister>(&location);
        auto const source = reg != nullptr ? *reg : scratchOf(type);
        if (reg == nullptr)
            assembler.emit(
                valueFormOf(type).load,
                { operandOf(source),
                  at(Gpr::Rsp, static_cast<int64_t>(std::get<machine::StackOffset>(location).bytes)) });
        if (type == Type::Ref)
        {
            refWordToCell(
                emit,
                [&](RefRegister which, OperandRole /*role*/) {
                    return which == RefRegister::Word ? operandOf(source) : gpr(Gpr::R10);
                },
                at(Gpr::R11, cell),
                at(Gpr::R11, cell + Word));
            continue;
        }
        // Every other value is its bits in word 0, zero-extended, and 0 in word 1.
        assembler.emit(Mnemonic::Mov64MI32, { at(Gpr::R11, cell), immediate(0) });
        assembler.emit(Mnemonic::Mov64MI32, { at(Gpr::R11, cell + Word), immediate(0) });
        assembler.emit(valueFormOf(type).store, { at(Gpr::R11, cell), operandOf(source) });
    }
    assembler.emit(Mnemonic::MovqRX, { gpr(Gpr::Rax), physicalOperand(Xmm::Xmm14) });
    auto const arguments =
        std::to_array<StubArgument>({ { .reg = Gpr::R15 }, { .reg = Gpr::R14 }, { .reg = Gpr::Rax } });
    callStubNow(emitter, RuntimeStub::CallHost, arguments);
    assembler.emit(Mnemonic::Mov64RM,
                   { gpr(Gpr::R11), at(Gpr::R15, offsetOf(ExecutionStateField::HostScratch)) });
    for (auto const [index, location]: core::views::enumerate(assignment.results.locations))
    {
        auto const type = signature.results[static_cast<std::size_t>(index)];
        auto const cell = static_cast<int64_t>(index) * Cell;
        auto const* reg = std::get_if<PhysicalRegister>(&location);
        auto target = type == Type::Ref ? physical(Gpr::R10) : scratchOf(type);
        if (reg != nullptr)
            target = *reg;
        if (type == Type::Ref)
            refWordFromCell(
                emit,
                [&](RefRegister which, OperandRole /*role*/) {
                    return which == RefRegister::Word ? operandOf(target) : gpr(Gpr::R9);
                },
                at(Gpr::R11, cell),
                at(Gpr::R11, cell + Word));
        else
            assembler.emit(valueFormOf(type).load, { operandOf(target), at(Gpr::R11, cell) });
        if (reg == nullptr)
            assembler.emit(target.registerClass == RegisterClass::Float ? Mnemonic::MovsdMR
                                                                        : Mnemonic::Mov64MR,
                           { at(Gpr::Rsp, resultArea + std::get<machine::StackOffset>(location).bytes),
                             operandOf(target) });
    }
    assembler.jumpTo(path.resume);
}

namespace
{
    /// Branches to @p ifTrue on @p condition, else to @p ifFalse, falling through to the next block.
    void branchTo(Assembler& assembler,
                  ConditionCode condition,
                  MachineOperand const& ifTrue,
                  MachineOperand const& ifFalse,
                  std::optional<machine::BlockId> next)
    {
        auto const isNext = [&](MachineOperand const& label) {
            return next && std::get<machine::LabelOperand>(label).block == *next;
        };
        if (isNext(ifTrue))
        {
            assembler.emit(Mnemonic::Jcc, { conditionOperand(inverse(condition)), ifFalse });
            return;
        }
        assembler.emit(Mnemonic::Jcc, { conditionOperand(condition), ifTrue });
        if (!isNext(ifFalse))
            assembler.emit(Mnemonic::Jmp, { ifFalse });
    }

    /// The trap code of @p operand, an Immediate.
    [[nodiscard]] TrapCode trapCodeOf(MachineOperand const& operand) noexcept
    {
        return static_cast<TrapCode>(immediateOf(operand));
    }
} // namespace

void FunctionEmitter::expand(uint32_t instruction, std::optional<machine::BlockId> next)
{
    auto const operands = _function->operands(instruction);
    auto const pseudo = pseudoOf(_function->instruction(instruction).opcode);
    Require(pseudo.has_value());
    auto& assembler = *_assembler;
    switch (*pseudo)
    {
        case Pseudo::Branch:
            assembler.emit(Mnemonic::Test32RR, { operands[0], operands[0] });
            branchTo(assembler, ConditionCode::NotEqual, operands[1], operands[2], next);
            return;
        case Pseudo::CompareBranch:
            assembler.emit(static_cast<Mnemonic>(immediateOf(operands[0])), { operands[2], operands[3] });
            branchTo(assembler,
                     static_cast<ConditionCode>(immediateOf(operands[1])),
                     operands[4],
                     operands[5],
                     next);
            return;
        case Pseudo::Switch: switchTable(*this, instruction); return;
        case Pseudo::Return: leaveFrame(*this); return;
        case Pseudo::Trap: assembler.jumpTo(trapSiteOf(trapCodeOf(operands[0]))); return;
        case Pseudo::TrapIf:
            assembler.jumpTo(trapSiteOf(trapCodeOf(operands[1])),
                             static_cast<ConditionCode>(immediateOf(operands[0])));
            return;
        case Pseudo::TrapIfSubject: {
            auto const site = assembler.label();
            defer(OutOfLinePath { .kind = OutOfLinePath::Kind::TrapSubject,
                                  .label = site,
                                  .instruction = instruction,
                                  .code = trapCodeOf(operands[1]),
                                  .reg = registerOf(operands[2]) });
            assembler.jumpTo(site, static_cast<ConditionCode>(immediateOf(operands[0])));
        }
            return;
        case Pseudo::TrapIfPending:
            assembler.jumpTo(_pendingExit, static_cast<ConditionCode>(immediateOf(operands[0])));
            return;
        case Pseudo::LoadIncomingGeneral:
        case Pseudo::LoadIncomingFloat: {
            auto const offset = static_cast<int64_t>(_frame->totalBytes) + immediateOf(operands[1]);
            assembler.emit(*pseudo == Pseudo::LoadIncomingGeneral ? Mnemonic::Mov64RM : Mnemonic::MovsdRM,
                           { operands[0], at(Gpr::Rsp, offset) });
        }
            return;
        case Pseudo::Poll: poll(*this, instruction, registerOf(operands[0])); return;
        case Pseudo::PollStored: poll(*this, instruction, std::nullopt); return;
        case Pseudo::CallDirect: {
            auto const codeIndex = static_cast<std::size_t>(immediateOf(operands[0]));
            Require(codeIndex < _labels->functions.size());
            assembler.callTo(_labels->functions[codeIndex]);
        }
            return;
        case Pseudo::CallDispatched: callDispatched(*this, instruction); return;
        case Pseudo::CallStub: {
            auto const stub = immediateOf(operands[0]);
            assembler.emit(Mnemonic::Mov64RM,
                           { gpr(Gpr::Rax), at(Gpr::R15, offsetOf(ExecutionStateField::Stubs)) });
            assembler.emit(Mnemonic::Call64M,
                           { at(Gpr::Rax, stub * static_cast<int64_t>(sizeof(void const*))) });
        }
            return;
        case Pseudo::Entry: return; // enterFrame() ran it before the frame
        case Pseudo::RestoreInstanceContext:
            assembler.emit(Mnemonic::Mov64RR, { gpr(Gpr::R14), operands[0] });
            return;
    }
}

void FunctionEmitter::finish()
{
    auto& assembler = *_assembler;
    for (auto const& path: _deferred)
        switch (path.kind)
        {
            case OutOfLinePath::Kind::TrapSubject:
                assembler.bind(path.label);
                trapSite(*this, path.code, path.reg);
                break;
            case OutOfLinePath::Kind::PollSlow: pollSlowPath(*this, path); break;
            case OutOfLinePath::Kind::EntryPoll: entrySlowPath(*this, path); break;
            case OutOfLinePath::Kind::HostCall: hostCallPath(*this, path); break;
            case OutOfLinePath::Kind::JumpTable: {
                // count + 1 entries: the targets, then the fallback (the clamp's entry); each the target's
                // displacement from the table.
                auto const operands = _function->operands(path.instruction);
                auto const targets = operands.subspan(6);
                assembler.bind(path.label);
                auto const entry = [&](MachineOperand const& target) {
                    auto const block = std::get<machine::LabelOperand>(target).block;
                    assembler.buffer().emitFixup(
                        machine::FixupRequest { .kind = machine::FixupKind::TableEntry32,
                                                .target = assembler.blockLabel(block),
                                                .anchor = path.label });
                    assembler.buffer().emitLittleEndian(0, 4);
                };
                for (auto const& target: targets)
                    entry(target);
                entry(operands[5]);
            }
            break;
        }
    for (auto const& [code, label]: _trapSites)
    {
        assembler.bind(label);
        trapSite(*this, code, std::nullopt);
    }
    assembler.bind(_pendingExit);
    assembler.jumpTo(_labels->trapExit);
}

} // namespace isoline::compiler::x86_64
