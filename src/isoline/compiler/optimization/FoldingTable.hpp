// SPDX-License-Identifier: Apache-2.0
#pragma once

// What constant folding evaluates, as data: one row per foldable operation, naming the numerics:: kernel
// both interpreters evaluate it with (interpreter/Numerics.cpp's NumericTable), through an adapter over
// operand bits. A folded value is therefore bit-identical to the interpreters', canonical NaNs included,
// and a kernel's trap is the trap they raise: a zero divisor before a signed overflow, a NaN before an
// out-of-range truncation. Adding a numeric operation to Operations.def without a row here fails to
// compile (the coverage static_assert below).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/numerics/CanonicalNan.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/numerics/Integer.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::compiler::optimization
{

/// What a kernel computes: the result's bits (an integer's, zero-extended; a float's pattern), or the trap.
using FoldResult = std::expected<uint64_t, runtime::TrapCode>;

/// A folding kernel: the result of an operation on constant operands.
/// @param operands The operands' bits, in order.
/// @param type The instruction's result type; only canonicalizeNan reads it.
using FoldKernel = FoldResult (*)(std::span<uint64_t const> operands, intermediate::Type type) noexcept;

/// One foldable operation and its kernel.
struct FoldingRow
{
    intermediate::Operation operation; ///< The operation.
    FoldKernel kernel;                 ///< What it computes.
};

/// Whether constant folding evaluates @p row's operation: it is Pure or MayTrap, takes a fixed list of
/// numeric operands and no immediates, and has one numeric result. Every WebAssembly numeric instruction is,
/// and canonicalizeNan; select (any type), the guards and the memory operations are not.
[[nodiscard]] constexpr bool isFoldable(intermediate::OperationRow const& row) noexcept
{
    using intermediate::OperandType;
    using intermediate::ResultRule;
    auto const effect = row.effect;
    if (effect != intermediate::Effect::Pure && effect != intermediate::Effect::MayTrap)
        return false;
    if (row.immediates != intermediate::ImmediateSchema::None)
        return false;
    auto const& operands = intermediate::operandSignatureRow(row.operands);
    if (operands.tail != intermediate::OperandTail::Fixed || operands.count == 0)
        return false;
    for (auto const type: operands.fixed())
        if (type != OperandType::I32 && type != OperandType::I64 && type != OperandType::F32
            && type != OperandType::F64 && type != OperandType::Float)
            return false;
    switch (row.result)
    {
        case ResultRule::I32:
        case ResultRule::I64:
        case ResultRule::F32:
        case ResultRule::F64:
        case ResultRule::SameAsOperand0: return true;
        case ResultRule::None:
        case ResultRule::Ref:
        case ResultRule::Pointer:
        case ResultRule::FromImmediate:
        case ResultRule::Multiple: return false;
    }
    return false;
}

namespace detail
{
    using runtime::F32;
    using runtime::F64;
    using runtime::I32;
    using runtime::I64;

    /// The value of type @p Box whose bits are @p bits' low ones.
    template <typename Box>
    [[nodiscard]] constexpr Box boxOf(uint64_t bits) noexcept
    {
        return Box { static_cast<decltype(Box::bits)>(bits) };
    }

    /// The bits of @p value, zero-extended.
    template <typename Box>
    [[nodiscard]] constexpr uint64_t bitsOf(Box value) noexcept
    {
        return uint64_t { value.bits };
    }

    /// The bits of @p result, or its trap's code.
    template <typename Box>
    [[nodiscard]] constexpr FoldResult resultOf(Box value) noexcept
    {
        return bitsOf(value);
    }

    /// @copydoc resultOf(Box)
    template <typename Box>
    [[nodiscard]] constexpr FoldResult resultOf(std::expected<Box, runtime::Trap> const& value) noexcept
    {
        if (!value)
            return std::unexpected { value.error().code };
        return bitsOf(*value);
    }

    /// The operand types of a kernel.
    template <typename>
    struct Signature;

    template <typename R, typename A>
    struct Signature<R (*)(A) noexcept>
    {
        using Arg = A;
    };

    template <typename R, typename A, typename B>
    struct Signature<R (*)(A, B) noexcept>
    {
        using Lhs = A;
        using Rhs = B;
    };

    /// A unary kernel's adapter: a value of its operand's type in, a value or a trap out.
    template <auto Kernel>
    [[nodiscard]] FoldResult unary(std::span<uint64_t const> operands, intermediate::Type /*type*/) noexcept
    {
        return resultOf(Kernel(boxOf<typename Signature<decltype(Kernel)>::Arg>(operands[0])));
    }

    /// A binary kernel's adapter.
    template <auto Kernel>
    [[nodiscard]] FoldResult binary(std::span<uint64_t const> operands, intermediate::Type /*type*/) noexcept
    {
        using Types = Signature<decltype(Kernel)>;
        return resultOf(
            Kernel(boxOf<typename Types::Lhs>(operands[0]), boxOf<typename Types::Rhs>(operands[1])));
    }

    /// A test's adapter: its truth as the I32 1 or 0.
    template <auto Kernel>
    [[nodiscard]] FoldResult test(std::span<uint64_t const> operands, intermediate::Type /*type*/) noexcept
    {
        return Kernel(boxOf<typename Signature<decltype(Kernel)>::Arg>(operands[0])) ? 1U : 0U;
    }

    /// A comparison's adapter: its truth as the I32 1 or 0.
    template <auto Kernel>
    [[nodiscard]] FoldResult compare(std::span<uint64_t const> operands, intermediate::Type /*type*/) noexcept
    {
        using Types = Signature<decltype(Kernel)>;
        return Kernel(boxOf<typename Types::Lhs>(operands[0]), boxOf<typename Types::Rhs>(operands[1])) ? 1U
                                                                                                        : 0U;
    }

    /// canonicalizeNan: the canonical NaN of the type for a NaN, every other value as it is.
    [[nodiscard]] inline FoldResult canonicalizeNan(std::span<uint64_t const> operands,
                                                    intermediate::Type type) noexcept
    {
        if (type == intermediate::Type::F32)
            return numerics::isNan(boxOf<F32>(operands[0])) ? uint64_t { numerics::CanonicalNanF32 }
                                                            : operands[0];
        return numerics::isNan(boxOf<F64>(operands[0])) ? numerics::CanonicalNanF64 : operands[0];
    }

    /// A row of @p operation folded by @p Adapter.
    template <FoldKernel Adapter>
    [[nodiscard]] consteval FoldingRow row(intermediate::Operation operation) noexcept
    {
        return FoldingRow { .operation = operation, .kernel = Adapter };
    }
} // namespace detail

/// Every foldable operation, in Operation order, with the kernel of interpreter/Numerics.cpp's row of the
/// same instruction.
inline constexpr auto FoldingTable = [] {
    using namespace numerics;
    using detail::binary;
    using detail::compare;
    using detail::F32;
    using detail::F64;
    using detail::I32;
    using detail::I64;
    using detail::row;
    using detail::test;
    using detail::unary;
    using O = intermediate::Operation;
    return std::to_array<FoldingRow>({
        row<test<&eqz<I32>>>(O::I32Eqz),
        row<compare<&eq<I32>>>(O::I32Eq),
        row<compare<&ne<I32>>>(O::I32Ne),
        row<compare<&ltS<I32>>>(O::I32LtS),
        row<compare<&ltU<I32>>>(O::I32LtU),
        row<compare<&gtS<I32>>>(O::I32GtS),
        row<compare<&gtU<I32>>>(O::I32GtU),
        row<compare<&leS<I32>>>(O::I32LeS),
        row<compare<&leU<I32>>>(O::I32LeU),
        row<compare<&geS<I32>>>(O::I32GeS),
        row<compare<&geU<I32>>>(O::I32GeU),
        row<test<&eqz<I64>>>(O::I64Eqz),
        row<compare<&eq<I64>>>(O::I64Eq),
        row<compare<&ne<I64>>>(O::I64Ne),
        row<compare<&ltS<I64>>>(O::I64LtS),
        row<compare<&ltU<I64>>>(O::I64LtU),
        row<compare<&gtS<I64>>>(O::I64GtS),
        row<compare<&gtU<I64>>>(O::I64GtU),
        row<compare<&leS<I64>>>(O::I64LeS),
        row<compare<&leU<I64>>>(O::I64LeU),
        row<compare<&geS<I64>>>(O::I64GeS),
        row<compare<&geU<I64>>>(O::I64GeU),
        row<compare<&feq<F32>>>(O::F32Eq),
        row<compare<&fne<F32>>>(O::F32Ne),
        row<compare<&flt<F32>>>(O::F32Lt),
        row<compare<&fgt<F32>>>(O::F32Gt),
        row<compare<&fle<F32>>>(O::F32Le),
        row<compare<&fge<F32>>>(O::F32Ge),
        row<compare<&feq<F64>>>(O::F64Eq),
        row<compare<&fne<F64>>>(O::F64Ne),
        row<compare<&flt<F64>>>(O::F64Lt),
        row<compare<&fgt<F64>>>(O::F64Gt),
        row<compare<&fle<F64>>>(O::F64Le),
        row<compare<&fge<F64>>>(O::F64Ge),
        row<unary<&clz<I32>>>(O::I32Clz),
        row<unary<&ctz<I32>>>(O::I32Ctz),
        row<unary<&popcnt<I32>>>(O::I32Popcnt),
        row<binary<&add<I32>>>(O::I32Add),
        row<binary<&sub<I32>>>(O::I32Sub),
        row<binary<&mul<I32>>>(O::I32Mul),
        row<binary<&divS<I32>>>(O::I32DivS),
        row<binary<&divU<I32>>>(O::I32DivU),
        row<binary<&remS<I32>>>(O::I32RemS),
        row<binary<&remU<I32>>>(O::I32RemU),
        row<binary<&bitAnd<I32>>>(O::I32And),
        row<binary<&bitOr<I32>>>(O::I32Or),
        row<binary<&bitXor<I32>>>(O::I32Xor),
        row<binary<&shl<I32>>>(O::I32Shl),
        row<binary<&shrS<I32>>>(O::I32ShrS),
        row<binary<&shrU<I32>>>(O::I32ShrU),
        row<binary<&rotl<I32>>>(O::I32Rotl),
        row<binary<&rotr<I32>>>(O::I32Rotr),
        row<unary<&clz<I64>>>(O::I64Clz),
        row<unary<&ctz<I64>>>(O::I64Ctz),
        row<unary<&popcnt<I64>>>(O::I64Popcnt),
        row<binary<&add<I64>>>(O::I64Add),
        row<binary<&sub<I64>>>(O::I64Sub),
        row<binary<&mul<I64>>>(O::I64Mul),
        row<binary<&divS<I64>>>(O::I64DivS),
        row<binary<&divU<I64>>>(O::I64DivU),
        row<binary<&remS<I64>>>(O::I64RemS),
        row<binary<&remU<I64>>>(O::I64RemU),
        row<binary<&bitAnd<I64>>>(O::I64And),
        row<binary<&bitOr<I64>>>(O::I64Or),
        row<binary<&bitXor<I64>>>(O::I64Xor),
        row<binary<&shl<I64>>>(O::I64Shl),
        row<binary<&shrS<I64>>>(O::I64ShrS),
        row<binary<&shrU<I64>>>(O::I64ShrU),
        row<binary<&rotl<I64>>>(O::I64Rotl),
        row<binary<&rotr<I64>>>(O::I64Rotr),
        row<unary<&fabs<F32>>>(O::F32Abs),
        row<unary<&fneg<F32>>>(O::F32Neg),
        row<unary<&ceil<F32>>>(O::F32Ceil),
        row<unary<&floor<F32>>>(O::F32Floor),
        row<unary<&trunc<F32>>>(O::F32Trunc),
        row<unary<&nearest<F32>>>(O::F32Nearest),
        row<unary<&sqrt<F32>>>(O::F32Sqrt),
        row<binary<&fadd<F32>>>(O::F32Add),
        row<binary<&fsub<F32>>>(O::F32Sub),
        row<binary<&fmul<F32>>>(O::F32Mul),
        row<binary<&fdiv<F32>>>(O::F32Div),
        row<binary<&fmin<F32>>>(O::F32Min),
        row<binary<&fmax<F32>>>(O::F32Max),
        row<binary<&copysign<F32>>>(O::F32Copysign),
        row<unary<&fabs<F64>>>(O::F64Abs),
        row<unary<&fneg<F64>>>(O::F64Neg),
        row<unary<&ceil<F64>>>(O::F64Ceil),
        row<unary<&floor<F64>>>(O::F64Floor),
        row<unary<&trunc<F64>>>(O::F64Trunc),
        row<unary<&nearest<F64>>>(O::F64Nearest),
        row<unary<&sqrt<F64>>>(O::F64Sqrt),
        row<binary<&fadd<F64>>>(O::F64Add),
        row<binary<&fsub<F64>>>(O::F64Sub),
        row<binary<&fmul<F64>>>(O::F64Mul),
        row<binary<&fdiv<F64>>>(O::F64Div),
        row<binary<&fmin<F64>>>(O::F64Min),
        row<binary<&fmax<F64>>>(O::F64Max),
        row<binary<&copysign<F64>>>(O::F64Copysign),
        row<unary<&wrap>>(O::I32WrapI64),
        row<unary<&truncTrapping<I32, int32_t, F32>>>(O::I32TruncF32S),
        row<unary<&truncTrapping<I32, uint32_t, F32>>>(O::I32TruncF32U),
        row<unary<&truncTrapping<I32, int32_t, F64>>>(O::I32TruncF64S),
        row<unary<&truncTrapping<I32, uint32_t, F64>>>(O::I32TruncF64U),
        row<unary<&extendS>>(O::I64ExtendI32S),
        row<unary<&extendU>>(O::I64ExtendI32U),
        row<unary<&truncTrapping<I64, int64_t, F32>>>(O::I64TruncF32S),
        row<unary<&truncTrapping<I64, uint64_t, F32>>>(O::I64TruncF32U),
        row<unary<&truncTrapping<I64, int64_t, F64>>>(O::I64TruncF64S),
        row<unary<&truncTrapping<I64, uint64_t, F64>>>(O::I64TruncF64U),
        row<unary<&convert<F32, int32_t, I32>>>(O::F32ConvertI32S),
        row<unary<&convert<F32, uint32_t, I32>>>(O::F32ConvertI32U),
        row<unary<&convert<F32, int64_t, I64>>>(O::F32ConvertI64S),
        row<unary<&convert<F32, uint64_t, I64>>>(O::F32ConvertI64U),
        row<unary<&demote>>(O::F32DemoteF64),
        row<unary<&convert<F64, int32_t, I32>>>(O::F64ConvertI32S),
        row<unary<&convert<F64, uint32_t, I32>>>(O::F64ConvertI32U),
        row<unary<&convert<F64, int64_t, I64>>>(O::F64ConvertI64S),
        row<unary<&convert<F64, uint64_t, I64>>>(O::F64ConvertI64U),
        row<unary<&promote>>(O::F64PromoteF32),
        row<unary<&reinterpret<I32, F32>>>(O::I32ReinterpretF32),
        row<unary<&reinterpret<I64, F64>>>(O::I64ReinterpretF64),
        row<unary<&reinterpret<F32, I32>>>(O::F32ReinterpretI32),
        row<unary<&reinterpret<F64, I64>>>(O::F64ReinterpretI64),
        row<unary<&extendSigned<I32, int8_t>>>(O::I32Extend8S),
        row<unary<&extendSigned<I32, int16_t>>>(O::I32Extend16S),
        row<unary<&extendSigned<I64, int8_t>>>(O::I64Extend8S),
        row<unary<&extendSigned<I64, int16_t>>>(O::I64Extend16S),
        row<unary<&extendSigned<I64, int32_t>>>(O::I64Extend32S),
        row<unary<&truncSaturating<I32, int32_t, F32>>>(O::I32TruncSatF32S),
        row<unary<&truncSaturating<I32, uint32_t, F32>>>(O::I32TruncSatF32U),
        row<unary<&truncSaturating<I32, int32_t, F64>>>(O::I32TruncSatF64S),
        row<unary<&truncSaturating<I32, uint32_t, F64>>>(O::I32TruncSatF64U),
        row<unary<&truncSaturating<I64, int64_t, F32>>>(O::I64TruncSatF32S),
        row<unary<&truncSaturating<I64, uint64_t, F32>>>(O::I64TruncSatF32U),
        row<unary<&truncSaturating<I64, int64_t, F64>>>(O::I64TruncSatF64S),
        row<unary<&truncSaturating<I64, uint64_t, F64>>>(O::I64TruncSatF64U),
        row<detail::canonicalizeNan>(O::CanonicalizeNan),
    });
}();

static_assert(
    [] {
        // In Operation order, each operation once, and exactly the foldable ones.
        auto rows = std::size_t { 0 };
        for (auto const& operation: intermediate::OperationTable)
        {
            auto const has =
                rows < FoldingTable.size() && FoldingTable[rows].operation == operation.operation;
            if (has != isFoldable(operation))
                return false;
            if (has)
                ++rows;
        }
        return rows == FoldingTable.size();
    }(),
    "FoldingTable must hold one row per foldable operation (isFoldable()), in Operation order");

namespace detail
{
    /// Per Operation, the index of its FoldingTable row plus one; 0 for none.
    inline constexpr auto FoldingRowIndex = [] {
        auto index = std::array<uint16_t, intermediate::OperationCount> {};
        for (auto const position: std::views::iota(std::size_t { 0 }, FoldingTable.size()))
            index[std::to_underlying(FoldingTable[position].operation)] = static_cast<uint16_t>(position + 1);
        return index;
    }();
} // namespace detail

/// The FoldingTable row of @p operation.
/// @return It; none when the operation is not foldable.
[[nodiscard]] constexpr std::optional<FoldingRow> foldingRowOf(intermediate::Operation operation) noexcept
{
    auto const index = detail::FoldingRowIndex[std::to_underlying(operation)];
    if (index == 0)
        return std::nullopt;
    return FoldingTable[index - 1];
}

} // namespace isoline::compiler::optimization
