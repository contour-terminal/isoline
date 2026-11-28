// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Script.hpp"
#include "SpecCheck.hpp"
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::V128;
using isoline::runtime::Value;
using isoline::spectest::checkFailure;
using isoline::spectest::checkResults;
using isoline::spectest::describeAll;
using isoline::spectest::ExpectedValue;
using isoline::spectest::Failure;
using isoline::spectest::LaneShape;
using isoline::spectest::matches;
using isoline::spectest::Stage;
using isoline::spectest::stageName;

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

TEST_CASE("SpecCheck.lanePatterns")
{
    auto expected = pattern(Pattern::Lanes);
    expected.laneShape = LaneShape::F32x4;
    expected.lanes = { Pattern::CanonicalNan32, Pattern::Exact, Pattern::ArithmeticNan32, Pattern::Exact };
    expected.value = V128 { .lanes = { 0x3f80'0000'0000'0000, 0x0000'0000'0000'0000 } };
    CHECK(matches(expected, V128 { .lanes = { 0x3f80'0000'7fc0'0000, 0x0000'0000'ffc0'0001 } }));
    CHECK_FALSE(
        matches(expected, V128 { .lanes = { 0x3f80'0000'7fa0'0000, 0x0000'0000'7fc0'0000 } })); // lane 0
    CHECK_FALSE(
        matches(expected, V128 { .lanes = { 0x3f80'0001'7fc0'0000, 0x0000'0000'7fc0'0000 } })); // lane 1
    CHECK_FALSE(
        matches(expected, V128 { .lanes = { 0x3f80'0000'7fc0'0000, 0x0000'0001'7fc0'0000 } })); // lane 3
    CHECK_FALSE(matches(expected, F32 { 0x7fc0'0000 }));                                        // no vector

    auto wide = pattern(Pattern::Lanes);
    wide.laneShape = LaneShape::F64x2;
    wide.lanes = { Pattern::Exact, Pattern::ArithmeticNan64, Pattern::Exact, Pattern::Exact };
    wide.value = V128 { .lanes = { 0x3ff0'0000'0000'0000, 0 } };
    CHECK(matches(wide, V128 { .lanes = { 0x3ff0'0000'0000'0000, 0xfff8'0000'0000'0001 } }));
    CHECK_FALSE(matches(wide, V128 { .lanes = { 0x3ff0'0000'0000'0000, 0x7ff4'0000'0000'0000 } }));
    CHECK(describeAll(std::vector { expected, wide })
          == "v128:f32x4:nan:canonical,3f800000,nan:arithmetic,0 v128:f64x2:3ff0000000000000,nan:arithmetic");
}

TEST_CASE("SpecCheck.either")
{
    auto expected = pattern(Pattern::Either);
    expected.alternatives = { exactly(I32 { 1 }), exactly(I32 { 2 }) };
    CHECK(matches(expected, I32 { 1 }));
    CHECK(matches(expected, I32 { 2 }));
    CHECK_FALSE(matches(expected, I32 { 3 }));
    CHECK(describeAll(std::vector { expected }) == "either(i32:1;i32:2)");
}

TEST_CASE("SpecCheck.anyNull")
{
    CHECK(
        matches(pattern(Pattern::AnyNullRef), Ref { .kind = RefKind::Null, .heap = FuncHeap, .address = 0 }));
    CHECK(matches(pattern(Pattern::AnyNullRef),
                  Ref { .kind = RefKind::Null, .heap = ExternHeap, .address = 0 }));
    CHECK_FALSE(
        matches(pattern(Pattern::AnyNullRef), Ref { .kind = RefKind::Func, .heap = FuncHeap, .address = 3 }));
    CHECK_FALSE(matches(pattern(Pattern::AnyNullRef), I32 { 0 }));
    CHECK(describeAll(std::vector { pattern(Pattern::AnyNullRef) }) == "ref.null");
}

TEST_CASE("SpecCheck.newStages")
{
    CHECK(stageName(Stage::Exception) == "exception");
    CHECK(stageName(Stage::Custom) == "custom");
    // An exception is not a trap: assert_exception is not satisfied by an execute failure.
    auto const trap = Failure { .stage = Stage::Execute, .message = "unreachable" };
    CHECK_FALSE(checkFailure(Stage::Exception, "", trap).has_value());
}

TEST_CASE("SpecCheck.malformedCustomStage")
{
    // assert_malformed_custom is satisfied by a malformed custom annotation only: any other malformed
    // text is a Decode failure, which it does not accept, whatever the message.
    CHECK(stageName(Stage::MalformedCustom) == "malformed custom");
    auto const decode =
        Failure { .stage = Stage::Decode, .message = "@custom annotation: missing section name" };
    CHECK_FALSE(checkFailure(Stage::MalformedCustom, "@custom", decode).has_value());
    auto const custom =
        Failure { .stage = Stage::MalformedCustom, .message = "@custom annotation: missing section name" };
    CHECK(checkFailure(Stage::MalformedCustom, "@custom", custom).has_value());
    CHECK_FALSE(checkFailure(Stage::Decode, "@custom", custom).has_value());
}

TEST_CASE("SpecCheck.anyReferenceOfAKind")
{
    // (ref.extern) and (ref.exn) match any non-null reference of their kind, and nothing else.
    auto const extern1 = Ref { .kind = RefKind::Extern, .heap = ExternHeap, .address = 1 };
    auto const nullExtern = Ref { .kind = RefKind::Null, .heap = ExternHeap, .address = 0 };
    auto const exception = Ref { .kind = RefKind::Exn,
                                 .heap = { .kind = isoline::binary::AbstractHeapType::Exn },
                                 .address = 2 };
    CHECK(matches(pattern(Pattern::AnyExternRef), extern1));
    CHECK_FALSE(matches(pattern(Pattern::AnyExternRef), nullExtern));
    CHECK_FALSE(matches(pattern(Pattern::AnyExternRef), exception));
    CHECK(matches(pattern(Pattern::AnyExceptionRef), exception));
    CHECK_FALSE(matches(pattern(Pattern::AnyExceptionRef), extern1));
    CHECK_FALSE(matches(pattern(Pattern::AnyExceptionRef), I32 { 0 }));
    CHECK(describeAll(std::vector { pattern(Pattern::AnyExternRef), pattern(Pattern::AnyExceptionRef) })
          == "ref.extern ref.exn");
}

TEST_CASE("SpecCheck.nullOfAConcreteTypeIsANullFunc")
{
    // The suite writes the null of a function type (ref null $t) as ref.null func: a null matches the
    // expected null of the top of its heap's hierarchy.
    auto const nullFunc = Ref { .kind = RefKind::Null, .heap = FuncHeap, .address = 0 };
    auto const nullOfType =
        Ref { .kind = RefKind::Null,
              .heap = { .kind = isoline::binary::AbstractHeapType::Concrete, .typeIndex = 3 },
              .address = 0 };
    CHECK(matches(exactly(nullFunc), nullOfType));
    CHECK_FALSE(
        matches(exactly(Ref { .kind = RefKind::Null, .heap = ExternHeap, .address = 0 }), nullOfType));
    CHECK(describeAll(std::vector<Value> { nullOfType }) == "ref.null:func");
}

TEST_CASE("SpecCheck.exceptionReferencesAreSpelledExn")
{
    // An exnref, null or not, is named by its hierarchy, as ref.null:exn and ref.exn:<address>.
    auto const nullExn = Ref { .kind = RefKind::Null,
                               .heap = { .kind = isoline::binary::AbstractHeapType::Exn },
                               .address = 0 };
    auto const noExn = Ref { .kind = RefKind::Null,
                             .heap = { .kind = isoline::binary::AbstractHeapType::NoExn },
                             .address = 0 };
    auto const exception = Ref { .kind = RefKind::Exn,
                                 .heap = { .kind = isoline::binary::AbstractHeapType::Exn },
                                 .address = 3 };
    CHECK(describeAll(std::vector<Value> { nullExn, noExn, exception })
          == "ref.null:exn ref.null:exn ref.exn:3");
    CHECK(matches(exactly(nullExn), noExn));
    CHECK_FALSE(matches(exactly(nullExn), Ref { .kind = RefKind::Null, .heap = FuncHeap, .address = 0 }));
}

TEST_CASE("SpecCheck.messageRules")
{
    auto const trap = std::optional { Failure { .stage = Stage::Execute,
                                                .message = "wasm `unreachable` instruction executed" } };
    using isoline::spectest::MessageRule;
    CHECK(checkFailure(Stage::Execute, "unreachable", trap, MessageRule::Substring).has_value());
    CHECK_FALSE(checkFailure(Stage::Execute, "unreachable", trap, MessageRule::Prefix).has_value());
    CHECK(checkFailure(Stage::Execute, "wasm `unreachable`", trap, MessageRule::Prefix).has_value());
    CHECK_FALSE(checkFailure(Stage::Execute, "unreachable code", trap, MessageRule::Substring).has_value());
    CHECK_FALSE(checkFailure(Stage::Validate, "unreachable", trap, MessageRule::Substring).has_value());
    CHECK_FALSE(checkFailure(Stage::Execute, "", std::nullopt, MessageRule::Substring).has_value());
}

TEST_CASE("SpecCheck.skipReasons")
{
    using isoline::spectest::skipReasonOf;
    using isoline::spectest::SkipReasons;
    for (auto const reason: std::to_array<std::string_view>({
             "async (P12)",
             "gated 🧵",
             "gated 🧵②",
             "upstream nyi.txt",
             "core feature cut gc",
             "legacy text indices",
             "diverges from CM pin (definitions.py:1234)",
             "diverges from CM pin (Binary.md:350)",
         }))
    {
        INFO(reason);
        CHECK(skipReasonOf(reason) != nullptr);
    }
    CHECK(skipReasonOf("diverges from CM pin (Binary.md:350)") == &SkipReasons.back());
    for (auto const reason: std::to_array<std::string_view>({
             "",
             "because",
             "async (P12) later",
             "async",
             "gated 🐙",
             "gated ",
             "core feature cut ",
             "core feature cut two words",
             "diverges from CM pin (Binary.md:)",
             "diverges from CM pin (Binary.md:3x0)",
             "diverges from CM pin (Binary.md:350",
             "diverges from CM pin (Explainer.md:1)",
         }))
    {
        INFO(reason);
        CHECK(skipReasonOf(reason) == nullptr);
    }
}
