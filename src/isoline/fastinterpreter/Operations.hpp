// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Context.hpp>
#include <isoline/fastinterpreter/LittleEndian.hpp>
#include <isoline/fastinterpreter/LoweringTable.hpp>
#include <isoline/fastinterpreter/SlotCodec.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/numerics/Integer.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

// The body of every operation, written once: operation<Id>(State&), which both dispatches instantiate,
// the switch in its cases and the tail-call backend in its handlers. A body reads its operands after the
// header word at State::pc, and leaves pc at the operation that runs next.
//
// Nothing here names an operation's semantics twice. A numeric operation runs its instruction's kernel
// (ScalarKernel, by the opcode LoweringRows lowers to it); a fused comparison and branch runs its
// comparison's kernel and branches as FusionTable says; a conditional branch tests its condition as
// ConditionalBranchTable says; the rest are bodies of their own (specialOperation()).

namespace isoline::fastinterpreter
{

/// The typed kernel of a numeric instruction (numerics/), the same the reference interpreter runs: a
/// function of one or two boxed operands (runtime::I32 and the rest) to a boxed result, a truth value,
/// or a std::expected of either and the trap. nullptr for an instruction that is not numeric.
template <binary::Opcode>
inline constexpr auto ScalarKernel = nullptr;

// clang-format off
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Eqz> = &numerics::eqz<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Eq> = &numerics::eq<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Ne> = &numerics::ne<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32LtS> = &numerics::ltS<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32LtU> = &numerics::ltU<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32GtS> = &numerics::gtS<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32GtU> = &numerics::gtU<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32LeS> = &numerics::leS<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32LeU> = &numerics::leU<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32GeS> = &numerics::geS<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32GeU> = &numerics::geU<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Clz> = &numerics::clz<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Ctz> = &numerics::ctz<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Popcnt> = &numerics::popcnt<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Add> = &numerics::add<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Sub> = &numerics::sub<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Mul> = &numerics::mul<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32DivS> = &numerics::divS<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32DivU> = &numerics::divU<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32RemS> = &numerics::remS<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32RemU> = &numerics::remU<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32And> = &numerics::bitAnd<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Or> = &numerics::bitOr<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Xor> = &numerics::bitXor<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Shl> = &numerics::shl<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32ShrS> = &numerics::shrS<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32ShrU> = &numerics::shrU<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Rotl> = &numerics::rotl<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Rotr> = &numerics::rotr<runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Extend8S> = &numerics::extendSigned<runtime::I32, int8_t>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32Extend16S> = &numerics::extendSigned<runtime::I32, int16_t>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Eqz> = &numerics::eqz<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Eq> = &numerics::eq<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Ne> = &numerics::ne<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64LtS> = &numerics::ltS<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64LtU> = &numerics::ltU<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64GtS> = &numerics::gtS<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64GtU> = &numerics::gtU<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64LeS> = &numerics::leS<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64LeU> = &numerics::leU<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64GeS> = &numerics::geS<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64GeU> = &numerics::geU<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Clz> = &numerics::clz<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Ctz> = &numerics::ctz<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Popcnt> = &numerics::popcnt<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Add> = &numerics::add<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Sub> = &numerics::sub<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Mul> = &numerics::mul<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64DivS> = &numerics::divS<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64DivU> = &numerics::divU<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64RemS> = &numerics::remS<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64RemU> = &numerics::remU<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64And> = &numerics::bitAnd<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Or> = &numerics::bitOr<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Xor> = &numerics::bitXor<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Shl> = &numerics::shl<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64ShrS> = &numerics::shrS<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64ShrU> = &numerics::shrU<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Rotl> = &numerics::rotl<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Rotr> = &numerics::rotr<runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Extend8S> = &numerics::extendSigned<runtime::I64, int8_t>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Extend16S> = &numerics::extendSigned<runtime::I64, int16_t>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64Extend32S> = &numerics::extendSigned<runtime::I64, int32_t>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Eq> = &numerics::feq<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Ne> = &numerics::fne<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Lt> = &numerics::flt<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Gt> = &numerics::fgt<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Le> = &numerics::fle<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Ge> = &numerics::fge<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Abs> = &numerics::fabs<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Neg> = &numerics::fneg<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Ceil> = &numerics::ceil<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Floor> = &numerics::floor<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Trunc> = &numerics::trunc<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Nearest> = &numerics::nearest<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Sqrt> = &numerics::sqrt<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Add> = &numerics::fadd<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Sub> = &numerics::fsub<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Mul> = &numerics::fmul<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Div> = &numerics::fdiv<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Min> = &numerics::fmin<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Max> = &numerics::fmax<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32Copysign> = &numerics::copysign<runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Eq> = &numerics::feq<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Ne> = &numerics::fne<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Lt> = &numerics::flt<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Gt> = &numerics::fgt<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Le> = &numerics::fle<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Ge> = &numerics::fge<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Abs> = &numerics::fabs<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Neg> = &numerics::fneg<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Ceil> = &numerics::ceil<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Floor> = &numerics::floor<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Trunc> = &numerics::trunc<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Nearest> = &numerics::nearest<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Sqrt> = &numerics::sqrt<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Add> = &numerics::fadd<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Sub> = &numerics::fsub<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Mul> = &numerics::fmul<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Div> = &numerics::fdiv<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Min> = &numerics::fmin<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Max> = &numerics::fmax<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64Copysign> = &numerics::copysign<runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32WrapI64> = &numerics::wrap;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32TruncF32S> = &numerics::truncTrapping<runtime::I32, int32_t, runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32TruncF32U> = &numerics::truncTrapping<runtime::I32, uint32_t, runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32TruncF64S> = &numerics::truncTrapping<runtime::I32, int32_t, runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32TruncF64U> = &numerics::truncTrapping<runtime::I32, uint32_t, runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64ExtendI32S> = &numerics::extendS;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64ExtendI32U> = &numerics::extendU;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64TruncF32S> = &numerics::truncTrapping<runtime::I64, int64_t, runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64TruncF32U> = &numerics::truncTrapping<runtime::I64, uint64_t, runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64TruncF64S> = &numerics::truncTrapping<runtime::I64, int64_t, runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64TruncF64U> = &numerics::truncTrapping<runtime::I64, uint64_t, runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32ConvertI32S> = &numerics::convert<runtime::F32, int32_t, runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32ConvertI32U> = &numerics::convert<runtime::F32, uint32_t, runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32ConvertI64S> = &numerics::convert<runtime::F32, int64_t, runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32ConvertI64U> = &numerics::convert<runtime::F32, uint64_t, runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32DemoteF64> = &numerics::demote;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64ConvertI32S> = &numerics::convert<runtime::F64, int32_t, runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64ConvertI32U> = &numerics::convert<runtime::F64, uint32_t, runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64ConvertI64S> = &numerics::convert<runtime::F64, int64_t, runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64ConvertI64U> = &numerics::convert<runtime::F64, uint64_t, runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64PromoteF32> = &numerics::promote;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32ReinterpretF32> = &numerics::reinterpret<runtime::I32, runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64ReinterpretF64> = &numerics::reinterpret<runtime::I64, runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F32ReinterpretI32> = &numerics::reinterpret<runtime::F32, runtime::I32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::F64ReinterpretI64> = &numerics::reinterpret<runtime::F64, runtime::I64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32TruncSatF32S> = &numerics::truncSaturating<runtime::I32, int32_t, runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32TruncSatF32U> = &numerics::truncSaturating<runtime::I32, uint32_t, runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32TruncSatF64S> = &numerics::truncSaturating<runtime::I32, int32_t, runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I32TruncSatF64U> = &numerics::truncSaturating<runtime::I32, uint32_t, runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64TruncSatF32S> = &numerics::truncSaturating<runtime::I64, int64_t, runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64TruncSatF32U> = &numerics::truncSaturating<runtime::I64, uint64_t, runtime::F32>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64TruncSatF64S> = &numerics::truncSaturating<runtime::I64, int64_t, runtime::F64>;
template <>
inline constexpr auto ScalarKernel<binary::Opcode::I64TruncSatF64U> = &numerics::truncSaturating<runtime::I64, uint64_t, runtime::F64>;
// clang-format on

namespace detail
{
    /// The operand and result types of a kernel.
    template <typename>
    struct KernelSignature;

