// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/validate/ValidationError.hpp>
#include <isoline/validate/Validator.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::binary::DecodeErrorCode;
using isoline::binary::decodeModule;
using isoline::binary::Export;
using isoline::binary::ExternKind;
using isoline::binary::F32;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Import;
using isoline::binary::Limits;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::binary::testing::ModuleBuilder;
using isoline::validate::NoFunction;
using isoline::validate::NoInstruction;
using isoline::validate::specMessage;
using isoline::validate::validateModule;
using isoline::validate::ValidationError;
using isoline::validate::ValidationErrorCode;

namespace
{

/// Limits of at least @p min, unbounded.
Limits atLeast(uint64_t min)
{
    return Limits { .min = min, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 };
}

/// Limits from @p min to @p max.
Limits between(uint64_t min, uint64_t max)
{
    return Limits { .min = min, .max = max, .addressType = isoline::binary::AddressType::I32 };
}

/// Decodes and validates @p builder's module; fails the test when it does not decode.
std::expected<void, ValidationError> validated(ModuleBuilder const& builder,
                                               FeatureSet const& features = FeatureSet::wasm2())
{
    auto const bytes = builder.build();
    auto const module = decodeModule(bytes, features);
    if (!module)
        UNSCOPED_INFO("decode error " << static_cast<int>(module.error().code) << " at "
                                      << module.error().offset);
    REQUIRE(module.has_value());
    return validateModule(*module, features);
}

/// The error validating @p builder's module reports, or nullopt when it is valid.
std::optional<ValidationErrorCode> errorOf(ModuleBuilder const& builder,
                                           FeatureSet const& features = FeatureSet::wasm2())
{
    auto const result = validated(builder, features);
    if (result)
        return std::nullopt;
    return result.error().code;
}

/// A module of one function of type @p type, with @p locals, whose body is @p body.
ModuleBuilder functionOf(FuncType type,
                         std::vector<uint8_t> body,
                         std::vector<isoline::binary::ValueType> locals = {})
{
    auto builder = ModuleBuilder {};
    builder.type(std::move(type)).function(0, std::move(locals), std::move(body));
    return builder;
}

/// A module of one function [] -> [] whose body is @p body.
ModuleBuilder voidFunctionOf(std::vector<uint8_t> body)
{
    return functionOf(FuncType {}, std::move(body));
}

/// A module of one function [] -> [i32] whose body is @p body.
ModuleBuilder i32FunctionOf(std::vector<uint8_t> body)
{
    return functionOf(FuncType { .params = {}, .results = { I32 } }, std::move(body));
}

} // namespace

TEST_CASE("Validator.acceptsAdd")
{
    auto const builder =
        functionOf(FuncType { .params = { I32, I32 }, .results = { I32 } }, { 0x20, 0x00, 0x20, 0x01, 0x6A });
    CHECK(validated(builder).has_value());
}

TEST_CASE("Validator.typeMismatch")
{
    auto const result = validated(i32FunctionOf({ 0x42, 0x00 }));
    REQUIRE_FALSE(result.has_value());
    // Found at the final end, where the body's results are popped.
    CHECK(result.error() == ValidationError { ValidationErrorCode::TypeMismatch, 0, 1 });
    // An operand of the wrong type, and one missing.
    CHECK(errorOf(i32FunctionOf({ 0x42, 0x00, 0x41, 0x00, 0x6A })) == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(i32FunctionOf({ 0x41, 0x00, 0x6A })) == ValidationErrorCode::TypeMismatch);
    // A value left over.
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x00 })) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.wideNeedsTwoResults")
{
    auto const features = FeatureSet::wasm3().with(Feature::WideArithmetic);
    auto const mulWideU = std::vector<uint8_t> { 0x20, 0x00, 0x20, 0x01, 0xFC, 0x16 };
    auto const add128 = std::vector<uint8_t> { 0x20, 0x00, 0x20, 0x01, 0x20, 0x00, 0x20, 0x01, 0xFC, 0x13 };
    CHECK(errorOf(functionOf(FuncType { .params = { I64, I64 }, .results = { I64 } }, mulWideU), features)
          == ValidationErrorCode::TypeMismatch);
    CHECK(
        errorOf(functionOf(FuncType { .params = { I64, I64 }, .results = { I64, I64 } }, mulWideU), features)
        == std::nullopt);
    CHECK(errorOf(functionOf(FuncType { .params = { I64, I64 }, .results = { I64, I64 } }, add128), features)
          == std::nullopt);
    // Three operands are one too few.
    CHECK(errorOf(functionOf(FuncType { .params = { I64, I64 }, .results = { I64, I64 } },
                             { 0x20, 0x00, 0x20, 0x01, 0x20, 0x00, 0xFC, 0x13 }),
                  features)
          == ValidationErrorCode::TypeMismatch);
    // An i32 operand is not an i64.
    CHECK(
        errorOf(functionOf(FuncType { .params = { I64, I32 }, .results = { I64, I64 } }, mulWideU), features)
        == ValidationErrorCode::TypeMismatch);
    // Without the proposal the instruction does not decode.
    auto const bytes =
        functionOf(FuncType { .params = { I64, I64 }, .results = { I64, I64 } }, mulWideU).build();
    auto const decoded = decodeModule(bytes, FeatureSet::wasm3());
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == DecodeErrorCode::IllegalOpcode);
}

TEST_CASE("Validator.unreachablePolymorphic")
{
    CHECK(errorOf(i32FunctionOf({ 0x00, 0x6A })) == std::nullopt);
    // The polymorphic stack still types what is pushed onto it.
    CHECK(errorOf(i32FunctionOf({ 0x00, 0x42, 0x00, 0x6A })) == ValidationErrorCode::TypeMismatch);
    // After br and return the rest of the block is unreachable too.
    CHECK(errorOf(i32FunctionOf({ 0x41, 0x00, 0x0C, 0x00, 0x6A })) == std::nullopt);
    CHECK(errorOf(i32FunctionOf({ 0x41, 0x00, 0x0F, 0x1A, 0x1A })) == std::nullopt);
    CHECK(errorOf(i32FunctionOf({ 0x0F })) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.unknownLocal")
{
    auto const result = validated(functionOf(FuncType {}, { 0x20, 0x05, 0x1A }, { I32 }));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ValidationError { ValidationErrorCode::UnknownLocal, 0, 0, 5 });
}

TEST_CASE("Validator.locals")
{
    // (i32) -> (i64) with locals f32 i64 i64: index 0 the param, 1 the f32, 2 and 3 the i64s.
    auto const type = FuncType { .params = { I32 }, .results = { I64 } };
    auto const locals = std::vector { F32, I64, I64 };
    CHECK(errorOf(functionOf(type, { 0x20, 0x03 }, locals)) == std::nullopt);
    CHECK(errorOf(functionOf(type, { 0x20, 0x02 }, locals)) == std::nullopt);
    CHECK(errorOf(functionOf(type, { 0x20, 0x01 }, locals)) == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(functionOf(type, { 0x20, 0x04 }, locals)) == ValidationErrorCode::UnknownLocal);
    // local.set pops, local.tee pops and pushes.
    CHECK(errorOf(functionOf(type, { 0x20, 0x00, 0x21, 0x00, 0x20, 0x02 }, locals)) == std::nullopt);
    CHECK(errorOf(functionOf(type, { 0x42, 0x00, 0x22, 0x03 }, locals)) == std::nullopt);
    CHECK(errorOf(functionOf(type, { 0x42, 0x00, 0x21, 0x00, 0x20, 0x02 }, locals))
          == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.brTableArity")
{
    // block (result i32) block i32.const 0 br_table 0 1 end i32.const 0 end drop: label 0 carries
    // nothing, label 1 (the default) an i32.
    auto const result = validated(voidFunctionOf(
        { 0x02, 0x7F, 0x02, 0x40, 0x41, 0x00, 0x0E, 0x01, 0x00, 0x01, 0x0B, 0x41, 0x00, 0x0B, 0x1A }));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ValidationError { ValidationErrorCode::TypeMismatch, 0, 3 });
    // Labels of one arity: block (result i32) i32.const 1 i32.const 0 br_table 0 0 end drop.
    CHECK(errorOf(voidFunctionOf({ 0x02, 0x7F, 0x41, 0x01, 0x41, 0x00, 0x0E, 0x01, 0x00, 0x00, 0x0B, 0x1A }))
          == std::nullopt);
    // A label past the outermost frame.
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x00, 0x0E, 0x01, 0x00, 0x01 }))
          == ValidationErrorCode::UnknownLabel);
}

