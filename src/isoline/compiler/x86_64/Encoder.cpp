// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/x86_64/Encoder.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <variant>

namespace isoline::compiler::x86_64
{

namespace
{
    using machine::CodeBuffer;
    using machine::CodeLabel;
    using machine::ConstantOperand;
    using machine::FixupKind;
    using machine::FixupRequest;
    using machine::IndexExtension;
    using machine::LabelOperand;
    using machine::MachineOperand;
    using machine::MemoryOperand;
    using machine::RegisterClass;
    using machine::RegisterKind;
    using machine::RegisterName;
    using machine::RegisterOperand;

    /// Whether an instruction needs a REX for its 8-bit operands alone.
    enum class ByteRex : uint8_t
    {
        Optional, ///< None names spl, bpl, sil or dil.
        Required, ///< One does: without a REX, 4–7 are ah, ch, dh and bh.
    };

    /// The rm field and REX.B of a register in ModRM.rm or the opcode's low bits, and REX.X with a SIB.
    struct Rex
    {
        uint8_t w = 0;
        uint8_t r = 0;
        uint8_t x = 0;
        uint8_t b = 0;
        ByteRex byteRex = ByteRex::Optional; ///< Whether an 8-bit operand needs a REX of its own.

        [[nodiscard]] bool needed() const noexcept
        {
            return byteRex == ByteRex::Required || (w | r | x | b) != 0;
        }
        [[nodiscard]] uint8_t byte() const noexcept
        {
            return static_cast<uint8_t>(0x40U | (w << 3U) | (r << 2U) | (x << 1U) | b);
        }
    };

    /// Whether a memory operand takes a SIB byte. (A presence enum rather than std::optional: GCC 14's
    /// -Wmaybe-uninitialized misreads an optional member of a designated-initialized aggregate.)
    enum class SibByte : uint8_t
    {
        None,    ///< ModRM alone.
        Present, ///< A SIB byte follows ModRM.
    };

    /// What a memory operand's displacement field holds.
    enum class Displacement : uint8_t
    {
        Value,       ///< The displacement itself, displacementBytes wide.
        RipRelative, ///< A rel32 to ripTarget, counted from the instruction's end.
    };

    /// What a memory operand becomes: ModRM's mod and rm, a SIB byte when it takes one, and the
    /// displacement's width (0, 1 or 4), or a rip-relative label.
    struct MemoryEncoding
    {
        uint8_t mod = 0;
        uint8_t rm = 0;
        SibByte sibByte = SibByte::None;
        uint8_t sib = 0;
        uint8_t displacementBytes = 0;
        int32_t displacement = 0;
        Displacement displacementKind = Displacement::Value;
        CodeLabel ripTarget {}; ///< The label a RipRelative displacement reaches.
    };

    /// The instruction being assembled from its row and operands, before a byte is emitted.
    struct Assembly
    {
        Rex rex {};
        uint8_t regField = 0;
        std::optional<uint8_t> registerRm {}; ///< ModRM.rm holding a register (mod 3).
        std::optional<MemoryEncoding> memory {};
        uint8_t opcodeAddend = 0; ///< The opcode register's low bits, or the condition code.
        std::optional<int64_t> immediate {};
        std::optional<CodeLabel> relativeTarget {};
    };

    /// The register an operand names, checked against its slot's kind.
    std::expected<uint8_t, EncodeError> registerOf(MachineOperand const& operand, OperandKind kind)
    {
        auto const* reg = std::get_if<RegisterOperand>(&operand);
        if (reg == nullptr)
            return std::unexpected { EncodeError::OperandMismatch };
        if (reg->name.kind == RegisterKind::Virtual)
            return std::unexpected { EncodeError::UnallocatedRegister };
        auto const wanted = kind == OperandKind::Vector ? RegisterClass::Float : RegisterClass::General;
        if (reg->name.kind != RegisterKind::Physical || reg->name.registerClass != wanted
            || reg->name.number >= RegistersPerFile)
            return std::unexpected { EncodeError::OperandMismatch };
        return static_cast<uint8_t>(reg->name.number);
    }