    template <typename Result, typename Operand>
    struct KernelSignature<Result (*)(Operand) noexcept>
    {
        using Left = Operand;
        using Right = Operand;
        static constexpr auto Arity = 1;
    };

    template <typename Result, typename LeftOperand, typename RightOperand>
    struct KernelSignature<Result (*)(LeftOperand, RightOperand) noexcept>
    {
        using Left = LeftOperand;
        using Right = RightOperand;
        static constexpr auto Arity = 2;
    };

    /// The signature of @p Kernel.
    template <auto Kernel>
    using SignatureOf = KernelSignature<std::remove_cv_t<decltype(Kernel)>>;

    /// Whether @p T is a std::expected: a kernel that may trap.
    template <typename T>
    inline constexpr auto IsExpected = false;

    template <typename T, typename E>
    inline constexpr auto IsExpected<std::expected<T, E>> = true;

    /// The boxed value of type @p Box a slot holds: a 32-bit value from the low half of its slot, where it
    /// is stored zero-extended, an immediate included.
    template <typename Box>
    [[nodiscard]] constexpr Box decode(Word word) noexcept
    {
        return Box { static_cast<decltype(Box {}.bits)>(word) };
    }

    /// The slot holding @p value: a 32-bit value zero-extended, a truth value as the i32 0 or 1.
    template <typename Box>
    [[nodiscard]] constexpr Slot encode(Box value) noexcept
    {
        if constexpr (std::is_same_v<Box, bool>)
            return Slot { value ? 1U : 0U };
        else
            return Slot { value.bits };
    }