TEST_CASE("Validator.controlFrames")
{
    CHECK(errorOf(voidFunctionOf({ 0x0C, 0x01 })) == ValidationErrorCode::UnknownLabel);
    // br_if looks its label up before popping its condition, as the reference interpreter does.
    CHECK(errorOf(voidFunctionOf({ 0x0D, 0x05 })) == ValidationErrorCode::UnknownLabel);
    // A loop's label carries its params, not its results: br 0 inside loop (result i32) takes nothing.
    CHECK(errorOf(voidFunctionOf({ 0x03, 0x7F, 0x0C, 0x00, 0x0B, 0x1A })) == std::nullopt);
    // br_if keeps its label's values on the stack.
    CHECK(errorOf(voidFunctionOf({ 0x02, 0x7F, 0x41, 0x01, 0x41, 0x00, 0x0D, 0x00, 0x0B, 0x1A }))
          == std::nullopt);
    // if (result i32) without else cannot produce its result.
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x00, 0x04, 0x7F, 0x41, 0x01, 0x0B, 0x1A }))
          == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x00, 0x04, 0x7F, 0x41, 0x01, 0x05, 0x41, 0x02, 0x0B, 0x1A }))
          == std::nullopt);
    // Each branch of an if must leave exactly its results.
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x00, 0x04, 0x7F, 0x41, 0x01, 0x05, 0x42, 0x02, 0x0B, 0x1A }))
          == ValidationErrorCode::TypeMismatch);
    // A block type by index: (i32) -> (i32) takes its param from the enclosing stack.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .type(FuncType { .params = { I32 }, .results = { I32 } })
        .function(0, {}, { 0x41, 0x05, 0x02, 0x01, 0x41, 0x01, 0x6A, 0x0B, 0x1A });
    CHECK(errorOf(builder) == std::nullopt);
}