    /// Whether an 8-bit operand in register @p number needs a REX to name it: spl, bpl, sil and dil (without
    /// one, 4–7 are ah, ch, dh and bh, which the compiler never uses).
    bool needsRexForByte(OperandKind kind, uint8_t number) noexcept
    {
        return kind == OperandKind::General8 && number >= 4 && number <= 7;
    }

    /// A base or index register of a memory operand: none, or a physical General register.
    std::expected<std::optional<uint8_t>, EncodeError> addressRegisterOf(RegisterName const& name)
    {
        if (name.kind == RegisterKind::None)
            return std::optional<uint8_t> {};
        if (name.kind == RegisterKind::Virtual)
            return std::unexpected { EncodeError::UnallocatedRegister };
        if (name.registerClass != RegisterClass::General || name.number >= RegistersPerFile)
            return std::unexpected { EncodeError::OperandMismatch };
        return std::optional { static_cast<uint8_t>(name.number) };
    }

    /// SIB's scale bits of @p scale.
    std::optional<uint8_t> scaleBitsOf(uint8_t scale) noexcept
    {
        switch (scale)
        {
            case 1: return 0;
            case 2: return 1;
            case 4: return 2;
            case 8: return 3;
            default: return std::nullopt;
        }
    }

    /// The ModRM, SIB and displacement of a base + index × scale + displacement address.
    std::expected<MemoryEncoding, EncodeError> encodeAddress(MemoryOperand const& address, Rex& rex)
    {
        if (address.extension != IndexExtension::None)
            return std::unexpected { EncodeError::IndexExtensionUnsupported };
        auto const base = addressRegisterOf(address.base);
        if (!base)
            return std::unexpected { base.error() };
        auto const index = addressRegisterOf(address.index);
        if (!index)
            return std::unexpected { index.error() };
        if (address.displacement < std::numeric_limits<int32_t>::min()
            || address.displacement > std::numeric_limits<int32_t>::max())
            return std::unexpected { EncodeError::DisplacementOutOfRange };
        if (!base->has_value() && !index->has_value())
            return std::unexpected { EncodeError::AbsoluteAddress };
        auto scaleBits = uint8_t { 0 };
        if (index->has_value())
        {
            if (**index == static_cast<uint8_t>(Gpr::Rsp))
                return std::unexpected { EncodeError::IndexIsStackPointer };
            auto const bits = scaleBitsOf(address.scale);
            if (!bits)
                return std::unexpected { EncodeError::InvalidScale };
            scaleBits = *bits;
            rex.x = static_cast<uint8_t>(**index >> 3U);
        }

        auto encoding = MemoryEncoding { .displacement = static_cast<int32_t>(address.displacement) };
        constexpr auto SibFollows = uint8_t { 0b100 };  // rm 100: a SIB byte follows
        constexpr auto NoBaseOrRbp = uint8_t { 0b101 }; // a base of 101 under mod 00: no base, disp32
        if (!base->has_value())
        {
            // An index without a base: SIB's base 101 under mod 00, always a disp32.
            encoding.mod = 0b00;
            encoding.rm = SibFollows;
            encoding.sibByte = SibByte::Present;
            encoding.sib = static_cast<uint8_t>((scaleBits << 6U) | ((**index & 7U) << 3U) | NoBaseOrRbp);
            encoding.displacementBytes = 4;
            return encoding;
        }
        auto const baseLow = static_cast<uint8_t>(**base & 7U);
        rex.b = static_cast<uint8_t>(**base >> 3U);
        // rbp and r13 as a base take a displacement: mod 00 with rm or SIB base 101 means none, or rip.
        if (encoding.displacement == 0 && baseLow != NoBaseOrRbp)
            encoding.mod = 0b00;
        else if (encoding.displacement >= std::numeric_limits<int8_t>::min()
                 && encoding.displacement <= std::numeric_limits<int8_t>::max())
        {
            encoding.mod = 0b01;
            encoding.displacementBytes = 1;
        }
        else
        {
            encoding.mod = 0b10;
            encoding.displacementBytes = 4;
        }
        // rsp and r12 as a base take a SIB (rm 100 means one follows); so does any index.
        if (index->has_value() || baseLow == SibFollows)
        {
            auto const indexBits = index->has_value() ? static_cast<uint8_t>(**index & 7U) : SibFollows;
            encoding.rm = SibFollows;
            encoding.sibByte = SibByte::Present;
            encoding.sib = static_cast<uint8_t>((scaleBits << 6U) | (indexBits << 3U) | baseLow);
        }
        else
            encoding.rm = baseLow;
        return encoding;
    }