    /// For operations fused or folded from an instruction, LoweringRows and the tables after it: an index
    /// past the end when no row holds the operation.
    inline constexpr auto NoRow = std::size_t { 0xFFFF'FFFF };

    /// The instruction whose Unary or Binary row lowers to @p id, or whose BinaryImmediate row folds into
    /// it: the instruction whose kernel it runs.
    [[nodiscard]] consteval std::size_t numericRowOf(OperationId id) noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, LoweringRows.size()))
        {
            auto const& row = LoweringRows[index];
            auto const numeric = row.form == OperationForm::Unary || row.form == OperationForm::Binary
                                 || row.form == OperationForm::BinaryImmediate;
            if (numeric && (row.operation == id || row.immediate == id))
                return index;
        }
        return NoRow;
    }

    /// The Load or Store row of LoweringRows whose memory 0 operations @p id is one of, or NoRow.
    [[nodiscard]] consteval std::size_t memoryRowOf(OperationId id) noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, LoweringRows.size()))
        {
            auto const& row = LoweringRows[index];
            auto const memory = row.form == OperationForm::Load || row.form == OperationForm::Store;
            if (memory && (row.operation == id || row.memory64 == id))
                return index;
        }
        return NoRow;
    }

    /// The FusionTable row whose comparison and branch fuse into @p id, or NoRow.
    [[nodiscard]] consteval std::size_t fusionRowOf(OperationId id) noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, FusionTable.size()))
        {
            auto const& row = FusionTable[index];
            if (row.fused == id || row.fusedLoop == id || row.immediate == id || row.immediateLoop == id)
                return index;
        }
        return NoRow;
    }

    /// The ConditionalBranchTable row with @p id among its operations, or NoRow.
    [[nodiscard]] consteval std::size_t conditionalBranchRowOf(OperationId id) noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, ConditionalBranchTable.size()))
        {
            auto const& row = ConditionalBranchTable[index];
            if (row.branch == id || row.branchLoop == id || row.move == id || row.moveLoop == id)
                return index;
        }
        return NoRow;
    }

    /// The CallTable row of @p id, or NoRow.
    [[nodiscard]] consteval std::size_t callRowOf(OperationId id) noexcept
    {
        auto const row = std::ranges::find(CallTable, id, &CallRow::operation);
        return row == CallTable.end() ? NoRow : static_cast<std::size_t>(row - CallTable.begin());
    }

    /// Whether every operation of the Call form has its row of CallTable, which its body runs by. A loop, not
    /// std::ranges::all_of: MSVC calls no immediate function (callRowOf()) from a lambda (C7595).
    [[nodiscard]] consteval bool everyCallHasARow() noexcept
    {
        for (auto const& row: OperationTable)
            if (row.form == OperationForm::Call && callRowOf(row.id) == NoRow)
                return false;
        return true;
    }

    static_assert(everyCallHasARow(), "a call operation without its CallTable row has no body");

    /// False, for an operation: what a static_assert in a branch no operation reaches tests.
    template <OperationId>
    inline constexpr auto NoOperation = false;
} // namespace detail

/// The 32-bit slot operand @p Index of the operation at @p pc: they follow the header two to a word, the
/// first in the low half.
template <uint32_t Index>
[[nodiscard]] inline uint32_t slotOperand(Word const* pc) noexcept
{
    auto const word = pc[1 + (Index / 2)];
    if constexpr (Index % 2 == 0)
        return static_cast<uint32_t>(word);
    else
        return static_cast<uint32_t>(word >> 32U);
}

