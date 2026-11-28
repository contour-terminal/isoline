// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/x86_64/Encoder.hpp>
#include <isoline/compiler/x86_64/EncodingSamples.hpp>

using isoline::compiler::machine::CodeBuffer;
using isoline::compiler::machine::ConstantOperand;
using isoline::compiler::machine::IndexExtension;
using isoline::compiler::machine::LabelOperand;
using isoline::compiler::machine::MachineOperand;
using isoline::compiler::machine::MemoryOperand;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::machine::RegisterKind;
using isoline::compiler::machine::RegisterName;
using isoline::compiler::machine::RegisterOperand;
using isoline::compiler::machine::VirtualRegister;
using isoline::compiler::x86_64::ConditionCode;
using isoline::compiler::x86_64::encode;
using isoline::compiler::x86_64::EncodeError;
using isoline::compiler::x86_64::EncodeLabels;
using isoline::compiler::x86_64::encodeSample;
using isoline::compiler::x86_64::EncodingSample;
using isoline::compiler::x86_64::EncodingTable;
using isoline::compiler::x86_64::explicitSlots;
using isoline::compiler::x86_64::Gpr;
using isoline::compiler::x86_64::hexOf;
using isoline::compiler::x86_64::intendedText;
using isoline::compiler::x86_64::isMemoryKind;
using isoline::compiler::x86_64::isRegisterKind;
using isoline::compiler::x86_64::MaximumSamplesPerRow;
using isoline::compiler::x86_64::Mnemonic;
using isoline::compiler::x86_64::MnemonicCount;
using isoline::compiler::x86_64::physical;
using isoline::compiler::x86_64::RegistersPerFile;
using isoline::compiler::x86_64::sampleLine;
using isoline::compiler::x86_64::samplesFor;
using isoline::compiler::x86_64::Xmm;

namespace
{

/// An operand naming @p reg.
MachineOperand operandOf(Gpr reg)
{
    return RegisterOperand { .name = RegisterName::of(physical(reg)) };
}

/// An operand naming @p reg.
MachineOperand operandOf(Xmm reg)
{
    return RegisterOperand { .name = RegisterName::of(physical(reg)) };
}

MachineOperand operandOf(MachineOperand operand)
{
    return operand;
}

/// An immediate operand.
MachineOperand operandOf(int64_t value)
{
    return isoline::compiler::machine::Immediate { .value = value };
}

/// An immediate operand holding @p code.
MachineOperand operandOf(ConditionCode code)
{
    return isoline::compiler::machine::Immediate { .value = static_cast<int64_t>(code) };
}

/// [@p base + @p displacement].
MachineOperand mem(Gpr base, int64_t displacement = 0)
{
    return MemoryOperand { .base = RegisterName::of(physical(base)), .displacement = displacement };
}

/// [@p base + @p scale × @p index + @p displacement].
MachineOperand mem(Gpr base, Gpr index, uint8_t scale, int64_t displacement = 0)
{
    return MemoryOperand { .base = RegisterName::of(physical(base)),
                           .index = RegisterName::of(physical(index)),
                           .scale = scale,
                           .displacement = displacement };
}

/// What encode() makes of @p operands: their bytes as hex, every label bound at the instruction's end.
template <typename... Operands>
std::expected<std::string, EncodeError> encoded(Mnemonic mnemonic, Operands... operands)
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto buffer = CodeBuffer { memory };
    auto const label = std::to_array({ buffer.createLabel() });
    auto const list = std::to_array<MachineOperand>({ operandOf(operands)... });
    auto const result = encode(mnemonic, list, EncodeLabels { .blocks = label, .constants = label }, buffer);
    if (!result)
        return std::unexpected { result.error() };
    buffer.bind(label[0]);
    REQUIRE(buffer.resolve().has_value());
    return hexOf(buffer.bytes());
}

/// The bytes encode() makes of @p operands, as hex.
template <typename... Operands>
std::string bytesOf(Mnemonic mnemonic, Operands... operands)
{
    auto const result = encoded(mnemonic, operands...);
    if (!result)
        UNSCOPED_INFO("refused: " << isoline::compiler::x86_64::describe(result.error()));
    REQUIRE(result.has_value());
    return *result;
}

