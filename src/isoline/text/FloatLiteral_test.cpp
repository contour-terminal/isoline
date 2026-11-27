// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <isoline/text/FloatLiteral.hpp>
#include <isoline/text/LiteralError.hpp>
#include <isoline/text/TextLimits.hpp>

using isoline::text::LiteralError;
using isoline::text::parseF32;
using isoline::text::parseF64;
using isoline::text::TextLimits;

// Every expected value here is checked against an exact rational computation (Python's `fractions`),
// and the edge cases against the spec suite's const.wast where it has them.

namespace
{

/// Reads an f32 literal with the default limits.
std::expected<uint32_t, LiteralError> f32(std::string_view text)
{
    return parseF32(text, TextLimits {});
}

/// Reads an f64 literal with the default limits.
std::expected<uint64_t, LiteralError> f64(std::string_view text)
{
    return parseF64(text, TextLimits {});
}

/// Exactly half of the smallest f32 subnormal, 2^-150, in decimal.
constexpr auto HalfSmallestF32Subnormal =
    std::string_view { "7."
                       "0064923216240853546186479164495806564013097093825788587853414194489554134293030074331"
                       "9094181060791015625e-46" };

/// The rounded bits of one literal.
template <typename Bits>
struct Rounding
{
    std::string_view text; ///< The literal.
    Bits bits;             ///< Its bits.
};

} // namespace

