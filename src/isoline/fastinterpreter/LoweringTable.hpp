// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <string_view>
#include <utility>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/fastinterpreter/Code.hpp>

namespace isoline::fastinterpreter
{

/// How lowering translates one instruction.
///
/// A Unary, Binary or BinaryImmediate row is lowered by the table alone: its operands are popped,
/// its result pushed, and `operation` emitted over their slots. A Load or Store row, one per scalar row of
/// binary::MemoryAccessTable, is lowered alike to `operation` for memory 0 of i32 addresses, to `memory64`
/// for memory 0 of i64 addresses, and to LoadIndexed or StoreIndexed naming either for any other memory. A
/// Special row is lowered by code of its own, and names the operation that code emits for it, or Nop where
/// it emits none or several.
struct LoweringRow
{
    binary::Opcode opcode; ///< The instruction; equal to the row's index in LoweringTable.
    OperationForm form;    ///< Unary, Binary, BinaryImmediate (a Binary whose constant right operand
                           ///< folds into `immediate`), Load, Store, or Special.
    OperationId operation; ///< The operation it lowers to; for Load and Store, of memory 0 of i32 addresses.
    OperationId immediate; ///< For BinaryImmediate, the twin taking the right operand as an immediate;
                           ///< otherwise `operation` again. (No std::optional: libstdc++ 14 cannot copy
                           ///< a disengaged one during constant evaluation, which building the table does.)
    OperationId memory64;  ///< For Load and Store, the twin of memory 0 of i64 addresses; otherwise
                           ///< `operation` again.

    /// Whether both rows lower alike.
    [[nodiscard]] constexpr bool operator==(LoweringRow const&) const = default;
};

namespace detail
{
    /// The row of a one-operand instruction.
    [[nodiscard]] consteval LoweringRow unaryRow(binary::Opcode opcode, OperationId operation) noexcept
    {
        return { .opcode = opcode,
                 .form = OperationForm::Unary,
                 .operation = operation,
                 .immediate = operation,
                 .memory64 = operation };
    }

    /// The row of a two-operand instruction without an immediate twin.
    [[nodiscard]] consteval LoweringRow binaryRow(binary::Opcode opcode, OperationId operation) noexcept
    {
        return { .opcode = opcode,
                 .form = OperationForm::Binary,
                 .operation = operation,
                 .immediate = operation,
                 .memory64 = operation };
    }

    /// The row of a two-operand instruction whose constant right operand folds into @p immediate.
    [[nodiscard]] consteval LoweringRow foldingRow(binary::Opcode opcode,
                                                   OperationId operation,
                                                   OperationId immediate) noexcept
    {
        return { .opcode = opcode,
                 .form = OperationForm::BinaryImmediate,
                 .operation = operation,
                 .immediate = immediate,
                 .memory64 = operation };
    }

    /// The row of an instruction lowered by code of its own, which emits @p operation for it.
    [[nodiscard]] consteval LoweringRow specialRow(binary::Opcode opcode, OperationId operation) noexcept
    {
        return { .opcode = opcode,
                 .form = OperationForm::Special,
                 .operation = operation,
                 .immediate = operation,
                 .memory64 = operation };
    }