    /// The label of slot @p index of @p labels.
    std::expected<CodeLabel, EncodeError> labelOf(std::span<CodeLabel const> labels, std::size_t index)
    {
        if (index >= labels.size())
            return std::unexpected { EncodeError::UnknownLabel };
        return labels[index];
    }

    /// A memory operand in ModRM.rm: an address, or rip + a constant's label.
    std::expected<MemoryEncoding, EncodeError> encodeMemory(MachineOperand const& operand,
                                                            EncodeLabels const& labels,
                                                            Rex& rex)
    {
        if (auto const* address = std::get_if<MemoryOperand>(&operand))
            return encodeAddress(*address, rex);
        if (auto const* constant = std::get_if<ConstantOperand>(&operand))
        {
            auto const label = labelOf(labels.constants, constant->slot);
            if (!label)
                return std::unexpected { label.error() };
            return MemoryEncoding { .mod = 0b00,
                                    .rm = 0b101,
                                    .displacementBytes = 4,
                                    .displacementKind = Displacement::RipRelative,
                                    .ripTarget = *label };
        }
        return std::unexpected { EncodeError::OperandMismatch };
    }

    /// Whether @p value fits the immediate kind @p kind; the rounding kind refuses an MXCSR mode apart.
    std::expected<int64_t, EncodeError> immediateOf(MachineOperand const& operand, OperandKind kind)
    {
        auto const* immediate = std::get_if<machine::Immediate>(&operand);
        if (immediate == nullptr)
            return std::unexpected { EncodeError::OperandMismatch };
        auto const value = immediate->value;
        auto const within = [value](int64_t lowest, int64_t highest) {
            return value >= lowest && value <= highest;
        };
        constexpr auto Int32Minimum = int64_t { std::numeric_limits<int32_t>::min() };
        constexpr auto Int32Maximum = int64_t { std::numeric_limits<int32_t>::max() };
        constexpr auto Uint32Maximum = int64_t { std::numeric_limits<uint32_t>::max() };
        auto fitsKind = true;
        switch (kind)
        {
            case OperandKind::Signed8: fitsKind = within(-128, 127); break;
            case OperandKind::Unsigned8: fitsKind = within(0, 255); break;
            case OperandKind::Signed32: fitsKind = within(Int32Minimum, Int32Maximum); break;
            // A 32-bit operation's immediate is its 32 bits, whichever way the selector extended them.
            case OperandKind::Unsigned32: fitsKind = within(Int32Minimum, Uint32Maximum); break;
            case OperandKind::Signed64: break;
            case OperandKind::Rounding:
                if (!within(0, 15))
                    return std::unexpected { EncodeError::ImmediateOutOfRange };
                // Bit 2 selects MXCSR's rounding mode: compiled code never reads it (P5 Global Constraints).
                if ((value & 0b100) != 0)
                    return std::unexpected { EncodeError::RoundingUsesMxcsr };
                break;
            case OperandKind::Condition:
                fitsKind = within(0, static_cast<int64_t>(ConditionCodeCount) - 1);
                break;
            // Every row's immediate and condition slots are of the kinds above (isWellFormed()).
            default: return std::unexpected { EncodeError::OperandMismatch }; // uncovered(non-immediate-kind)
        }
        if (!fitsKind)
            return std::unexpected { EncodeError::ImmediateOutOfRange };
        return value;
    }

