// SPDX-License-Identifier: Apache-2.0
#include <isoline/reference/Numerics.hpp>

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <variant>

#include <isoline/binary/Opcode.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/numerics/Integer.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

// The Value-level face of the typed kernels in numerics/: one table row per numeric instruction,
// each a thunk that unboxes the operands, calls the kernel and boxes its result.

namespace isoline::reference
{

namespace
{
    using binary::Opcode;
    using runtime::F32;
    using runtime::F64;
    using runtime::I32;
    using runtime::I64;
    using runtime::Trap;
    using runtime::Value;
    using namespace numerics;

    /// The outcome of a numeric operator: a value, or the trap it ends with.
    using Result = std::expected<Value, Trap>;

    // --- The table ----------------------------------------------------------------------------

    using UnaryFn = Result (*)(Value);
    using BinaryFn = Result (*)(Value, Value);
    using WideFn = Halves (*)(std::span<uint64_t const>) noexcept;

    /// The parameter and result types of a function pointer.
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

    /// The operand @p value holds, of type @p T. Precondition (checked): it holds one.
    template <typename T>
    [[nodiscard]] T operand(Value const& value)
    {
        Require(std::holds_alternative<T>(value));
        return std::get<T>(value);
    }

    template <typename T>
    [[nodiscard]] Result lift(T result)
    {
        return Value { result };
    }

    template <typename T>
    [[nodiscard]] Result lift(std::expected<T, Trap> result)
    {
        return result.transform([](T value) { return Value { value }; });
    }

    [[nodiscard]] Result truth(bool holds)
    {
        return Value { I32 { holds ? 1U : 0U } };
    }

    template <auto Fn>
    [[nodiscard]] Result unaryThunk(Value value)
    {
        return lift(Fn(operand<typename Signature<decltype(Fn)>::Arg>(value)));
    }

    template <auto Fn>
    [[nodiscard]] Result binaryThunk(Value lhs, Value rhs)
    {
        using Types = Signature<decltype(Fn)>;
        return lift(Fn(operand<typename Types::Lhs>(lhs), operand<typename Types::Rhs>(rhs)));
    }

    template <auto Fn>
    [[nodiscard]] Result testThunk(Value value)
    {
        return truth(Fn(operand<typename Signature<decltype(Fn)>::Arg>(value)));
    }

    template <auto Fn>
    [[nodiscard]] Result compareThunk(Value lhs, Value rhs)
    {
        using Types = Signature<decltype(Fn)>;
        return truth(Fn(operand<typename Types::Lhs>(lhs), operand<typename Types::Rhs>(rhs)));
    }

    /// Applies a 128-bit operator to its two {low, high} operands, given as four limbs.
    template <auto Fn>
    [[nodiscard]] Halves pairThunk(std::span<uint64_t const> operands) noexcept
    {
        return Fn(Halves { operands[0], operands[1] }, Halves { operands[2], operands[3] });
    }

    /// Applies a widening multiplication to its two 64-bit operands.
    template <auto Fn>
    [[nodiscard]] Halves productThunk(std::span<uint64_t const> operands) noexcept
    {
        return Fn(operands[0], operands[1]);
    }

    /// One numeric instruction: how it takes its operands, and the function evaluating it.
    struct NumericRow
    {
        Opcode opcode {};                        ///< The instruction.
        NumericShape shape = NumericShape::None; ///< How it takes its operands.
        UnaryFn unary = nullptr;                 ///< Unary and Test: the evaluator.
        BinaryFn binary = nullptr;               ///< Binary and Compare: the evaluator.
        WideFn wide = nullptr;                   ///< Wide: the evaluator.
    };

    template <auto Fn>
    [[nodiscard]] consteval NumericRow unary(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Unary, .unary = &unaryThunk<Fn> };
    }