    /// The row of a load or store of memory: @p memory32 for memory 0 of i32 addresses, @p memory64 for
    /// memory 0 of i64 addresses. Its form is the access's (binary::MemoryAccessTable), or Special where
    /// the opcode accesses no memory, which loweringRowsFitTheirOperations() refuses.
    [[nodiscard]] consteval LoweringRow memoryRow(binary::Opcode opcode,
                                                  OperationId memory32,
                                                  OperationId memory64) noexcept
    {
        auto const* access = binary::memoryAccessOf(opcode);
        auto form = OperationForm::Special;
        if (access != nullptr)
            form =
                access->access == binary::MemoryAccessKind::Load ? OperationForm::Load : OperationForm::Store;
        return {
            .opcode = opcode, .form = form, .operation = memory32, .immediate = memory32, .memory64 = memory64
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
    // Control flow: a block or loop emits nothing, an else the jump over its branch, the function's end its
    // return; each branch names its operation to a block label, whose loop twin it emits to a loop label.
    detail::specialRow(binary::Opcode::Block, OperationId::Nop),
    detail::specialRow(binary::Opcode::Loop, OperationId::Nop),
    detail::specialRow(binary::Opcode::If, OperationId::If),
    detail::specialRow(binary::Opcode::Else, OperationId::Jump),
    detail::specialRow(binary::Opcode::Br, OperationId::Jump),
    detail::specialRow(binary::Opcode::BrIf, OperationId::BrIf),
    detail::specialRow(binary::Opcode::BrTable, OperationId::BrTable),
    detail::specialRow(binary::Opcode::BrOnNull, OperationId::BrOnNull),
    detail::specialRow(binary::Opcode::BrOnNonNull, OperationId::BrOnNonNull),
    detail::specialRow(binary::Opcode::End, OperationId::Return),
    detail::specialRow(binary::Opcode::Return, OperationId::Return),
    detail::specialRow(binary::Opcode::Call, OperationId::CallInternal),
    detail::specialRow(binary::Opcode::CallIndirect, OperationId::CallIndirect),
    detail::specialRow(binary::Opcode::CallRef, OperationId::CallRef),
    detail::specialRow(binary::Opcode::ReturnCall, OperationId::ReturnCallInternal),
    detail::specialRow(binary::Opcode::ReturnCallIndirect, OperationId::ReturnCallIndirect),
    detail::specialRow(binary::Opcode::ReturnCallRef, OperationId::ReturnCallRef),
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
    detail::specialRow(binary::Opcode::MemorySize, OperationId::MemorySize),
    detail::specialRow(binary::Opcode::MemoryGrow, OperationId::MemoryGrow),

    // Lowered by the table: the scalar loads and stores, each to the operations of its name.
    detail::memoryRow(binary::Opcode::I32Load, OperationId::I32Load, OperationId::I32LoadMemory64),
    detail::memoryRow(binary::Opcode::I64Load, OperationId::I64Load, OperationId::I64LoadMemory64),
    detail::memoryRow(binary::Opcode::F32Load, OperationId::F32Load, OperationId::F32LoadMemory64),
    detail::memoryRow(binary::Opcode::F64Load, OperationId::F64Load, OperationId::F64LoadMemory64),
    detail::memoryRow(binary::Opcode::I32Load8S, OperationId::I32Load8S, OperationId::I32Load8SMemory64),
    detail::memoryRow(binary::Opcode::I32Load8U, OperationId::I32Load8U, OperationId::I32Load8UMemory64),
    detail::memoryRow(binary::Opcode::I32Load16S, OperationId::I32Load16S, OperationId::I32Load16SMemory64),
    detail::memoryRow(binary::Opcode::I32Load16U, OperationId::I32Load16U, OperationId::I32Load16UMemory64),
    detail::memoryRow(binary::Opcode::I64Load8S, OperationId::I64Load8S, OperationId::I64Load8SMemory64),
    detail::memoryRow(binary::Opcode::I64Load8U, OperationId::I64Load8U, OperationId::I64Load8UMemory64),
    detail::memoryRow(binary::Opcode::I64Load16S, OperationId::I64Load16S, OperationId::I64Load16SMemory64),
    detail::memoryRow(binary::Opcode::I64Load16U, OperationId::I64Load16U, OperationId::I64Load16UMemory64),
    detail::memoryRow(binary::Opcode::I64Load32S, OperationId::I64Load32S, OperationId::I64Load32SMemory64),
    detail::memoryRow(binary::Opcode::I64Load32U, OperationId::I64Load32U, OperationId::I64Load32UMemory64),
    detail::memoryRow(binary::Opcode::I32Store, OperationId::I32Store, OperationId::I32StoreMemory64),
    detail::memoryRow(binary::Opcode::I64Store, OperationId::I64Store, OperationId::I64StoreMemory64),
    detail::memoryRow(binary::Opcode::F32Store, OperationId::F32Store, OperationId::F32StoreMemory64),
    detail::memoryRow(binary::Opcode::F64Store, OperationId::F64Store, OperationId::F64StoreMemory64),
    detail::memoryRow(binary::Opcode::I32Store8, OperationId::I32Store8, OperationId::I32Store8Memory64),
    detail::memoryRow(binary::Opcode::I32Store16, OperationId::I32Store16, OperationId::I32Store16Memory64),
    detail::memoryRow(binary::Opcode::I64Store8, OperationId::I64Store8, OperationId::I64Store8Memory64),
    detail::memoryRow(binary::Opcode::I64Store16, OperationId::I64Store16, OperationId::I64Store16Memory64),
    detail::memoryRow(binary::Opcode::I64Store32, OperationId::I64Store32, OperationId::I64Store32Memory64),

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

    /// Whether a Load or Store row's operations are of its form and of the Memory family, and named
    /// after its instruction: `operation` its text, `memory64` its text and ".memory64". constexpr, not
    /// consteval: MSVC calls no immediate function from the lambda of loweringRowsFitTheirOperations()
    /// (C7595). That static_assert evaluates it, and LoweringRows.namingChecksDecide runs it at run time.
    [[nodiscard]] constexpr bool memoryRowFits(LoweringRow const& row) noexcept
    {
        auto const text = binary::OpcodeTable[std::to_underlying(row.opcode)].text;
        auto const fits = [&row](OperationId operation) {
            auto const& info = operationRow(operation);
            return info.form == row.form && info.family == OperationFamily::Memory;
        };
        auto const memory64 = operationRow(row.memory64).text;
        return fits(row.operation) && fits(row.memory64) && operationRow(row.operation).text == text
               && memory64.starts_with(text) && memory64.substr(text.size()) == ".memory64";
    }

    /// Whether every row the table lowers names operations of its own form, over operands the
    /// instruction's signature has: one for Unary, two for Binary and BinaryImmediate; a load or store, the
    /// operations of its access.
    [[nodiscard]] consteval bool loweringRowsFitTheirOperations() noexcept
    {
        return std::ranges::all_of(LoweringRows, [](LoweringRow const& row) {
            auto const operands =
                binary::signatureInfo(binary::OpcodeTable[std::to_underlying(row.opcode)].signature)
                    .paramCount;
            switch (row.form)
            {
                // A load's or store's operands are its address type's (a Special signature), so its
                // access row, which gave the form, stands for them.
                case OperationForm::Load:
                case OperationForm::Store: return memoryRowFits(row);
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

namespace detail
{
    /// Whether every scalar row of binary::MemoryAccessTable has its Load or Store row, and every Memory
    /// operation is a twin of one: the vector accesses are the SIMD operations'.
    [[nodiscard]] consteval bool everyScalarAccessIsLowered() noexcept
    {
        auto const lowered = [](binary::MemoryAccess const& access) {
            return std::ranges::any_of(LoweringRows, [&access](LoweringRow const& row) {
                return row.opcode == access.opcode
                       && (row.form == OperationForm::Load || row.form == OperationForm::Store);
            });
        };
        auto const twinned = [](OperationRow const& operation) {
            return std::ranges::any_of(LoweringRows, [&operation](LoweringRow const& row) {
                return (row.form == OperationForm::Load || row.form == OperationForm::Store)
                       && (row.operation == operation.id || row.memory64 == operation.id);
            });
        };
        return std::ranges::all_of(binary::MemoryAccessTable,
                                   [&lowered](binary::MemoryAccess const& access) {
                                       return access.value == binary::ValueKind::V128 || lowered(access);
                                   })
               && std::ranges::all_of(OperationTable, [&twinned](OperationRow const& operation) {
                      return operation.family != OperationFamily::Memory || twinned(operation);
                  });
    }
} // namespace detail

static_assert(detail::everyScalarAccessIsLowered(),
              "a scalar load or store without its LoweringRows row, or a memory operation of no row");

/// How lowering translates each instruction, indexed by binary::Opcode.
inline constexpr auto LoweringTable = detail::spreadLoweringRows();

/// How lowering translates @p opcode.
/// @param opcode The instruction.
/// @return Its row of LoweringTable.
[[nodiscard]] constexpr LoweringRow const& loweringRow(binary::Opcode opcode) noexcept
{
    return LoweringTable[std::to_underlying(opcode)];
}

/// A comparison fused with the br_if or if that consumes its result: one operation for both.
///
/// Lowering fuses an i32 or i64 comparison, or eqz, with the br_if or if right after it, unless the br_if
/// must move its label's values, which only BranchMove operations do. A Binary comparison whose right
/// operand is a constant fuses into `immediate`, as its LoweringRow folds it.
struct FusionRow
{
    binary::Opcode compare;    ///< The comparison.
    binary::Opcode branch;     ///< The branch consuming its result: br_if or if.
    OperationId fused;         ///< The fused operation, to a block label (or, for if, its else or end).
    OperationId fusedLoop;     ///< Its twin to a loop label, which polls; `fused` again for if, whose
                               ///< target is never a loop's.
    OperationId immediate;     ///< The fused operation of a constant right operand; `fused` for eqz.
    OperationId immediateLoop; ///< Its loop twin; `fusedLoop` for eqz, `immediate` for if.

    /// Whether both rows fuse alike.
    [[nodiscard]] constexpr bool operator==(FusionRow const&) const = default;
};

namespace detail
{
    /// The fusion of eqz with @p branch.
    [[nodiscard]] consteval FusionRow unaryFusion(binary::Opcode compare,
                                                  binary::Opcode branch,
                                                  OperationId fused,
                                                  OperationId fusedLoop) noexcept
    {
        return { .compare = compare,
                 .branch = branch,
                 .fused = fused,
                 .fusedLoop = fusedLoop,
                 .immediate = fused,
                 .immediateLoop = fusedLoop };
    }

    /// The fusion of a two-operand comparison with @p branch.
    [[nodiscard]] consteval FusionRow binaryFusion(binary::Opcode compare,
                                                   binary::Opcode branch,
                                                   OperationId fused,
                                                   OperationId fusedLoop,
                                                   OperationId immediate,
                                                   OperationId immediateLoop) noexcept
    {
        return { .compare = compare,
                 .branch = branch,
                 .fused = fused,
                 .fusedLoop = fusedLoop,
                 .immediate = immediate,
                 .immediateLoop = immediateLoop };
    }
} // namespace detail

/// Every fusion of a comparison and a branch, one row each.
// clang-format off
inline constexpr auto FusionTable = std::to_array<FusionRow>({
    detail::unaryFusion(binary::Opcode::I32Eqz, binary::Opcode::BrIf, OperationId::I32EqzBrIf, OperationId::I32EqzBrIfLoop),
    detail::unaryFusion(binary::Opcode::I32Eqz, binary::Opcode::If, OperationId::I32EqzIf, OperationId::I32EqzIf),
    detail::binaryFusion(binary::Opcode::I32Eq, binary::Opcode::BrIf, OperationId::I32EqBrIf, OperationId::I32EqBrIfLoop, OperationId::I32EqImmediateBrIf, OperationId::I32EqImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32Eq, binary::Opcode::If, OperationId::I32EqIf, OperationId::I32EqIf, OperationId::I32EqImmediateIf, OperationId::I32EqImmediateIf),
    detail::binaryFusion(binary::Opcode::I32Ne, binary::Opcode::BrIf, OperationId::I32NeBrIf, OperationId::I32NeBrIfLoop, OperationId::I32NeImmediateBrIf, OperationId::I32NeImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32Ne, binary::Opcode::If, OperationId::I32NeIf, OperationId::I32NeIf, OperationId::I32NeImmediateIf, OperationId::I32NeImmediateIf),
    detail::binaryFusion(binary::Opcode::I32LtS, binary::Opcode::BrIf, OperationId::I32LtSBrIf, OperationId::I32LtSBrIfLoop, OperationId::I32LtSImmediateBrIf, OperationId::I32LtSImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32LtS, binary::Opcode::If, OperationId::I32LtSIf, OperationId::I32LtSIf, OperationId::I32LtSImmediateIf, OperationId::I32LtSImmediateIf),
    detail::binaryFusion(binary::Opcode::I32LtU, binary::Opcode::BrIf, OperationId::I32LtUBrIf, OperationId::I32LtUBrIfLoop, OperationId::I32LtUImmediateBrIf, OperationId::I32LtUImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32LtU, binary::Opcode::If, OperationId::I32LtUIf, OperationId::I32LtUIf, OperationId::I32LtUImmediateIf, OperationId::I32LtUImmediateIf),
    detail::binaryFusion(binary::Opcode::I32GtS, binary::Opcode::BrIf, OperationId::I32GtSBrIf, OperationId::I32GtSBrIfLoop, OperationId::I32GtSImmediateBrIf, OperationId::I32GtSImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32GtS, binary::Opcode::If, OperationId::I32GtSIf, OperationId::I32GtSIf, OperationId::I32GtSImmediateIf, OperationId::I32GtSImmediateIf),
    detail::binaryFusion(binary::Opcode::I32GtU, binary::Opcode::BrIf, OperationId::I32GtUBrIf, OperationId::I32GtUBrIfLoop, OperationId::I32GtUImmediateBrIf, OperationId::I32GtUImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32GtU, binary::Opcode::If, OperationId::I32GtUIf, OperationId::I32GtUIf, OperationId::I32GtUImmediateIf, OperationId::I32GtUImmediateIf),
    detail::binaryFusion(binary::Opcode::I32LeS, binary::Opcode::BrIf, OperationId::I32LeSBrIf, OperationId::I32LeSBrIfLoop, OperationId::I32LeSImmediateBrIf, OperationId::I32LeSImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32LeS, binary::Opcode::If, OperationId::I32LeSIf, OperationId::I32LeSIf, OperationId::I32LeSImmediateIf, OperationId::I32LeSImmediateIf),
    detail::binaryFusion(binary::Opcode::I32LeU, binary::Opcode::BrIf, OperationId::I32LeUBrIf, OperationId::I32LeUBrIfLoop, OperationId::I32LeUImmediateBrIf, OperationId::I32LeUImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32LeU, binary::Opcode::If, OperationId::I32LeUIf, OperationId::I32LeUIf, OperationId::I32LeUImmediateIf, OperationId::I32LeUImmediateIf),
    detail::binaryFusion(binary::Opcode::I32GeS, binary::Opcode::BrIf, OperationId::I32GeSBrIf, OperationId::I32GeSBrIfLoop, OperationId::I32GeSImmediateBrIf, OperationId::I32GeSImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32GeS, binary::Opcode::If, OperationId::I32GeSIf, OperationId::I32GeSIf, OperationId::I32GeSImmediateIf, OperationId::I32GeSImmediateIf),
    detail::binaryFusion(binary::Opcode::I32GeU, binary::Opcode::BrIf, OperationId::I32GeUBrIf, OperationId::I32GeUBrIfLoop, OperationId::I32GeUImmediateBrIf, OperationId::I32GeUImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I32GeU, binary::Opcode::If, OperationId::I32GeUIf, OperationId::I32GeUIf, OperationId::I32GeUImmediateIf, OperationId::I32GeUImmediateIf),
    detail::unaryFusion(binary::Opcode::I64Eqz, binary::Opcode::BrIf, OperationId::I64EqzBrIf, OperationId::I64EqzBrIfLoop),
    detail::unaryFusion(binary::Opcode::I64Eqz, binary::Opcode::If, OperationId::I64EqzIf, OperationId::I64EqzIf),
    detail::binaryFusion(binary::Opcode::I64Eq, binary::Opcode::BrIf, OperationId::I64EqBrIf, OperationId::I64EqBrIfLoop, OperationId::I64EqImmediateBrIf, OperationId::I64EqImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64Eq, binary::Opcode::If, OperationId::I64EqIf, OperationId::I64EqIf, OperationId::I64EqImmediateIf, OperationId::I64EqImmediateIf),
    detail::binaryFusion(binary::Opcode::I64Ne, binary::Opcode::BrIf, OperationId::I64NeBrIf, OperationId::I64NeBrIfLoop, OperationId::I64NeImmediateBrIf, OperationId::I64NeImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64Ne, binary::Opcode::If, OperationId::I64NeIf, OperationId::I64NeIf, OperationId::I64NeImmediateIf, OperationId::I64NeImmediateIf),
    detail::binaryFusion(binary::Opcode::I64LtS, binary::Opcode::BrIf, OperationId::I64LtSBrIf, OperationId::I64LtSBrIfLoop, OperationId::I64LtSImmediateBrIf, OperationId::I64LtSImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64LtS, binary::Opcode::If, OperationId::I64LtSIf, OperationId::I64LtSIf, OperationId::I64LtSImmediateIf, OperationId::I64LtSImmediateIf),
    detail::binaryFusion(binary::Opcode::I64LtU, binary::Opcode::BrIf, OperationId::I64LtUBrIf, OperationId::I64LtUBrIfLoop, OperationId::I64LtUImmediateBrIf, OperationId::I64LtUImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64LtU, binary::Opcode::If, OperationId::I64LtUIf, OperationId::I64LtUIf, OperationId::I64LtUImmediateIf, OperationId::I64LtUImmediateIf),
    detail::binaryFusion(binary::Opcode::I64GtS, binary::Opcode::BrIf, OperationId::I64GtSBrIf, OperationId::I64GtSBrIfLoop, OperationId::I64GtSImmediateBrIf, OperationId::I64GtSImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64GtS, binary::Opcode::If, OperationId::I64GtSIf, OperationId::I64GtSIf, OperationId::I64GtSImmediateIf, OperationId::I64GtSImmediateIf),
    detail::binaryFusion(binary::Opcode::I64GtU, binary::Opcode::BrIf, OperationId::I64GtUBrIf, OperationId::I64GtUBrIfLoop, OperationId::I64GtUImmediateBrIf, OperationId::I64GtUImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64GtU, binary::Opcode::If, OperationId::I64GtUIf, OperationId::I64GtUIf, OperationId::I64GtUImmediateIf, OperationId::I64GtUImmediateIf),
    detail::binaryFusion(binary::Opcode::I64LeS, binary::Opcode::BrIf, OperationId::I64LeSBrIf, OperationId::I64LeSBrIfLoop, OperationId::I64LeSImmediateBrIf, OperationId::I64LeSImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64LeS, binary::Opcode::If, OperationId::I64LeSIf, OperationId::I64LeSIf, OperationId::I64LeSImmediateIf, OperationId::I64LeSImmediateIf),
    detail::binaryFusion(binary::Opcode::I64LeU, binary::Opcode::BrIf, OperationId::I64LeUBrIf, OperationId::I64LeUBrIfLoop, OperationId::I64LeUImmediateBrIf, OperationId::I64LeUImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64LeU, binary::Opcode::If, OperationId::I64LeUIf, OperationId::I64LeUIf, OperationId::I64LeUImmediateIf, OperationId::I64LeUImmediateIf),
    detail::binaryFusion(binary::Opcode::I64GeS, binary::Opcode::BrIf, OperationId::I64GeSBrIf, OperationId::I64GeSBrIfLoop, OperationId::I64GeSImmediateBrIf, OperationId::I64GeSImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64GeS, binary::Opcode::If, OperationId::I64GeSIf, OperationId::I64GeSIf, OperationId::I64GeSImmediateIf, OperationId::I64GeSImmediateIf),
    detail::binaryFusion(binary::Opcode::I64GeU, binary::Opcode::BrIf, OperationId::I64GeUBrIf, OperationId::I64GeUBrIfLoop, OperationId::I64GeUImmediateBrIf, OperationId::I64GeUImmediateBrIfLoop),
    detail::binaryFusion(binary::Opcode::I64GeU, binary::Opcode::If, OperationId::I64GeUIf, OperationId::I64GeUIf, OperationId::I64GeUImmediateIf, OperationId::I64GeUImmediateIf),
});
// clang-format on

/// The index of a FusionTable row in FusionIndex, where there is none.
inline constexpr auto NoFusion = uint16_t { 0xFFFF };

namespace detail
{
    /// The branches a comparison fuses with, by their index in FusionIndex.
    inline constexpr auto FusedBranches =
        std::to_array<binary::Opcode>({ binary::Opcode::BrIf, binary::Opcode::If });

    /// FusionTable spread over every comparison opcode, once per FusedBranches entry: the row's index, or
    /// NoFusion.
    [[nodiscard]] consteval std::array<std::array<uint16_t, binary::OpcodeCount>, FusedBranches.size()>
    spreadFusionRows() noexcept
    {
        auto index = std::array<std::array<uint16_t, binary::OpcodeCount>, FusedBranches.size()> {};
        for (auto& branch: index)
            branch.fill(NoFusion);
        for (auto const row: std::views::iota(std::size_t { 0 }, FusionTable.size()))
        {
            auto const& fusion = FusionTable[row];
            auto const branch = std::ranges::find(FusedBranches, fusion.branch) - FusedBranches.begin();
            index[static_cast<std::size_t>(branch)][std::to_underlying(fusion.compare)] =
                static_cast<uint16_t>(row);
        }
        return index;
    }

    /// Whether every row fuses a comparison the table lowers with a branch it fuses, once, into operations
    /// of the comparison's operands and a target, and an if's twins are its plain operations.
    [[nodiscard]] consteval bool fusionRowsAreWellFormed() noexcept
    {
        auto pairs = std::array<std::array<bool, binary::OpcodeCount>, FusedBranches.size()> {};
        return std::ranges::all_of(FusionTable, [&pairs](FusionRow const& row) {
            auto const branch = std::ranges::find(FusedBranches, row.branch);
            if (branch == FusedBranches.end())
                return false;
            auto& seen = pairs[static_cast<std::size_t>(branch - FusedBranches.begin())];
            if (std::exchange(seen[std::to_underlying(row.compare)], true))
                return false;
            auto const& lowering = LoweringTable[std::to_underlying(row.compare)];
            auto const operands = lowering.form == OperationForm::Unary ? 1U : 2U;
            auto const fits = [](OperationId operation, uint32_t slots, uint32_t immediates) {
                auto const& operationInfo = operationRow(operation);
                return operationInfo.form == OperationForm::Branch && operationInfo.slots == slots
                       && operationInfo.immediates == immediates;
            };
            // An immediate form reads the left operand's slot and carries the right one: eqz has none.
            auto const immediateImmediates = operands == 1 ? 1U : 2U;
            auto const loopTwinsAreRight =
                row.branch != binary::Opcode::If
                || (row.fusedLoop == row.fused && row.immediateLoop == row.immediate);
            auto const eqzTwinsAreRight =
                operands == 2 || (row.immediate == row.fused && row.immediateLoop == row.fusedLoop);
            return (lowering.form == OperationForm::Unary || lowering.form == OperationForm::BinaryImmediate)
                   && fits(row.fused, operands, 1) && fits(row.fusedLoop, operands, 1)
                   && fits(row.immediate, 1, immediateImmediates)
                   && fits(row.immediateLoop, 1, immediateImmediates) && loopTwinsAreRight
                   && eqzTwinsAreRight;
        });
    }

    /// FusionRow indices by branch and comparison.
    inline constexpr auto FusionIndex = spreadFusionRows();

    /// Whether @p text is @p parts, concatenated.
    [[nodiscard]] constexpr bool spells(std::string_view text,
                                        std::initializer_list<std::string_view> parts) noexcept
    {
        for (auto const part: parts)
        {
            if (!text.starts_with(part))
                return false;
            text.remove_prefix(part.size());
        }
        return text.empty();
    }

    /// Whether every row's operations are the ones its texts name, so that no row swaps a block twin
    /// for its loop twin, which would leave a loop's back-edge without its poll: `fused` is the
    /// comparison's text, then the branch's; `immediate` the same with `.imm` after the comparison (eqz,
    /// with no right operand, has none); and a br_if's loop twins the same, then `.loop`.
    [[nodiscard]] consteval bool fusionRowsAreNamedByTheirParts() noexcept
    {
        return std::ranges::all_of(FusionTable, [](FusionRow const& row) {
            auto const compare = binary::OpcodeTable[std::to_underlying(row.compare)].text;
            auto const branch = binary::OpcodeTable[std::to_underlying(row.branch)].text;
            auto const textOf = [](OperationId operation) {
                return operationRow(operation).text;
            };
            auto const unary = LoweringTable[std::to_underlying(row.compare)].form == OperationForm::Unary;
            auto const immediate = unary ? std::string_view {} : std::string_view { ".imm" };
            auto const loop =
                row.branch == binary::Opcode::BrIf ? std::string_view { ".loop" } : std::string_view {};
            return spells(textOf(row.fused), { compare, ".", branch })
                   && spells(textOf(row.fusedLoop), { compare, ".", branch, loop })
                   && spells(textOf(row.immediate), { compare, immediate, ".", branch })
                   && spells(textOf(row.immediateLoop), { compare, immediate, ".", branch, loop });
        });
    }
} // namespace detail

static_assert(detail::fusionRowsAreWellFormed(), "a fusion row names an operation of another shape");
static_assert(detail::fusionRowsAreNamedByTheirParts(),
              "a fusion row names another comparison's, branch's or label kind's operation");

/// The fusion of @p compare with the @p branch right after it.
/// @param compare The comparison.
/// @param branch The instruction after it.
/// @return Its FusionTable row, or null where the two do not fuse.
[[nodiscard]] constexpr FusionRow const* fusionOf(binary::Opcode compare, binary::Opcode branch) noexcept
{
    auto const kind = std::ranges::find(detail::FusedBranches, branch);
    if (kind == detail::FusedBranches.end())
        return nullptr;
    auto const row = detail::FusionIndex[static_cast<std::size_t>(kind - detail::FusedBranches.begin())]
                                        [std::to_underlying(compare)];
    return row == NoFusion ? nullptr : &FusionTable[row];
}

/// The operations of a conditional branch, one per kind of label and of value move.
///
/// Which one lowering emits is data: the loop twins poll (LoopPoll::Yes), and the Move forms move the
/// label's values when the branch is taken, where they do not sit in the label's slots already.
struct ConditionalBranchRow
{
    binary::Opcode opcode;  ///< The branch.
    OperationId branch;     ///< To a block label, its values in place.
    OperationId branchLoop; ///< To a loop label, its values in place.
    OperationId move;       ///< To a block label, moving its values.
    OperationId moveLoop;   ///< To a loop label, moving its values.
};

/// Every conditional branch, one row each.
inline constexpr auto ConditionalBranchTable = std::to_array<ConditionalBranchRow>({
    { .opcode = binary::Opcode::BrIf,
      .branch = OperationId::BrIf,
      .branchLoop = OperationId::BrIfLoop,
      .move = OperationId::BrIfMove,
      .moveLoop = OperationId::BrIfMoveLoop },
    { .opcode = binary::Opcode::BrOnNull,
      .branch = OperationId::BrOnNull,
      .branchLoop = OperationId::BrOnNullLoop,
      .move = OperationId::BrOnNullMove,
      .moveLoop = OperationId::BrOnNullMoveLoop },
    { .opcode = binary::Opcode::BrOnNonNull,
      .branch = OperationId::BrOnNonNull,
      .branchLoop = OperationId::BrOnNonNullLoop,
      .move = OperationId::BrOnNonNullMove,
      .moveLoop = OperationId::BrOnNonNullMoveLoop },
});

/// The conditional branch @p opcode's operations.
/// @param opcode br_if, br_on_null or br_on_non_null (checked by the caller: a row exists).
/// @return Its row of ConditionalBranchTable, or null.
[[nodiscard]] constexpr ConditionalBranchRow const* conditionalBranchOf(binary::Opcode opcode) noexcept
{
    auto const row = std::ranges::find(ConditionalBranchTable, opcode, &ConditionalBranchRow::opcode);
    return row == ConditionalBranchTable.end() ? nullptr : &*row;
}

/// One call operation: the instruction it lowers, what names its callee, and what it does with the caller's
/// frame. call and return_call lower to their Internal operation for a function of the module's own code,
/// and to their Import twin for an imported one.
struct CallRow
{
    OperationId operation; ///< The operation.
    binary::Opcode opcode; ///< The instruction it lowers.
    CalleeKind callee;     ///< What names its callee.
    CallMode mode;         ///< Whether it keeps the caller's frame or replaces it.
};

/// Every call operation, one row each.
inline constexpr auto CallTable = std::to_array<CallRow>({
    { .operation = OperationId::CallInternal,
      .opcode = binary::Opcode::Call,
      .callee = CalleeKind::Internal,
      .mode = CallMode::Call },
    { .operation = OperationId::CallImport,
      .opcode = binary::Opcode::Call,
      .callee = CalleeKind::Import,
      .mode = CallMode::Call },
    { .operation = OperationId::CallIndirect,
      .opcode = binary::Opcode::CallIndirect,
      .callee = CalleeKind::Table,
      .mode = CallMode::Call },
    { .operation = OperationId::CallRef,
      .opcode = binary::Opcode::CallRef,
      .callee = CalleeKind::Reference,
      .mode = CallMode::Call },
    { .operation = OperationId::ReturnCallInternal,
      .opcode = binary::Opcode::ReturnCall,
      .callee = CalleeKind::Internal,
      .mode = CallMode::Tail },
    { .operation = OperationId::ReturnCallImport,
      .opcode = binary::Opcode::ReturnCall,
      .callee = CalleeKind::Import,
      .mode = CallMode::Tail },
    { .operation = OperationId::ReturnCallIndirect,
      .opcode = binary::Opcode::ReturnCallIndirect,
      .callee = CalleeKind::Table,
      .mode = CallMode::Tail },
    { .operation = OperationId::ReturnCallRef,
      .opcode = binary::Opcode::ReturnCallRef,
      .callee = CalleeKind::Reference,
      .mode = CallMode::Tail },
});

/// The slot operands of a call operation whose callee @p callee names: the first argument's, then the table
/// index's or the reference's where the callee is on the operand stack.
[[nodiscard]] constexpr uint32_t callSlots(CalleeKind callee) noexcept
{
    return callee == CalleeKind::Table || callee == CalleeKind::Reference ? 2 : 1;
}

/// The immediates of a call operation whose callee @p callee names: the type and table indices of a
/// call_indirect, the one index of every other.
[[nodiscard]] consteval uint32_t callImmediates(CalleeKind callee) noexcept
{
    return callee == CalleeKind::Table ? 2 : 1;
}

namespace detail
{
    /// Whether every row's operation is a call of the operands its callee needs, a tail call exactly when
    /// its instruction is a return_call, and every pair of instruction and callee has one row.
    [[nodiscard]] consteval bool callRowsAreWellFormed() noexcept
    {
        constexpr auto TailCalls = std::to_array<binary::Opcode>({ binary::Opcode::ReturnCall,
                                                                   binary::Opcode::ReturnCallIndirect,
                                                                   binary::Opcode::ReturnCallRef });
        // A loop, not std::ranges::all_of: MSVC calls no immediate function (callImmediates()) from a lambda
        // (C7595).
        for (auto const& row: CallTable)
        {
            auto const& info = operationRow(row.operation);
            auto const tail = std::ranges::find(TailCalls, row.opcode) != TailCalls.end();
            auto const pairs = std::ranges::count_if(CallTable, [&row](CallRow const& other) {
                return other.opcode == row.opcode && other.callee == row.callee;
            });
            if (info.form != OperationForm::Call || info.slots != callSlots(row.callee)
                || info.immediates != callImmediates(row.callee) || tail != (row.mode == CallMode::Tail)
                || pairs != 1)
                return false;
        }
        return true;
    }
} // namespace detail

static_assert(detail::callRowsAreWellFormed(), "a call row names an operation of another shape or mode");

/// The call operation of @p opcode whose callee @p callee names.
/// @param opcode A call instruction.
/// @param callee What names its callee.
/// @return Its row of CallTable, or null where none lowers the pair.
[[nodiscard]] constexpr CallRow const* callOf(binary::Opcode opcode, CalleeKind callee) noexcept
{
    auto const row = std::ranges::find_if(CallTable, [&](CallRow const& candidate) {
        return candidate.opcode == opcode && candidate.callee == callee;
    });
    return row == CallTable.end() ? nullptr : &*row;
}

/// What follows an instruction in its block.
enum class Continuation : uint8_t
{
    Next, ///< The next instruction may run.
    Dead, ///< Nothing up to the block's else or end runs: lowering skips it.
};

/// The instructions after which the rest of their block is dead code, which lowering skips up to the
/// matching else or end: those that trap, branch away or leave the function unconditionally.
inline constexpr auto Terminators = std::to_array<binary::Opcode>({
    binary::Opcode::Unreachable,
    binary::Opcode::Br,
    binary::Opcode::BrTable,
    binary::Opcode::Return,
    binary::Opcode::ReturnCall,
    binary::Opcode::ReturnCallIndirect,
    binary::Opcode::ReturnCallRef,
    binary::Opcode::Throw,
    binary::Opcode::ThrowRef,
});

namespace detail
{
    /// Terminators spread over every opcode.
    [[nodiscard]] consteval std::array<Continuation, binary::OpcodeCount> spreadTerminators() noexcept
    {
        auto table = std::array<Continuation, binary::OpcodeCount> {};
        table.fill(Continuation::Next);
        for (auto const opcode: Terminators)
            table[std::to_underlying(opcode)] = Continuation::Dead;
        return table;
    }

    /// The continuation of every opcode.
    inline constexpr auto ContinuationTable = spreadTerminators();
} // namespace detail

/// What follows @p opcode in its block.
/// @param opcode The instruction.
/// @return Dead for a Terminators entry, Next otherwise.
[[nodiscard]] constexpr Continuation continuationAfter(binary::Opcode opcode) noexcept
{
    return detail::ContinuationTable[std::to_underlying(opcode)];
}

} // namespace isoline::fastinterpreter