/// Why encode() refuses @p operands.
template <typename... Operands>
EncodeError refusalOf(Mnemonic mnemonic, Operands... operands)
{
    auto const result = encoded(mnemonic, operands...);
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// A sample of @p mnemonic over @p operands, its label at @p distance.
template <typename... Operands>
EncodingSample sampleOf(Mnemonic mnemonic, uint32_t distance, Operands... operands)
{
    auto sample = EncodingSample { .mnemonic = mnemonic,
                                   .operandCount = sizeof...(Operands),
                                   .labelDistance = distance };
    auto const list = std::to_array<MachineOperand>({ operandOf(operands)... });
    for (auto const index: std::views::iota(std::size_t { 0 }, list.size()))
        sample.operands[index] = list[index];
    return sample;
}

/// The lines of the cached agreement, tests/encoding/x86_64.golden.
std::vector<std::string> goldenLines()
{
    auto stream = std::ifstream { ISOLINE_SOURCE_DIR "/tests/encoding/x86_64.golden" };
    REQUIRE(stream.good());
    auto lines = std::vector<std::string> {};
    auto line = std::string {};
    while (std::getline(stream, line))
        lines.push_back(line);
    return lines;
}

} // namespace

TEST_CASE("Encoder.tableIsComplete")
{
    // Row order is Mnemonic order (static_asserted too), names are unique, and every row has samples.
    REQUIRE(EncodingTable.size() == MnemonicCount);
    auto names = std::set<std::string_view> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, EncodingTable.size()))
    {
        auto const& row = EncodingTable[index];
        INFO(row.name);
        CHECK(static_cast<std::size_t>(row.mnemonic) == index);
        CHECK(names.insert(row.name).second);
        auto const samples = samplesFor(row.mnemonic);
        CHECK_FALSE(samples.empty());
        CHECK(samples.size() <= MaximumSamplesPerRow);
    }
}

