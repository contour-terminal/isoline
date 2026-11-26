// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <utility>

#include <isoline/binary/Opcode.hpp>
#include <isoline/fastinterpreter/Code.hpp>

namespace isoline::fastinterpreter
{

/// How lowering translates one instruction.
///
/// A Unary, Binary or BinaryImmediate row is lowered by the table alone: its operands are popped,
/// its result pushed, and `operation` emitted over their slots. A Special row is lowered by code of its
/// own, and names the operation that code emits for it, or Nop where it emits none or several.
struct LoweringRow
{
    binary::Opcode opcode; ///< The instruction; equal to the row's index in LoweringTable.
    OperationForm form;    ///< Unary, Binary, BinaryImmediate (a Binary whose constant right operand
                           ///< folds into `immediate`), or Special.
    OperationId operation; ///< The operation it lowers to.
    OperationId immediate; ///< For BinaryImmediate, the twin taking the right operand as an immediate;
                           ///< otherwise `operation` again. (No std::optional: libstdc++ 14 cannot copy
                           ///< a disengaged one during constant evaluation, which building the table does.)

    /// Whether both rows lower alike.
    [[nodiscard]] constexpr bool operator==(LoweringRow const&) const = default;
};

namespace detail
{
    /// The row of a one-operand instruction.
    [[nodiscard]] consteval LoweringRow unaryRow(binary::Opcode opcode, OperationId operation) noexcept
    {
        return {
            .opcode = opcode, .form = OperationForm::Unary, .operation = operation, .immediate = operation
        };
    }

    /// The row of a two-operand instruction without an immediate twin.
    [[nodiscard]] consteval LoweringRow binaryRow(binary::Opcode opcode, OperationId operation) noexcept
    {
        return {
            .opcode = opcode, .form = OperationForm::Binary, .operation = operation, .immediate = operation
        };
    }

    /// The row of a two-operand instruction whose constant right operand folds into @p immediate.
    [[nodiscard]] consteval LoweringRow foldingRow(binary::Opcode opcode,
                                                   OperationId operation,
                                                   OperationId immediate) noexcept
    {
        return { .opcode = opcode,
                 .form = OperationForm::BinaryImmediate,
                 .operation = operation,
                 .immediate = immediate };
    }