TEST_CASE("Validator.unreachableSelect")
{
    // Both operands Unknown: the result is Unknown, which matches any type.
    CHECK(errorOf(i32FunctionOf({ 0x00, 0x1B })) == std::nullopt);
    CHECK(errorOf(voidFunctionOf({ 0x00, 0x1B, 0x1A })) == std::nullopt);
    // One operand known: the result takes its type, so an i64 select cannot end an [] -> [i32] body.
    CHECK(errorOf(i32FunctionOf({ 0x00, 0x42, 0x00, 0x41, 0x00, 0x1B }))
          == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(i32FunctionOf({ 0x00, 0x41, 0x05, 0x41, 0x00, 0x1B })) == std::nullopt);
    // Untyped select on a reference stays invalid in unreachable code: once as the condition, once as an
    // operand beside an Unknown one.
    CHECK(errorOf(voidFunctionOf({ 0x00, 0xD0, 0x70, 0x1B, 0x1A })) == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(voidFunctionOf({ 0x00, 0xD0, 0x70, 0x41, 0x00, 0x1B, 0x1A }))
          == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.brTableTargetTypes")
{
    // block (result i32) block (result i64) <operands> br_table 0 1 end drop i32.const 0 end drop: both
    // labels carry one value, of different types.
    auto const nested = [](std::vector<uint8_t> const& operands) {
        auto body = std::vector<uint8_t> { 0x02, 0x7F, 0x02, 0x7E };
        body.insert(body.end(), operands.begin(), operands.end());
        body.insert(body.end(), { 0x0E, 0x01, 0x00, 0x01, 0x0B, 0x1A, 0x41, 0x00, 0x0B, 0x1A });
        return voidFunctionOf(std::move(body));
    };
    // Reachable, an i32 operand does not suit label 0's i64.
    CHECK(errorOf(nested({ 0x41, 0x01, 0x41, 0x00 })) == ValidationErrorCode::TypeMismatch);
    // After unreachable the operand is Unknown, which suits both: the spec algorithm (and the interpreter's
    // peek of an Unknown stack) accepts targets of equal arity whatever their types.
    CHECK(errorOf(nested({ 0x00 })) == std::nullopt);
    // An Unknown condition with a known operand still checks the operand against every target.
    CHECK(errorOf(nested({ 0x00, 0x41, 0x01, 0x41, 0x00 })) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.multiValueBlock")
{
    // Type 1 is [] -> [i32 i64]; the body is block (type 1) <inner> end drop drop.
    auto const blockOf = [](std::vector<uint8_t> const& inner) {
        auto body = std::vector<uint8_t> { 0x02, 0x01 };
        body.insert(body.end(), inner.begin(), inner.end());
        body.insert(body.end(), { 0x0B, 0x1A, 0x1A });
        auto builder = ModuleBuilder {};
        builder.type(FuncType {})
            .type(FuncType { .params = {}, .results = { I32, I64 } })
            .function(0, {}, std::move(body));
        return builder;
    };
    CHECK(errorOf(blockOf({ 0x41, 0x01, 0x42, 0x02 })) == std::nullopt);
    CHECK(errorOf(blockOf({ 0x41, 0x01, 0x42, 0x02, 0x0C, 0x00 })) == std::nullopt);
    CHECK(errorOf(blockOf({ 0x42, 0x02, 0x41, 0x01 })) == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(blockOf({ 0x42, 0x02, 0x41, 0x01, 0x0C, 0x00 })) == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(blockOf({ 0x41, 0x01, 0x0C, 0x00 })) == ValidationErrorCode::TypeMismatch);
    // br 0 with nothing on the stack cannot supply the block's two results.
    CHECK(errorOf(blockOf({ 0x0C, 0x00 })) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.elseResetsReachability")
{
    // i32.const 0 if (result i32) unreachable else <branch> end drop: the then-branch is unreachable,
    // the else-branch is typed from a reachable, empty stack again.
    auto const ifOf = [](std::vector<uint8_t> const& elseBranch) {
        auto body = std::vector<uint8_t> { 0x41, 0x00, 0x04, 0x7F, 0x00, 0x05 };
        body.insert(body.end(), elseBranch.begin(), elseBranch.end());
        body.insert(body.end(), { 0x0B, 0x1A });
        return voidFunctionOf(std::move(body));
    };
    CHECK(errorOf(ifOf({ 0x41, 0x01 })) == std::nullopt);
    CHECK(errorOf(ifOf({ 0x42, 0x01 })) == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(ifOf({})) == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(ifOf({ 0x6A })) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.validateDeepNesting")
{
    constexpr auto Depth = 100'000;
    auto body = std::vector<uint8_t> {};
    body.reserve(std::size_t { 3 } * Depth);
    for ([[maybe_unused]] auto const level: std::views::iota(0, Depth))
        body.insert(body.end(), { 0x02, 0x40 });
    body.insert(body.end(), Depth, 0x0B);
    auto const deep = voidFunctionOf(std::move(body));
    CHECK(validated(deep).has_value());
}

TEST_CASE("Validator.immutableGlobal")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .global(GlobalType { .type = I32, .mutability = Mutability::Const }, { 0x41, 0x00 })
        .global(GlobalType { .type = I32, .mutability = Mutability::Var }, { 0x41, 0x00 })
        .function(0, {}, { 0x41, 0x01, 0x24, 0x00 });
    CHECK(errorOf(builder) == ValidationErrorCode::GlobalIsImmutable);

    auto mutableOne = ModuleBuilder {};
    mutableOne.type(FuncType {})
        .global(GlobalType { .type = I32, .mutability = Mutability::Var }, { 0x41, 0x00 })
        .function(0, {}, { 0x23, 0x00, 0x24, 0x00 });
    CHECK(errorOf(mutableOne) == std::nullopt);

    auto unknown = ModuleBuilder {};
    unknown.type(FuncType {}).function(0, {}, { 0x23, 0x00, 0x1A });
    CHECK(errorOf(unknown) == ValidationErrorCode::UnknownGlobal);
}

TEST_CASE("Validator.alignment")
{
    auto builder = voidFunctionOf({ 0x41, 0x00, 0x28, 0x03, 0x00, 0x1A });
    builder.memory(atLeast(1));
    CHECK(errorOf(builder) == ValidationErrorCode::AlignmentMustNotBeLarger);

    auto natural =
        voidFunctionOf({ 0x41, 0x00, 0x28, 0x02, 0x00, 0x1A, 0x41, 0x00, 0x42, 0x00, 0x3C, 0x00, 0x00 });
    natural.memory(atLeast(1));
    CHECK(errorOf(natural) == std::nullopt);

    // i64.store8 has a natural alignment of one byte.
    auto narrow = voidFunctionOf({ 0x41, 0x00, 0x42, 0x00, 0x3C, 0x01, 0x00 });
    narrow.memory(atLeast(1));
    CHECK(errorOf(narrow) == ValidationErrorCode::AlignmentMustNotBeLarger);

    // A load with no memory, checked before the alignment.
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x00, 0x28, 0x03, 0x00, 0x1A }))
          == ValidationErrorCode::UnknownMemory);
}

namespace
{

/// v128.const of 16 zero bytes.
std::vector<uint8_t> v128Zero()
{
    auto bytes = std::vector<uint8_t> { 0xFD, 0x0C };
    bytes.insert(bytes.end(), 16, 0x00);
    return bytes;
}

/// @p first, then each of @p rest, concatenated.
std::vector<uint8_t> concat(std::vector<uint8_t> first, std::initializer_list<std::vector<uint8_t>> rest)
{
    for (auto const& piece: rest)
        first.insert(first.end(), piece.begin(), piece.end());
    return first;
}

/// A module with one page of memory and one function [] -> [] whose body is @p body.
ModuleBuilder withMemory(std::vector<uint8_t> body)
{
    auto builder = voidFunctionOf(std::move(body));
    builder.memory(atLeast(1));
    return builder;
}

} // namespace

TEST_CASE("Validator.invalidLaneIndex")
{
    // i32x4.extract_lane 4: an i32x4 has lanes 0 .. 3.
    CHECK(errorOf(voidFunctionOf(concat(v128Zero(), { { 0xFD, 0x1B, 0x04, 0x1A } })))
          == ValidationErrorCode::InvalidLaneIndex);
    CHECK(errorOf(voidFunctionOf(concat(v128Zero(), { { 0xFD, 0x1B, 0x03, 0x1A } }))) == std::nullopt);
    // The lane is checked before the operands: on an empty stack, the lane is still the error.
    CHECK(errorOf(voidFunctionOf({ 0xFD, 0x1B, 0x04, 0x1A })) == ValidationErrorCode::InvalidLaneIndex);
    // i8x16.replace_lane 16, and 15.
    CHECK(errorOf(voidFunctionOf(concat(v128Zero(), { { 0x41, 0x00, 0xFD, 0x17, 0x10, 0x1A } })))
          == ValidationErrorCode::InvalidLaneIndex);
    CHECK(errorOf(voidFunctionOf(concat(v128Zero(), { { 0x41, 0x00, 0xFD, 0x17, 0x0F, 0x1A } })))
          == std::nullopt);
    // f64x2.extract_lane 2.
    CHECK(errorOf(voidFunctionOf(concat(v128Zero(), { { 0xFD, 0x21, 0x02, 0x1A } })))
          == ValidationErrorCode::InvalidLaneIndex);

    // i8x16.shuffle chooses among 32 bytes: lane 32 is one too many, 31 the last.
    auto const shuffleWith = [](uint8_t lane) {
        auto shuffle = std::vector<uint8_t> { 0xFD, 0x0D };
        shuffle.insert(shuffle.end(), 15, 0x00);
        shuffle.push_back(lane);
        return concat(v128Zero(), { v128Zero(), shuffle, { 0x1A } });
    };
    CHECK(errorOf(voidFunctionOf(shuffleWith(32))) == ValidationErrorCode::InvalidLaneIndex);
    CHECK(errorOf(voidFunctionOf(shuffleWith(0xFF))) == ValidationErrorCode::InvalidLaneIndex);
    CHECK(errorOf(voidFunctionOf(shuffleWith(31))) == std::nullopt);

    // v128.load32_lane 4 and v128.store64_lane 2, after their memory arguments.
    CHECK(errorOf(withMemory(concat({ 0x41, 0x00 }, { v128Zero(), { 0xFD, 0x56, 0x02, 0x00, 0x04, 0x1A } })))
          == ValidationErrorCode::InvalidLaneIndex);
    CHECK(errorOf(withMemory(concat({ 0x41, 0x00 }, { v128Zero(), { 0xFD, 0x56, 0x02, 0x00, 0x03, 0x1A } })))
          == std::nullopt);
    CHECK(errorOf(withMemory(concat({ 0x41, 0x00 }, { v128Zero(), { 0xFD, 0x5B, 0x03, 0x00, 0x02 } })))
          == ValidationErrorCode::InvalidLaneIndex);
    CHECK(errorOf(withMemory(concat({ 0x41, 0x00 }, { v128Zero(), { 0xFD, 0x5B, 0x03, 0x00, 0x01 } })))
          == std::nullopt);
    // The memory is checked first, then the alignment, then the lane.
    CHECK(errorOf(
              voidFunctionOf(concat({ 0x41, 0x00 }, { v128Zero(), { 0xFD, 0x56, 0x03, 0x00, 0x04, 0x1A } })))
          == ValidationErrorCode::UnknownMemory);
    CHECK(errorOf(withMemory(concat({ 0x41, 0x00 }, { v128Zero(), { 0xFD, 0x56, 0x03, 0x00, 0x04, 0x1A } })))
          == ValidationErrorCode::AlignmentMustNotBeLarger);
    CHECK(specMessage(ValidationErrorCode::InvalidLaneIndex) == "invalid lane index");
}

TEST_CASE("Validator.vectorMemoryAccess")
{
    // The natural alignment of each access is its width: 16, 8, 4, 2 and 1 bytes.
    struct Case
    {
        uint8_t code;
        uint32_t naturalAlignment;
    };
    for (auto const& testCase: { Case { 0x00, 4 },   // v128.load
                                 Case { 0x01, 3 },   // v128.load8x8_s
                                 Case { 0x07, 0 },   // v128.load8_splat
                                 Case { 0x08, 1 },   // v128.load16_splat
                                 Case { 0x5C, 2 },   // v128.load32_zero
                                 Case { 0x5D, 3 } }) // v128.load64_zero
    {
        INFO("code " << int { testCase.code });
        auto const load = [&](uint32_t align) {
            return withMemory({ 0x41, 0x00, 0xFD, testCase.code, static_cast<uint8_t>(align), 0x00, 0x1A });
        };
        CHECK(errorOf(load(testCase.naturalAlignment)) == std::nullopt);
        CHECK(errorOf(load(testCase.naturalAlignment + 1)) == ValidationErrorCode::AlignmentMustNotBeLarger);
    }
    // v128.store pops a v128 and an i32 address; an i32 value is a mismatch.
    CHECK(errorOf(withMemory(concat({ 0x41, 0x00 }, { v128Zero(), { 0xFD, 0x0B, 0x04, 0x00 } })))
          == std::nullopt);
    CHECK(errorOf(withMemory({ 0x41, 0x00, 0x41, 0x00, 0xFD, 0x0B, 0x04, 0x00 }))
          == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(withMemory({ 0x41, 0x00, 0xFD, 0x0B, 0x05, 0x00 }))
          == ValidationErrorCode::AlignmentMustNotBeLarger);
    // A lane load pops the vector it changes and pushes it back; without it, the i32 address is
    // popped as the vector, a type mismatch.
    CHECK(errorOf(withMemory({ 0x41, 0x00, 0xFD, 0x54, 0x00, 0x00, 0x00, 0x1A }))
          == ValidationErrorCode::TypeMismatch);
    // A load yields a v128, not an i32.
    auto i32Result = i32FunctionOf({ 0x41, 0x00, 0xFD, 0x00, 0x04, 0x00 });
    i32Result.memory(atLeast(1));
    CHECK(errorOf(i32Result) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.v128Constant")
{
    // v128.const is a constant expression.
    auto builder = ModuleBuilder {};
    builder.global(GlobalType { .type = isoline::binary::V128, .mutability = Mutability::Const }, v128Zero());
    CHECK(errorOf(builder) == std::nullopt);
    // Of type v128, so not an i32 global's initializer.
    auto mismatch = ModuleBuilder {};
    mismatch.global(GlobalType { .type = I32, .mutability = Mutability::Const }, v128Zero());
    CHECK(errorOf(mismatch) == ValidationErrorCode::TypeMismatch);
    // A SIMD operator is not constant.
    auto notConstant = ModuleBuilder {};
    notConstant.global(GlobalType { .type = isoline::binary::V128, .mutability = Mutability::Const },
                       concat(v128Zero(), { { 0xFD, 0x4D } }));
    CHECK(errorOf(notConstant) == ValidationErrorCode::ConstantExpressionRequired);
}

TEST_CASE("Validator.memoryInstructions")
{
    auto builder = voidFunctionOf(
        { 0x3F, 0x00, 0x40, 0x00, 0x1A, 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0B, 0x00 });
    builder.memory(atLeast(1));
    CHECK(errorOf(builder) == std::nullopt);
    CHECK(errorOf(voidFunctionOf({ 0x3F, 0x00, 0x1A })) == ValidationErrorCode::UnknownMemory);
}

TEST_CASE("Validator.refFuncUndeclared")
{
    auto const undeclared = voidFunctionOf({ 0xD2, 0x00, 0x1A });
    CHECK(errorOf(undeclared) == ValidationErrorCode::UndeclaredFunctionReference);

    // An export declares it, and so does a declarative element segment.
    auto exported = voidFunctionOf({ 0xD2, 0x00, 0x1A });
    exported.exportFunction("f", 0);
    CHECK(errorOf(exported) == std::nullopt);
    auto declared = voidFunctionOf({ 0xD2, 0x00, 0x1A });
    declared.element({ 0x03, 0x00, 0x01, 0x00 });
    CHECK(errorOf(declared) == std::nullopt);
    // So does a global initializer.
    auto global = voidFunctionOf({ 0xD2, 0x00, 0x1A });
    global.global(GlobalType { .type = FuncRef, .mutability = Mutability::Const }, { 0xD2, 0x00 });
    CHECK(errorOf(global) == std::nullopt);

    CHECK(errorOf(voidFunctionOf({ 0xD2, 0x01, 0x1A })) == ValidationErrorCode::UnknownFunction);
}

TEST_CASE("Validator.references")
{
    // ref.null funcref ref.is_null.
    CHECK(errorOf(i32FunctionOf({ 0xD0, 0x70, 0xD1 })) == std::nullopt);
    CHECK(errorOf(i32FunctionOf({ 0x41, 0x00, 0xD1 })) == ValidationErrorCode::TypeMismatch);
    // Untyped select takes numbers only; typed select takes references.
    CHECK(errorOf(voidFunctionOf({ 0xD0, 0x70, 0xD0, 0x70, 0x41, 0x00, 0x1B, 0x1A }))
          == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(voidFunctionOf({ 0xD0, 0x70, 0xD0, 0x70, 0x41, 0x00, 0x1C, 0x01, 0x70, 0x1A }))
          == std::nullopt);
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x01, 0x42, 0x02, 0x41, 0x00, 0x1B, 0x1A }))
          == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x01, 0x41, 0x02, 0x41, 0x00, 0x1B, 0x1A })) == std::nullopt);
    // Typed select names exactly one type.
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x01, 0x41, 0x02, 0x41, 0x00, 0x1C, 0x02, 0x7F, 0x7F, 0x1A }))
          == ValidationErrorCode::InvalidResultArity);
}

