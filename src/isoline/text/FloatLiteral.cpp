// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/FloatLiteral.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/text/Digits.hpp>
#include <isoline/text/LiteralError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

namespace
{

    /// An IEEE 754 binary format, and the reach of its literals that the clamp tests before any exact
    /// arithmetic: a literal of `n` significant digits (decimal) or bits (hexadecimal) times radix^e
    /// lies in [radix^(n+e-1), radix^(n+e)).
    struct FloatFormat
    {
        uint32_t width = 0;             ///< The bits of the format.
        uint32_t precision = 0;         ///< The bits of the significand, the implicit one included.
        int64_t minQuantumExponent = 0; ///< The exponent of the smallest subnormal, 2^this.
        /// The largest n + e a decimal literal may have: above it, the literal is at least 10^(n+e-1),
        /// beyond the largest finite value.
        int64_t maxDecimalMagnitude = 0;
        /// The smallest n + e a decimal literal is rounded for: below it, the literal is under 10^(n+e),
        /// less than half the smallest subnormal, so it is 0.
        int64_t minDecimalMagnitude = 0;
        /// The largest n + e a hexadecimal literal may have: above it, the literal is at least 2^(emax+1).
        int64_t maxBinaryMagnitude = 0;
        /// The smallest n + e a hexadecimal literal is rounded for: below it, the literal is at most half
        /// the smallest subnormal, which rounds to even, to 0.
        int64_t minBinaryMagnitude = 0;
    };

    /// binary32: the largest finite value is under 3.5 * 10^38, half the smallest subnormal 2^-150.
    constexpr auto F32Format = FloatFormat { .width = 32,
                                             .precision = 24,
                                             .minQuantumExponent = -149,
                                             .maxDecimalMagnitude = 39,
                                             .minDecimalMagnitude = -46,
                                             .maxBinaryMagnitude = 128,
                                             .minBinaryMagnitude = -149 };

    /// binary64: the largest finite value is under 1.8 * 10^308, half the smallest subnormal 2^-1075.
    constexpr auto F64Format = FloatFormat { .width = 64,
                                             .precision = 53,
                                             .minQuantumExponent = -1074,
                                             .maxDecimalMagnitude = 309,
                                             .minDecimalMagnitude = -325,
                                             .maxBinaryMagnitude = 1024,
                                             .minBinaryMagnitude = -1074 };

    /// The sign bit of a format.
    constexpr uint64_t signBitOf(FloatFormat const& format) noexcept
    {
        return uint64_t { 1 } << (format.width - 1);
    }

    /// The bits of +∞ in a format: every exponent bit set, the significand clear.
    constexpr uint64_t infinityOf(FloatFormat const& format) noexcept
    {
        return ((uint64_t { 1 } << (format.width - format.precision)) - 1) << (format.precision - 1);
    }

    /// How a mantissa is written: its digits, how many of them fit one bignum step, and what its
    /// exponent counts.
    struct Notation
    {
        Radix radix = Radix::Decimal;     ///< The mantissa's digits.
        std::size_t prefixLength = 0;     ///< The length of its prefix, `0x` or none.
        uint32_t base = 0;                ///< The value of one digit position.
        uint32_t chunkDigits = 0;         ///< How many digits one 32-bit multiply-add takes.
        uint32_t chunkFactor = 0;         ///< base^chunkDigits.
        std::string_view exponentMarkers; ///< The characters that start its exponent.
        int64_t exponentPerDigit = 0;     ///< How far one digit position moves the exponent.
    };

    /// Decimal: the exponent is of 10, one per digit.
    constexpr auto DecimalNotation = Notation { .radix = Radix::Decimal,
                                                .prefixLength = 0,
                                                .base = 10,
                                                .chunkDigits = 9,
                                                .chunkFactor = 1'000'000'000,
                                                .exponentMarkers = "eE",
                                                .exponentPerDigit = 1 };