/// The immediate @p Index of operation @p Id at @p pc: they follow its slot operands, one word each.
template <OperationId Id, uint32_t Index>
[[nodiscard]] inline Word immediateOperand(Word const* pc) noexcept
{
    constexpr auto Row = operationRow(Id);
    static_assert(Index < Row.immediates);
    return pc[1 + ((Row.slots + 1) / 2) + Index];
}

/// Goes on with the operation after @p Id's.
template <OperationId Id>
[[nodiscard]] inline Flow next(State& state) noexcept
{
    state.pc += operationRow(Id).words;
    return Flow::Continue;
}

/// Polls at a loop back-edge, as every transfer to a loop label does.
/// @return Flow::Continue to go on, or what leaving with the poll's trap gives.
[[nodiscard]] inline Flow pollBackEdge(State& state) noexcept
{
    if (auto const polled = state.context->gate.poll(runtime::PollSite::LoopBackEdge, state.stepsLeft);
        !polled) [[unlikely]]
        return leaveTrapped(*state.context, state.stepsLeft, polled.error());
    return Flow::Continue;
}

/// Jumps to @p target, a branch's target word (BranchTargetMask), polling first where @p Poll says so.
template <LoopPoll Poll>
[[nodiscard]] inline Flow branchTo(State& state, Word target) noexcept
{
    if constexpr (Poll == LoopPoll::Yes)
    {
        if (auto const polled = pollBackEdge(state); polled != Flow::Continue) [[unlikely]]
            return polled;
    }
    state.pc = state.context->code + (target & BranchTargetMask);
    return Flow::Continue;
}

/// Moves @p count slots from @p source to @p destination, which may overlap: a label's values, which
/// branches move down to the label's slots.
inline void moveSlots(Slot* fp, uint32_t destination, uint32_t source, uint64_t count) noexcept
{
    std::memmove(fp + destination, fp + source, count * sizeof(Slot));
}

/// Writes what @p Kernel gives for @p operands to slot @p destination, and goes on; or leaves with its trap.
template <OperationId Id, auto Kernel, typename... Operands>
[[nodiscard]] inline Flow applyKernel(State& state, uint32_t destination, Operands... operands) noexcept
{
    auto const result = Kernel(operands...);
    if constexpr (detail::IsExpected<std::remove_cv_t<decltype(result)>>)
    {
        if (!result) [[unlikely]]
            return leaveTrapped(*state.context, state.stepsLeft, result.error());
        state.fp[destination] = detail::encode(*result);
        return next<Id>(state);
    }
    else
    {
        state.fp[destination] = detail::encode(result);
        return next<Id>(state);
    }
}

/// A numeric operation: its instruction's kernel over its operand slots, or over its left operand's slot and
/// its immediate, into its destination slot.
template <OperationId Id>
[[nodiscard]] inline Flow numericOperation(State& state) noexcept
{
    constexpr auto RowIndex = detail::numericRowOf(Id);
    static_assert(RowIndex != detail::NoRow, "a numeric operation is some instruction's lowering");
    constexpr auto Kernel = ScalarKernel<LoweringRows[RowIndex].opcode>;
    using Signature = detail::SignatureOf<Kernel>;
    constexpr auto Form = operationRow(Id).form;
    auto const* fp = state.fp;
    auto const destination = slotOperand<0>(state.pc);
    auto const left = detail::decode<typename Signature::Left>(fp[slotOperand<1>(state.pc)]);
    if constexpr (Form == OperationForm::Unary)
        return applyKernel<Id, Kernel>(state, destination, left);
    else if constexpr (Form == OperationForm::Binary)
        return applyKernel<Id, Kernel>(
            state,
            destination,
            left,
            detail::decode<typename Signature::Right>(fp[slotOperand<2>(state.pc)]));
    else
        return applyKernel<Id, Kernel>(
            state,
            destination,
            left,
            detail::decode<typename Signature::Right>(immediateOperand<Id, 0>(state.pc)));
}

/// The unsigned integer of @p Width bytes: what a memory access of that width moves.
template <uint32_t Width>
using MemoryWord = std::conditional_t<
    Width == 1,
    uint8_t,
    std::conditional_t<Width == 2, uint16_t, std::conditional_t<Width == 4, uint32_t, uint64_t>>>;

