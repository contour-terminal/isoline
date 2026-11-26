// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <limits>

#include <isoline/numerics/Float.hpp>
#include <isoline/numerics/Integer.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::numerics::CanonicalNanF32;
using isoline::numerics::CanonicalNanF64;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::TrapCode;

namespace
{

constexpr auto Int32Min = static_cast<uint32_t>(std::numeric_limits<int32_t>::min());
constexpr auto MinusOne = std::numeric_limits<uint32_t>::max();

// The integer kernels stay usable in constant expressions, for constant folding.
static_assert(isoline::numerics::add(I32 { 0xFFFF'FFFF }, I32 { 2 }).bits == 1U);
static_assert(isoline::numerics::divS(I32 { 7 }, I32 { MinusOne })->bits == 0xFFFF'FFF9U); // -7
static_assert(!isoline::numerics::divU(I64 { 1 }, I64 { 0 }).has_value());
static_assert(isoline::numerics::nearest(F64 { std::bit_cast<uint64_t>(2.5) }).bits
              == std::bit_cast<uint64_t>(2.0));
static_assert(isoline::numerics::mulWideS(std::numeric_limits<uint64_t>::max(), 2)
              == isoline::numerics::Halves { std::numeric_limits<uint64_t>::max() - 1,
                                             std::numeric_limits<uint64_t>::max() });

} // namespace

TEST_CASE("Numerics.kernelsAreReachableOutsideTheInterpreter")
{
    SECTION("an integer kernel traps as the instruction does")
    {
        auto const overflow = isoline::numerics::divS(I32 { Int32Min }, I32 { MinusOne });
        REQUIRE_FALSE(overflow.has_value());
        CHECK(overflow.error().code == TrapCode::IntegerOverflow);

        auto const byZero = isoline::numerics::remU(I64 { 1 }, I64 { 0 });
        REQUIRE_FALSE(byZero.has_value());
        CHECK(byZero.error().code == TrapCode::IntegerDivideByZero);
    }

    SECTION("a float kernel canonicalizes a NaN result")
    {
        // A signalling NaN with a payload: the host would propagate it, the kernel must not.
        CHECK(isoline::numerics::fadd(F32 { 0x7F80'0001 }, F32 { std::bit_cast<uint32_t>(1.0F) }).bits
              == CanonicalNanF32);
        CHECK(
            isoline::numerics::fmul(F64 { 0xFFF0'0000'0000'00FF }, F64 { std::bit_cast<uint64_t>(2.0) }).bits
            == CanonicalNanF64);
        CHECK(isoline::numerics::fsub(F64 { std::bit_cast<uint64_t>(1.5) },
                                      F64 { std::bit_cast<uint64_t>(0.25) })
                  .bits
              == std::bit_cast<uint64_t>(1.25));
    }

    SECTION("a conversion reaches across the two headers")
    {
        auto const truncated = isoline::numerics::truncTrapping<I32, int32_t>(F64 { CanonicalNanF64 });
        REQUIRE_FALSE(truncated.has_value());
        CHECK(truncated.error().code == TrapCode::InvalidConversionToInteger);
        CHECK(isoline::numerics::convert<F32, uint64_t>(I64 { std::numeric_limits<uint64_t>::max() }).bits
              == std::bit_cast<uint32_t>(0x1p64F));
    }
}
