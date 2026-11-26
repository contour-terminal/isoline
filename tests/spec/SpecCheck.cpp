// SPDX-License-Identifier: Apache-2.0
#include "SpecCheck.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::spectest
{

namespace
{
    using runtime::Value;

    /// The names of the stages, by Stage.
    constexpr auto StageNames = std::to_array<std::string_view>({
        "harness",
        "decode",
        "validate",
        "link",
        "instantiate",
        "execute",
    });

    /// Whether @p actual is a @p Float whose bits, under @p mask, are @p required.
    template <typename Float>
    [[nodiscard]] bool isNan(Value const& actual, decltype(Float::bits) mask, decltype(Float::bits) required)
    {
        auto const* const value = std::get_if<Float>(&actual);
        return value && (value->bits & mask) == required;
    }

    [[nodiscard]] std::string describeExpected(ExpectedValue const& expected)
    {
        using Pattern = ExpectedValue::Pattern;
        switch (expected.pattern)
        {
            case Pattern::Exact: return spectest::describe(expected.value);
            case Pattern::CanonicalNan32: return "f32:nan:canonical";
            case Pattern::ArithmeticNan32: return "f32:nan:arithmetic";
            case Pattern::CanonicalNan64: return "f64:nan:canonical";
            case Pattern::ArithmeticNan64: return "f64:nan:arithmetic";
            case Pattern::AnyFunctionRef: return "ref.func";
        }
        return "?";
    }

    /// The tokens of @p values, as @p describeOne spells each, separated by spaces, or "(none)".
    template <typename T, typename Describe>
    [[nodiscard]] std::string joined(std::span<T const> values, Describe describeOne)
    {
        auto text = std::string {};
        for (auto const& value: values)
            text += (text.empty() ? "" : " ") + describeOne(value);
        return text.empty() ? std::string { "(none)" } : text;
    }
} // namespace

std::string_view stageName(Stage stage)
{
    return StageNames.at(static_cast<std::size_t>(stage));
}

bool matches(ExpectedValue const& expected, Value const& actual)
{
    using Pattern = ExpectedValue::Pattern;
    // Canonical: only the quiet bit of the payload; arithmetic: at least the quiet bit. Either sign.
    constexpr auto Abs32 = uint32_t { 0x7fff'ffff };
    constexpr auto Quiet32 = uint32_t { 0x7fc0'0000 };
    constexpr auto Abs64 = uint64_t { 0x7fff'ffff'ffff'ffff };
    constexpr auto Quiet64 = uint64_t { 0x7ff8'0000'0000'0000 };
    switch (expected.pattern)
    {
        case Pattern::CanonicalNan32: return isNan<runtime::F32>(actual, Abs32, Quiet32);
        case Pattern::ArithmeticNan32: return isNan<runtime::F32>(actual, Quiet32, Quiet32);
        case Pattern::CanonicalNan64: return isNan<runtime::F64>(actual, Abs64, Quiet64);
        case Pattern::ArithmeticNan64: return isNan<runtime::F64>(actual, Quiet64, Quiet64);
        case Pattern::AnyFunctionRef: {
            auto const* const ref = std::get_if<runtime::Ref>(&actual);
            return ref && ref->kind == runtime::RefKind::Func;
        }
        case Pattern::Exact: break;
    }
    auto const* const expectedRef = std::get_if<runtime::Ref>(&expected.value);
    auto const* const actualRef = std::get_if<runtime::Ref>(&actual);
    if (!expectedRef || !actualRef)
        return expected.value == actual;
    // A reference is compared by what it refers to; a null one by the heap it is the null of.
    if (expectedRef->kind != actualRef->kind)
        return false;
    if (expectedRef->kind == runtime::RefKind::Null)
        return expectedRef->heap.kind == actualRef->heap.kind;
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
            auto const* const heap = v.heap.kind == binary::AbstractHeapType::Extern ? "extern" : "func";
            if (v.kind == runtime::RefKind::Null)
                return std::format("ref.null:{}", heap);
            return std::format("ref.{}:{}", heap, v.address);
        }
    };
    return std::visit(Describe {}, value);
}

std::string describeAll(std::span<Value const> values)
{
    return joined(values, [](Value const& value) { return describe(value); });
}

std::string describeAll(std::span<ExpectedValue const> values)
{
    return joined(values, describeExpected);
}

} // namespace isoline::spectest