TEST_CASE("FloatLiteral.halfwayToEven")
{
    CHECK(f32("0x1.000001p0") == 0x3f80'0000U);
    CHECK(f32("0x1.0000010000000000001p0") == 0x3f80'0001U);
    CHECK(f32("1.00000005960464477539062500001") == 0x3f80'0001U);
    CHECK(f32("0x1.000003p0") == 0x3f80'0002U);
    CHECK(f64("0x1.00000000000008p0") == 0x3ff0'0000'0000'0000U);
    CHECK(f64("0x1.000000000000080000001p0") == 0x3ff0'0000'0000'0001U);
    CHECK(f64("9007199254740993") == 0x4340'0000'0000'0000U);
    CHECK(f32("16777217") == 0x4b80'0000U);
    CHECK(f32("16777219") == 0x4b80'0002U);
}

TEST_CASE("FloatLiteral.noDoubleRounding")
{
    // Around the tie between two f32s. The first two do not tell single from double rounding: the tie
    // itself goes to even either way, and so does one just below it. The third is the case that
    // detects double rounding (as halfwayToEven's 1.00000005960464477539062500001 does): f64 rounds
    // it down onto the tie, and ties-to-even then goes down again, where the exact value is above
    // the tie and rounds up.
    CHECK(f32("1.000000059604644775390625") == 0x3f80'0000U);
    CHECK(f32("1.000000059604644775390624") == 0x3f80'0000U);
    CHECK(f32("1.0000000596046447753906251") == 0x3f80'0001U);
}

TEST_CASE("FloatLiteral.subnormals")
{
    CHECK(f32("0x1p-149") == 0x0000'0001U);
    CHECK(f32("1e-46") == 0x0000'0000U);
    CHECK(f32("-0x1p-150") == 0x8000'0000U);
    CHECK(f32("0x1.8p-149") == 0x0000'0002U);
    CHECK(f32("0x1.7ffffffp-149") == 0x0000'0001U);
    CHECK(f32("0x0.000002p-126") == 0x0000'0001U);
    CHECK(f32("1.17549435e-38") == 0x0080'0000U);
    CHECK(f32("1.1754942e-38") == 0x007f'ffffU);
    CHECK(f64("4.9406564584124654e-324") == 1U);
    CHECK(f64("2.4703282292062328e-324") == 1U);
    CHECK(f64("2.4703282292062327e-324") == 0U);
    CHECK(f64("0x1p-1075") == 0U);
    CHECK(f64("0x1.8p-1075") == 1U);
    CHECK(f64("2.2250738585072014e-308") == 0x0010'0000'0000'0000U);
    // So far below the smallest subnormal that every quotient bit is dropped.
    CHECK(f64("1e-326") == 0U);
}

TEST_CASE("FloatLiteral.subnormalHalfway")
{
    CHECK(f32(HalfSmallestF32Subnormal) == 0x0000'0000U);
    auto above = std::string { HalfSmallestF32Subnormal };
    above.insert(above.find('e'), "1");
    CHECK(f32(above) == 0x0000'0001U);
}

TEST_CASE("FloatLiteral.outOfRange")
{
    // 0x1.fffffefp127 lies below the halfway point 0x1.ffffffp127, so it rounds to the largest f32
    // (const.wast:316 is valid for the same reason); the halfway point itself rounds to even, to
    // infinity (const.wast:327).
    CHECK(f32("0x1.fffffefp127") == 0x7f7f'ffffU);
    CHECK(f32("0x1.ffffffp127") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f32("-0x1.ffffffp127") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f32("1e39") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f32("1e38") == 0x7e96'7699U);
    CHECK(f32("0x1.fffffep127") == 0x7f7f'ffffU);
    CHECK(f32("3.4028235e38") == 0x7f7f'ffffU);
    CHECK(f32("3.40282357e38") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f64("0x1.fffffffffffffp1023") == 0x7fef'ffff'ffff'ffffU);
    CHECK(f64("0x1.fffffffffffff7ffffffp1023") == 0x7fef'ffff'ffff'ffffU);
    CHECK(f64("0x1.fffffffffffff8p1023") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f64("1.7976931348623157e308") == 0x7fef'ffff'ffff'ffffU);
    CHECK(f64("1.7976931348623159e308") == std::unexpected { LiteralError::OutOfRange });

    // The exponent saturates, and the clamp answers before any bignum is built.
    CHECK(f32("1e99999999999999999999") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f64("1e99999999999999999999") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f64("0x1p99999999999999999999") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f64("1e-99999999999999999999") == 0U);
    CHECK(f64("-0x1p-99999999999999999999") == 0x8000'0000'0000'0000U);
    CHECK(f64("0e99999999999999999999") == 0U);
}

TEST_CASE("FloatLiteral.nan")
{
    CHECK(f32("nan") == 0x7fc0'0000U);
    CHECK(f32("-nan") == 0xffc0'0000U);
    CHECK(f32("+nan") == 0x7fc0'0000U);
    CHECK(f32("-nan:0x200000") == 0xffa0'0000U);
    CHECK(f32("nan:0x1") == 0x7f80'0001U);
    CHECK(f32("nan:0x7f_ffff") == 0x7fff'ffffU);
    CHECK(f32("nan:0x0000000000000000000001") == 0x7f80'0001U);
    CHECK(f32("nan:0x0") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f32("nan:0x800000") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f32("nan:0xffffffffffffffffffffffffffff") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f64("nan") == 0x7ff8'0000'0000'0000U);
    CHECK(f64("nan:0xf_ffff_ffff_ffff") == 0x7fff'ffff'ffff'ffffU);
    CHECK(f64("nan:0x0") == std::unexpected { LiteralError::OutOfRange });
    CHECK(f64("nan:0x10_0000_0000_0000") == std::unexpected { LiteralError::OutOfRange });
    for (auto const text: { "nan:0x", "nan:0x1_", "nan:0xg", "nan:1", "nanx" })
        CHECK(f32(text) == std::unexpected { LiteralError::NotANumber });
}

TEST_CASE("FloatLiteral.infinityAndZero")
{
    CHECK(f32("inf") == 0x7f80'0000U);
    CHECK(f32("+inf") == 0x7f80'0000U);
    CHECK(f32("-inf") == 0xff80'0000U);
    CHECK(f64("-inf") == 0xfff0'0000'0000'0000U);
    CHECK(f32("0") == 0U);
    CHECK(f32("-0") == 0x8000'0000U);
    CHECK(f64("-0.0e0") == 0x8000'0000'0000'0000U);
    CHECK(f64("0x0p0") == 0U);
}

TEST_CASE("FloatLiteral.shapes")
{
    auto const f64Cases = std::to_array<Rounding<uint64_t>>({
        { .text = "0.1", .bits = 0x3fb9'9999'9999'999aU },
        // Its remainder outgrows the divisor by a limb while the quotient is taken.
        { .text = "6e-12", .bits = 0x3d9a'6366'41c4'df1aU },
        { .text = "1e23", .bits = 0x44b5'2d02'c7e1'4af6U },
        { .text = "000.000123e3", .bits = 0x3fbf'7ced'9168'72b0U },
        { .text = "1.e5", .bits = 0x40f8'6a00'0000'0000U },
        { .text = "1E-5", .bits = 0x3ee4'f8b5'88e3'68f1U },
        { .text = "1e+5", .bits = 0x40f8'6a00'0000'0000U },
        { .text = "1_0e0_1", .bits = 0x4059'0000'0000'0000U },
        { .text = "0x1.p2", .bits = 0x4010'0000'0000'0000U },
        { .text = "0x1P-2", .bits = 0x3fd0'0000'0000'0000U },
        { .text = "0x1p+2", .bits = 0x4010'0000'0000'0000U },
        { .text = "123456789012345678901234567890", .bits = 0x45f8'ee90'ff6c'373eU },
    });
    for (auto const& [text, bits]: f64Cases)
    {
        INFO(text);
        CHECK(f64(text) == bits);
    }
    CHECK(f32("0.1") == 0x3dcc'cccdU);
    CHECK(f32("0xABCDEFp-3") == 0x49ab'cdefU);
    CHECK(f32("0x1.fffffe8p127") == 0x7f7f'ffffU);
    for (auto const text: { "",
                            "-",
                            ".5",
                            "1e",
                            "1e+",
                            "0x",
                            "0x.1",
                            "0x1p",
                            "1_",
                            "1._5",
                            "1.5_",
                            "1p2",
                            "in",
                            "infinity",
                            "--1",
                            "1x" })
    {
        INFO(text);
        CHECK(f64(text) == std::unexpected { LiteralError::NotANumber });
    }
}

TEST_CASE("FloatLiteral.tooLong")
{
    CHECK(f64(std::string(10'000, '1')) == std::unexpected { LiteralError::TooLong });
    CHECK(f64("0." + std::string(3'998, '0') + "1") == 0U);
    CHECK(f32("0." + std::string(44, '0') + "1") == 0x0000'0001U);
    CHECK(parseF32("1.5", TextLimits { .maxNumberLength = 2 }) == std::unexpected { LiteralError::TooLong });
}

TEST_CASE("FloatLiteral.leadingZerosAreNotSignificant")
{
    CHECK(f64(std::string(4'000, '0') + "1e300") == 0x7e37'e43c'8800'759cU);
    CHECK(f64("0x" + std::string(4'000, '0') + "1p1023") == 0x7fe0'0000'0000'0000U);
    // Trailing zeros are exact powers of the radix, not digits to multiply.
    CHECK(f64("1" + std::string(300, '0')) == 0x7e37'e43c'8800'759cU);
    CHECK(f64("1" + std::string(310, '0')) == std::unexpected { LiteralError::OutOfRange });
}

TEST_CASE("FloatLiteral.longMantissas")
{
    // 4 000 significant digits each way: the exact arithmetic grows with the digits only.
    CHECK(f64("1." + std::string(3'990, '0') + "1") == 0x3ff0'0000'0000'0000U);
    CHECK(f64("0x1." + std::string(3'990, '0') + "1p0") == 0x3ff0'0000'0000'0000U);
    CHECK(f64(std::string(310, '9') + "." + std::string(3'000, '9'))
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(f32("0x1.000001" + std::string(3'000, '0') + "1p0") == 0x3f80'0001U);
}