/// The Spectre mask of an access of @p width bytes at offset @p effective into a memory of @p size bytes:
/// all ones where it fits, zero where it does not, computed without a branch (docs/fast-interpreter.md),
/// so that code running ahead of a mispredicted bounds check reads memory's first bytes, not past them.
///
/// It is the sign of the bytes left after the access, or of the offset itself: an offset of 2^63 or more
/// never fits (a memory is allocated, so its size stays below 2^63), and without its own sign the bytes
/// left would wrap to a positive count for it, letting a guest choose a speculative read below the base.
///
/// One case it cannot catch: a memory smaller than the access (@p size below @p width), whose bytes left
/// wrap for every small offset. Code ahead of the check may then read fewer than @p width bytes past such a
/// memory's end; the check itself traps.
/// @param effective The offset of the access's first byte: the address plus the static offset.
/// @param size The memory's size in bytes.
/// @param width The bytes accessed.
/// @return The mask.
[[nodiscard]] constexpr uint64_t accessMask(uint64_t effective, uint64_t size, uint64_t width) noexcept
{
    auto const left = static_cast<int64_t>(size - width - effective);
    return ~static_cast<uint64_t>((left | static_cast<int64_t>(effective)) >> 63U);
}

/// The offset into a memory of @p size bytes of an access of @p Width bytes at @p address + @p offset, or
/// nullopt where it does not fit, as the reference interpreter decides it; never overflowing.
///
/// A memory of i32 addresses has an address and an offset below 2^32 (validation), so their sum and the
/// width fit 64 bits and one comparison decides. A memory of i64 addresses (memory64) adds the two with a
/// check for the carry, and compares the width against what is left.
///
/// Spectre (docs/fast-interpreter.md): the offset returned is masked by accessMask(), a data-dependent
/// select computed from the same bound as the check but not from its outcome, which the compiler knows on
/// the path the branch takes and would fold away.
template <binary::AddressType Addressing, uint32_t Width>
[[nodiscard]] inline std::optional<uint64_t> boundsChecked(uint64_t address,
                                                           uint64_t offset,
                                                           uint64_t size) noexcept
{
    auto const effective = address + offset;
    auto fits = false;
    if constexpr (Addressing == binary::AddressType::I32)
        fits = effective + Width <= size;
    else
        fits = effective >= address && size >= Width && effective <= size - Width;
    if (!fits) [[unlikely]]
        return std::nullopt;
    return effective & accessMask(effective, size, Width);
}

/// The access of memory 0 operation @p Id (a Load or Store row's) to the memory @p base of @p size bytes:
/// a load writes the value it reads, widened as binary::MemoryAccessTable says, to slot operand 0; a store
/// writes the low bytes of slot operand 1. The pc stays at the operation.
/// @param offset The access's static offset.
/// @return Flow::Continue, or what leaving with OutOfBoundsMemoryAccess gives.
template <OperationId Id>
[[nodiscard]] inline Flow accessMemory(State& state, std::byte* base, uint64_t size, uint64_t offset) noexcept
{
    constexpr auto Row = LoweringRows[detail::memoryRowOf(Id)];
    constexpr auto Access = *binary::memoryAccessOf(Row.opcode);
    constexpr auto Addressing = Id == Row.memory64 ? binary::AddressType::I64 : binary::AddressType::I32;
    constexpr auto Width = uint32_t { Access.width };
    using Moved = MemoryWord<Width>;
    auto* const fp = state.fp;
    // An i32 address is held zero-extended in its slot (detail::encode()), and read as 32 bits all the same:
    // the bounds check's single comparison is exact only below 2^32, so no slot a producer left with high
    // bits set may reach it.
    auto const slot = fp[slotOperand<Access.access == binary::MemoryAccessKind::Load ? 1 : 0>(state.pc)];
    auto address = slot;
    if constexpr (Addressing == binary::AddressType::I32)
        address = static_cast<uint32_t>(slot);
    auto const checked = boundsChecked<Addressing, Width>(address, offset, size);
    if (!checked) [[unlikely]]
        return leaveTrapped(
            *state.context, state.stepsLeft, runtime::Trap { runtime::TrapCode::OutOfBoundsMemoryAccess });
    auto* const bytes = base + *checked;
    if constexpr (Access.access == binary::MemoryAccessKind::Store)
        storeLittleEndian<Moved>(bytes, static_cast<Moved>(fp[slotOperand<1>(state.pc)]));
    else
    {
        auto const loaded = loadLittleEndian<Moved>(bytes);
        auto value = uint64_t { loaded };
        if constexpr (Access.extension == binary::Extension::Sign)
            value =
                static_cast<uint64_t>(static_cast<int64_t>(static_cast<std::make_signed_t<Moved>>(loaded)));
        // A 32-bit value is held zero-extended.
        if constexpr (Access.value == binary::ValueKind::I32 || Access.value == binary::ValueKind::F32)
            value = static_cast<uint32_t>(value);
        fp[slotOperand<0>(state.pc)] = value;
    }
    return Flow::Continue;
}