    /// The bytes of each Immediate column value's field, by enumerator (a rel32 is a fixup, emitted apart).
    constexpr auto ImmediateFieldBytes = std::to_array<uint32_t>({ 0, 1, 4, 8, 0 });
    static_assert(ImmediateFieldBytes.size() == static_cast<std::size_t>(Immediate::Relative32) + 1);

    /// The bytes of a row's immediate field.
    uint32_t immediateBytesOf(Immediate immediate) noexcept
    {
        return ImmediateFieldBytes[static_cast<std::size_t>(immediate)];
    }

    /// The legacy prefix byte of each Prefix, by enumerator; Prefix::None emits none.
    constexpr auto PrefixBytes = std::to_array<uint8_t>({ 0x00, 0x66, 0xf2, 0xf3 });
    static_assert(PrefixBytes.size() == static_cast<std::size_t>(Prefix::HexF3) + 1);

    /// Reads @p operands into an Assembly, field by field of @p row's form.
    std::expected<Assembly, EncodeError> assemble(EncodingRow const& row,
                                                  std::span<MachineOperand const> operands,
                                                  EncodeLabels const& labels)
    {
        auto const& form = formRow(row.form);
        auto const slots = explicitSlots(row);
        if (operands.size() != slots.size())
            return std::unexpected { EncodeError::OperandMismatch };
        auto assembly = Assembly {};
        assembly.rex.w = row.rexW == RexW::W1 ? 1 : 0;
        if (row.modRm != ModRm::None && row.modRm != ModRm::Register)
            assembly.regField =
                static_cast<uint8_t>(static_cast<uint8_t>(row.modRm) - static_cast<uint8_t>(ModRm::Digit0));
        for (auto const index: std::views::iota(std::size_t { 0 }, slots.size()))
        {
            auto const& operand = operands[index];
            auto const kind = slots[index].kind;
            switch (form.fields[index])
            {
                case Field::ModRmReg: {
                    auto const reg = registerOf(operand, kind);
                    if (!reg)
                        return std::unexpected { reg.error() };
                    assembly.rex.r = static_cast<uint8_t>(*reg >> 3U);
                    if (needsRexForByte(kind, *reg))
                        assembly.rex.byteRex = ByteRex::Required;
                    assembly.regField = static_cast<uint8_t>(*reg & 7U);
                    break;
                }
                case Field::ModRmRm: {
                    if (isRegisterKind(kind))
                    {
                        auto const reg = registerOf(operand, kind);
                        if (!reg)
                            return std::unexpected { reg.error() };
                        assembly.rex.b = static_cast<uint8_t>(*reg >> 3U);
                        if (needsRexForByte(kind, *reg))
                            assembly.rex.byteRex = ByteRex::Required;
                        assembly.registerRm = static_cast<uint8_t>(*reg & 7U);
                        break;
                    }
                    auto memory = encodeMemory(operand, labels, assembly.rex);
                    if (!memory)
                        return std::unexpected { memory.error() };
                    assembly.memory = *memory;
                    break;
                }
                case Field::OpcodeRegister: {
                    auto const reg = registerOf(operand, kind);
                    if (!reg)
                        return std::unexpected { reg.error() };
                    assembly.rex.b = static_cast<uint8_t>(*reg >> 3U);
                    if (needsRexForByte(kind, *reg))
                        assembly.rex.byteRex = ByteRex::Required;
                    assembly.opcodeAddend = static_cast<uint8_t>(*reg & 7U);
                    break;
                }
                case Field::Immediate: {
                    auto const value = immediateOf(operand, kind);
                    if (!value)
                        return std::unexpected { value.error() };
                    assembly.immediate = *value;
                    break;
                }
                case Field::Relative: {
                    auto const* target = std::get_if<LabelOperand>(&operand);
                    if (target == nullptr)
                        return std::unexpected { EncodeError::OperandMismatch };
                    auto const label = labelOf(labels.blocks, static_cast<std::size_t>(target->block));
                    if (!label)
                        return std::unexpected { label.error() };
                    assembly.relativeTarget = *label;
                    break;
                }
                case Field::Condition: {
                    auto const code = immediateOf(operand, kind);
                    if (!code)
                        return std::unexpected { code.error() };
                    assembly.opcodeAddend = static_cast<uint8_t>(*code);
                    break;
                }
            }
        }
        return assembly;
    }