TEST_CASE("Encoder.knownBytes")
{
    // Hand-checked anchors: REX.R for r9 in ModRM.reg; r12's SIB and r13's disp8 as bases; sil's own REX.
    CHECK(bytesOf(Mnemonic::Add64RR, Gpr::Rax, Gpr::R9) == "4c 01 c8");
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::R12)) == "49 8b 04 24");
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::R13)) == "49 8b 45 00");
    CHECK(bytesOf(Mnemonic::Movzx32R8, Gpr::Rax, Gpr::Rsi) == "40 0f b6 c6");

    // And the special cases one at a time: rsp and rbp bases, a disp32, an index with REX.X, rip.
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::Rsp, 8)) == "48 8b 44 24 08");
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::Rbp)) == "48 8b 45 00");
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rcx, mem(Gpr::Rax, 128)) == "48 8b 88 80 00 00 00");
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::Rcx, Gpr::R10, 8, -8)) == "4a 8b 44 d1 f8");
    // A SIB base of 101 (rbp, r13) under mod 00 means no base: with an index too, it takes a disp8 of 0.
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::R13, Gpr::Rcx, 1)) == "49 8b 44 0d 00");
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::R13, Gpr::R9, 1)) == "4b 8b 44 0d 00");
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::Rbp, Gpr::Rax, 2)) == "48 8b 44 45 00");
    CHECK(bytesOf(Mnemonic::Lea64RM, Gpr::Rax, MachineOperand { ConstantOperand { .slot = 0 } })
          == "48 8d 05 00 00 00 00");
    // A rip-relative operand counts from the instruction's end, past its immediate.
    CHECK(bytesOf(Mnemonic::Mov64MI32, MachineOperand { ConstantOperand { .slot = 0 } }, int64_t { -1 })
          == "48 c7 05 00 00 00 00 ff ff ff ff");
    // The opcode's low bits, a condition code, an 8-byte immediate and a rel32.
    CHECK(bytesOf(Mnemonic::Push64R, Gpr::R13) == "41 55");
    CHECK(bytesOf(Mnemonic::Setcc8R, ConditionCode::Equal, Gpr::Rdi) == "40 0f 94 c7");
    CHECK(bytesOf(Mnemonic::Mov64RI, Gpr::R8, int64_t { -2 }) == "49 b8 fe ff ff ff ff ff ff ff");
    CHECK(bytesOf(Mnemonic::Mov64RI, Gpr::Rax, int64_t { 0x1234'5678'9abc'def0 })
          == "48 b8 f0 de bc 9a 78 56 34 12");
    CHECK(bytesOf(Mnemonic::Jcc, ConditionCode::Below, MachineOperand { LabelOperand {} })
          == "0f 82 00 00 00 00");
    // The mandatory prefix comes before REX.
    CHECK(bytesOf(Mnemonic::Lzcnt64RR, Gpr::R11, Gpr::Rax) == "f3 4c 0f bd d8");
    CHECK(bytesOf(Mnemonic::RoundsdRRI, Xmm::Xmm9, Xmm::Xmm1, int64_t { 9 }) == "66 44 0f 3a 0b c9 09");
}

TEST_CASE("EncodingSamples.intendedTextNamesEveryOperand")
{
    CHECK(intendedText(sampleOf(Mnemonic::Mov64RR, 0, Gpr::Rax, Gpr::R9)) == "mov rax, r9");
    CHECK(intendedText(sampleOf(Mnemonic::Mov64RM, 0, Gpr::Rax, mem(Gpr::R12, 8)))
          == "mov rax, qword ptr [r12 + 8]");
    CHECK(intendedText(sampleOf(Mnemonic::Mov32RM, 0, Gpr::R9, mem(Gpr::Rcx, Gpr::Rax, 4, -128)))
          == "mov r9d, dword ptr [rcx + 4*rax - 128]");
    CHECK(intendedText(sampleOf(Mnemonic::Movzx32R8, 0, Gpr::Rax, Gpr::Rsi)) == "movzx eax, sil");
    CHECK(intendedText(sampleOf(Mnemonic::Lea64RM, 8, Gpr::Rax, MachineOperand { ConstantOperand {} }))
          == "lea rax, [rip + 8]");
    CHECK(intendedText(sampleOf(Mnemonic::Add32RI, 0, Gpr::Rax, int64_t { -1 })) == "add eax, 4294967295");
    CHECK(intendedText(sampleOf(Mnemonic::Add64RI, 0, Gpr::Rax, int64_t { -1 })) == "add rax, -1");
    CHECK(intendedText(sampleOf(Mnemonic::Cmovcc64RR, 0, ConditionCode::AboveOrEqual, Gpr::Rax, Gpr::Rcx))
          == "cmovae rax, rcx");
    CHECK(
        intendedText(sampleOf(Mnemonic::Jcc, 128, ConditionCode::Greater, MachineOperand { LabelOperand {} }))
        == "jg 128");
    CHECK(intendedText(sampleOf(Mnemonic::Shl64RCl, 0, Gpr::R8)) == "shl r8, cl");

    // Every row's samples render distinct texts: a template that dropped an operand would render two samples
    // differing only in it alike.
    for (auto const& row: EncodingTable)
    {
        INFO(row.name);
        auto texts = std::set<std::string> {};
        auto const samples = samplesFor(row.mnemonic);
        for (auto const& sample: samples)
            texts.insert(intendedText(sample));
        CHECK(texts.size() == samples.size());
    }
}

TEST_CASE("EncodingSamples.coverEverySlotValue")
{
    // Each register in each register slot, and every base, index, scale and rip in each memory slot.
    for (auto const& row: EncodingTable)
    {
        INFO(row.name);
        auto const slots = explicitSlots(row);
        auto const samples = samplesFor(row.mnemonic);
        for (auto const slot: std::views::iota(std::size_t { 0 }, slots.size()))
        {
            auto const kind = slots[slot].kind;
            INFO("slot " << slot);
            if (isRegisterKind(kind))
            {
                auto seen = std::set<uint32_t> {};
                for (auto const& sample: samples)
                    seen.insert(std::get<RegisterOperand>(sample.operands[slot]).name.number);
                CHECK(seen.size() == RegistersPerFile);
            }
            if (isMemoryKind(kind))
            {
                auto bases = std::set<uint32_t> {};
                auto indexes = std::set<uint32_t> {};
                auto scales = std::set<uint32_t> {};
                auto rip = false;
                auto baseWithoutDisplacement = std::set<uint32_t> {};
                auto indexedBaseWithoutDisplacement = std::set<uint32_t> {};
                for (auto const& sample: samples)
                {
                    if (std::holds_alternative<ConstantOperand>(sample.operands[slot]))
                    {
                        rip = true;
                        continue;
                    }
                    auto const& address = std::get<MemoryOperand>(sample.operands[slot]);
                    bases.insert(address.base.number);
                    if (address.index.kind == RegisterKind::Physical)
                    {
                        indexes.insert(address.index.number);
                        scales.insert(address.scale);
                        if (address.displacement == 0)
                            indexedBaseWithoutDisplacement.insert(address.base.number);
                    }
                    else if (address.displacement == 0)
                        baseWithoutDisplacement.insert(address.base.number);
                }
                CHECK(bases.size() == RegistersPerFile);
                CHECK(indexes.size() == RegistersPerFile - 1); // every one but rsp
                CHECK(scales.size() == 4);
                CHECK(rip);
                CHECK(baseWithoutDisplacement.contains(static_cast<uint32_t>(Gpr::Rbp)));
                CHECK(baseWithoutDisplacement.contains(static_cast<uint32_t>(Gpr::R13)));
                CHECK(baseWithoutDisplacement.contains(static_cast<uint32_t>(Gpr::Rsp)));
                CHECK(baseWithoutDisplacement.contains(static_cast<uint32_t>(Gpr::R12)));
                // An rbp or r13 base with an index and no displacement: SIB's base 101 needs a disp8 then
                // too.
                CHECK(indexedBaseWithoutDisplacement.contains(static_cast<uint32_t>(Gpr::Rbp)));
                CHECK(indexedBaseWithoutDisplacement.contains(static_cast<uint32_t>(Gpr::R13)));
            }
        }
    }
}

TEST_CASE("Encoder.matchesTheCachedAgreement")
{
    // Every sample's hex, intended text and row equal tests/encoding/x86_64.golden, line by line: the cached
    // agreement of llvm-mc (scripts/check-encodings.py) with the intended text, which an offline run cannot
    // ask llvm-mc for.
    auto const golden = goldenLines();
    auto rows = std::set<std::string> {};
    auto line = std::size_t { 0 };
    for (auto const& row: EncodingTable)
    {
        for (auto const& sample: samplesFor(row.mnemonic))
        {
            auto const text = sampleLine(sample);
            REQUIRE(text.has_value());
            REQUIRE(line < golden.size());
            if (*text != golden[line])
            {
                INFO("golden line " << line + 1);
                REQUIRE(*text == golden[line]);
            }
            ++line;
        }
        rows.insert(std::string { row.name });
    }
    CHECK(line == golden.size());
    // Every row appears in the golden, by its name in the last column.
    auto goldenRows = std::set<std::string> {};
    for (auto const& entry: golden)
        goldenRows.insert(entry.substr(entry.rfind('\t') + 1));
    CHECK(goldenRows == rows);
}

TEST_CASE("Encoder.refusesUnencodable")
{
    // A displacement past a signed 32 bits, either way.
    CHECK(refusalOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::Rcx, int64_t { 1 } << 31))
          == EncodeError::DisplacementOutOfRange);
    CHECK(refusalOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::Rcx, -(int64_t { 1 } << 31) - 1))
          == EncodeError::DisplacementOutOfRange);
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::Rcx, std::numeric_limits<int32_t>::min()))
          == "48 8b 81 00 00 00 80");

    // An index SIB cannot hold, a scale it cannot, an index extension, an absolute address.
    CHECK(refusalOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::Rcx, Gpr::Rsp, 1))
          == EncodeError::IndexIsStackPointer);
    CHECK(refusalOf(Mnemonic::Mov64RM, Gpr::Rax, mem(Gpr::Rcx, Gpr::Rdx, 3)) == EncodeError::InvalidScale);
    auto extended = std::get<MemoryOperand>(mem(Gpr::Rcx, Gpr::Rdx, 4));
    extended.extension = IndexExtension::ZeroExtend32;
    CHECK(refusalOf(Mnemonic::Mov64RM, Gpr::Rax, MachineOperand { extended })
          == EncodeError::IndexExtensionUnsupported);
    CHECK(refusalOf(Mnemonic::Mov64RM, Gpr::Rax, MachineOperand { MemoryOperand { .displacement = 8 } })
          == EncodeError::AbsoluteAddress);

    // Immediates past their field, and a rounding mode that defers to MXCSR.
    CHECK(refusalOf(Mnemonic::Add64RI8, Gpr::Rax, int64_t { 128 }) == EncodeError::ImmediateOutOfRange);
    CHECK(refusalOf(Mnemonic::Add64RI8, Gpr::Rax, int64_t { -129 }) == EncodeError::ImmediateOutOfRange);
    CHECK(refusalOf(Mnemonic::Shl64RI8, Gpr::Rax, int64_t { -1 }) == EncodeError::ImmediateOutOfRange);
    CHECK(refusalOf(Mnemonic::Add64RI, Gpr::Rax, int64_t { 1 } << 31) == EncodeError::ImmediateOutOfRange);
    CHECK(refusalOf(Mnemonic::Add32RI, Gpr::Rax, int64_t { 1 } << 32) == EncodeError::ImmediateOutOfRange);
    CHECK(bytesOf(Mnemonic::Add32RI, Gpr::Rax, (int64_t { 1 } << 32) - 1) == "81 c0 ff ff ff ff");
    CHECK(refusalOf(Mnemonic::RoundssRRI, Xmm::Xmm0, Xmm::Xmm1, int64_t { 4 })
          == EncodeError::RoundingUsesMxcsr);
    CHECK(refusalOf(Mnemonic::RoundssRRI, Xmm::Xmm0, Xmm::Xmm1, int64_t { 16 })
          == EncodeError::ImmediateOutOfRange);
    CHECK(refusalOf(Mnemonic::Jcc, int64_t { 16 }, MachineOperand { LabelOperand {} })
          == EncodeError::ImmediateOutOfRange);

    // Operands of the wrong count, kind or class; a virtual register; a label no label stands for.
    CHECK(refusalOf(Mnemonic::Add64RR, Gpr::Rax) == EncodeError::OperandMismatch);
    CHECK(refusalOf(Mnemonic::Add64RR, Gpr::Rax, Xmm::Xmm1) == EncodeError::OperandMismatch);
    CHECK(refusalOf(Mnemonic::AddssRR, Xmm::Xmm0, Gpr::Rax) == EncodeError::OperandMismatch);
    CHECK(refusalOf(Mnemonic::Add64RR, Gpr::Rax, int64_t { 1 }) == EncodeError::OperandMismatch);
    CHECK(refusalOf(Mnemonic::Add64RM, Gpr::Rax, Gpr::Rcx) == EncodeError::OperandMismatch);
    CHECK(refusalOf(Mnemonic::Mov64RM, Gpr::Rax, Xmm::Xmm0) == EncodeError::OperandMismatch);
    auto const virtualRegister = MachineOperand { RegisterOperand {
        .name = RegisterName::of(VirtualRegister { 3 }, RegisterClass::General) } };
    CHECK(refusalOf(Mnemonic::Add64RR, Gpr::Rax, virtualRegister) == EncodeError::UnallocatedRegister);
    CHECK(refusalOf(Mnemonic::Mov64RM,
                    Gpr::Rax,
                    MachineOperand { MemoryOperand {
                        .base = RegisterName::of(VirtualRegister { 3 }, RegisterClass::General) } })
          == EncodeError::UnallocatedRegister);
    CHECK(refusalOf(Mnemonic::Mov64RM,
                    Gpr::Rax,
                    MachineOperand { MemoryOperand { .base = RegisterName::of(physical(Xmm::Xmm2)) } })
          == EncodeError::OperandMismatch);
    CHECK(refusalOf(Mnemonic::Jmp,
                    MachineOperand { LabelOperand { .block = isoline::compiler::machine::BlockId { 1 } } })
          == EncodeError::UnknownLabel);
    CHECK(refusalOf(Mnemonic::Lea64RM, Gpr::Rax, MachineOperand { ConstantOperand { .slot = 1 } })
          == EncodeError::UnknownLabel);

    // A refused instruction emits nothing.
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto buffer = CodeBuffer { memory };
    auto const operands = std::to_array<MachineOperand>({ operandOf(Gpr::Rax), mem(Gpr::Rcx, Gpr::Rsp, 1) });
    REQUIRE_FALSE(encode(Mnemonic::Mov64RM, operands, EncodeLabels {}, buffer).has_value());
    CHECK(buffer.offset() == 0);
}