/// A load or store of memory 0 (a Load or Store row's operation): its access to the registers' memory 0.
template <OperationId Id>
[[nodiscard]] inline Flow memoryOperation(State& state) noexcept
{
    if (auto const flow =
            accessMemory<Id>(state, state.memory0, state.memory0Size, immediateOperand<Id, 0>(state.pc));
        flow != Flow::Continue) [[unlikely]]
        return flow;
    return next<Id>(state);
}

namespace detail
{
    /// An access of a memory other than memory 0, by the memory 0 operation whose body it runs.
    using IndexedAccessFunction = Flow (*)(State&, std::byte*, uint64_t, uint64_t) noexcept;

    /// accessMemory() of every memory 0 operation, by OperationId; null for every other operation.
    [[nodiscard]] consteval std::array<IndexedAccessFunction, OperationCount>
    indexedAccessFunctions() noexcept
    {
        return []<std::size_t... Index>(std::index_sequence<Index...>) {
            return std::array<IndexedAccessFunction, OperationCount> { [] {
                // No named constant for the id: MSVC takes one the else branch does not read for unused
                // (C4189).
                if constexpr (memoryRowOf(static_cast<OperationId>(Index)) != NoRow)
                    return IndexedAccessFunction { &accessMemory<static_cast<OperationId>(Index)> };
                else
                    return IndexedAccessFunction { nullptr };
            }()... };
        }(std::make_index_sequence<OperationCount> {});
    }

    /// indexedAccessFunctions(), built once.
    inline constexpr auto IndexedAccessFunctions = indexedAccessFunctions();
} // namespace detail

/// A load or store of a memory other than memory 0 (LoadIndexed, StoreIndexed): its memory 0 twin's access,
/// to the memory's bytes as the store holds them now.
template <OperationId Id>
[[nodiscard]] inline Flow indexedMemoryOperation(State& state) noexcept
{
    auto const named = indexedAccessOf(immediateOperand<Id, 1>(state.pc));
    auto const bytes = state.context->store.memory(state.context->instance->memories[named.memory]).bytes();
    auto const access = detail::IndexedAccessFunctions[std::to_underlying(named.access)];
    if (auto const flow = access(state, bytes.data(), bytes.size(), immediateOperand<Id, 0>(state.pc));
        flow != Flow::Continue) [[unlikely]]
        return flow;
    return next<Id>(state);
}

/// A comparison fused with the br_if or if that consumes its result (FusionTable): br_if branches when the
/// comparison holds, if continues at its target when it does not; to a loop label it polls first.
template <OperationId Id>
[[nodiscard]] inline Flow fusedBranch(State& state) noexcept
{
    constexpr auto Fusion = FusionTable[detail::fusionRowOf(Id)];
    constexpr auto Kernel = ScalarKernel<Fusion.compare>;
    using Signature = detail::SignatureOf<Kernel>;
    constexpr auto Row = operationRow(Id);
    constexpr auto Loop =
        Fusion.branch == binary::Opcode::BrIf && (Id == Fusion.fusedLoop || Id == Fusion.immediateLoop)
            ? LoopPoll::Yes
            : LoopPoll::No;
    auto const left = detail::decode<typename Signature::Left>(state.fp[slotOperand<0>(state.pc)]);
    auto holds = false;
    if constexpr (Signature::Arity == 1)
        holds = Kernel(left);
    else if constexpr (Row.slots == 2)
        holds = Kernel(left, detail::decode<typename Signature::Right>(state.fp[slotOperand<1>(state.pc)]));
    else
        holds = Kernel(left, detail::decode<typename Signature::Right>(immediateOperand<Id, 0>(state.pc)));
    constexpr auto BranchesWhen = Fusion.branch == binary::Opcode::BrIf;
    if (holds == BranchesWhen)
        return branchTo<Loop>(state, immediateOperand<Id, Row.immediates - 1>(state.pc));
    return next<Id>(state);
}

