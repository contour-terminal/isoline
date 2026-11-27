// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <isoline/text/IntegerLiteral.hpp>
#include <isoline/text/LiteralError.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

using isoline::text::IntegerSyntax;
using isoline::text::IntegerWidth;
using isoline::text::LiteralError;
using isoline::text::parseInteger;
using isoline::text::TextErrorCode;
using isoline::text::textErrorCodeOf;
using isoline::text::TextLimits;

namespace
{

/// Reads @p text with the default limits.
std::expected<uint64_t, LiteralError> integer(std::string_view text, IntegerWidth width, IntegerSyntax syntax)
{
    return parseInteger(text, width, syntax, TextLimits {});
}

} // namespace

TEST_CASE("IntegerLiteral.ranges")
{
    CHECK(integer("255", IntegerWidth::I8, IntegerSyntax::Either) == 0xFFU);
    CHECK(integer("-128", IntegerWidth::I8, IntegerSyntax::Either) == 0x80U);
    CHECK(integer("256", IntegerWidth::I8, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("-129", IntegerWidth::I8, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("-1", IntegerWidth::I32, IntegerSyntax::Unsigned)
          == std::unexpected { LiteralError::NotANumber });
    CHECK(integer("+1", IntegerWidth::I32, IntegerSyntax::Unsigned)
          == std::unexpected { LiteralError::NotANumber });
    CHECK(integer("+1", IntegerWidth::I32, IntegerSyntax::Either) == 1U);
    // As the reference interpreter and wasm-tools, not the formal grammar: `+` admits the unsigned range.
    CHECK(integer("+255", IntegerWidth::I8, IntegerSyntax::Either) == 0xFFU);
    CHECK(integer("+256", IntegerWidth::I8, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("-1", IntegerWidth::I16, IntegerSyntax::Either) == 0xFFFFU);
    CHECK(integer("65535", IntegerWidth::I16, IntegerSyntax::Unsigned) == 0xFFFFU);
    CHECK(integer("-0", IntegerWidth::I32, IntegerSyntax::Either) == 0U);

    // const.wast:264-304.
    CHECK(integer("4294967295", IntegerWidth::I32, IntegerSyntax::Either) == 0xFFFF'FFFFU);
    CHECK(integer("-2147483648", IntegerWidth::I32, IntegerSyntax::Either) == 0x8000'0000U);
    CHECK(integer("4294967296", IntegerWidth::I32, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("-2147483649", IntegerWidth::I32, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("18446744073709551615", IntegerWidth::I64, IntegerSyntax::Either) == UINT64_MAX);
    CHECK(integer("-9223372036854775808", IntegerWidth::I64, IntegerSyntax::Either)
          == 0x8000'0000'0000'0000U);
    CHECK(integer("18446744073709551616", IntegerWidth::I64, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("-9223372036854775809", IntegerWidth::I64, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("99999999999999999999999", IntegerWidth::I64, IntegerSyntax::Unsigned)
          == std::unexpected { LiteralError::OutOfRange });
}

TEST_CASE("IntegerLiteral.hexAndUnderscores")
{
    CHECK(integer("0x_1", IntegerWidth::I32, IntegerSyntax::Either)
          == std::unexpected { LiteralError::NotANumber });
    CHECK(integer("1__0", IntegerWidth::I32, IntegerSyntax::Either)
          == std::unexpected { LiteralError::NotANumber });
    CHECK(integer("1_", IntegerWidth::I32, IntegerSyntax::Either)
          == std::unexpected { LiteralError::NotANumber });
    CHECK(integer("_1", IntegerWidth::I32, IntegerSyntax::Either)
          == std::unexpected { LiteralError::NotANumber });
    CHECK(integer("0xFFFF_FFFF", IntegerWidth::I32, IntegerSyntax::Either) == 0xFFFF'FFFFU);
    CHECK(integer("-0x8000_0000", IntegerWidth::I32, IntegerSyntax::Either) == 0x8000'0000U);
    CHECK(integer("-0x8000_0000_0000_0000", IntegerWidth::I64, IntegerSyntax::Either)
          == 0x8000'0000'0000'0000U);
    CHECK(integer("0x1_0000_0000", IntegerWidth::I32, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("-0x8000_0001", IntegerWidth::I32, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("0x1_0000_0000_0000_0000", IntegerWidth::I64, IntegerSyntax::Either)
          == std::unexpected { LiteralError::OutOfRange });
    CHECK(integer("0_123_456_789", IntegerWidth::I32, IntegerSyntax::Either) == 123'456'789U);
    CHECK(integer("0x0_9acf_fBDF", IntegerWidth::I32, IntegerSyntax::Either) == 0x9ACF'FBDFU);
    CHECK(integer("0x0125_6789_ADEF_bcef", IntegerWidth::I64, IntegerSyntax::Either)
          == 0x0125'6789'ADEF'BCEFU);
}

TEST_CASE("IntegerLiteral.malformed")
{
    for (auto const text: { "", "-", "+", "0x", "1x", "0xg", "1.0", "--1", "0X1", "1e3", "inf" })
        CHECK(integer(text, IntegerWidth::I64, IntegerSyntax::Either)
              == std::unexpected { LiteralError::NotANumber });
}

TEST_CASE("IntegerLiteral.tooLong")
{
    auto const zeros = std::string(4'095, '0');
    CHECK(integer(zeros + "7", IntegerWidth::I32, IntegerSyntax::Either) == 7U);
    CHECK(integer(zeros + "07", IntegerWidth::I32, IntegerSyntax::Either)
          == std::unexpected { LiteralError::TooLong });
    CHECK(parseInteger("1234", IntegerWidth::I32, IntegerSyntax::Either, TextLimits { .maxNumberLength = 3 })
          == std::unexpected { LiteralError::TooLong });
}

TEST_CASE("LiteralError.textErrorCodes")
{
    CHECK(textErrorCodeOf(LiteralError::NotANumber) == TextErrorCode::UnknownOperator);
    CHECK(textErrorCodeOf(LiteralError::OutOfRange) == TextErrorCode::ConstantOutOfRange);
    CHECK(textErrorCodeOf(LiteralError::TooLong) == TextErrorCode::NumberTooLong);
}
