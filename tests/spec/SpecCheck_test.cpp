// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "IsotestReader.hpp"
#include "SpecCheck.hpp"
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Value;
using isoline::spectest::checkFailure;
using isoline::spectest::checkResults;
using isoline::spectest::ExpectedValue;
using isoline::spectest::Failure;
using isoline::spectest::matches;
using isoline::spectest::Stage;

namespace
{

using Pattern = ExpectedValue::Pattern;

ExpectedValue exactly(Value value)
{
    return ExpectedValue { .pattern = Pattern::Exact, .value = value };
}

ExpectedValue pattern(Pattern kind)
{
    return ExpectedValue { .pattern = kind, .value = {} };
}

constexpr auto FuncHeap = isoline::binary::HeapType { .kind = isoline::binary::AbstractHeapType::Func };
constexpr auto ExternHeap = isoline::binary::HeapType { .kind = isoline::binary::AbstractHeapType::Extern };

} // namespace

TEST_CASE("SpecCheck.exactValuesCompareBits")
{
    CHECK(matches(exactly(I32 { 3 }), I32 { 3 }));
    CHECK_FALSE(matches(exactly(I32 { 3 }), I32 { 4 }));
    CHECK_FALSE(matches(exactly(I32 { 3 }), I64 { 3 })); // same bits, another type
    // A float compares bits: +0 is not -0, and a NaN's payload must match.
    CHECK(matches(exactly(F32 { 0x7fc0'0001 }), F32 { 0x7fc0'0001 }));
    CHECK_FALSE(matches(exactly(F32 { 0x0000'0000 }), F32 { 0x8000'0000 }));
    CHECK_FALSE(matches(exactly(F64 { 0x7ff8'0000'0000'0001 }), F64 { 0x7ff8'0000'0000'0000 }));
}

TEST_CASE("SpecCheck.referencesCompareByReferent")
{
    auto const nullFunc = Ref { .kind = RefKind::Null, .heap = FuncHeap, .address = 0 };
    auto const nullExtern = Ref { .kind = RefKind::Null, .heap = ExternHeap, .address = 0 };
    auto const extern1 = Ref { .kind = RefKind::Extern, .heap = ExternHeap, .address = 1 };
    CHECK(matches(exactly(nullFunc), nullFunc));
    CHECK_FALSE(matches(exactly(nullFunc), nullExtern));
    CHECK(matches(exactly(extern1), extern1));
    CHECK_FALSE(matches(exactly(extern1), Ref { .kind = RefKind::Extern, .heap = ExternHeap, .address = 2 }));
    CHECK_FALSE(matches(exactly(nullExtern), extern1));
    auto const function = Ref { .kind = RefKind::Func, .heap = FuncHeap, .address = 7 };
    CHECK(matches(pattern(Pattern::AnyFunctionRef), function));
    CHECK_FALSE(matches(pattern(Pattern::AnyFunctionRef), nullFunc));
}

TEST_CASE("SpecCheck.canonicalNan")
{
    CHECK(matches(pattern(Pattern::CanonicalNan32), F32 { 0x7fc0'0000 }));
    CHECK(matches(pattern(Pattern::CanonicalNan32), F32 { 0xffc0'0000 }));       // either sign
    CHECK_FALSE(matches(pattern(Pattern::CanonicalNan32), F32 { 0x7fc0'0001 })); // arithmetic, not canonical
    CHECK_FALSE(matches(pattern(Pattern::CanonicalNan32), F32 { 0x7f80'0000 })); // infinity
    CHECK_FALSE(matches(pattern(Pattern::CanonicalNan32), F64 { 0x7ff8'0000'0000'0000 })); // wrong width
    CHECK(matches(pattern(Pattern::CanonicalNan64), F64 { 0x7ff8'0000'0000'0000 }));
    CHECK(matches(pattern(Pattern::CanonicalNan64), F64 { 0xfff8'0000'0000'0000 }));
    CHECK_FALSE(matches(pattern(Pattern::CanonicalNan64), F64 { 0x7ff8'0000'0000'0001 }));
    CHECK_FALSE(matches(pattern(Pattern::CanonicalNan64), F32 { 0x7fc0'0000 }));
}

TEST_CASE("SpecCheck.arithmeticNan")
{
    CHECK(matches(pattern(Pattern::ArithmeticNan32), F32 { 0x7fc0'0000 }));
    CHECK(matches(pattern(Pattern::ArithmeticNan32), F32 { 0xffc0'0123 }));
    CHECK_FALSE(matches(pattern(Pattern::ArithmeticNan32), F32 { 0x7fa0'0000 })); // signalling NaN
    CHECK_FALSE(matches(pattern(Pattern::ArithmeticNan32), F32 { 0x3f80'0000 })); // 1.0
    CHECK_FALSE(matches(pattern(Pattern::ArithmeticNan32), I32 { 0x7fc0'0000 })); // not a float
    CHECK(matches(pattern(Pattern::ArithmeticNan64), F64 { 0x7ff8'0000'0000'0001 }));
    CHECK_FALSE(matches(pattern(Pattern::ArithmeticNan64), F64 { 0x7ff4'0000'0000'0000 })); // signalling
    CHECK_FALSE(matches(pattern(Pattern::ArithmeticNan64), F64 { 0x7ff0'0000'0000'0000 })); // infinity
}

TEST_CASE("SpecCheck.results")
{
    auto const expected = std::vector { exactly(I32 { 1 }), pattern(Pattern::CanonicalNan32) };
    CHECK(checkResults(expected, std::vector<Value> { I32 { 1 }, F32 { 0x7fc0'0000 } }).has_value());
    auto const mismatch = checkResults(expected, std::vector<Value> { I32 { 2 }, F32 { 0x7fc0'0000 } });
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(mismatch.error() == "expected i32:1 f32:nan:canonical, got i32:2 f32:7fc00000");
    // Too few and too many results fail, even when the ones present match.
    CHECK_FALSE(checkResults(expected, std::vector<Value> { I32 { 1 } }).has_value());
    CHECK_FALSE(
        checkResults(expected, std::vector<Value> { I32 { 1 }, F32 { 0x7fc0'0000 }, I32 { 0 } }).has_value());
    CHECK_FALSE(checkResults({}, std::vector<Value> { I32 { 0 } }).has_value());
    CHECK(checkResults({}, {}).has_value());
}

TEST_CASE("SpecCheck.failures")
{
    auto const trap = Failure { .stage = Stage::Execute, .message = "uninitialized element 2" };
    CHECK(checkFailure(Stage::Execute, "uninitialized element 2", trap).has_value());
    CHECK(checkFailure(Stage::Execute, "uninitialized element", trap).has_value()); // a prefix
    // The spec's rule is a prefix, so an empty expected message accepts any message at the stage.
    CHECK(checkFailure(Stage::Execute, "", trap).has_value());

    auto const wrongMessage = checkFailure(Stage::Execute, "undefined element", trap);
    REQUIRE_FALSE(wrongMessage.has_value());
    CHECK(wrongMessage.error()
          == "expected execute failure 'undefined element', got execute failure 'uninitialized element 2'");
    // The actual message must start with the expected one, not merely contain it.
    CHECK_FALSE(checkFailure(Stage::Execute, "element 2", trap).has_value());
    // A longer expected message than the actual one is no prefix of it.
    CHECK_FALSE(checkFailure(Stage::Execute, "uninitialized element 23", trap).has_value());

    // The right message at the wrong stage fails: assert_invalid wants the validator, not the decoder.
    auto const decodeError = Failure { .stage = Stage::Decode, .message = "type mismatch" };
    CHECK_FALSE(checkFailure(Stage::Validate, "type mismatch", decodeError).has_value());
    CHECK_FALSE(checkFailure(Stage::Validate, "", decodeError).has_value());

    // Success where a failure was expected fails.
    auto const succeeded = checkFailure(Stage::Execute, "unreachable", std::nullopt);
    REQUIRE_FALSE(succeeded.has_value());
    CHECK(succeeded.error() == "expected execute failure 'unreachable', but it succeeded");

    // A broken harness satisfies no assertion, whatever its message says.
    auto const harness = Failure { .stage = Stage::Harness, .message = "cannot open a.wasm" };
    auto const broken = checkFailure(Stage::Harness, "", harness);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error() == "cannot open a.wasm");
}
