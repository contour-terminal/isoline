// SPDX-License-Identifier: Apache-2.0
#include "SpecCheck.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/Subtyping.hpp>

namespace isoline::spectest
{

namespace
{
    using runtime::Value;
    using Pattern = ExpectedValue::Pattern;

    /// The names of the stages, by Stage.
    constexpr auto StageNames = std::to_array<std::string_view>({
        "harness",
        "decode",
        "validate",
        "link",
        "instantiate",
        "execute",
        "exception",
        "custom",
        "malformed custom",
    });

    // A NaN is canonical when, apart from the sign, only the quiet bit is set, and arithmetic when
    // at least the quiet bit is set.
    constexpr auto Abs32 = uint64_t { 0x7fff'ffff };
    constexpr auto Quiet32 = uint64_t { 0x7fc0'0000 };
    constexpr auto Abs64 = uint64_t { 0x7fff'ffff'ffff'ffff };
    constexpr auto Quiet64 = uint64_t { 0x7ff8'0000'0000'0000 };

    /// Whether @p bits, a float of the NaN pattern's width, are a NaN that @p nan allows.
    [[nodiscard]] bool isNan(Pattern nan, uint64_t bits)
    {
        switch (nan)
        {
            case Pattern::CanonicalNan32: return (bits & Abs32) == Quiet32;
            case Pattern::ArithmeticNan32: return (bits & Quiet32) == Quiet32;
            case Pattern::CanonicalNan64: return (bits & Abs64) == Quiet64;
            case Pattern::ArithmeticNan64: return (bits & Quiet64) == Quiet64;
            default: return false;
        }
    }

    /// Lane @p index of @p value, split into lanes of @p width bits.
    [[nodiscard]] uint64_t laneBits(runtime::V128 const& value, unsigned width, std::size_t index)
    {
        auto const offset = index * width;
        auto const word = value.lanes.at(offset / 64) >> (offset % 64);
        return width == 64 ? word : word & ((uint64_t { 1 } << width) - 1);
    }

    /// Whether each lane of @p actual matches its pattern in @p expected.
    [[nodiscard]] bool lanesMatch(ExpectedValue const& expected, Value const& actual)
    {
        auto const* const vector = std::get_if<runtime::V128>(&actual);
        auto const* const exactBits = std::get_if<runtime::V128>(&expected.value);
        if (!vector || !exactBits)
            return false;
        auto const& format = laneFormat(expected.laneShape);
        return std::ranges::all_of(std::views::iota(std::size_t { 0 }, format.count), [&](std::size_t index) {
            auto const lane = laneBits(*vector, format.width, index);
            auto const lanePattern = expected.lanes.at(index);
            return lanePattern == Pattern::Exact ? lane == laneBits(*exactBits, format.width, index)
                                                 : isNan(lanePattern, lane);
        });
    }

    /// The bits of @p actual when it is a @p Float, else nullopt.
    template <typename Float>
    [[nodiscard]] std::optional<uint64_t> floatBits(Value const& actual)
    {
        auto const* const value = std::get_if<Float>(&actual);
        return value ? std::optional<uint64_t> { value->bits } : std::nullopt;
    }

    [[nodiscard]] std::string describeLanes(ExpectedValue const& expected)
    {
        auto const& format = laneFormat(expected.laneShape);
        auto text = std::string { format.prefix };
        auto const* const exactBits = std::get_if<runtime::V128>(&expected.value);
        for (auto const index: std::views::iota(std::size_t { 0 }, format.count))
        {
            if (index != 0)
                text += ',';
            switch (expected.lanes.at(index))
            {
                case Pattern::CanonicalNan32:
                case Pattern::CanonicalNan64: text += "nan:canonical"; break;
                case Pattern::ArithmeticNan32:
                case Pattern::ArithmeticNan64: text += "nan:arithmetic"; break;
                default:
                    text += std::format("{:x}", exactBits ? laneBits(*exactBits, format.width, index) : 0);
                    break;
            }
        }
        return text;
    }

    /// The tokens of @p values, as @p describeOne spells each, separated by @p separator, or "(none)".
    template <typename T, typename Describe>
    [[nodiscard]] std::string joined(std::span<T const> values,
                                     Describe describeOne,
                                     std::string_view separator)
    {
        auto text = std::string {};
        for (auto const& value: values)
        {
            text += text.empty() ? std::string_view {} : separator;
            text += describeOne(value);
        }
        return text.empty() ? std::string { "(none)" } : text;
    }

    [[nodiscard]] std::string describeExpected(ExpectedValue const& expected);

    /// The token of the hierarchy whose top is @p top, as a reference's token names it.
    [[nodiscard]] std::string_view heapToken(binary::AbstractHeapType top)
    {
        switch (top)
        {
            case binary::AbstractHeapType::Extern: return "extern";
            case binary::AbstractHeapType::Exn: return "exn";
            default: return "func";
        }
    }

    [[nodiscard]] std::string describeEither(ExpectedValue const& expected)
    {
        return std::format(
            "either({})",
            joined(std::span<ExpectedValue const> { expected.alternatives }, describeExpected, ";"));
    }

    [[nodiscard]] std::string describeExpected(ExpectedValue const& expected)
    {
        switch (expected.pattern)
        {
            case Pattern::Exact: return spectest::describe(expected.value);
            case Pattern::CanonicalNan32: return "f32:nan:canonical";
            case Pattern::ArithmeticNan32: return "f32:nan:arithmetic";
            case Pattern::CanonicalNan64: return "f64:nan:canonical";
            case Pattern::ArithmeticNan64: return "f64:nan:arithmetic";
            case Pattern::AnyFunctionRef: return "ref.func";
            case Pattern::AnyExternRef: return "ref.extern";
            case Pattern::AnyExceptionRef: return "ref.exn";
            case Pattern::AnyNullRef: return "ref.null";
            case Pattern::Lanes: return describeLanes(expected);
            case Pattern::Either: return describeEither(expected);
        }
        return "?";
    }

} // namespace

std::string_view stageName(Stage stage)
{
    return StageNames.at(static_cast<std::size_t>(stage));
}

bool matches(ExpectedValue const& expected, Value const& actual)
{
    auto const* const actualRef = std::get_if<runtime::Ref>(&actual);
    switch (expected.pattern)
    {
        case Pattern::CanonicalNan32:
        case Pattern::ArithmeticNan32: {
            auto const bits = floatBits<runtime::F32>(actual);
            return bits && isNan(expected.pattern, *bits);
        }
        case Pattern::CanonicalNan64:
        case Pattern::ArithmeticNan64: {
            auto const bits = floatBits<runtime::F64>(actual);
            return bits && isNan(expected.pattern, *bits);
        }
        case Pattern::AnyFunctionRef: return actualRef && actualRef->kind == runtime::RefKind::Func;
        case Pattern::AnyExternRef: return actualRef && actualRef->kind == runtime::RefKind::Extern;
        case Pattern::AnyExceptionRef: return actualRef && actualRef->kind == runtime::RefKind::Exn;
        case Pattern::AnyNullRef: return actualRef && actualRef->kind == runtime::RefKind::Null;
        case Pattern::Lanes: return lanesMatch(expected, actual);
        case Pattern::Either:
            return std::ranges::any_of(expected.alternatives, [&](ExpectedValue const& alternative) {
                return matches(alternative, actual);
            });
        case Pattern::Exact: break;
    }
    auto const* const expectedRef = std::get_if<runtime::Ref>(&expected.value);
    if (!expectedRef || !actualRef)
        return expected.value == actual;
    // A reference is compared by what it refers to; a null one by the hierarchy of the heap it is
    // the null of, as the suite writes the null of a function type (ref null $t) as ref.null func.
    if (expectedRef->kind != actualRef->kind)
        return false;
    if (expectedRef->kind == runtime::RefKind::Null)
        return validate::topOf(expectedRef->heap.kind) == validate::topOf(actualRef->heap.kind);
    return expectedRef->address == actualRef->address;
}

CheckResult checkResults(std::span<ExpectedValue const> expected, std::span<Value const> actual)
{
    if (expected.size() == actual.size() && std::ranges::equal(expected, actual, &matches))
        return {};
    return std::unexpected { std::format("expected {}, got {}", describeAll(expected), describeAll(actual)) };
}

CheckResult checkFailure(Stage stage, std::string_view expectedMessage, std::optional<Failure> const& actual)
{
    if (!actual)
        return std::unexpected { std::format(
            "expected {} failure '{}', but it succeeded", stageName(stage), expectedMessage) };
    if (actual->stage == Stage::Harness)
        return std::unexpected { actual->message };
    if (actual->stage != stage || !actual->message.starts_with(expectedMessage))
        return std::unexpected { std::format("expected {} failure '{}', got {} failure '{}'",
                                             stageName(stage),
                                             expectedMessage,
                                             stageName(actual->stage),
                                             actual->message) };
    return {};
}

std::string describe(Value const& value)
{
    struct Describe
    {
        std::string operator()(runtime::I32 v) const { return std::format("i32:{:x}", v.bits); }
        std::string operator()(runtime::I64 v) const { return std::format("i64:{:x}", v.bits); }
        std::string operator()(runtime::F32 v) const { return std::format("f32:{:x}", v.bits); }
        std::string operator()(runtime::F64 v) const { return std::format("f64:{:x}", v.bits); }
        std::string operator()(runtime::V128 v) const
        {
            return std::format("v128:{:016x}{:016x}", v.lanes[1], v.lanes[0]);
        }
        std::string operator()(runtime::Ref const& v) const
        {
            auto const heap = heapToken(validate::topOf(v.heap.kind));
            if (v.kind == runtime::RefKind::Null)
                return std::format("ref.null:{}", heap);
            return std::format("ref.{}:{}", heap, v.address);
        }
    };
    return std::visit(Describe {}, value);
}

std::string describeAll(std::span<Value const> values)
{
    return joined(values, [](Value const& value) { return describe(value); }, " ");
}

std::string describeAll(std::span<ExpectedValue const> values)
{
    return joined(values, describeExpected, " ");
}

// Script.hpp declares it out of line: inlined, its recursion through alternatives reads to GCC's optimizer
// as a potential null dereference of an empty vector's data.
bool ExpectedValue::operator==(ExpectedValue const& other) const = default;

} // namespace isoline::spectest