TEST_CASE("EncodingSamples.everySampleEncodes")
{
    // The samples are encodable by construction; a refusal would hide a row from the oracle.
    for (auto const& row: EncodingTable)
        for (auto const& sample: samplesFor(row.mnemonic))
        {
            auto const bytes = encodeSample(sample);
            if (!bytes)
            {
                INFO(row.name << ": " << intendedText(sample));
                CHECK(bytes.has_value());
            }
        }
}

TEST_CASE("Encoder.wellFormedRefusesBrokenRows")
{
    // The table's static_assert, run on the rows at run time and on rows broken one column at a time.
    for (auto const& row: EncodingTable)
    {
        INFO(row.name);
        CHECK(isoline::compiler::x86_64::isWellFormed(row));
    }
    using isoline::compiler::x86_64::encodingRow;
    using isoline::compiler::x86_64::Immediate;
    using isoline::compiler::x86_64::isWellFormed;
    using isoline::compiler::x86_64::ModRm;
    auto const& add = encodingRow(Mnemonic::Add64RR);
    auto broken = add;
    broken.modRm = ModRm::Digit0; // ModRM.reg holds an operand
    CHECK_FALSE(isWellFormed(broken));
    broken = add;
    broken.immediate = Immediate::Bits8; // no immediate slot
    CHECK_FALSE(isWellFormed(broken));
    broken = add;
    broken.intelTemplate = "add {0}"; // an operand unnamed
    CHECK_FALSE(isWellFormed(broken));
    broken.intelTemplate = "add {0}, {0}"; // one named twice
    CHECK_FALSE(isWellFormed(broken));
    broken.intelTemplate = "add {0}, {2}"; // a third operand the form lacks
    CHECK_FALSE(isWellFormed(broken));
    broken.intelTemplate = "add {0}, {1"; // an open placeholder
    CHECK_FALSE(isWellFormed(broken));
    broken = add;
    broken.opcode.count = 0;
    CHECK_FALSE(isWellFormed(broken));
    broken = add;
    broken.slots = add.slots.first(1); // fewer slots than the form's operands
    CHECK_FALSE(isWellFormed(broken));
    // An immediate slot in ModRM.rm, and an explicit operand where an implicit one belongs.
    auto const& addImmediate = encodingRow(Mnemonic::Add64RI);
    broken = add;
    broken.slots = addImmediate.slots;
    CHECK_FALSE(isWellFormed(broken));
    auto const& divide = encodingRow(Mnemonic::Div64R);
    auto const wrongImplicit = std::to_array({ divide.slots[0], divide.slots[0] });
    broken = divide;
    broken.slots = wrongImplicit;
    CHECK_FALSE(isWellFormed(broken));
    // A form without ModRM that names one, and a ModRM-only form without a digit.
    broken = encodingRow(Mnemonic::Ret);
    broken.modRm = ModRm::Register;
    CHECK_FALSE(isWellFormed(broken));
    broken = encodingRow(Mnemonic::Neg64R);
    broken.modRm = ModRm::Register;
    CHECK_FALSE(isWellFormed(broken));
}