/// br_if, br_on_null and br_on_non_null (ConditionalBranchTable): when its condition holds it moves the
/// label's values where it has to, polls to a loop label, and branches.
template <OperationId Id>
[[nodiscard]] inline Flow conditionalBranch(State& state) noexcept
{
    constexpr auto Branch = ConditionalBranchTable[detail::conditionalBranchRowOf(Id)];
    constexpr auto Loop = Id == Branch.branchLoop || Id == Branch.moveLoop ? LoopPoll::Yes : LoopPoll::No;
    constexpr auto Moves = Id == Branch.move || Id == Branch.moveLoop;
    auto const condition = state.fp[slotOperand<0>(state.pc)];
    auto holds = false;
    if constexpr (Branch.opcode == binary::Opcode::BrIf)
        holds = static_cast<uint32_t>(condition) != 0;
    else if constexpr (Branch.opcode == binary::Opcode::BrOnNull)
        holds = isNullReference(condition);
    else
        holds = !isNullReference(condition);
    if (!holds)
        return next<Id>(state);
    auto const target = immediateOperand<Id, 0>(state.pc);
    if constexpr (Moves)
        moveSlots(state.fp, slotOperand<1>(state.pc), slotOperand<2>(state.pc), target >> 32U);
    return branchTo<Loop>(state, target);
}

/// br_table: the index chooses a label's record, the default's for an index past the others; the record
/// names the target, where the values go, and whether the label is a loop's.
[[nodiscard]] inline Flow branchTable(State& state) noexcept
{
    constexpr auto Id = OperationId::BrTable;
    constexpr auto Words = operationRow(Id).words;
    auto const index = static_cast<uint32_t>(state.fp[slotOperand<0>(state.pc)]);
    auto const source = slotOperand<1>(state.pc);
    auto const count = immediateOperand<Id, 0>(state.pc);
    auto const labels = immediateOperand<Id, 1>(state.pc);
    auto const* record = state.pc + Words + (std::min<uint64_t>(index, labels) * BranchTableEntryWords);
    auto const destination = static_cast<uint32_t>(record[0] >> 32U);
    if (count > 0 && destination != source)
        moveSlots(state.fp, destination, source, count);
    if (static_cast<LoopPoll>(record[1]) == LoopPoll::Yes)
        return branchTo<LoopPoll::Yes>(state, record[0]);
    return branchTo<LoopPoll::No>(state, record[0]);
}

/// A call (CallTable): names the callee as its row says, and calls it out of line (callFunction()), which
/// leaves the registers at the callee's first operation, or after the call once a host function returned.
template <OperationId Id>
[[nodiscard]] inline Flow callOperation(State& state) noexcept
{
    constexpr auto Row = CallTable[detail::callRowOf(Id)];
    auto site = CallSite { .callee = Row.callee,
                           .mode = Row.mode,
                           .words = operationRow(Id).words,
                           .argumentSlot = slotOperand<0>(state.pc),
                           .operandSlot = 0,
                           .index = immediateOperand<Id, 0>(state.pc),
                           .table = 0 };
    if constexpr (callSlots(Row.callee) == 2)
        site.operandSlot = slotOperand<1>(state.pc);
    if constexpr (callImmediates(Row.callee) == 2)
        site.table = immediateOperand<Id, 1>(state.pc);
    auto const step = callFunction(state, site);
    state = step.state;
    return step.flow;
}