    /// Emits the 32-bit field of a fixup to @p target, whose displacement counts from the instruction's
    /// end: @p trailing bytes after the field.
    void emitRelative(CodeBuffer& buffer, CodeLabel target, uint32_t trailing)
    {
        buffer.emitFixup(FixupRequest { .kind = FixupKind::Rel32,
                                        .target = target,
                                        .anchor = std::nullopt,
                                        .addend = -(4 + int64_t { trailing }) });
        buffer.emitLittleEndian(0, 4);
    }
} // namespace

std::string_view describe(EncodeError error) noexcept
{
    constexpr auto Names = std::to_array<std::string_view>({ "operand mismatch",
                                                             "unallocated register",
                                                             "displacement out of range",
                                                             "immediate out of range",
                                                             "invalid scale",
                                                             "index is the stack pointer",
                                                             "absolute address",
                                                             "index extension unsupported",
                                                             "rounding uses MXCSR",
                                                             "unknown label" });
    static_assert(Names.size() == static_cast<std::size_t>(EncodeError::UnknownLabel) + 1);
    return Names[static_cast<std::size_t>(error)];
}

std::expected<void, EncodeError> encode(Mnemonic mnemonic,
                                        std::span<MachineOperand const> operands,
                                        EncodeLabels const& labels,
                                        CodeBuffer& buffer)
{
    auto const& row = encodingRow(mnemonic);
    auto const assembly = assemble(row, operands, labels);
    if (!assembly)
        return std::unexpected { assembly.error() };

    if (row.prefix != Prefix::None)
        buffer.emitLittleEndian(PrefixBytes[static_cast<std::size_t>(row.prefix)], 1);
    if (assembly->rex.needed())
        buffer.emitLittleEndian(assembly->rex.byte(), 1);
    auto const opcode = std::span { row.opcode.bytes }.first(row.opcode.count);
    buffer.emit(opcode.first(opcode.size() - 1));
    buffer.emitLittleEndian(static_cast<uint8_t>(opcode.back() + assembly->opcodeAddend), 1);

    auto const immediateBytes = immediateBytesOf(row.immediate);
    if (row.modRm != ModRm::None)
    {
        auto const mod = assembly->registerRm ? uint8_t { 0b11 } : assembly->memory->mod;
        auto const rm = assembly->registerRm ? *assembly->registerRm : assembly->memory->rm;
        buffer.emitLittleEndian(static_cast<uint8_t>((mod << 6U) | (assembly->regField << 3U) | rm), 1);
        if (assembly->memory)
        {
            auto const& memory = *assembly->memory;
            if (memory.sibByte == SibByte::Present)
                buffer.emitLittleEndian(memory.sib, 1);
            if (memory.displacementKind == Displacement::RipRelative)
                emitRelative(buffer, memory.ripTarget, immediateBytes);
            else if (memory.displacementBytes != 0)
                buffer.emitLittleEndian(static_cast<uint32_t>(memory.displacement), memory.displacementBytes);
        }
    }
    if (assembly->immediate)
        buffer.emitLittleEndian(static_cast<uint64_t>(*assembly->immediate), immediateBytes);
    if (assembly->relativeTarget)
        emitRelative(buffer, *assembly->relativeTarget, 0);
    return {};
}

} // namespace isoline::compiler::x86_64