    /// The row of an instruction lowered by code of its own, which emits @p operation for it.
    [[nodiscard]] consteval LoweringRow specialRow(binary::Opcode opcode, OperationId operation) noexcept
    {
        return {
            .opcode = opcode, .form = OperationForm::Special, .operation = operation, .immediate = operation
        };
    }
} // namespace detail

/// The instructions lowering translates, one row each; LoweringTable spreads them over every opcode.
///
/// Adding an instruction is adding its row here, and its operation's to Operations.def. An opcode with no
/// row is Special, lowered by code a later part of lowering adds.
inline constexpr auto LoweringRows = std::to_array<LoweringRow>({
    // Lowered by code: what the abstract operand stack forwards, folds or moves.
    detail::specialRow(binary::Opcode::Unreachable, OperationId::Unreachable),
    detail::specialRow(binary::Opcode::Nop, OperationId::Nop),
    detail::specialRow(binary::Opcode::End, OperationId::Return),
    detail::specialRow(binary::Opcode::Return, OperationId::Return),
    detail::specialRow(binary::Opcode::Drop, OperationId::Nop),
    detail::specialRow(binary::Opcode::Select, OperationId::Select64),
    detail::specialRow(binary::Opcode::SelectTyped, OperationId::Select64),
    detail::specialRow(binary::Opcode::LocalGet, OperationId::Move64),
    detail::specialRow(binary::Opcode::LocalSet, OperationId::Move64),
    detail::specialRow(binary::Opcode::LocalTee, OperationId::Move64),
    detail::specialRow(binary::Opcode::GlobalGet, OperationId::GlobalGet),
    detail::specialRow(binary::Opcode::GlobalSet, OperationId::GlobalSet),
    detail::specialRow(binary::Opcode::I32Const, OperationId::Const64),
    detail::specialRow(binary::Opcode::I64Const, OperationId::Const64),
    detail::specialRow(binary::Opcode::F32Const, OperationId::Const64),
    detail::specialRow(binary::Opcode::F64Const, OperationId::Const64),

    // Lowered by the table: the numeric instructions, each to the operation of its name.
    detail::unaryRow(binary::Opcode::I32Eqz, OperationId::I32Eqz),
    detail::foldingRow(binary::Opcode::I32Eq, OperationId::I32Eq, OperationId::I32EqImmediate),
    detail::foldingRow(binary::Opcode::I32Ne, OperationId::I32Ne, OperationId::I32NeImmediate),
    detail::foldingRow(binary::Opcode::I32LtS, OperationId::I32LtS, OperationId::I32LtSImmediate),
    detail::foldingRow(binary::Opcode::I32LtU, OperationId::I32LtU, OperationId::I32LtUImmediate),
    detail::foldingRow(binary::Opcode::I32GtS, OperationId::I32GtS, OperationId::I32GtSImmediate),
    detail::foldingRow(binary::Opcode::I32GtU, OperationId::I32GtU, OperationId::I32GtUImmediate),
    detail::foldingRow(binary::Opcode::I32LeS, OperationId::I32LeS, OperationId::I32LeSImmediate),
    detail::foldingRow(binary::Opcode::I32LeU, OperationId::I32LeU, OperationId::I32LeUImmediate),
    detail::foldingRow(binary::Opcode::I32GeS, OperationId::I32GeS, OperationId::I32GeSImmediate),
    detail::foldingRow(binary::Opcode::I32GeU, OperationId::I32GeU, OperationId::I32GeUImmediate),
    detail::unaryRow(binary::Opcode::I64Eqz, OperationId::I64Eqz),
    detail::foldingRow(binary::Opcode::I64Eq, OperationId::I64Eq, OperationId::I64EqImmediate),
    detail::foldingRow(binary::Opcode::I64Ne, OperationId::I64Ne, OperationId::I64NeImmediate),
    detail::foldingRow(binary::Opcode::I64LtS, OperationId::I64LtS, OperationId::I64LtSImmediate),
    detail::foldingRow(binary::Opcode::I64LtU, OperationId::I64LtU, OperationId::I64LtUImmediate),
    detail::foldingRow(binary::Opcode::I64GtS, OperationId::I64GtS, OperationId::I64GtSImmediate),
    detail::foldingRow(binary::Opcode::I64GtU, OperationId::I64GtU, OperationId::I64GtUImmediate),
    detail::foldingRow(binary::Opcode::I64LeS, OperationId::I64LeS, OperationId::I64LeSImmediate),
    detail::foldingRow(binary::Opcode::I64LeU, OperationId::I64LeU, OperationId::I64LeUImmediate),
    detail::foldingRow(binary::Opcode::I64GeS, OperationId::I64GeS, OperationId::I64GeSImmediate),
    detail::foldingRow(binary::Opcode::I64GeU, OperationId::I64GeU, OperationId::I64GeUImmediate),
    detail::binaryRow(binary::Opcode::F32Eq, OperationId::F32Eq),
    detail::binaryRow(binary::Opcode::F32Ne, OperationId::F32Ne),
    detail::binaryRow(binary::Opcode::F32Lt, OperationId::F32Lt),
    detail::binaryRow(binary::Opcode::F32Gt, OperationId::F32Gt),
    detail::binaryRow(binary::Opcode::F32Le, OperationId::F32Le),
    detail::binaryRow(binary::Opcode::F32Ge, OperationId::F32Ge),
    detail::binaryRow(binary::Opcode::F64Eq, OperationId::F64Eq),
    detail::binaryRow(binary::Opcode::F64Ne, OperationId::F64Ne),
    detail::binaryRow(binary::Opcode::F64Lt, OperationId::F64Lt),
    detail::binaryRow(binary::Opcode::F64Gt, OperationId::F64Gt),
    detail::binaryRow(binary::Opcode::F64Le, OperationId::F64Le),
    detail::binaryRow(binary::Opcode::F64Ge, OperationId::F64Ge),
    detail::unaryRow(binary::Opcode::I32Clz, OperationId::I32Clz),
    detail::unaryRow(binary::Opcode::I32Ctz, OperationId::I32Ctz),
    detail::unaryRow(binary::Opcode::I32Popcnt, OperationId::I32Popcnt),
    detail::foldingRow(binary::Opcode::I32Add, OperationId::I32Add, OperationId::I32AddImmediate),
    detail::foldingRow(binary::Opcode::I32Sub, OperationId::I32Sub, OperationId::I32SubImmediate),
    detail::foldingRow(binary::Opcode::I32Mul, OperationId::I32Mul, OperationId::I32MulImmediate),
    detail::binaryRow(binary::Opcode::I32DivS, OperationId::I32DivS),
    detail::binaryRow(binary::Opcode::I32DivU, OperationId::I32DivU),
    detail::binaryRow(binary::Opcode::I32RemS, OperationId::I32RemS),
    detail::binaryRow(binary::Opcode::I32RemU, OperationId::I32RemU),
    detail::foldingRow(binary::Opcode::I32And, OperationId::I32And, OperationId::I32AndImmediate),
    detail::foldingRow(binary::Opcode::I32Or, OperationId::I32Or, OperationId::I32OrImmediate),
    detail::foldingRow(binary::Opcode::I32Xor, OperationId::I32Xor, OperationId::I32XorImmediate),
    detail::foldingRow(binary::Opcode::I32Shl, OperationId::I32Shl, OperationId::I32ShlImmediate),
    detail::foldingRow(binary::Opcode::I32ShrS, OperationId::I32ShrS, OperationId::I32ShrSImmediate),
    detail::foldingRow(binary::Opcode::I32ShrU, OperationId::I32ShrU, OperationId::I32ShrUImmediate),
    detail::foldingRow(binary::Opcode::I32Rotl, OperationId::I32Rotl, OperationId::I32RotlImmediate),
    detail::foldingRow(binary::Opcode::I32Rotr, OperationId::I32Rotr, OperationId::I32RotrImmediate),
    detail::unaryRow(binary::Opcode::I64Clz, OperationId::I64Clz),
    detail::unaryRow(binary::Opcode::I64Ctz, OperationId::I64Ctz),
    detail::unaryRow(binary::Opcode::I64Popcnt, OperationId::I64Popcnt),
    detail::foldingRow(binary::Opcode::I64Add, OperationId::I64Add, OperationId::I64AddImmediate),
    detail::foldingRow(binary::Opcode::I64Sub, OperationId::I64Sub, OperationId::I64SubImmediate),
    detail::foldingRow(binary::Opcode::I64Mul, OperationId::I64Mul, OperationId::I64MulImmediate),
    detail::binaryRow(binary::Opcode::I64DivS, OperationId::I64DivS),
    detail::binaryRow(binary::Opcode::I64DivU, OperationId::I64DivU),
    detail::binaryRow(binary::Opcode::I64RemS, OperationId::I64RemS),
    detail::binaryRow(binary::Opcode::I64RemU, OperationId::I64RemU),
    detail::foldingRow(binary::Opcode::I64And, OperationId::I64And, OperationId::I64AndImmediate),
    detail::foldingRow(binary::Opcode::I64Or, OperationId::I64Or, OperationId::I64OrImmediate),
    detail::foldingRow(binary::Opcode::I64Xor, OperationId::I64Xor, OperationId::I64XorImmediate),
    detail::foldingRow(binary::Opcode::I64Shl, OperationId::I64Shl, OperationId::I64ShlImmediate),
    detail::foldingRow(binary::Opcode::I64ShrS, OperationId::I64ShrS, OperationId::I64ShrSImmediate),
    detail::foldingRow(binary::Opcode::I64ShrU, OperationId::I64ShrU, OperationId::I64ShrUImmediate),
    detail::foldingRow(binary::Opcode::I64Rotl, OperationId::I64Rotl, OperationId::I64RotlImmediate),
    detail::foldingRow(binary::Opcode::I64Rotr, OperationId::I64Rotr, OperationId::I64RotrImmediate),
    detail::unaryRow(binary::Opcode::F32Abs, OperationId::F32Abs),
    detail::unaryRow(binary::Opcode::F32Neg, OperationId::F32Neg),
    detail::unaryRow(binary::Opcode::F32Ceil, OperationId::F32Ceil),
    detail::unaryRow(binary::Opcode::F32Floor, OperationId::F32Floor),
    detail::unaryRow(binary::Opcode::F32Trunc, OperationId::F32Trunc),
    detail::unaryRow(binary::Opcode::F32Nearest, OperationId::F32Nearest),
    detail::unaryRow(binary::Opcode::F32Sqrt, OperationId::F32Sqrt),
    detail::binaryRow(binary::Opcode::F32Add, OperationId::F32Add),
    detail::binaryRow(binary::Opcode::F32Sub, OperationId::F32Sub),
    detail::binaryRow(binary::Opcode::F32Mul, OperationId::F32Mul),
    detail::binaryRow(binary::Opcode::F32Div, OperationId::F32Div),
    detail::binaryRow(binary::Opcode::F32Min, OperationId::F32Min),
    detail::binaryRow(binary::Opcode::F32Max, OperationId::F32Max),
    detail::binaryRow(binary::Opcode::F32Copysign, OperationId::F32Copysign),
    detail::unaryRow(binary::Opcode::F64Abs, OperationId::F64Abs),
    detail::unaryRow(binary::Opcode::F64Neg, OperationId::F64Neg),
    detail::unaryRow(binary::Opcode::F64Ceil, OperationId::F64Ceil),
    detail::unaryRow(binary::Opcode::F64Floor, OperationId::F64Floor),
    detail::unaryRow(binary::Opcode::F64Trunc, OperationId::F64Trunc),
    detail::unaryRow(binary::Opcode::F64Nearest, OperationId::F64Nearest),
    detail::unaryRow(binary::Opcode::F64Sqrt, OperationId::F64Sqrt),
    detail::binaryRow(binary::Opcode::F64Add, OperationId::F64Add),
    detail::binaryRow(binary::Opcode::F64Sub, OperationId::F64Sub),
    detail::binaryRow(binary::Opcode::F64Mul, OperationId::F64Mul),
    detail::binaryRow(binary::Opcode::F64Div, OperationId::F64Div),
    detail::binaryRow(binary::Opcode::F64Min, OperationId::F64Min),
    detail::binaryRow(binary::Opcode::F64Max, OperationId::F64Max),
    detail::binaryRow(binary::Opcode::F64Copysign, OperationId::F64Copysign),
    detail::unaryRow(binary::Opcode::I32WrapI64, OperationId::I32WrapI64),
    detail::unaryRow(binary::Opcode::I32TruncF32S, OperationId::I32TruncF32S),
    detail::unaryRow(binary::Opcode::I32TruncF32U, OperationId::I32TruncF32U),
    detail::unaryRow(binary::Opcode::I32TruncF64S, OperationId::I32TruncF64S),
    detail::unaryRow(binary::Opcode::I32TruncF64U, OperationId::I32TruncF64U),
    detail::unaryRow(binary::Opcode::I64ExtendI32S, OperationId::I64ExtendI32S),
    detail::unaryRow(binary::Opcode::I64ExtendI32U, OperationId::I64ExtendI32U),
    detail::unaryRow(binary::Opcode::I64TruncF32S, OperationId::I64TruncF32S),
    detail::unaryRow(binary::Opcode::I64TruncF32U, OperationId::I64TruncF32U),
    detail::unaryRow(binary::Opcode::I64TruncF64S, OperationId::I64TruncF64S),
    detail::unaryRow(binary::Opcode::I64TruncF64U, OperationId::I64TruncF64U),
    detail::unaryRow(binary::Opcode::F32ConvertI32S, OperationId::F32ConvertI32S),
    detail::unaryRow(binary::Opcode::F32ConvertI32U, OperationId::F32ConvertI32U),
    detail::unaryRow(binary::Opcode::F32ConvertI64S, OperationId::F32ConvertI64S),
    detail::unaryRow(binary::Opcode::F32ConvertI64U, OperationId::F32ConvertI64U),
    detail::unaryRow(binary::Opcode::F32DemoteF64, OperationId::F32DemoteF64),
    detail::unaryRow(binary::Opcode::F64ConvertI32S, OperationId::F64ConvertI32S),
    detail::unaryRow(binary::Opcode::F64ConvertI32U, OperationId::F64ConvertI32U),
    detail::unaryRow(binary::Opcode::F64ConvertI64S, OperationId::F64ConvertI64S),
    detail::unaryRow(binary::Opcode::F64ConvertI64U, OperationId::F64ConvertI64U),
    detail::unaryRow(binary::Opcode::F64PromoteF32, OperationId::F64PromoteF32),
    detail::unaryRow(binary::Opcode::I32ReinterpretF32, OperationId::I32ReinterpretF32),
    detail::unaryRow(binary::Opcode::I64ReinterpretF64, OperationId::I64ReinterpretF64),
    detail::unaryRow(binary::Opcode::F32ReinterpretI32, OperationId::F32ReinterpretI32),
    detail::unaryRow(binary::Opcode::F64ReinterpretI64, OperationId::F64ReinterpretI64),
    detail::unaryRow(binary::Opcode::I32Extend8S, OperationId::I32Extend8S),
    detail::unaryRow(binary::Opcode::I32Extend16S, OperationId::I32Extend16S),
    detail::unaryRow(binary::Opcode::I64Extend8S, OperationId::I64Extend8S),
    detail::unaryRow(binary::Opcode::I64Extend16S, OperationId::I64Extend16S),
    detail::unaryRow(binary::Opcode::I64Extend32S, OperationId::I64Extend32S),
    detail::unaryRow(binary::Opcode::I32TruncSatF32S, OperationId::I32TruncSatF32S),
    detail::unaryRow(binary::Opcode::I32TruncSatF32U, OperationId::I32TruncSatF32U),
    detail::unaryRow(binary::Opcode::I32TruncSatF64S, OperationId::I32TruncSatF64S),
    detail::unaryRow(binary::Opcode::I32TruncSatF64U, OperationId::I32TruncSatF64U),
    detail::unaryRow(binary::Opcode::I64TruncSatF32S, OperationId::I64TruncSatF32S),
    detail::unaryRow(binary::Opcode::I64TruncSatF32U, OperationId::I64TruncSatF32U),
    detail::unaryRow(binary::Opcode::I64TruncSatF64S, OperationId::I64TruncSatF64S),
    detail::unaryRow(binary::Opcode::I64TruncSatF64U, OperationId::I64TruncSatF64U),
});

namespace detail
{
    /// LoweringRows spread over every opcode: a Special row naming Nop for each opcode without one.
    [[nodiscard]] consteval std::array<LoweringRow, binary::OpcodeCount> spreadLoweringRows() noexcept
    {
        auto table = std::array<LoweringRow, binary::OpcodeCount> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, binary::OpcodeCount))
            table[index] = specialRow(static_cast<binary::Opcode>(index), OperationId::Nop);
        for (auto const& row: LoweringRows)
            table[std::to_underlying(row.opcode)] = row;
        return table;
    }

    /// Whether no opcode has two rows in LoweringRows.
    [[nodiscard]] consteval bool loweringRowsAreUnique() noexcept
    {
        auto seen = std::array<bool, binary::OpcodeCount> {};
        return std::ranges::all_of(LoweringRows, [&seen](LoweringRow const& row) {
            return !std::exchange(seen[std::to_underlying(row.opcode)], true);
        });
    }

    /// Whether every row the table lowers names operations of its own form, over operands the
    /// instruction's signature has: one for Unary, two for Binary and BinaryImmediate.
    [[nodiscard]] consteval bool loweringRowsFitTheirOperations() noexcept
    {
        return std::ranges::all_of(LoweringRows, [](LoweringRow const& row) {
            auto const operands =
                binary::signatureInfo(binary::OpcodeTable[std::to_underlying(row.opcode)].signature)
                    .paramCount;
            switch (row.form)
            {
                case OperationForm::Unary:
                    return operands == 1 && operationRow(row.operation).form == OperationForm::Unary;
                case OperationForm::Binary:
                    return operands == 2 && operationRow(row.operation).form == OperationForm::Binary;
                case OperationForm::BinaryImmediate:
                    return operands == 2 && operationRow(row.operation).form == OperationForm::Binary
                           && operationRow(row.immediate).form == OperationForm::BinaryImmediate;
                default: return row.form == OperationForm::Special;
            }
        });
    }
} // namespace detail

static_assert(detail::loweringRowsAreUnique(), "LoweringRows holds one row per opcode at most");
static_assert(detail::loweringRowsFitTheirOperations(), "a lowering row names an operation of another shape");

/// How lowering translates each instruction, indexed by binary::Opcode.
inline constexpr auto LoweringTable = detail::spreadLoweringRows();

/// How lowering translates @p opcode.
/// @param opcode The instruction.
/// @return Its row of LoweringTable.
[[nodiscard]] constexpr LoweringRow const& loweringRow(binary::Opcode opcode) noexcept
{
    return LoweringTable[std::to_underlying(opcode)];
}

} // namespace isoline::fastinterpreter
