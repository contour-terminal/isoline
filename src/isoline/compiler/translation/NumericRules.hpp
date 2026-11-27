// SPDX-License-Identifier: Apache-2.0
#pragma once

// How translation turns a WebAssembly numeric instruction into IR, as data: which operation each opcode
// is, which checks precede it, and whether Deterministic mode canonicalizes its NaN results. Every trap
// a numeric instruction can raise is an explicit trapIf before it, in the order the reference's
// numerics:: kernels check: a zero divisor before a signed overflow, a NaN before an out-of-range value.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

#include <isoline/binary/Opcode.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>

namespace isoline::compiler::translation
{

/// A run of WebAssembly numeric opcodes that are, in order, a run of IR operations of the same names.
struct NumericRun
{
    binary::Opcode first;              ///< Its first opcode.
    binary::Opcode last;               ///< Its last opcode, included.
    intermediate::Operation operation; ///< The operation of its first opcode.
};

/// Every numeric opcode the compiler translates one to one, the constants included: the MVP's and sign
/// extension's run, then the saturating truncations.
inline constexpr auto NumericRunTable = std::to_array<NumericRun>({
    { .first = binary::Opcode::I32Const,
      .last = binary::Opcode::I64Extend32S,
      .operation = intermediate::Operation::I32Const },
    { .first = binary::Opcode::I32TruncSatF32S,
      .last = binary::Opcode::I64TruncSatF64U,
      .operation = intermediate::Operation::I32TruncSatF32S },
});

namespace detail
{
    /// Whether every opcode of every run is named as the operation it maps to.
    [[nodiscard]] consteval bool runsMatchByName() noexcept
    {
        for (auto const& run: NumericRunTable)
        {
            auto const first = std::size_t { std::to_underlying(run.first) };
            auto const count = std::size_t { std::to_underlying(run.last) } - first + 1;
            auto const operation = std::size_t { std::to_underlying(run.operation) };
            if (operation + count > intermediate::OperationCount)
                return false;
            for (auto const offset: std::views::iota(std::size_t { 0 }, count))
                if (binary::OpcodeTable[first + offset].text
                    != intermediate::OperationTable[operation + offset].text)
                    return false;
        }
        return true;
    }
} // namespace detail

static_assert(detail::runsMatchByName(), "a numeric run must map each opcode onto the operation of its name");

/// The operation of a numeric @p opcode.
/// @return It; none for an opcode no run holds.
[[nodiscard]] constexpr std::optional<intermediate::Operation> numericOperationOf(
    binary::Opcode opcode) noexcept
{
    for (auto const& run: NumericRunTable)
        if (std::to_underlying(run.first) <= std::to_underlying(opcode)
            && std::to_underlying(opcode) <= std::to_underlying(run.last))
            return static_cast<intermediate::Operation>(std::to_underlying(run.operation)
                                                        + std::to_underlying(opcode)
                                                        - std::to_underlying(run.first));
    return std::nullopt;
}

/// What translation checks before a numeric operation.
enum class NumericCheck : uint8_t
{
    None,                  ///< Nothing: it cannot trap.
    ZeroDivisor,           ///< div_u, rem_u, rem_s: IntegerDivideByZero when the divisor is 0.
    ZeroDivisorOrOverflow, ///< div_s: that, then IntegerOverflow for the minimum divided by -1.
    Truncation, ///< A trapping float-to-int: InvalidConversionToInteger for a NaN, then IntegerOverflow
                ///< outside the TruncationBoundTable row's open interval.
};

/// Whether Deterministic mode canonicalizes an operation's result: the operations that may produce a NaN
/// from their operands.
enum class NanResult : uint8_t
{
    Kept,          ///< Never produces a NaN it was not given bit for bit, or produces none.
    Canonicalized, ///< May produce one: Deterministic mode follows it with canonicalizeNan.
};

/// One operation's translation rule.
struct NumericRuleRow
{
    intermediate::Operation operation; ///< The operation.
    NumericCheck check;                ///< What precedes it.
    NanResult nan;                     ///< Whether Deterministic mode canonicalizes it.
};

/// The numeric operations with a check or a canonicalized result; every other is neither.
inline constexpr auto NumericRuleTable = [] {
    using enum intermediate::Operation;
    using enum NumericCheck;
    using enum NanResult;
    return std::to_array<NumericRuleRow>({
        { I32DivS, ZeroDivisorOrOverflow, Kept }, { I32DivU, ZeroDivisor, Kept },
        { I32RemS, ZeroDivisor, Kept },           { I32RemU, ZeroDivisor, Kept },
        { I64DivS, ZeroDivisorOrOverflow, Kept }, { I64DivU, ZeroDivisor, Kept },
        { I64RemS, ZeroDivisor, Kept },           { I64RemU, ZeroDivisor, Kept },
        { I32TruncF32S, Truncation, Kept },       { I32TruncF32U, Truncation, Kept },
        { I32TruncF64S, Truncation, Kept },       { I32TruncF64U, Truncation, Kept },
        { I64TruncF32S, Truncation, Kept },       { I64TruncF32U, Truncation, Kept },
        { I64TruncF64S, Truncation, Kept },       { I64TruncF64U, Truncation, Kept },
        { F32Ceil, None, Canonicalized },         { F32Floor, None, Canonicalized },
        { F32Trunc, None, Canonicalized },        { F32Nearest, None, Canonicalized },
        { F32Sqrt, None, Canonicalized },         { F32Add, None, Canonicalized },
        { F32Sub, None, Canonicalized },          { F32Mul, None, Canonicalized },
        { F32Div, None, Canonicalized },          { F32Min, None, Canonicalized },
        { F32Max, None, Canonicalized },          { F64Ceil, None, Canonicalized },
        { F64Floor, None, Canonicalized },        { F64Trunc, None, Canonicalized },
        { F64Nearest, None, Canonicalized },      { F64Sqrt, None, Canonicalized },
        { F64Add, None, Canonicalized },          { F64Sub, None, Canonicalized },
        { F64Mul, None, Canonicalized },          { F64Div, None, Canonicalized },
        { F64Min, None, Canonicalized },          { F64Max, None, Canonicalized },
        { F32DemoteF64, None, Canonicalized },    { F64PromoteF32, None, Canonicalized },
    });
}();

/// The rule of @p operation.
/// @return Its NumericRuleTable row; for an operation without one, no check and a kept result.
[[nodiscard]] constexpr NumericRuleRow numericRuleOf(intermediate::Operation operation) noexcept
{
    for (auto const& row: NumericRuleTable)
        if (row.operation == operation)
            return row;
    return NumericRuleRow { .operation = operation, .check = NumericCheck::None, .nan = NanResult::Kept };
}

/// The open interval of the values a trapping truncation converts: the operand traps IntegerOverflow
/// unless lower < operand < upper, as floats of the operand's type. Each bound is the nearest float of
/// that type outside the integral range, so that the interval holds exactly the values whose integral
/// part the result type holds (numerics::truncTrapping).
struct TruncationBoundRow
{
    intermediate::Operation operation; ///< The truncation.
    uint64_t lower;                    ///< The exclusive lower bound's bits, an f32's zero-extended.
    uint64_t upper;                    ///< The exclusive upper bound's bits.
};

/// Every trapping truncation's bounds.
inline constexpr auto TruncationBoundTable = std::to_array<TruncationBoundRow>({
    // -2^31 - 256 (the f32 below -2^31), 2^31.
    { .operation = intermediate::Operation::I32TruncF32S, .lower = 0xCF00'0001, .upper = 0x4F00'0000 },
    // -1, 2^32.
    { .operation = intermediate::Operation::I32TruncF32U, .lower = 0xBF80'0000, .upper = 0x4F80'0000 },
    // -2^31 - 1, 2^31.
    { .operation = intermediate::Operation::I32TruncF64S,
      .lower = 0xC1E0'0000'0020'0000,
      .upper = 0x41E0'0000'0000'0000 },
    // -1, 2^32.
    { .operation = intermediate::Operation::I32TruncF64U,
      .lower = 0xBFF0'0000'0000'0000,
      .upper = 0x41F0'0000'0000'0000 },
    // The f32 below -2^63, 2^63.
    { .operation = intermediate::Operation::I64TruncF32S, .lower = 0xDF00'0001, .upper = 0x5F00'0000 },
    // -1, 2^64.
    { .operation = intermediate::Operation::I64TruncF32U, .lower = 0xBF80'0000, .upper = 0x5F80'0000 },
    // The f64 below -2^63, 2^63.
    { .operation = intermediate::Operation::I64TruncF64S,
      .lower = 0xC3E0'0000'0000'0001,
      .upper = 0x43E0'0000'0000'0000 },
    // -1, 2^64.
    { .operation = intermediate::Operation::I64TruncF64U,
      .lower = 0xBFF0'0000'0000'0000,
      .upper = 0x43F0'0000'0000'0000 },
});

/// The bounds of truncation @p operation.
/// @return Its row; none for an operation that is no trapping truncation.
[[nodiscard]] constexpr std::optional<TruncationBoundRow> truncationBoundsOf(
    intermediate::Operation operation) noexcept
{
    for (auto const& row: TruncationBoundTable)
        if (row.operation == operation)
            return row;
    return std::nullopt;
}

/// The operations a check of one operand type is built from.
struct CheckOperationsRow
{
    intermediate::Type type;          ///< The operand type.
    intermediate::Operation constant; ///< Its constant.
    intermediate::Operation isZero;   ///< eqz (integers).
    intermediate::Operation equal;    ///< eq (integers), ne (floats: x ne x is a NaN).
    intermediate::Operation greater;  ///< gt (floats).
    intermediate::Operation less;     ///< lt (floats).
    uint64_t minimum;                 ///< The signed minimum's bits (integers).
    uint64_t minusOne;                ///< -1's bits (integers).
};

/// The check operations of every numeric operand type, in Type order.
inline constexpr auto CheckOperationsTable = [] {
    using enum intermediate::Operation;
    using intermediate::Type;
    return std::to_array<CheckOperationsRow>({
        { Type::I32, I32Const, I32Eqz, I32Eq, I32GtS, I32LtS, 0x8000'0000, 0xFFFF'FFFF },
        { Type::I64, I64Const, I64Eqz, I64Eq, I64GtS, I64LtS, 0x8000'0000'0000'0000, 0xFFFF'FFFF'FFFF'FFFF },
        { Type::F32, F32Const, Removed, F32Ne, F32Gt, F32Lt, 0, 0 },
        { Type::F64, F64Const, Removed, F64Ne, F64Gt, F64Lt, 0, 0 },
    });
}();

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CheckOperationsTable)
            if (std::to_underlying(row.type) != index++)
                return false;
        return true;
    }(),
    "CheckOperationsTable must hold one row per numeric Type, at the index of its enumerator");

/// The check operations of numeric @p type.
[[nodiscard]] constexpr CheckOperationsRow const& checkOperationsOf(intermediate::Type type) noexcept
{
    return CheckOperationsTable[std::to_underlying(type)];
}

} // namespace isoline::compiler::translation
