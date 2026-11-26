// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <limits>
#include <variant>

#include <isoline/binary/Opcode.hpp>
#include <isoline/interp/Numerics.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::binary::Opcode;
using isoline::binary::OpcodeSignature;
using isoline::binary::OpcodeTable;
using isoline::interp::applyBinary;
using isoline::interp::applyCompare;
using isoline::interp::applyTest;
using isoline::interp::applyUnary;
using isoline::interp::applyWide;
using isoline::interp::CanonicalNanF32;
using isoline::interp::CanonicalNanF64;
using isoline::interp::NumericShape;
using isoline::interp::numericShape;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;

namespace
{

constexpr auto Int32Min = static_cast<uint32_t>(std::numeric_limits<int32_t>::min());
constexpr auto Int32Max = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());

I32 i32(int32_t value)
{
    return I32 { static_cast<uint32_t>(value) };
}

F32 f32(float value)
{
    return F32 { std::bit_cast<uint32_t>(value) };
}

F64 f64(double value)
{
    return F64 { std::bit_cast<uint64_t>(value) };
}

/// The value @p result holds, which must be one.
Value valueOf(std::expected<Value, Trap> const& result)
{
    REQUIRE(result.has_value());
    return *result;
}

/// The trap @p result ended with, which must be one.
TrapCode trapOf(std::expected<Value, Trap> const& result)
{
    REQUIRE_FALSE(result.has_value());
    return result.error().code;
}

/// The bits of the f32 @p result holds.
uint32_t f32Bits(std::expected<Value, Trap> const& result)
{
    auto const value = valueOf(result);
    REQUIRE(std::holds_alternative<F32>(value));
    return std::get<F32>(value).bits;
}

/// The bits of the f64 @p result holds.
uint64_t f64Bits(std::expected<Value, Trap> const& result)
{
    auto const value = valueOf(result);
    REQUIRE(std::holds_alternative<F64>(value));
    return std::get<F64>(value).bits;
}

} // namespace

TEST_CASE("Numerics.everyFixedSignatureOpcodeHasARow")
{
    for (auto const& info: OpcodeTable)
    {
        INFO(info.text);
        auto const shape = numericShape(info.opcode);
        switch (info.signature)
        {
            case OpcodeSignature::Special:
            case OpcodeSignature::None:
            case OpcodeSignature::ToI32:
            case OpcodeSignature::ToI64:
            case OpcodeSignature::ToF32:
            case OpcodeSignature::ToF64: CHECK(shape == NumericShape::None); break;
            default: {
                auto const& row = isoline::binary::signatureInfo(info.signature);
                auto const params = row.paramCount;
                if (row.resultCount == 2)
                    CHECK(shape == NumericShape::Wide);
                else if (params == 1)
                    CHECK((shape == NumericShape::Unary || shape == NumericShape::Test));
                else
                    CHECK((shape == NumericShape::Binary || shape == NumericShape::Compare));
            }
        }
    }
}

TEST_CASE("Numerics.add128Carries")
{
    using Operands = std::array<uint64_t, 4>;
    using Result = std::array<uint64_t, 2>;
    CHECK(applyWide(Opcode::I64Add128, Operands { ~0ULL, 0, 1, 0 }) == Result { 0, 1 });
    // The carry out of the high half is lost: 2^128 - 1 + 1 wraps to 0.
    CHECK(applyWide(Opcode::I64Add128, Operands { ~0ULL, ~0ULL, 1, 0 }) == Result { 0, 0 });
    CHECK(applyWide(Opcode::I64Add128, Operands { 1, 2, 3, 4 }) == Result { 4, 6 });
}

TEST_CASE("Numerics.sub128Borrows")
{
    using Operands = std::array<uint64_t, 4>;
    using Result = std::array<uint64_t, 2>;
    CHECK(applyWide(Opcode::I64Sub128, Operands { 0, 0, 1, 0 }) == Result { ~0ULL, ~0ULL });
    CHECK(applyWide(Opcode::I64Sub128, Operands { 0, 1, 1, 0 }) == Result { ~0ULL, 0 });
    CHECK(applyWide(Opcode::I64Sub128, Operands { 2, 5, 1, 2 }) == Result { 1, 3 });
}