    template <auto Fn>
    [[nodiscard]] consteval NumericRow binary(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Binary, .binary = &binaryThunk<Fn> };
    }

    template <auto Fn>
    [[nodiscard]] consteval NumericRow test(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Test, .unary = &testThunk<Fn> };
    }

    template <auto Fn>
    [[nodiscard]] consteval NumericRow compare(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Compare, .binary = &compareThunk<Fn> };
    }

    template <auto Fn>
    [[nodiscard]] consteval NumericRow widePair(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Wide, .wide = &pairThunk<Fn> };
    }

    template <auto Fn>
    [[nodiscard]] consteval NumericRow wideProduct(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Wide, .wide = &productThunk<Fn> };
    }

    /// Every numeric instruction, one row each.
    constexpr auto NumericTable = std::to_array<NumericRow>({
        // i32
        test<&eqz<I32>>(Opcode::I32Eqz),
        compare<&eq<I32>>(Opcode::I32Eq),
        compare<&ne<I32>>(Opcode::I32Ne),
        compare<&ltS<I32>>(Opcode::I32LtS),
        compare<&ltU<I32>>(Opcode::I32LtU),
        compare<&gtS<I32>>(Opcode::I32GtS),
        compare<&gtU<I32>>(Opcode::I32GtU),
        compare<&leS<I32>>(Opcode::I32LeS),
        compare<&leU<I32>>(Opcode::I32LeU),
        compare<&geS<I32>>(Opcode::I32GeS),
        compare<&geU<I32>>(Opcode::I32GeU),
        unary<&clz<I32>>(Opcode::I32Clz),
        unary<&ctz<I32>>(Opcode::I32Ctz),
        unary<&popcnt<I32>>(Opcode::I32Popcnt),
        binary<&add<I32>>(Opcode::I32Add),
        binary<&sub<I32>>(Opcode::I32Sub),
        binary<&mul<I32>>(Opcode::I32Mul),
        binary<&divS<I32>>(Opcode::I32DivS),
        binary<&divU<I32>>(Opcode::I32DivU),
        binary<&remS<I32>>(Opcode::I32RemS),
        binary<&remU<I32>>(Opcode::I32RemU),
        binary<&bitAnd<I32>>(Opcode::I32And),
        binary<&bitOr<I32>>(Opcode::I32Or),
        binary<&bitXor<I32>>(Opcode::I32Xor),
        binary<&shl<I32>>(Opcode::I32Shl),
        binary<&shrS<I32>>(Opcode::I32ShrS),
        binary<&shrU<I32>>(Opcode::I32ShrU),
        binary<&rotl<I32>>(Opcode::I32Rotl),
        binary<&rotr<I32>>(Opcode::I32Rotr),
        unary<&extendSigned<I32, int8_t>>(Opcode::I32Extend8S),
        unary<&extendSigned<I32, int16_t>>(Opcode::I32Extend16S),
        // i64
        test<&eqz<I64>>(Opcode::I64Eqz),
        compare<&eq<I64>>(Opcode::I64Eq),
        compare<&ne<I64>>(Opcode::I64Ne),
        compare<&ltS<I64>>(Opcode::I64LtS),
        compare<&ltU<I64>>(Opcode::I64LtU),
        compare<&gtS<I64>>(Opcode::I64GtS),
        compare<&gtU<I64>>(Opcode::I64GtU),
        compare<&leS<I64>>(Opcode::I64LeS),
        compare<&leU<I64>>(Opcode::I64LeU),
        compare<&geS<I64>>(Opcode::I64GeS),
        compare<&geU<I64>>(Opcode::I64GeU),
        unary<&clz<I64>>(Opcode::I64Clz),
        unary<&ctz<I64>>(Opcode::I64Ctz),
        unary<&popcnt<I64>>(Opcode::I64Popcnt),
        binary<&add<I64>>(Opcode::I64Add),
        binary<&sub<I64>>(Opcode::I64Sub),
        binary<&mul<I64>>(Opcode::I64Mul),
        binary<&divS<I64>>(Opcode::I64DivS),
        binary<&divU<I64>>(Opcode::I64DivU),
        binary<&remS<I64>>(Opcode::I64RemS),
        binary<&remU<I64>>(Opcode::I64RemU),
        binary<&bitAnd<I64>>(Opcode::I64And),
        binary<&bitOr<I64>>(Opcode::I64Or),
        binary<&bitXor<I64>>(Opcode::I64Xor),
        binary<&shl<I64>>(Opcode::I64Shl),
        binary<&shrS<I64>>(Opcode::I64ShrS),
        binary<&shrU<I64>>(Opcode::I64ShrU),
        binary<&rotl<I64>>(Opcode::I64Rotl),
        binary<&rotr<I64>>(Opcode::I64Rotr),
        unary<&extendSigned<I64, int8_t>>(Opcode::I64Extend8S),
        unary<&extendSigned<I64, int16_t>>(Opcode::I64Extend16S),
        unary<&extendSigned<I64, int32_t>>(Opcode::I64Extend32S),
        // f32
        compare<&feq<F32>>(Opcode::F32Eq),
        compare<&fne<F32>>(Opcode::F32Ne),
        compare<&flt<F32>>(Opcode::F32Lt),
        compare<&fgt<F32>>(Opcode::F32Gt),
        compare<&fle<F32>>(Opcode::F32Le),
        compare<&fge<F32>>(Opcode::F32Ge),
        unary<&fabs<F32>>(Opcode::F32Abs),
        unary<&fneg<F32>>(Opcode::F32Neg),
        unary<&ceil<F32>>(Opcode::F32Ceil),
        unary<&floor<F32>>(Opcode::F32Floor),
        unary<&trunc<F32>>(Opcode::F32Trunc),
        unary<&nearest<F32>>(Opcode::F32Nearest),
        unary<&sqrt<F32>>(Opcode::F32Sqrt),
        binary<&fadd<F32>>(Opcode::F32Add),
        binary<&fsub<F32>>(Opcode::F32Sub),
        binary<&fmul<F32>>(Opcode::F32Mul),
        binary<&fdiv<F32>>(Opcode::F32Div),
        binary<&fmin<F32>>(Opcode::F32Min),
        binary<&fmax<F32>>(Opcode::F32Max),
        binary<&copysign<F32>>(Opcode::F32Copysign),
        // f64
        compare<&feq<F64>>(Opcode::F64Eq),
        compare<&fne<F64>>(Opcode::F64Ne),
        compare<&flt<F64>>(Opcode::F64Lt),
        compare<&fgt<F64>>(Opcode::F64Gt),
        compare<&fle<F64>>(Opcode::F64Le),
        compare<&fge<F64>>(Opcode::F64Ge),
        unary<&fabs<F64>>(Opcode::F64Abs),
        unary<&fneg<F64>>(Opcode::F64Neg),
        unary<&ceil<F64>>(Opcode::F64Ceil),
        unary<&floor<F64>>(Opcode::F64Floor),
        unary<&trunc<F64>>(Opcode::F64Trunc),
        unary<&nearest<F64>>(Opcode::F64Nearest),
        unary<&sqrt<F64>>(Opcode::F64Sqrt),
        binary<&fadd<F64>>(Opcode::F64Add),
        binary<&fsub<F64>>(Opcode::F64Sub),
        binary<&fmul<F64>>(Opcode::F64Mul),
        binary<&fdiv<F64>>(Opcode::F64Div),
        binary<&fmin<F64>>(Opcode::F64Min),
        binary<&fmax<F64>>(Opcode::F64Max),
        binary<&copysign<F64>>(Opcode::F64Copysign),
        // conversions
        unary<&wrap>(Opcode::I32WrapI64),
        unary<&truncTrapping<I32, int32_t, F32>>(Opcode::I32TruncF32S),
        unary<&truncTrapping<I32, uint32_t, F32>>(Opcode::I32TruncF32U),
        unary<&truncTrapping<I32, int32_t, F64>>(Opcode::I32TruncF64S),
        unary<&truncTrapping<I32, uint32_t, F64>>(Opcode::I32TruncF64U),
        unary<&extendS>(Opcode::I64ExtendI32S),
        unary<&extendU>(Opcode::I64ExtendI32U),
        unary<&truncTrapping<I64, int64_t, F32>>(Opcode::I64TruncF32S),
        unary<&truncTrapping<I64, uint64_t, F32>>(Opcode::I64TruncF32U),
        unary<&truncTrapping<I64, int64_t, F64>>(Opcode::I64TruncF64S),
        unary<&truncTrapping<I64, uint64_t, F64>>(Opcode::I64TruncF64U),
        unary<&convert<F32, int32_t, I32>>(Opcode::F32ConvertI32S),
        unary<&convert<F32, uint32_t, I32>>(Opcode::F32ConvertI32U),
        unary<&convert<F32, int64_t, I64>>(Opcode::F32ConvertI64S),
        unary<&convert<F32, uint64_t, I64>>(Opcode::F32ConvertI64U),
        unary<&demote>(Opcode::F32DemoteF64),
        unary<&convert<F64, int32_t, I32>>(Opcode::F64ConvertI32S),
        unary<&convert<F64, uint32_t, I32>>(Opcode::F64ConvertI32U),
        unary<&convert<F64, int64_t, I64>>(Opcode::F64ConvertI64S),
        unary<&convert<F64, uint64_t, I64>>(Opcode::F64ConvertI64U),
        unary<&promote>(Opcode::F64PromoteF32),
        unary<&reinterpret<I32, F32>>(Opcode::I32ReinterpretF32),
        unary<&reinterpret<I64, F64>>(Opcode::I64ReinterpretF64),
        unary<&reinterpret<F32, I32>>(Opcode::F32ReinterpretI32),
        unary<&reinterpret<F64, I64>>(Opcode::F64ReinterpretI64),
        unary<&truncSaturating<I32, int32_t, F32>>(Opcode::I32TruncSatF32S),
        unary<&truncSaturating<I32, uint32_t, F32>>(Opcode::I32TruncSatF32U),
        unary<&truncSaturating<I32, int32_t, F64>>(Opcode::I32TruncSatF64S),
        unary<&truncSaturating<I32, uint32_t, F64>>(Opcode::I32TruncSatF64U),
        unary<&truncSaturating<I64, int64_t, F32>>(Opcode::I64TruncSatF32S),
        unary<&truncSaturating<I64, uint64_t, F32>>(Opcode::I64TruncSatF32U),
        unary<&truncSaturating<I64, int64_t, F64>>(Opcode::I64TruncSatF64S),
        unary<&truncSaturating<I64, uint64_t, F64>>(Opcode::I64TruncSatF64U),
        // wide arithmetic
        widePair<&add128>(Opcode::I64Add128),
        widePair<&sub128>(Opcode::I64Sub128),
        wideProduct<&mulWideS>(Opcode::I64MulWideS),
        wideProduct<&mulWideU>(Opcode::I64MulWideU),
    });

    /// NumericTable indexed by opcode, for a constant-time lookup; None for every other opcode.
    constexpr auto NumericByOpcode = [] {
        auto table = std::array<NumericRow, binary::OpcodeCount> {};
        for (auto const& row: NumericTable)
            table.at(static_cast<std::size_t>(row.opcode)) = row;
        return table;
    }();

    /// The row of @p opcode. Precondition (checked): it is a numeric instruction of @p shape, and
    /// its signature pops @p operandCount operands. One check for both, so that every apply
    /// function shares it.
    [[nodiscard]] NumericRow const& rowOf(Opcode opcode, NumericShape shape, std::size_t operandCount)
    {
        auto const index = static_cast<std::size_t>(opcode);
        auto const& row = NumericByOpcode[index];
        auto const arity =
            std::size_t { binary::signatureInfo(binary::OpcodeTable[index].signature).paramCount };
        // The pair comparison keeps this one branch in the coverage ratchet's view; an
        // interpreter should not reuse rowOf on its hot path.
        Require((std::pair { row.shape, arity } == std::pair { shape, operandCount }));
        return row;
    }
} // namespace

NumericShape numericShape(Opcode opcode) noexcept
{
    return NumericByOpcode[static_cast<std::size_t>(opcode)].shape;
}

std::expected<Value, Trap> applyUnary(Opcode opcode, Value operand)
{
    return rowOf(opcode, NumericShape::Unary, 1).unary(operand);
}

std::expected<Value, Trap> applyBinary(Opcode opcode, Value lhs, Value rhs)
{
    return rowOf(opcode, NumericShape::Binary, 2).binary(lhs, rhs);
}

I32 applyTest(Opcode opcode, Value operand)
{
    return std::get<I32>(*rowOf(opcode, NumericShape::Test, 1).unary(operand));
}

I32 applyCompare(Opcode opcode, Value lhs, Value rhs)
{
    return std::get<I32>(*rowOf(opcode, NumericShape::Compare, 2).binary(lhs, rhs));
}

std::array<uint64_t, 2> applyWide(Opcode opcode, std::span<uint64_t const> operands) noexcept
{
    return rowOf(opcode, NumericShape::Wide, operands.size()).wide(operands);
}

} // namespace isoline::reference