TEST_CASE("Validator.calls")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .type(FuncType { .params = { I32 }, .results = { I64 } })
        .function(0, {}, { 0x41, 0x00, 0x10, 0x01, 0x1A })
        .function(1, {}, { 0x42, 0x00 });
    CHECK(errorOf(builder) == std::nullopt);
    CHECK(errorOf(voidFunctionOf({ 0x10, 0x01 })) == ValidationErrorCode::UnknownFunction);
    CHECK(errorOf(voidFunctionOf({ 0x10, 0x00, 0x10, 0x00 })) == std::nullopt);

    // call_indirect needs a table of functions and a known type.
    CHECK(errorOf(voidFunctionOf({ 0x41, 0x00, 0x11, 0x00, 0x00 })) == ValidationErrorCode::UnknownTable);
    auto indirect = voidFunctionOf({ 0x41, 0x00, 0x11, 0x00, 0x00 });
    indirect.table(TableType { .element = FuncRef, .limits = atLeast(1) });
    CHECK(errorOf(indirect) == std::nullopt);
    auto unknownType = voidFunctionOf({ 0x41, 0x00, 0x11, 0x07, 0x00 });
    unknownType.table(TableType { .element = FuncRef, .limits = atLeast(1) });
    CHECK(errorOf(unknownType) == ValidationErrorCode::UnknownType);
    auto externTable = voidFunctionOf({ 0x41, 0x00, 0x11, 0x00, 0x00 });
    externTable.table(TableType { .element = isoline::binary::ExternRef, .limits = atLeast(1) });
    CHECK(errorOf(externTable) == ValidationErrorCode::TypeMismatch);

    // Active, table 0, offset (i32.const 0), funcref expressions: (i32.const 0) is no funcref.
    auto i32Item = voidFunctionOf({});
    i32Item.table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .element({ 0x04, 0x41, 0x00, 0x0B, 0x01, 0x41, 0x00, 0x0B });
    CHECK(errorOf(i32Item) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.tags")
{
    // Wasm 2.0 has no tags, so no decoder yields one: the modules are built as data.
    auto const bytes = ModuleBuilder {}.build();
    auto const decoded = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE(decoded.has_value());

    auto imported = *decoded;
    imported.imports.push_back(Import { .module = "m", .name = "t", .kind = ExternKind::Tag });
    auto const importResult = validateModule(imported, FeatureSet::wasm2());
    REQUIRE_FALSE(importResult.has_value());
    CHECK(importResult.error().code == ValidationErrorCode::UnknownTag);

    auto exported = *decoded;
    exported.exports.push_back(Export { .name = "t", .kind = ExternKind::Tag, .index = 0 });
    auto const exportResult = validateModule(exported, FeatureSet::wasm2());
    REQUIRE_FALSE(exportResult.has_value());
    CHECK(exportResult.error().code == ValidationErrorCode::UnknownTag);
}

TEST_CASE("Validator.functionIndicesAreFuncrefs")
{
    // A segment of function indices is a funcref segment; the binary format can say nothing else, so
    // the externref one is built as data.
    auto builder = voidFunctionOf({});
    builder.table(TableType { .element = isoline::binary::ExternRef, .limits = atLeast(1) })
        .element({ 0x01, 0x00, 0x01, 0x00 }); // passive, elemkind funcref, function 0
    auto const bytes = builder.build();
    auto decoded = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE(decoded.has_value());
    REQUIRE(validateModule(*decoded, FeatureSet::wasm2()).has_value());
    decoded->elements[0].type = isoline::binary::ExternRef;
    auto const result = validateModule(*decoded, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.tableInstructions")
{
    // table.size, table.get, table.set, table.grow, table.fill on table 0, then elem.drop 0.
    auto builder = voidFunctionOf({ 0xFC, 0x10, 0x00, 0x1A, 0x41, 0x00, 0x41, 0x00, 0x25, 0x00, 0x26,
                                    0x00, 0xD0, 0x70, 0x41, 0x01, 0xFC, 0x0F, 0x00, 0x1A, 0x41, 0x00,
                                    0xD0, 0x70, 0x41, 0x01, 0xFC, 0x11, 0x00, 0xFC, 0x0D, 0x00 });
    builder.table(TableType { .element = FuncRef, .limits = atLeast(1) }).element({ 0x01, 0x00, 0x00 });
    CHECK(errorOf(builder) == std::nullopt);
    CHECK(errorOf(voidFunctionOf({ 0xFC, 0x0D, 0x00 })) == ValidationErrorCode::UnknownElemSegment);
    CHECK(errorOf(voidFunctionOf({ 0xFC, 0x10, 0x00, 0x1A })) == ValidationErrorCode::UnknownTable);

    // table.copy between tables of different element types.
    auto mixed = voidFunctionOf({ 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0E, 0x00, 0x01 });
    mixed.table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .table(TableType { .element = isoline::binary::ExternRef, .limits = atLeast(1) });
    CHECK(errorOf(mixed) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.duplicateExport")
{
    auto builder = voidFunctionOf({});
    builder.exportFunction("a", 0).exportFunction("a", 0);
    CHECK(errorOf(builder) == ValidationErrorCode::DuplicateExportName);

    auto distinct = voidFunctionOf({});
    distinct.exportFunction("a", 0).exportFunction("b", 0);
    CHECK(errorOf(distinct) == std::nullopt);

    // Names sort out of declaration order, and the duplicates are not adjacent.
    auto scattered = voidFunctionOf({});
    scattered.exportFunction("c", 0).exportFunction("a", 0).exportFunction("b", 0).exportFunction("a", 0);
    CHECK(errorOf(scattered) == ValidationErrorCode::DuplicateExportName);

    // Checked in declaration order: an unknown index before the first duplicate is reported first,
    // and a duplicate before an unknown index is.
    auto unknownFirst = voidFunctionOf({});
    unknownFirst.exportFunction("a", 0).exportFunction("b", 7).exportFunction("a", 0);
    CHECK(errorOf(unknownFirst) == ValidationErrorCode::UnknownFunction);
    auto duplicateFirst = voidFunctionOf({});
    duplicateFirst.exportFunction("a", 0).exportFunction("a", 0).exportFunction("b", 7);
    CHECK(errorOf(duplicateFirst) == ValidationErrorCode::DuplicateExportName);

    auto unknown = voidFunctionOf({});
    unknown.exportEntity("m", ExternKind::Memory, 0);
    CHECK(errorOf(unknown) == ValidationErrorCode::UnknownMemory);
    auto unknownFunction = voidFunctionOf({});
    unknownFunction.exportFunction("f", 1);
    auto const result = validated(unknownFunction);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == ValidationError { ValidationErrorCode::UnknownFunction, NoFunction, NoInstruction, 1 });
}

TEST_CASE("Validator.constExpr")
{
    auto builder = ModuleBuilder {};
    builder.global(GlobalType { .type = I32, .mutability = Mutability::Const },
                   { 0x41, 0x01, 0x41, 0x02, 0x6A });
    CHECK(errorOf(builder) == ValidationErrorCode::ConstantExpressionRequired);
    CHECK(errorOf(builder, FeatureSet::wasm2().with(Feature::ExtendedConst)) == std::nullopt);

    // A constant expression is typed as a body is.
    auto wrongType = ModuleBuilder {};
    wrongType.global(GlobalType { .type = I32, .mutability = Mutability::Const }, { 0x42, 0x01 });
    CHECK(errorOf(wrongType) == ValidationErrorCode::TypeMismatch);

    // global.get sees only imported globals, and only immutable ones are constant.
    auto defined = ModuleBuilder {};
    defined.global(GlobalType { .type = I32, .mutability = Mutability::Const }, { 0x41, 0x01 })
        .global(GlobalType { .type = I32, .mutability = Mutability::Const }, { 0x23, 0x00 });
    CHECK(errorOf(defined) == ValidationErrorCode::UnknownGlobal);
    auto imported = ModuleBuilder {};
    imported.importGlobal("env", "g", GlobalType { .type = I32, .mutability = Mutability::Const })
        .global(GlobalType { .type = I32, .mutability = Mutability::Const }, { 0x23, 0x00 });
    CHECK(errorOf(imported) == std::nullopt);
    auto importedMutable = ModuleBuilder {};
    importedMutable.importGlobal("env", "g", GlobalType { .type = I32, .mutability = Mutability::Var })
        .global(GlobalType { .type = I32, .mutability = Mutability::Const }, { 0x23, 0x00 });
    CHECK(errorOf(importedMutable) == ValidationErrorCode::ConstantExpressionRequired);
}

TEST_CASE("Validator.elementSegments")
{
    // Active, table 0, offset (i32.const 0), function indices.
    auto builder = voidFunctionOf({});
    builder.table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .element({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00 });
    CHECK(errorOf(builder) == std::nullopt);

    auto noTable = voidFunctionOf({});
    noTable.element({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00 });
    CHECK(errorOf(noTable) == ValidationErrorCode::UnknownTable);

    auto unknownFunction = voidFunctionOf({});
    unknownFunction.table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .element({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0x05 });
    CHECK(errorOf(unknownFunction) == ValidationErrorCode::UnknownFunction);

    auto i64Offset = voidFunctionOf({});
    i64Offset.table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .element({ 0x00, 0x42, 0x00, 0x0B, 0x01, 0x00 });
    CHECK(errorOf(i64Offset) == ValidationErrorCode::TypeMismatch);

    // A funcref segment into an externref table.
    auto externTable = voidFunctionOf({});
    externTable.table(TableType { .element = isoline::binary::ExternRef, .limits = atLeast(1) })
        .element({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00 });
    CHECK(errorOf(externTable) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.limits")
{
    auto tooLarge = ModuleBuilder {};
    tooLarge.memory(atLeast(65537));
    CHECK(errorOf(tooLarge) == ValidationErrorCode::MemorySizeMustBeAtMost);
    auto tooLargeMax = ModuleBuilder {};
    tooLargeMax.memory(between(1, 65537));
    CHECK(errorOf(tooLargeMax) == ValidationErrorCode::MemorySizeMustBeAtMost);
    auto inverted = ModuleBuilder {};
    inverted.memory(between(2, 1));
    CHECK(errorOf(inverted) == ValidationErrorCode::SizeMinimumMustNotBeGreaterThanMaximum);
    auto full = ModuleBuilder {};
    full.memory(between(65536, 65536));
    CHECK(errorOf(full) == std::nullopt);
    auto invertedTable = ModuleBuilder {};
    invertedTable.table(TableType { .element = FuncRef, .limits = between(2, 1) });
    CHECK(errorOf(invertedTable) == ValidationErrorCode::SizeMinimumMustNotBeGreaterThanMaximum);
}

TEST_CASE("Validator.counts")
{
    auto memories = ModuleBuilder {};
    memories.memory(atLeast(1)).memory(atLeast(1));
    CHECK(errorOf(memories, FeatureSet::wasm2().with(Feature::MultiMemory)) == std::nullopt);
    auto tables = ModuleBuilder {};
    tables.table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .table(TableType { .element = FuncRef, .limits = atLeast(1) });
    CHECK(errorOf(tables) == std::nullopt);
    CHECK(errorOf(tables, FeatureSet::none()) == ValidationErrorCode::MultipleTables);
}

TEST_CASE("Validator.multipleMemories")
{
    // The decoder reads any number of memories; the validator allows one before multi-memory.
    auto memories = ModuleBuilder {};
    memories.memory(atLeast(1)).memory(atLeast(1));
    CHECK(errorOf(memories) == ValidationErrorCode::MultipleMemories);
}

TEST_CASE("Validator.startFunction")
{
    auto valid = voidFunctionOf({});
    valid.start(0);
    CHECK(errorOf(valid) == std::nullopt);
    auto withParam = functionOf(FuncType { .params = { I32 }, .results = {} }, {});
    withParam.start(0);
    CHECK(errorOf(withParam) == ValidationErrorCode::StartFunction);
    auto unknown = voidFunctionOf({});
    unknown.start(1);
    CHECK(errorOf(unknown) == ValidationErrorCode::UnknownFunction);
}

TEST_CASE("Validator.resultArity")
{
    // Before multi-value, a function type has at most one result.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { I32, I32 } });
    CHECK(errorOf(builder, FeatureSet::none()) == ValidationErrorCode::InvalidResultArity);
    CHECK(errorOf(builder) == std::nullopt);
}

TEST_CASE("Validator.errorPosition")
{
    // An imported function comes first in the function index space.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("env", "f", 0).function(0, {}, { 0x01, 0x20, 0x00 });
    auto const result = validated(builder);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ValidationError { ValidationErrorCode::UnknownLocal, 1, 1, 0 });

    auto unknownType = ModuleBuilder {};
    unknownType.type(FuncType {}).importFunction("env", "f", 3);
    CHECK(errorOf(unknownType) == ValidationErrorCode::UnknownType);
}

TEST_CASE("Validator.specMessages")
{
    CHECK(specMessage(ValidationErrorCode::TypeMismatch) == "type mismatch");
    CHECK(specMessage(ValidationErrorCode::UnknownElemSegment) == "unknown elem segment");
    CHECK(specMessage(ValidationErrorCode::AlignmentMustNotBeLarger)
          == "alignment must not be larger than natural");
    CHECK(specMessage(ValidationErrorCode::MemorySizeMustBeAtMost) == "memory size must be at most");
    CHECK(specMessage(ValidationErrorCode::UndeclaredFunctionReference) == "undeclared function reference");
}

TEST_CASE("Validator.messagesNameTheUnknownIndex")
{
    // The reference interpreter appends the index an unknown or undeclared entity has, and the spec
    // suite asserts it ("unknown memory 1"); errors without an index keep the bare message.
    auto const messageOf = [](ModuleBuilder const& builder) {
        auto const result = validated(builder);
        REQUIRE_FALSE(result.has_value());
        return specMessage(result.error());
    };
    CHECK(messageOf(functionOf(FuncType {}, { 0x20, 0x05, 0x1A }, { I32 })) == "unknown local 5");
    CHECK(messageOf(voidFunctionOf({ 0x23, 0x03, 0x1A })) == "unknown global 3");
    // The largest index is an index too; the interpreter spells it unsigned (I32.to_string_u).
    CHECK(messageOf(voidFunctionOf({ 0x23, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x1A }))
          == "unknown global 4294967295");
    CHECK(messageOf(voidFunctionOf({ 0x0C, 0x02 })) == "unknown label 2");
    CHECK(messageOf(voidFunctionOf({ 0x41, 0x00, 0x28, 0x02, 0x00, 0x1A })) == "unknown memory 0");
    CHECK(messageOf(voidFunctionOf({ 0xD2, 0x00, 0x1A })) == "undeclared function reference 0");
    CHECK(messageOf(voidFunctionOf({ 0x10, 0x04 })) == "unknown function 4");
    CHECK(messageOf(voidFunctionOf({ 0xFC, 0x0D, 0x02 })) == "unknown elem segment 2");
    CHECK(messageOf(i32FunctionOf({ 0x42, 0x00 })) == "type mismatch");
    auto exported = voidFunctionOf({});
    exported.exportEntity("m", ExternKind::Memory, 1);
    CHECK(messageOf(exported) == "unknown memory 1");
}

namespace
{

/// Limits of at least @p min pages or elements, unbounded, of i64 addresses.
Limits atLeast64(uint64_t min)
{
    return Limits { .min = min, .max = std::nullopt, .addressType = isoline::binary::AddressType::I64 };
}

/// The error validating @p builder's module under WebAssembly 3.0 reports, or nullopt when it is valid.
std::optional<ValidationErrorCode> error3Of(ModuleBuilder const& builder)
{
    return errorOf(builder, FeatureSet::wasm3());
}

} // namespace

TEST_CASE("Validator.offsetOutOfRange")
{
    // i32.load offset=2^32: an offset an i32 memory cannot reach, after i32/i64.const 0.
    auto const load = [](uint8_t constOpcode) {
        return voidFunctionOf({ constOpcode, 0x00, 0x28, 0x02, 0x80, 0x80, 0x80, 0x80, 0x10, 0x1A });
    };
    auto narrow = load(0x41);
    narrow.memory(atLeast(1));
    CHECK(error3Of(narrow) == ValidationErrorCode::OffsetOutOfRange);
    CHECK(specMessage(ValidationErrorCode::OffsetOutOfRange) == "offset out of range");
    auto wide = load(0x42);
    wide.memory(atLeast64(1));
    CHECK(error3Of(wide) == std::nullopt);
    // The largest offset an i32 memory reaches.
    auto largest = voidFunctionOf({ 0x41, 0x00, 0x28, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x1A });
    largest.memory(atLeast(1));
    CHECK(error3Of(largest) == std::nullopt);
}

TEST_CASE("Validator.memory64AddressType")
{
    auto const withMemory64 = [](std::vector<uint8_t> body) {
        auto builder = voidFunctionOf(std::move(body));
        builder.memory(atLeast64(1));
        return error3Of(builder);
    };
    // Loads and stores take the memory's address type.
    CHECK(withMemory64({ 0x41, 0x00, 0x28, 0x02, 0x00, 0x1A }) == ValidationErrorCode::TypeMismatch);
    CHECK(withMemory64({ 0x42, 0x00, 0x28, 0x02, 0x00, 0x1A }) == std::nullopt);
    CHECK(withMemory64({ 0x42, 0x00, 0x41, 0x00, 0x36, 0x02, 0x00 }) == std::nullopt);
    CHECK(withMemory64({ 0x41, 0x00, 0x41, 0x00, 0x36, 0x02, 0x00 }) == ValidationErrorCode::TypeMismatch);
    // A vector load too.
    CHECK(withMemory64({ 0x42, 0x00, 0xFD, 0x00, 0x04, 0x00, 0x1A }) == std::nullopt);
    // memory.size yields, and memory.grow takes and yields, an i64.
    CHECK(withMemory64({ 0x3F, 0x00, 0x50, 0x1A }) == std::nullopt);
    CHECK(withMemory64({ 0x3F, 0x00, 0x45, 0x1A }) == ValidationErrorCode::TypeMismatch);
    CHECK(withMemory64({ 0x42, 0x01, 0x40, 0x00, 0x50, 0x1A }) == std::nullopt);
    CHECK(withMemory64({ 0x41, 0x01, 0x40, 0x00, 0x1A }) == ValidationErrorCode::TypeMismatch);
    // memory.fill: [i64 destination, i32 value, i64 count].
    CHECK(withMemory64({ 0x42, 0x00, 0x41, 0x00, 0x42, 0x00, 0xFC, 0x0B, 0x00 }) == std::nullopt);
    CHECK(withMemory64({ 0x42, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0B, 0x00 })
          == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.memory64CopyAndInit")
{
    // Memory 0 takes i64 addresses, memory 1 i32 ones. Until the decoder reads multi-memory's memory
    // indices, a copy between them is the decoded copy 0 0 with its source moved to memory 1.
    auto const copy = [](std::vector<uint8_t> body, uint32_t destination, uint32_t source) {
        auto builder = voidFunctionOf(std::move(body));
        builder.memory(atLeast64(1)).memory(atLeast(1));
        auto const bytes = builder.build();
        auto module = decodeModule(bytes, FeatureSet::wasm3());
        REQUIRE(module.has_value());
        auto& instruction = module->code[0].body[3];
        REQUIRE(instruction.opcode == isoline::binary::Opcode::MemoryCopy);
        instruction.a = destination;
        instruction.b = source;
        auto const result = validateModule(*module, FeatureSet::wasm3());
        return result ? std::nullopt : std::optional { result.error().code };
    };
    // memory.copy 0 0: every operand an i64.
    CHECK(copy({ 0x42, 0x00, 0x42, 0x00, 0x42, 0x00, 0xFC, 0x0A, 0x00, 0x00 }, 0, 0) == std::nullopt);
    // memory.copy 0 1: an i64 destination, an i32 source, and an i32 count, the narrower of the two.
    CHECK(copy({ 0x42, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0A, 0x00, 0x00 }, 0, 1) == std::nullopt);
    CHECK(copy({ 0x42, 0x00, 0x41, 0x00, 0x42, 0x00, 0xFC, 0x0A, 0x00, 0x00 }, 0, 1)
          == ValidationErrorCode::TypeMismatch);
    // memory.copy 1 0: an i32 destination and an i64 source.
    CHECK(copy({ 0x41, 0x00, 0x42, 0x00, 0x41, 0x00, 0xFC, 0x0A, 0x00, 0x00 }, 1, 0) == std::nullopt);
    CHECK(copy({ 0x42, 0x00, 0x42, 0x00, 0x41, 0x00, 0xFC, 0x0A, 0x00, 0x00 }, 1, 0)
          == ValidationErrorCode::TypeMismatch);

    // memory.init 0 0: the destination by address type, the source and count i32.
    auto const init = [](std::vector<uint8_t> body) {
        auto builder = voidFunctionOf(std::move(body));
        builder.memory(atLeast64(1)).dataCount(1).data({ 0x01, 0x00 });
        return error3Of(builder);
    };
    CHECK(init({ 0x42, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x08, 0x00, 0x00 }) == std::nullopt);
    CHECK(init({ 0x42, 0x00, 0x41, 0x00, 0x42, 0x00, 0xFC, 0x08, 0x00, 0x00 })
          == ValidationErrorCode::TypeMismatch);
    CHECK(init({ 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x08, 0x00, 0x00 })
          == ValidationErrorCode::TypeMismatch);
    // An active data segment's offset is of its memory's address type.
    auto active = ModuleBuilder {};
    active.memory(atLeast64(1)).data({ 0x00, 0x42, 0x00, 0x0B, 0x00 });
    CHECK(error3Of(active) == std::nullopt);
    auto narrowOffset = ModuleBuilder {};
    narrowOffset.memory(atLeast64(1)).data({ 0x00, 0x41, 0x00, 0x0B, 0x00 });
    CHECK(error3Of(narrowOffset) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.table64AddressType")
{
    auto const table64 = TableType { .element = FuncRef, .limits = atLeast64(1) };
    auto const withTable64 = [&](std::vector<uint8_t> body) {
        auto builder = voidFunctionOf(std::move(body));
        builder.table(table64).table(TableType { .element = FuncRef, .limits = atLeast(1) });
        builder.element({ 0x01, 0x00, 0x00 });
        return error3Of(builder);
    };
    // table.get, table.set, table.size, table.grow, table.fill on the table64.
    CHECK(withTable64({ 0x42, 0x00, 0x25, 0x00, 0x1A }) == std::nullopt);
    CHECK(withTable64({ 0x41, 0x00, 0x25, 0x00, 0x1A }) == ValidationErrorCode::TypeMismatch);
    CHECK(withTable64({ 0x42, 0x00, 0xD0, 0x70, 0x26, 0x00 }) == std::nullopt);
    CHECK(withTable64({ 0xFC, 0x10, 0x00, 0x50, 0x1A }) == std::nullopt);
    CHECK(withTable64({ 0xD0, 0x70, 0x42, 0x01, 0xFC, 0x0F, 0x00, 0x50, 0x1A }) == std::nullopt);
    CHECK(withTable64({ 0xD0, 0x70, 0x41, 0x01, 0xFC, 0x0F, 0x00, 0x1A })
          == ValidationErrorCode::TypeMismatch);
    CHECK(withTable64({ 0x42, 0x00, 0xD0, 0x70, 0x42, 0x01, 0xFC, 0x11, 0x00 }) == std::nullopt);
    // table.copy 0 1: i64 destination, i32 source, i32 count; table.copy 0 0: all i64.
    CHECK(withTable64({ 0x42, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0E, 0x00, 0x01 }) == std::nullopt);
    CHECK(withTable64({ 0x42, 0x00, 0x42, 0x00, 0x42, 0x00, 0xFC, 0x0E, 0x00, 0x00 }) == std::nullopt);
    CHECK(withTable64({ 0x42, 0x00, 0x42, 0x00, 0x41, 0x00, 0xFC, 0x0E, 0x00, 0x00 })
          == ValidationErrorCode::TypeMismatch);
    // table.init 0 0: an i64 destination, an i32 source and count.
    CHECK(withTable64({ 0x42, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0C, 0x00, 0x00 }) == std::nullopt);
    // call_indirect through the table64 takes an i64 index.
    auto const call = [&](uint8_t constOpcode) {
        auto builder = voidFunctionOf({ constOpcode, 0x00, 0x11, 0x00, 0x00 });
        builder.table(table64);
        return error3Of(builder);
    };
    CHECK(call(0x42) == std::nullopt);
    CHECK(call(0x41) == ValidationErrorCode::TypeMismatch);
    // An active element segment's offset is of its table's address type (flags 0: table 0).
    auto active = ModuleBuilder {};
    active.type(FuncType {})
        .function(0, {}, {})
        .table(table64)
        .element({ 0x00, 0x42, 0x00, 0x0B, 0x01, 0x00 });
    CHECK(error3Of(active) == std::nullopt);
    auto narrowOffset = ModuleBuilder {};
    narrowOffset.type(FuncType {})
        .function(0, {}, {})
        .table(table64)
        .element({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00 });
    CHECK(error3Of(narrowOffset) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.addressTypeBounds")
{
    using isoline::binary::maxPages;
    using isoline::binary::maxTableSize;
    using isoline::binary::MemoryType;
    CHECK(maxPages(MemoryType { .limits = atLeast(0) }) == 65'536);
    CHECK(maxPages(MemoryType { .limits = atLeast64(0) }) == uint64_t { 1 } << 48);
    CHECK(maxTableSize(TableType { .element = FuncRef, .limits = atLeast(0) }) == 0xFFFF'FFFF);
    CHECK(maxTableSize(TableType { .element = FuncRef, .limits = atLeast64(0) }) == ~uint64_t { 0 });

    auto const messageOf = [](ModuleBuilder const& builder) {
        auto const result = validated(builder, FeatureSet::wasm3());
        REQUIRE_FALSE(result.has_value());
        return specMessage(result.error());
    };
    auto memory = ModuleBuilder {};
    memory.memory(atLeast(65'537));
    CHECK(messageOf(memory) == "memory size must be at most 2^16 pages (4 GiB) for i32");
    auto memory64 = ModuleBuilder {};
    memory64.memory(atLeast64((uint64_t { 1 } << 48) + 1));
    CHECK(messageOf(memory64) == "memory size must be at most 2^48 pages (256 TiB) for i64");
    auto full64 = ModuleBuilder {};
    full64.memory(
        Limits { .min = 0, .max = uint64_t { 1 } << 48, .addressType = isoline::binary::AddressType::I64 });
    CHECK(error3Of(full64) == std::nullopt);
    // An i32 table's u64 bounds may pass 2^32-1 in the 3.0 grammar.
    auto table = ModuleBuilder {};
    table.table(TableType { .element = FuncRef, .limits = atLeast(uint64_t { 1 } << 32) });
    CHECK(messageOf(table) == "table size must be at most 2^32-1 for i32");
    auto table64 = ModuleBuilder {};
    table64.table(TableType { .element = FuncRef, .limits = atLeast64(~uint64_t { 0 }) });
    CHECK(error3Of(table64) == std::nullopt);
}

TEST_CASE("Validator.subjectOfAnErrorWithoutDetail")
{
    // A row without a detail spells its text alone, whatever subject an error carries.
    CHECK(specMessage(ValidationError { ValidationErrorCode::TypeMismatch, NoFunction, NoInstruction, 3 })
          == "type mismatch");
}
