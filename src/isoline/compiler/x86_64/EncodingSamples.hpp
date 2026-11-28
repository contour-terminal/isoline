// SPDX-License-Identifier: Apache-2.0
#pragma once

// The samples the encoding oracle checks every row of the encoding table with, and the text each one is
// meant to be. samplesFor() picks, per row, every register in each register slot, every addressing mode
// (base, disp8, disp32, base + index × {1, 2, 4, 8}, the rsp and r12 bases that need a SIB, the rbp and r13
// bases that need a displacement, rip + disp32) and the immediates {0, 1, −1, 127, −128, 2^31 − 1} the slot
// takes (a 64-bit one also 0x123456789abcdef0), at most MaximumSamplesPerRow of them by a deterministic
// stride. intendedText() renders the row's Intel template with the sample's operands, as llvm-mc spells them,
// and knows nothing of the encoder: the oracle (scripts/check-encodings.py) disassembles the encoder's bytes
// and compares the two, so a dropped REX.B reads as `mov rax, rcx` where the intent says `mov rax, r9`. Test
// support: built into the unit tests and the isoline_encoding_samples program, never into the library.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/x86_64/Encoder.hpp>

namespace isoline::compiler::x86_64
{

/// The most samples samplesFor() returns for one row.
inline constexpr auto MaximumSamplesPerRow = std::size_t { 512 };

/// One instruction to encode and check: a row, its explicit operands, and where its label lies.
struct EncodingSample
{
    Mnemonic mnemonic = Mnemonic {}; ///< The row.
    std::array<machine::MachineOperand, MaximumExplicitOperands>
        operands {};          ///< Its operands; operandCount used.
    uint8_t operandCount = 0; ///< How many operands it has.
    /// For a label operand (block 0) or a rip-relative one (constant slot 0): how many bytes past the
    /// instruction's end its label is bound, which is the displacement llvm-mc prints.
    uint32_t labelDistance = 0;

    /// The sample's operands.
    [[nodiscard]] std::span<machine::MachineOperand const> explicitOperands() const noexcept
    {
        return std::span { operands }.first(operandCount);
    }
};

/// The samples of @p mnemonic's row, in a deterministic order: every value of each operand slot at least
/// once, then a stride through all their combinations, at most MaximumSamplesPerRow in all.
/// @param mnemonic The row.
/// @return Its samples; at least one.
[[nodiscard]] std::vector<EncodingSample> samplesFor(Mnemonic mnemonic);

/// The instruction @p sample is meant to be: its row's Intel template with its operands, spelled as llvm-mc
/// prints them (registers by their width, `qword ptr [r12 + 8*rax - 128]`, `[rip + 8]`, decimal immediates,
/// a 32-bit operation's immediate unsigned, a branch's displacement from the instruction's end).
/// @param sample The sample.
/// @return Its text.
[[nodiscard]] std::string intendedText(EncodingSample const& sample);

/// The encoder's bytes of @p sample, its label bound labelDistance bytes past the instruction's end.
/// @param sample The sample.
/// @return The instruction's bytes; or why the encoder refused it.
[[nodiscard]] std::expected<std::vector<uint8_t>, EncodeError> encodeSample(EncodingSample const& sample);

/// @p bytes as lowercase hex, space-separated: `4c 01 c8`.
[[nodiscard]] std::string hexOf(std::span<uint8_t const> bytes);

/// The oracle's line of @p sample: `hex<TAB>intended<TAB>row`.
/// @return The line; or why the encoder refused the sample.
[[nodiscard]] std::expected<std::string, EncodeError> sampleLine(EncodingSample const& sample);

} // namespace isoline::compiler::x86_64