TEST_CASE("Numerics.mulWideU")
{
    using Operands = std::array<uint64_t, 2>;
    using Result = std::array<uint64_t, 2>;
    CHECK(applyWide(Opcode::I64MulWideU, Operands { ~0ULL, 2 }) == Result { 0xFFFF'FFFF'FFFF'FFFE, 1 });
    // (2^64 - 1)^2 = 2^128 - 2^65 + 1.
    CHECK(applyWide(Opcode::I64MulWideU, Operands { ~0ULL, ~0ULL }) == Result { 1, 0xFFFF'FFFF'FFFF'FFFE });
    // A cross product's high half lands in the high limb; the middle column does not carry.
    CHECK(applyWide(Opcode::I64MulWideU, Operands { 0xFFFF'FFFF'0000'0001, 0x0000'0001'FFFF'FFFF })
          == Result { 0x0000'0002'FFFF'FFFF, 0x0000'0001'FFFF'FFFD });
    // (2^33 - 1)^2 = 3 * 2^64 + (2^64 - 2^34 + 1): the middle column sums to 0x2'FFFF'FFFC, so two of
    // the high limb's three come from its carry alone.
    CHECK(applyWide(Opcode::I64MulWideU, Operands { 0x0000'0001'FFFF'FFFF, 0x0000'0001'FFFF'FFFF })
          == Result { 0xFFFF'FFFC'0000'0001, 3 });
    CHECK(applyWide(Opcode::I64MulWideU, Operands { 0, ~0ULL }) == Result { 0, 0 });
}

TEST_CASE("Numerics.mulWideS")
{
    using Operands = std::array<uint64_t, 2>;
    using Result = std::array<uint64_t, 2>;
    CHECK(applyWide(Opcode::I64MulWideS, Operands { ~0ULL /* -1 */, 2 })
          == Result { 0xFFFF'FFFF'FFFF'FFFE, ~0ULL });
    CHECK(applyWide(Opcode::I64MulWideS, Operands { ~0ULL, ~0ULL }) == Result { 1, 0 }); // -1 * -1
    // INT64_MIN squared is 2^126.
    CHECK(applyWide(Opcode::I64MulWideS, Operands { 0x8000'0000'0000'0000, 0x8000'0000'0000'0000 })
          == Result { 0, 0x4000'0000'0000'0000 });
    CHECK(applyWide(Opcode::I64MulWideS, Operands { 3, 4 }) == Result { 12, 0 });
}

TEST_CASE("Numerics.i32DivS")
{
    CHECK(trapOf(applyBinary(Opcode::I32DivS, I32 { Int32Min }, i32(-1))) == TrapCode::IntegerOverflow);
    CHECK(trapOf(applyBinary(Opcode::I32DivS, i32(7), i32(0))) == TrapCode::IntegerDivideByZero);
    CHECK(valueOf(applyBinary(Opcode::I32DivS, i32(-7), i32(2))) == Value { i32(-3) });
    CHECK(trapOf(applyBinary(Opcode::I32DivU, i32(7), i32(0))) == TrapCode::IntegerDivideByZero);
    CHECK(valueOf(applyBinary(Opcode::I32DivU, i32(-7), i32(2))) == Value { I32 { 0x7FFF'FFFC } });
}

TEST_CASE("Numerics.i32RemS")
{
    CHECK(valueOf(applyBinary(Opcode::I32RemS, I32 { Int32Min }, i32(-1))) == Value { i32(0) });
    CHECK(valueOf(applyBinary(Opcode::I32RemS, i32(-7), i32(2))) == Value { i32(-1) });
    CHECK(trapOf(applyBinary(Opcode::I32RemS, i32(7), i32(0))) == TrapCode::IntegerDivideByZero);
    auto const int64Min = I64 { uint64_t { 1 } << 63 };
    CHECK(valueOf(applyBinary(Opcode::I64RemS, int64Min, I64 { ~uint64_t { 0 } })) == Value { I64 {} });
    CHECK(trapOf(applyBinary(Opcode::I64DivS, int64Min, I64 { ~uint64_t { 0 } }))
          == TrapCode::IntegerOverflow);
}

TEST_CASE("Numerics.truncF32S")
{
    CHECK(trapOf(applyUnary(Opcode::I32TruncF32S, f32(2147483648.0F))) == TrapCode::IntegerOverflow);
    CHECK(trapOf(applyUnary(Opcode::I32TruncF32S, F32 { 0x7FC0'0000 }))
          == TrapCode::InvalidConversionToInteger);
    CHECK(valueOf(applyUnary(Opcode::I32TruncF32S, f32(-2147483648.0F))) == Value { I32 { Int32Min } });
    // The f32 just below -2^31 is -2147483904: out of range.
    CHECK(trapOf(applyUnary(Opcode::I32TruncF32S, f32(-2147483904.0F))) == TrapCode::IntegerOverflow);
    // The f32 just below 2^31 is 2147483520: in range.
    CHECK(valueOf(applyUnary(Opcode::I32TruncF32S, f32(2147483520.0F))) == Value { I32 { 2147483520 } });
    CHECK(valueOf(applyUnary(Opcode::I32TruncF32S, f32(-1.9F))) == Value { i32(-1) });

    SECTION("unsigned")
    {
        CHECK(valueOf(applyUnary(Opcode::I32TruncF32U, f32(-0.9F))) == Value { i32(0) });
        CHECK(trapOf(applyUnary(Opcode::I32TruncF32U, f32(-1.0F))) == TrapCode::IntegerOverflow);
        CHECK(valueOf(applyUnary(Opcode::I32TruncF32U, f32(4294967040.0F))) == Value { I32 { 4294967040U } });
        CHECK(trapOf(applyUnary(Opcode::I32TruncF32U, f32(4294967296.0F))) == TrapCode::IntegerOverflow);
    }
    SECTION("f64 to i32 uses the exact bounds")
    {
        CHECK(valueOf(applyUnary(Opcode::I32TruncF64S, f64(-2147483648.9))) == Value { I32 { Int32Min } });
        CHECK(trapOf(applyUnary(Opcode::I32TruncF64S, f64(-2147483649.0))) == TrapCode::IntegerOverflow);
        CHECK(valueOf(applyUnary(Opcode::I32TruncF64S, f64(2147483647.9))) == Value { I32 { Int32Max } });
        CHECK(trapOf(applyUnary(Opcode::I32TruncF64S, f64(2147483648.0))) == TrapCode::IntegerOverflow);
    }
    SECTION("to i64")
    {
        CHECK(valueOf(applyUnary(Opcode::I64TruncF64S, f64(-9223372036854775808.0)))
              == Value { I64 { uint64_t { 1 } << 63 } });
        CHECK(trapOf(applyUnary(Opcode::I64TruncF64S, f64(9223372036854775808.0)))
              == TrapCode::IntegerOverflow);
        CHECK(valueOf(applyUnary(Opcode::I64TruncF64U, f64(18446744073709549568.0)))
              == Value { I64 { 18446744073709549568ULL } });
        CHECK(trapOf(applyUnary(Opcode::I64TruncF64U, f64(18446744073709551616.0)))
              == TrapCode::IntegerOverflow);
        CHECK(trapOf(applyUnary(Opcode::I64TruncF32S, F32 { 0xFF80'0000 })) == TrapCode::IntegerOverflow);
    }
}

TEST_CASE("Numerics.truncSat")
{
    CHECK(valueOf(applyUnary(Opcode::I32TruncSatF32S, F32 { 0x7FC0'0000 })) == Value { i32(0) });
    CHECK(valueOf(applyUnary(Opcode::I32TruncSatF32S, F32 { 0x7F80'0000 })) == Value { I32 { Int32Max } });
    CHECK(valueOf(applyUnary(Opcode::I32TruncSatF32S, F32 { 0xFF80'0000 })) == Value { I32 { Int32Min } });
    CHECK(valueOf(applyUnary(Opcode::I32TruncSatF32U, f32(-5.0F))) == Value { i32(0) });
    CHECK(valueOf(applyUnary(Opcode::I32TruncSatF32U, f32(1e10F))) == Value { I32 { 0xFFFF'FFFF } });
    CHECK(valueOf(applyUnary(Opcode::I64TruncSatF64S, f64(1e300)))
          == Value { I64 { static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) } });
    CHECK(valueOf(applyUnary(Opcode::I64TruncSatF64U, f64(-1e300))) == Value { I64 {} });
    CHECK(valueOf(applyUnary(Opcode::I32TruncSatF64S, f64(-7.5))) == Value { i32(-7) });
}

TEST_CASE("Numerics.f32MinNegZero")
{
    CHECK(f32Bits(applyBinary(Opcode::F32Min, f32(0.0F), f32(-0.0F))) == 0x8000'0000);
    CHECK(f32Bits(applyBinary(Opcode::F32Min, f32(-0.0F), f32(0.0F))) == 0x8000'0000);
    CHECK(f32Bits(applyBinary(Opcode::F32Max, f32(-0.0F), f32(0.0F))) == 0x0000'0000);
    CHECK(f32Bits(applyBinary(Opcode::F32Min, f32(1.0F), f32(-2.0F))) == std::bit_cast<uint32_t>(-2.0F));
    CHECK(f32Bits(applyBinary(Opcode::F32Max, f32(1.0F), f32(-2.0F))) == std::bit_cast<uint32_t>(1.0F));
    // A NaN operand, signalling or quiet, gives the canonical NaN.
    CHECK(f32Bits(applyBinary(Opcode::F32Min, F32 { 0x7F80'0001 }, f32(1.0F))) == CanonicalNanF32);
    CHECK(f64Bits(applyBinary(Opcode::F64Max, f64(1.0), F64 { 0xFFF0'0000'0000'0001 })) == CanonicalNanF64);
}

TEST_CASE("Numerics.f32Nearest")
{
    auto const nearest = [](float value) {
        return f32Bits(applyUnary(Opcode::F32Nearest, f32(value)));
    };
    CHECK(nearest(2.5F) == std::bit_cast<uint32_t>(2.0F));
    CHECK(nearest(-0.5F) == 0x8000'0000);
    CHECK(nearest(0.5F) == 0x0000'0000);
    CHECK(nearest(1.5F) == std::bit_cast<uint32_t>(2.0F));
    CHECK(nearest(3.5F) == std::bit_cast<uint32_t>(4.0F));
    CHECK(nearest(-1.5F) == std::bit_cast<uint32_t>(-2.0F));
    CHECK(nearest(0.75F) == std::bit_cast<uint32_t>(1.0F));
    CHECK(nearest(2.25F) == std::bit_cast<uint32_t>(2.0F));
    CHECK(nearest(-0.0F) == 0x8000'0000);
    CHECK(nearest(8388609.0F) == std::bit_cast<uint32_t>(8388609.0F)); // 2^23 + 1: integral
    CHECK(nearest(4194304.5F) == std::bit_cast<uint32_t>(4194304.0F)); // 2^22 + 0.5: tie to even
    CHECK(nearest(4194305.5F) == std::bit_cast<uint32_t>(4194306.0F));
    CHECK(nearest(std::numeric_limits<float>::infinity()) == 0x7F80'0000);
    CHECK(nearest(std::numeric_limits<float>::denorm_min()) == 0);
    CHECK(f32Bits(applyUnary(Opcode::F32Nearest, F32 { 0x7FA0'0000 })) == CanonicalNanF32);
    CHECK(f64Bits(applyUnary(Opcode::F64Nearest, f64(-2.5))) == std::bit_cast<uint64_t>(-2.0));
    CHECK(f64Bits(applyUnary(Opcode::F64Nearest, f64(4503599627370495.5)))
          == std::bit_cast<uint64_t>(4503599627370496.0));
}

TEST_CASE("Numerics.rounding")
{
    auto const round = [](Opcode opcode, float value) {
        return f32Bits(applyUnary(opcode, f32(value)));
    };
    CHECK(round(Opcode::F32Floor, -0.5F) == std::bit_cast<uint32_t>(-1.0F));
    CHECK(round(Opcode::F32Floor, 0.5F) == 0);
    CHECK(round(Opcode::F32Floor, -1.5F) == std::bit_cast<uint32_t>(-2.0F));
    CHECK(round(Opcode::F32Ceil, -0.5F) == 0x8000'0000);
    CHECK(round(Opcode::F32Ceil, 0.5F) == std::bit_cast<uint32_t>(1.0F));
    CHECK(round(Opcode::F32Ceil, 1.5F) == std::bit_cast<uint32_t>(2.0F));
    CHECK(round(Opcode::F32Ceil, 1.0F) == std::bit_cast<uint32_t>(1.0F));
    CHECK(round(Opcode::F32Trunc, -1.9F) == std::bit_cast<uint32_t>(-1.0F));
    CHECK(round(Opcode::F32Trunc, -0.9F) == 0x8000'0000);
    CHECK(f64Bits(applyUnary(Opcode::F64Floor, f64(-3.25))) == std::bit_cast<uint64_t>(-4.0));
    CHECK(f64Bits(applyUnary(Opcode::F64Ceil, f64(1e300))) == std::bit_cast<uint64_t>(1e300));
}

TEST_CASE("Numerics.copysignKeepsPayload")
{
    CHECK(f32Bits(applyBinary(Opcode::F32Copysign, F32 { 0x7FA0'0000 }, f32(-1.0F))) == 0xFFA0'0000);
    CHECK(f32Bits(applyUnary(Opcode::F32Neg, F32 { 0x7FA0'0000 })) == 0xFFA0'0000);
    CHECK(f32Bits(applyUnary(Opcode::F32Abs, F32 { 0xFF80'0001 })) == 0x7F80'0001);
    CHECK(valueOf(applyUnary(Opcode::I32ReinterpretF32, F32 { 0x7F80'0001 }))
          == Value { I32 { 0x7F80'0001 } });
}

TEST_CASE("Numerics.arithmeticNan")
{
    auto const infinity = f32(std::numeric_limits<float>::infinity());
    CHECK(f32Bits(applyBinary(Opcode::F32Sub, infinity, infinity)) == CanonicalNanF32);
    CHECK(f32Bits(applyBinary(Opcode::F32Add, F32 { 0x7F80'0001 }, f32(1.0F))) == CanonicalNanF32);
    CHECK(f32Bits(applyUnary(Opcode::F32Sqrt, f32(-1.0F))) == CanonicalNanF32);
    CHECK(f64Bits(applyBinary(Opcode::F64Div, f64(0.0), f64(0.0))) == CanonicalNanF64);
    CHECK(f64Bits(applyUnary(Opcode::F64PromoteF32, F32 { 0xFF80'0001 })) == CanonicalNanF64);
    CHECK(f32Bits(applyUnary(Opcode::F32DemoteF64, F64 { 0x7FF0'0000'0000'0001 })) == CanonicalNanF32);
    CHECK(f32Bits(applyBinary(Opcode::F32Add, f32(1.5F), f32(2.25F))) == std::bit_cast<uint32_t>(3.75F));
    CHECK(f64Bits(applyUnary(Opcode::F64PromoteF32, f32(0.1F))) == std::bit_cast<uint64_t>(double { 0.1F }));
    CHECK(f32Bits(applyUnary(Opcode::F32DemoteF64, f64(1e300))) == 0x7F80'0000);
}

TEST_CASE("Numerics.convertRoundsToNearestEven")
{
    // 2^24 + 1 is the first integer an f32 cannot hold: it ties, and rounds to the even 2^24.
    CHECK(f32Bits(applyUnary(Opcode::F32ConvertI32S, i32(16777217))) == std::bit_cast<uint32_t>(16777216.0F));
    CHECK(f32Bits(applyUnary(Opcode::F32ConvertI32S, i32(16777219))) == std::bit_cast<uint32_t>(16777220.0F));
    CHECK(f32Bits(applyUnary(Opcode::F32ConvertI32S, i32(-1))) == std::bit_cast<uint32_t>(-1.0F));
    CHECK(f32Bits(applyUnary(Opcode::F32ConvertI32S, I32 { Int32Min }))
          == std::bit_cast<uint32_t>(-2147483648.0F));
    CHECK(f32Bits(applyUnary(Opcode::F32ConvertI32U, I32 { 0xFFFF'FFFF }))
          == std::bit_cast<uint32_t>(4294967296.0F));
    CHECK(f32Bits(applyUnary(Opcode::F32ConvertI32U, i32(0))) == 0);
    // Rounding twice (through f64) would give 0x5F000000; the nearest f32 is one ulp above it.
    CHECK(f32Bits(applyUnary(Opcode::F32ConvertI64U, I64 { 0x8000'0080'0000'0001 })) == 0x5F00'0001);
    CHECK(f32Bits(applyUnary(Opcode::F32ConvertI64S, I64 { 0x7FFF'FFFF'FFFF'FFFF })) == 0x5F00'0000);
    CHECK(f64Bits(applyUnary(Opcode::F64ConvertI64U, I64 { ~uint64_t { 0 } })) == 0x43F0'0000'0000'0000);
    CHECK(f64Bits(applyUnary(Opcode::F64ConvertI64S, I64 { uint64_t { 1 } << 63 })) == 0xC3E0'0000'0000'0000);
    CHECK(f64Bits(applyUnary(Opcode::F64ConvertI64S, I64 { 9007199254740993 })) == 0x4340'0000'0000'0000);
    CHECK(f64Bits(applyUnary(Opcode::F64ConvertI32U, I32 { 0xFFFF'FFFF }))
          == std::bit_cast<uint64_t>(4294967295.0));
}

TEST_CASE("Numerics.rotl")
{
    CHECK(valueOf(applyBinary(Opcode::I32Rotl, I32 { 0x8000'0001 }, i32(1))) == Value { I32 { 3 } });
    CHECK(valueOf(applyBinary(Opcode::I32Rotr, I32 { 3 }, i32(1))) == Value { I32 { 0x8000'0001 } });
    CHECK(valueOf(applyBinary(Opcode::I32Rotl, I32 { 0x8000'0001 }, i32(33))) == Value { I32 { 3 } });
    CHECK(valueOf(applyBinary(Opcode::I64Rotl, I64 { 0x8000'0000'0000'0001 }, I64 { 65 }))
          == Value { I64 { 3 } });
}

TEST_CASE("Numerics.integerBits")
{
    CHECK(valueOf(applyBinary(Opcode::I32Shl, i32(1), i32(33))) == Value { I32 { 2 } });
    CHECK(valueOf(applyBinary(Opcode::I32ShrS, I32 { Int32Min }, i32(31))) == Value { i32(-1) });
    CHECK(valueOf(applyBinary(Opcode::I32ShrU, I32 { Int32Min }, i32(31))) == Value { I32 { 1 } });
    CHECK(valueOf(applyUnary(Opcode::I32Clz, i32(0))) == Value { I32 { 32 } });
    CHECK(valueOf(applyUnary(Opcode::I64Ctz, I64 { 0 })) == Value { I64 { 64 } });
    CHECK(valueOf(applyUnary(Opcode::I32Popcnt, i32(-1))) == Value { I32 { 32 } });
    CHECK(valueOf(applyUnary(Opcode::I32Extend8S, I32 { 0x80 })) == Value { i32(-128) });
    CHECK(valueOf(applyUnary(Opcode::I64Extend32S, I64 { 0x8000'0000 }))
          == Value { I64 { 0xFFFF'FFFF'8000'0000 } });
    CHECK(valueOf(applyUnary(Opcode::I64ExtendI32S, i32(-2))) == Value { I64 { ~uint64_t { 1 } } });
    CHECK(valueOf(applyUnary(Opcode::I64ExtendI32U, i32(-2))) == Value { I64 { 0xFFFF'FFFE } });
    CHECK(valueOf(applyUnary(Opcode::I32WrapI64, I64 { 0x1'2345'6789 })) == Value { I32 { 0x2345'6789 } });
    CHECK(valueOf(applyBinary(Opcode::I32Mul, I32 { 0x1'0001 }, I32 { 0x1'0001 }))
          == Value { I32 { 0x2'0001 } });
}

TEST_CASE("Numerics.testsAndComparisons")
{
    CHECK(applyTest(Opcode::I32Eqz, i32(0)) == I32 { 1 });
    CHECK(applyTest(Opcode::I64Eqz, I64 { 5 }) == I32 { 0 });
    CHECK(applyCompare(Opcode::I32LtS, i32(-1), i32(0)) == I32 { 1 });
    CHECK(applyCompare(Opcode::I32LtU, i32(-1), i32(0)) == I32 { 0 });
    CHECK(applyCompare(Opcode::I64GeU, I64 { 5 }, I64 { 5 }) == I32 { 1 });
    auto const nan = F32 { 0x7F80'0001 };
    CHECK(applyCompare(Opcode::F32Eq, nan, nan) == I32 { 0 });
    CHECK(applyCompare(Opcode::F32Ne, nan, nan) == I32 { 1 });
    CHECK(applyCompare(Opcode::F32Lt, f32(-0.0F), f32(0.0F)) == I32 { 0 });
    CHECK(applyCompare(Opcode::F32Eq, f32(-0.0F), f32(0.0F)) == I32 { 1 });
    CHECK(applyCompare(Opcode::F64Le, f64(1.0), f64(2.0)) == I32 { 1 });
}