/// The operations of a body of their own: control without operands, moves, constants, selects, globals,
/// the accesses of memories other than memory 0, memory.size and memory.grow, unconditional branches, if,
/// br_table and return.
template <OperationId Id>
[[nodiscard]] inline Flow specialOperation(State& state) noexcept
{
    auto* fp = state.fp;
    auto const* pc = state.pc;
    if constexpr (Id == OperationId::Unreachable)
        return leaveTrapped(
            *state.context, state.stepsLeft, runtime::Trap { runtime::TrapCode::Unreachable });
    else if constexpr (Id == OperationId::Nop)
        return next<Id>(state);
    else if constexpr (Id == OperationId::Move64)
    {
        fp[slotOperand<0>(pc)] = fp[slotOperand<1>(pc)];
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::Move128)
    {
        moveSlots(fp, slotOperand<0>(pc), slotOperand<1>(pc), 2);
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::MoveRange)
    {
        moveSlots(fp, slotOperand<0>(pc), slotOperand<1>(pc), immediateOperand<Id, 0>(pc));
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::Const64)
    {
        fp[slotOperand<0>(pc)] = immediateOperand<Id, 0>(pc);
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::Const128)
    {
        auto const destination = slotOperand<0>(pc);
        fp[destination] = immediateOperand<Id, 0>(pc);
        fp[destination + 1] = immediateOperand<Id, 1>(pc);
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::Return)
    {
        auto const step = returnFromFunction(state, slotOperand<0>(pc));
        state = step.state;
        return step.flow;
    }
    else if constexpr (Id == OperationId::GlobalGet)
    {
        auto const& global = state.context->store.global(
            state.context->instance->globals[static_cast<std::size_t>(immediateOperand<Id, 0>(pc))]);
        // A global's cell holds the value's words in the slots' own encoding (runtime::encodeWords()).
        auto const width = slotWidth(slotClassOf(global.type.type));
        std::ranges::copy(std::span { global.cell.words }.first(width), fp + slotOperand<0>(pc));
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::GlobalSet)
    {
        auto& global = state.context->store.global(
            state.context->instance->globals[static_cast<std::size_t>(immediateOperand<Id, 0>(pc))]);
        // A scalar takes one slot and leaves the cell's second word 0, as the encoding has it.
        auto const width = slotWidth(slotClassOf(global.type.type));
        global.cell.words = { fp[slotOperand<0>(pc)], width == 2 ? fp[slotOperand<0>(pc) + 1] : Slot { 0 } };
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::LoadIndexed || Id == OperationId::StoreIndexed)
        return indexedMemoryOperation<Id>(state);
    else if constexpr (Id == OperationId::MemorySize)
    {
        auto const& memory = state.context->store.memory(
            state.context->instance->memories[static_cast<std::size_t>(immediateOperand<Id, 0>(pc))]);
        // Pages of a memory of i32 addresses number below 2^32, so the slot holds them zero-extended.
        fp[slotOperand<0>(pc)] = std::as_const(memory).bytes().size() / binary::pageSize(memory.type);
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::MemoryGrow)
    {
        auto const step = growMemory(state,
                                     slotOperand<0>(pc),
                                     slotOperand<1>(pc),
                                     static_cast<uint32_t>(immediateOperand<Id, 0>(pc)));
        state = step.state;
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::Select64 || Id == OperationId::Select128)
    {
        // The result goes to the first value's slot, so both values are read before it is written.
        constexpr auto Width = Id == OperationId::Select64 ? 1U : 2U;
        auto const chosen =
            static_cast<uint32_t>(fp[slotOperand<3>(pc)]) != 0 ? slotOperand<1>(pc) : slotOperand<2>(pc);
        moveSlots(fp, slotOperand<0>(pc), chosen, Width);
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::Jump)
        return branchTo<LoopPoll::No>(state, immediateOperand<Id, 0>(pc));
    else if constexpr (Id == OperationId::JumpLoop)
        return branchTo<LoopPoll::Yes>(state, immediateOperand<Id, 0>(pc));
    else if constexpr (Id == OperationId::If)
    {
        if (static_cast<uint32_t>(fp[slotOperand<0>(pc)]) == 0)
            return branchTo<LoopPoll::No>(state, immediateOperand<Id, 0>(pc));
        return next<Id>(state);
    }
    else if constexpr (Id == OperationId::BrTable)
        return branchTable(state);
    else
        static_assert(detail::NoOperation<Id>, "every operation of Operations.def has a body");
}

/// Runs operation @p Id at State::pc: its body, chosen by its row.
/// @param state The registers; pc is left at the operation that runs next.
/// @return Flow::Continue to go on, or how execution stops.
template <OperationId Id>
[[nodiscard]] inline Flow operation(State& state) noexcept
{
    if constexpr (operationRow(Id).family == OperationFamily::Numeric)
        return numericOperation<Id>(state);
    else if constexpr (operationRow(Id).family == OperationFamily::Memory)
        return memoryOperation<Id>(state);
    else if constexpr (detail::fusionRowOf(Id) != detail::NoRow)
        return fusedBranch<Id>(state);
    else if constexpr (detail::conditionalBranchRowOf(Id) != detail::NoRow)
        return conditionalBranch<Id>(state);
    else if constexpr (detail::callRowOf(Id) != detail::NoRow)
        return callOperation<Id>(state);
    else
        return specialOperation<Id>(state);
}

} // namespace isoline::fastinterpreter