TEST_CASE("Encoder.rolesSpellTheirSlots")
{
    // The Roles column's spelling, which the table evaluates at compile time, at run time.
    namespace roles = isoline::compiler::x86_64::roles;
    using isoline::compiler::machine::OperandRole;
    using isoline::compiler::x86_64::OperandKind;
    using isoline::compiler::x86_64::Placement;
    CHECK(roles::use(roles::G64).role == OperandRole::Use);
    CHECK(roles::def(roles::X).kind == OperandKind::Vector);
    CHECK(roles::useDef(roles::M32).role == OperandRole::UseDef);
    auto const implicit = std::to_array(
        { roles::implicitUse(Gpr::Rcx), roles::implicitDef(Gpr::Rdx), roles::implicitUseDef(Gpr::Rax) });
    for (auto const& slot: implicit)
        CHECK(slot.placement == Placement::Implicit);
    CHECK(implicit[1].fixed == Gpr::Rdx);
    CHECK(roles::slots(roles::use(roles::G8), roles::use(roles::S8)).size() == 2);
    CHECK(isoline::compiler::x86_64::opcodeBytes(0x0f, 0x3a, 0x0a).count == 3);
    CHECK(isoline::compiler::x86_64::formRow(isoline::compiler::x86_64::Form::ConditionModRm).count == 2);
}

