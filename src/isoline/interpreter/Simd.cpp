// SPDX-License-Identifier: Apache-2.0
#include <isoline/interpreter/Simd.hpp>

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <variant>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/numerics/SimdKernels.hpp>
#include <isoline/numerics/SimdLanes.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::interpreter
{

namespace
{
    using binary::Extension;
    using binary::Opcode;
    using runtime::F32;
    using runtime::F64;
    using runtime::I32;
    using runtime::I64;
    using runtime::V128;
    using runtime::Value;

    /// The kernel @p kernel names. Precondition (checked): there is one, that is, the opcode has
    /// the shape its getter stands for.
    template <typename Kernel>
    [[nodiscard]] Kernel required(Kernel kernel)
    {
        Require(kernel != nullptr);
        return kernel;
    }

    // --- Lane operations ----------------------------------------------------------------------
    //
    // They take or give a scalar, which the reference interpreter holds boxed, so they live here
    // rather than in numerics; the fast interpreter has dedicated rows of its own for them.

    /// The bits @p value holds, of the scalar type @p Box. Precondition (checked): it holds one.
    template <typename Box>
    [[nodiscard]] auto bitsOf(Value const& value)
    {
        Require(std::holds_alternative<Box>(value));
        return std::get<Box>(value).bits;
    }

    template <std::unsigned_integral Lane, typename Box>
    [[nodiscard]] V128 splatOf(Value const& value)
    {
        auto result = V128 {};
        auto const lane = static_cast<Lane>(bitsOf<Box>(value));
        for (auto const index: std::views::iota(0U, numerics::LaneCount<Lane>))
            numerics::setLane<Lane>(result, index, lane);
        return result;
    }

    /// Lane @p index of @p vector as a @p Box; a lane narrower than it is widened per @p Widening.
    template <std::unsigned_integral Lane, typename Box, Extension Widening = Extension::Zero>
    [[nodiscard]] Value extractOf(V128 vector, uint8_t index)
    {
        using Bits = decltype(Box {}.bits);
        return Box { numerics::widened<Bits, Widening>(numerics::laneOf<Lane>(vector, index)) };
    }

    template <std::unsigned_integral Lane, typename Box>
    [[nodiscard]] V128 replaceOf(V128 vector, uint8_t index, Value const& value)
    {
        numerics::setLane<Lane>(vector, index, static_cast<Lane>(bitsOf<Box>(value)));
        return vector;
    }

    using SplatFn = V128 (*)(Value const&);
    using ExtractFn = Value (*)(V128, uint8_t);
    using ReplaceFn = V128 (*)(V128, uint8_t, Value const&);

    /// A lane operation's evaluator; which of the three it holds agrees with
    /// numerics::simdShape(opcode), as Simd.shapesFollowSignatures checks.
    using LaneEvaluator = std::variant<std::monostate, SplatFn, ExtractFn, ReplaceFn>;

    /// One lane operation: the instruction and its evaluator.
    struct LaneRow
    {
        Opcode opcode {};        ///< The instruction.
        LaneEvaluator evaluator; ///< What evaluates it.
    };

    /// Every lane operation, one row each.
    constexpr auto LaneTable = std::to_array<LaneRow>({
        // Splats
        { .opcode = Opcode::I8x16Splat, .evaluator = SplatFn { &splatOf<uint8_t, I32> } },
        { .opcode = Opcode::I16x8Splat, .evaluator = SplatFn { &splatOf<uint16_t, I32> } },
        { .opcode = Opcode::I32x4Splat, .evaluator = SplatFn { &splatOf<uint32_t, I32> } },
        { .opcode = Opcode::I64x2Splat, .evaluator = SplatFn { &splatOf<uint64_t, I64> } },
        { .opcode = Opcode::F32x4Splat, .evaluator = SplatFn { &splatOf<uint32_t, F32> } },
        { .opcode = Opcode::F64x2Splat, .evaluator = SplatFn { &splatOf<uint64_t, F64> } },
        // Lanes
        { .opcode = Opcode::I8x16ExtractLaneS,
          .evaluator = ExtractFn { &extractOf<uint8_t, I32, Extension::Sign> } },
        { .opcode = Opcode::I8x16ExtractLaneU, .evaluator = ExtractFn { &extractOf<uint8_t, I32> } },
        { .opcode = Opcode::I16x8ExtractLaneS,
          .evaluator = ExtractFn { &extractOf<uint16_t, I32, Extension::Sign> } },
        { .opcode = Opcode::I16x8ExtractLaneU, .evaluator = ExtractFn { &extractOf<uint16_t, I32> } },
        { .opcode = Opcode::I32x4ExtractLane, .evaluator = ExtractFn { &extractOf<uint32_t, I32> } },
        { .opcode = Opcode::I64x2ExtractLane, .evaluator = ExtractFn { &extractOf<uint64_t, I64> } },
        { .opcode = Opcode::F32x4ExtractLane, .evaluator = ExtractFn { &extractOf<uint32_t, F32> } },
        { .opcode = Opcode::F64x2ExtractLane, .evaluator = ExtractFn { &extractOf<uint64_t, F64> } },
        { .opcode = Opcode::I8x16ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint8_t, I32> } },
        { .opcode = Opcode::I16x8ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint16_t, I32> } },
        { .opcode = Opcode::I32x4ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint32_t, I32> } },
        { .opcode = Opcode::I64x2ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint64_t, I64> } },
        { .opcode = Opcode::F32x4ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint32_t, F32> } },
        { .opcode = Opcode::F64x2ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint64_t, F64> } },
    });

    /// Each opcode's lane evaluator, by opcode; monostate for an opcode without a row.
    constexpr auto LaneByOpcode = [] {
        auto table = std::array<LaneEvaluator, binary::OpcodeCount> {};
        for (auto const& row: LaneTable)
            table.at(static_cast<std::size_t>(row.opcode)) = row.evaluator;
        return table;
    }();

    /// The lane evaluator of @p opcode, of type @p Fn. Precondition (checked): its row holds one.
    template <typename Fn>
    [[nodiscard]] Fn laneEvaluatorOf(Opcode opcode)
    {
        auto const* const evaluator = std::get_if<Fn>(&LaneByOpcode[static_cast<std::size_t>(opcode)]);
        Require(evaluator != nullptr);
        return *evaluator;
    }
} // namespace

V128 applySimdUnary(Opcode opcode, V128 operand)
{
    return required(numerics::simdUnaryKernel(opcode))(operand);
}

V128 applySimdBinary(Opcode opcode, V128 lhs, V128 rhs)
{
    return required(numerics::simdBinaryKernel(opcode))(lhs, rhs);
}

V128 applySimdTernary(Opcode opcode, V128 first, V128 second, V128 third)
{
    return required(numerics::simdTernaryKernel(opcode))(first, second, third);
}

I32 applySimdTest(Opcode opcode, V128 operand)
{
    return required(numerics::simdTestKernel(opcode))(operand);
}

V128 applySimdShift(Opcode opcode, V128 operand, uint32_t count)
{
    return required(numerics::simdShiftKernel(opcode))(operand, count);
}

V128 splat(Opcode opcode, Value const& value)
{
    return laneEvaluatorOf<SplatFn>(opcode)(value);
}

Value extractLane(Opcode opcode, V128 vector, uint8_t lane)
{
    return laneEvaluatorOf<ExtractFn>(opcode)(vector, lane);
}

V128 replaceLane(Opcode opcode, V128 vector, uint8_t lane, Value const& value)
{
    return laneEvaluatorOf<ReplaceFn>(opcode)(vector, lane, value);
}

} // namespace isoline::interpreter