    /// Hexadecimal: the exponent is of 2, four per digit.
    constexpr auto HexadecimalNotation = Notation { .radix = Radix::Hexadecimal,
                                                    .prefixLength = 2,
                                                    .base = 16,
                                                    .chunkDigits = 7,
                                                    .chunkFactor = 1U << 28U,
                                                    .exponentMarkers = "pP",
                                                    .exponentPerDigit = 4 };

    /// Where a written exponent saturates: far beyond every clamp, however many digits the mantissa
    /// has, and far from overflowing the sums it enters.
    constexpr auto ExponentSaturation = int64_t { 1'000'000'000'000'000 };

    /// A natural number of any size, as little-endian 32-bit limbs without leading zero limbs. Only
    /// what exact rounding needs: products by small factors, shifts, comparison and subtraction. Every
    /// product is formed in 64 bits.
    class Natural
    {
      public:
        /// Builds the number @p value.
        explicit Natural(uint32_t value)
        {
            if (value != 0)
                _limbs.push_back(value);
        }

        /// Sets the number to number * @p factor + @p addend.
        void multiplyAdd(uint32_t factor, uint32_t addend)
        {
            auto carry = uint64_t { addend };
            for (auto& limb: _limbs)
            {
                auto const product = (uint64_t { limb } * factor) + carry;
                limb = static_cast<uint32_t>(product);
                carry = product >> 32U;
            }
            if (carry != 0)
                _limbs.push_back(static_cast<uint32_t>(carry));
        }

        /// Multiplies the number by 10^@p exponent.
        void multiplyByPowerOfTen(uint64_t exponent)
        {
            for ([[maybe_unused]] auto const chunk: std::views::iota(uint64_t { 0 }, exponent / 9))
                multiplyAdd(DecimalNotation.chunkFactor, 0);
            auto factor = uint32_t { 1 };
            for ([[maybe_unused]] auto const digit: std::views::iota(uint64_t { 0 }, exponent % 9))
                factor *= 10;
            multiplyAdd(factor, 0);
        }

        /// Multiplies the number by 2^@p bits.
        void shiftLeft(uint64_t bits)
        {
            if (_limbs.empty())
                return;
            _limbs.insert(_limbs.begin(), static_cast<std::size_t>(bits / 32), 0);
            auto const shift = static_cast<uint32_t>(bits % 32);
            auto carry = uint32_t { 0 };
            for (auto& limb: _limbs)
            {
                auto const wide = uint64_t { limb } << shift;
                limb = static_cast<uint32_t>(wide) | carry;
                carry = static_cast<uint32_t>(wide >> 32U);
            }
            if (carry != 0)
                _limbs.push_back(carry);
        }

        /// Subtracts @p other, which must not be larger.
        void subtract(Natural const& other)
        {
            auto borrow = uint64_t { 0 };
            for (auto const index: std::views::iota(std::size_t { 0 }, _limbs.size()))
            {
                auto const subtrahend =
                    (index < other._limbs.size() ? uint64_t { other._limbs[index] } : 0) + borrow;
                auto const minuend = uint64_t { _limbs[index] };
                borrow = minuend < subtrahend ? 1 : 0;
                _limbs[index] = static_cast<uint32_t>(minuend + (borrow << 32U) - subtrahend);
            }
            while (!_limbs.empty() && _limbs.back() == 0)
                _limbs.pop_back();
        }

        /// The number of bits the number needs.
        /// @return Its bit width; the number must not be zero.
        [[nodiscard]] uint64_t bitWidth() const noexcept
        {
            Require(!_limbs.empty());
            return ((_limbs.size() - 1) * 32) + std::bit_width(_limbs.back());
        }

        /// Whether the number is zero.
        [[nodiscard]] bool isZero() const noexcept { return _limbs.empty(); }

        /// Compares two numbers by value.
        [[nodiscard]] std::strong_ordering operator<=>(Natural const& other) const noexcept
        {
            if (_limbs.size() != other._limbs.size())
                return _limbs.size() <=> other._limbs.size();
            return std::lexicographical_compare_three_way(
                _limbs.rbegin(), _limbs.rend(), other._limbs.rbegin(), other._limbs.rend());
        }

      private:
        std::vector<uint32_t> _limbs; ///< The limbs, least significant first.
    };

    /// Rounds numerator / denominator * 2^@p exponent to the nearest value of @p format, ties to even.
    ///
    /// It scales one of the two so that the quotient has precision + 3 or + 4 bits, takes them by
    /// shift-and-subtract, and lets a non-zero remainder stand for every bit below them (the sticky
    /// bit): that decides every tie, at the format's precision and in its subnormal range alike.
    /// @return The bits of the positive result, or OutOfRange when it rounds to infinity.
    std::expected<uint64_t, LiteralError> roundQuotient(Natural numerator,
                                                        Natural denominator,
                                                        int64_t exponent,
                                                        FloatFormat const& format)
    {
        auto const quotientBits = static_cast<int64_t>(format.precision) + 4;
        auto const scale =
            (quotientBits - 1)
            - (static_cast<int64_t>(numerator.bitWidth()) - static_cast<int64_t>(denominator.bitWidth()));
        if (scale >= 0)
            numerator.shiftLeft(static_cast<uint64_t>(scale));
        else
            denominator.shiftLeft(static_cast<uint64_t>(-scale));

        // Now numerator < 2^quotientBits * denominator. Rather than shifting the divisor right per
        // quotient bit, the remainder is shifted left against a fixed divisor * 2^(quotientBits - 1).
        denominator.shiftLeft(static_cast<uint64_t>(quotientBits - 1));
        auto quotient = uint64_t { 0 };
        for ([[maybe_unused]] auto const bit: std::views::iota(int64_t { 0 }, quotientBits))
        {
            quotient <<= 1U;
            if (numerator >= denominator)
            {
                numerator.subtract(denominator);
                quotient |= 1U;
            }
            numerator.shiftLeft(1);
        }
        auto const sticky = !numerator.isZero();

        // The value is (quotient + fraction) * 2^lowestExponent; it keeps `precision` bits from its
        // leading one, but none below the smallest subnormal's.
        auto const lowestExponent = exponent - scale;
        auto const leadingExponent = lowestExponent + std::bit_width(quotient) - 1;
        auto const quantumExponent = std::max(leadingExponent - (static_cast<int64_t>(format.precision) - 1),
                                              format.minQuantumExponent);
        auto const dropped = quantumExponent - lowestExponent;
        // The quotient's leading one lies at least precision + 2 bits above its lowest bit, and the
        // quantum at most precision - 1 below that one, so at least the three guard bits are dropped,
        // and the half below is a bit of the quotient.
        Require(dropped > 0);
        auto significand = uint64_t { 0 };
        if (dropped < 64)
        {
            significand = quotient >> dropped;
            auto const rest = quotient & ((uint64_t { 1 } << dropped) - 1);
            auto const half = uint64_t { 1 } << (dropped - 1);
            if (rest > half || (rest == half && (sticky || (significand & 1U) != 0)))
                ++significand;
        }

        // A normal significand has its leading one at `precision - 1`, which adds one to the biased
        // exponent field; so the exponent field and the significand sum, and a significand that
        // rounded up to 2^precision carries into the exponent by itself.
        auto const bits =
            (static_cast<uint64_t>(quantumExponent - format.minQuantumExponent) << (format.precision - 1))
            + significand;
        if (bits >= infinityOf(format))
            return std::unexpected { LiteralError::OutOfRange };
        return bits;
    }

    /// What the first pass over a mantissa found, without any arithmetic: where its significant
    /// digits are, and its exponent.
    struct MantissaScan
    {
        int64_t digits = 0;                        ///< Every digit, of the integral part and the fraction.
        int64_t fractionDigits = 0;                ///< The digits after the point.
        std::optional<int64_t> firstSignificant;   ///< The index of the first non-zero digit, if any.
        int64_t lastSignificant = 0;               ///< The index of the last non-zero digit.
        uint32_t leadingDigit = 0;                 ///< The value of the first non-zero digit.
        std::optional<std::size_t> fractionOffset; ///< Where the fraction's digits start, if any.
        int64_t exponent = 0;                      ///< The written exponent, saturated.
    };

    /// Checks a mantissa's shape and finds its significant digits (spec §6.3.2).
    /// @return The scan, or nothing when @p text is no mantissa of @p notation.
    std::optional<MantissaScan> scanMantissa(std::string_view text, Notation const& notation)
    {
        auto scan = MantissaScan {};
        auto const visit = [&](uint32_t digit) {
            if (digit != 0)
            {
                if (!scan.firstSignificant)
                {
                    scan.firstSignificant = scan.digits;
                    scan.leadingDigit = digit;
                }
                scan.lastSignificant = scan.digits;
            }
            ++scan.digits;
        };
        auto const integralEnd = readDigits(text, notation.prefixLength, notation.radix, visit);
        if (!integralEnd)
            return std::nullopt;
        auto position = *integralEnd;
        auto const integralDigits = scan.digits;
        if (position < text.size() && text[position] == '.')
        {
            ++position;
            if (auto const fractionEnd = readDigits(text, position, notation.radix, visit))
            {
                scan.fractionOffset = position;
                position = *fractionEnd;
            }
        }
        scan.fractionDigits = scan.digits - integralDigits;

        if (position < text.size() && notation.exponentMarkers.contains(text[position]))
        {
            ++position;
            auto const isNegative = position < text.size() && text[position] == '-';
            if (position < text.size() && (isNegative || text[position] == '+'))
                ++position;
            auto magnitude = int64_t { 0 };
            auto const accumulate = [&](uint32_t digit) {
                magnitude = std::min((magnitude * 10) + digit, ExponentSaturation);
            };
            auto const exponentEnd = readDigits(text, position, Radix::Decimal, accumulate);
            if (!exponentEnd)
                return std::nullopt;
            position = *exponentEnd;
            scan.exponent = isNegative ? -magnitude : magnitude;
        }
        if (position != text.size())
            return std::nullopt;
        return scan;
    }

    /// Reads the significant digits of a scanned mantissa as one number.
    Natural significantDigitsOf(std::string_view text, Notation const& notation, MantissaScan const& scan)
    {
        auto value = Natural { 0 };
        auto index = int64_t { 0 };
        auto chunk = uint32_t { 0 };
        auto chunkFactor = uint32_t { 1 };
        auto const visit = [&](uint32_t digit) {
            if (index >= *scan.firstSignificant && index <= scan.lastSignificant)
            {
                chunk = (chunk * notation.base) + digit;
                chunkFactor *= notation.base;
                if (chunkFactor == notation.chunkFactor)
                {
                    value.multiplyAdd(chunkFactor, chunk);
                    chunk = 0;
                    chunkFactor = 1;
                }
            }
            ++index;
        };
        (void) readDigits(text, notation.prefixLength, notation.radix, visit);
        if (scan.fractionOffset)
            (void) readDigits(text, *scan.fractionOffset, notation.radix, visit);
        value.multiplyAdd(chunkFactor, chunk);
        return value;
    }

    /// Reads a finite literal's unsigned text.
    /// @return The positive value's bits, or NotANumber or OutOfRange.
    std::expected<uint64_t, LiteralError> parseMantissa(std::string_view text, FloatFormat const& format)
    {
        auto const& notation = text.starts_with("0x") ? HexadecimalNotation : DecimalNotation;
        auto const scan = scanMantissa(text, notation);
        if (!scan)
            return std::unexpected { LiteralError::NotANumber };
        if (!scan->firstSignificant)
            return 0;

        // The value is D * radix^exponent, D the significant digits: the trailing zeros, like the
        // fraction, only move the exponent.
        auto const significantDigits = scan->lastSignificant - *scan->firstSignificant + 1;
        auto const exponent =
            scan->exponent
            + (notation.exponentPerDigit * (scan->digits - 1 - scan->lastSignificant - scan->fractionDigits));
        auto const isDecimal = notation.radix == Radix::Decimal;
        // n of the clamp: decimal digits, or bits.
        auto const width = isDecimal ? significantDigits
                                     : (4 * (significantDigits - 1)) + std::bit_width(scan->leadingDigit);
        auto const magnitude = width + exponent;
        if (magnitude > (isDecimal ? format.maxDecimalMagnitude : format.maxBinaryMagnitude))
            return std::unexpected { LiteralError::OutOfRange };
        if (magnitude < (isDecimal ? format.minDecimalMagnitude : format.minBinaryMagnitude))
            return 0;

        auto numerator = significantDigitsOf(text, notation, *scan);
        auto denominator = Natural { 1 };
        if (!isDecimal)
            return roundQuotient(std::move(numerator), std::move(denominator), exponent, format);
        if (exponent >= 0)
            numerator.multiplyByPowerOfTen(static_cast<uint64_t>(exponent));
        else
            denominator.multiplyByPowerOfTen(static_cast<uint64_t>(-exponent));
        return roundQuotient(std::move(numerator), std::move(denominator), 0, format);
    }

    /// Reads a NaN's payload, the hexadecimal digits after `nan:0x`.
    /// @return The NaN's bits, or NotANumber, or OutOfRange when the payload is zero or does not fit
    ///         the significand.
    std::expected<uint64_t, LiteralError> parseNanPayload(std::string_view digits, FloatFormat const& format)
    {
        // Saturates at the first value that does not fit, so any number of digits reads.
        auto const limit = uint64_t { 1 } << (format.precision - 1);
        auto payload = uint64_t { 0 };
        auto const accumulate = [&](uint32_t digit) {
            payload = std::min((payload * 16) + digit, limit);
        };
        if (readDigits(digits, 0, Radix::Hexadecimal, accumulate) != digits.size())
            return std::unexpected { LiteralError::NotANumber };
        if (payload == 0 || payload == limit)
            return std::unexpected { LiteralError::OutOfRange };
        return infinityOf(format) | payload;
    }

    /// Reads a float literal of @p format.
    /// @return Its bits, or why it has none.
    std::expected<uint64_t, LiteralError> parseFloat(std::string_view text,
                                                     FloatFormat const& format,
                                                     TextLimits const& limits)
    {
        if (text.size() > limits.maxNumberLength)
            return std::unexpected { LiteralError::TooLong };
        auto const sign = text.starts_with('-') ? signBitOf(format) : 0;
        if (text.starts_with('-') || text.starts_with('+'))
            text.remove_prefix(1);

        constexpr auto NanPayloadPrefix = std::string_view { "nan:0x" };
        auto const bits = [&]() -> std::expected<uint64_t, LiteralError> {
            if (text == "inf")
                return infinityOf(format);
            if (text == "nan")
                return infinityOf(format) | (uint64_t { 1 } << (format.precision - 2));
            if (text.starts_with(NanPayloadPrefix))
                return parseNanPayload(text.substr(NanPayloadPrefix.size()), format);
            return parseMantissa(text, format);
        }();
        return bits.transform([&](uint64_t value) { return value | sign; });
    }

} // namespace

std::expected<uint32_t, LiteralError> parseF32(std::string_view text, TextLimits const& limits)
{
    return parseFloat(text, F32Format, limits).transform([](uint64_t bits) {
        return static_cast<uint32_t>(bits);
    });
}

std::expected<uint64_t, LiteralError> parseF64(std::string_view text, TextLimits const& limits)
{
    return parseFloat(text, F64Format, limits);
}

} // namespace isoline::text