TEST_CASE("Encoder.describesEveryError")
{
    auto names = std::set<std::string_view> {};
    for (auto const error: { EncodeError::OperandMismatch,
                             EncodeError::UnallocatedRegister,
                             EncodeError::DisplacementOutOfRange,
                             EncodeError::ImmediateOutOfRange,
                             EncodeError::InvalidScale,
                             EncodeError::IndexIsStackPointer,
                             EncodeError::AbsoluteAddress,
                             EncodeError::IndexExtensionUnsupported,
                             EncodeError::RoundingUsesMxcsr,
                             EncodeError::UnknownLabel })
        names.insert(isoline::compiler::x86_64::describe(error));
    CHECK(names.size() == 10);
    CHECK_FALSE(names.contains("unknown encode error"));
}

TEST_CASE("Encoder.encodesEveryAddressShape")
{
    // An index without a base: SIB's base 101 under mod 00, always a disp32.
    auto const indexOnly = MachineOperand { MemoryOperand {
        .index = RegisterName::of(physical(Gpr::Rcx)), .scale = 4, .displacement = 8 } };
    CHECK(bytesOf(Mnemonic::Mov64RM, Gpr::Rax, indexOnly) == "48 8b 04 8d 08 00 00 00");
    CHECK(intendedText(sampleOf(Mnemonic::Mov64RM, 0, Gpr::Rax, indexOnly))
          == "mov rax, qword ptr [4*rcx + 8]");
    // The text of an absolute address, which the encoder refuses (no code is position-dependent).
    auto const absolute = MachineOperand { MemoryOperand { .displacement = 8 } };
    CHECK(intendedText(sampleOf(Mnemonic::Mov64RM, 0, Gpr::Rax, absolute)) == "mov rax, qword ptr [8]");
    auto const refused = sampleOf(Mnemonic::Mov64RM, 0, Gpr::Rax, absolute);
    auto const bytes = encodeSample(refused);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error() == EncodeError::AbsoluteAddress);
    auto const line = sampleLine(refused);
    REQUIRE_FALSE(line.has_value());
    CHECK(line.error() == EncodeError::AbsoluteAddress);
}

TEST_CASE("Encoder.refusesOperandsOfAnotherKind")
{
    auto const virtualIndex = MachineOperand { MemoryOperand {
        .base = RegisterName::of(physical(Gpr::Rcx)),
        .index = RegisterName::of(VirtualRegister { 2 }, RegisterClass::General),
        .scale = 2 } };
    CHECK(refusalOf(Mnemonic::Mov64RM, Gpr::Rax, virtualIndex) == EncodeError::UnallocatedRegister);
    CHECK(refusalOf(Mnemonic::Add64RI, Gpr::Rax, Gpr::Rcx) == EncodeError::OperandMismatch); // an immediate
    CHECK(refusalOf(Mnemonic::Push64R, Xmm::Xmm1) == EncodeError::OperandMismatch); // the opcode's register
    CHECK(refusalOf(Mnemonic::Jmp, int64_t { 4 }) == EncodeError::OperandMismatch); // a label
    CHECK(refusalOf(Mnemonic::Setcc8R, Gpr::Rax, Gpr::Rax) == EncodeError::OperandMismatch); // a condition
    CHECK(isoline::compiler::x86_64::immediateWidthOf(isoline::compiler::x86_64::OperandKind::General64)
          == isoline::compiler::x86_64::Immediate::None);
}
