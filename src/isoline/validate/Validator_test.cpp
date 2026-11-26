// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
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
using isoline::binary::ExternKind;
using isoline::binary::F32;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::ModuleBuilder;
using isoline::validate::FrameShape;
using isoline::validate::ModuleShape;
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
std::expected<ModuleShape, ValidationError> validated(ModuleBuilder const& builder,
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

TEST_CASE("Validator.frameShape")
{
    // (block (block i32.const 1 i32.const 2 drop drop)): two operands at most, two labels below the
    // function's own.
    auto const nestedPushes =
        voidFunctionOf({ 0x02, 0x40, 0x02, 0x40, 0x41, 0x01, 0x41, 0x02, 0x1A, 0x1A, 0x0B, 0x0B });
    auto const shape = validated(nestedPushes, FeatureSet::wasm3());
    REQUIRE(shape.has_value());
    REQUIRE(shape->functions.size() == 1);
    CHECK(shape->functions.at(0) == FrameShape { .maxOperands = 2, .maxLabels = 2 });
}

TEST_CASE("Validator.frameShapePerFunction")
{
    // One shape per code entry, in order; an empty body has none of either, and the Unknown operands
    // of unreachable code count like any other.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .function(0, {}, {})
        .function(0, {}, { 0x00, 0x1A, 0x1A, 0x1A }) // unreachable drop drop drop: pops, pushes nothing
        .function(0, {}, { 0x00, 0x41, 0x01, 0x41, 0x02, 0x41, 0x03, 0x1A, 0x1A, 0x1A });
    auto const shape = validated(builder, FeatureSet::wasm3());
    REQUIRE(shape.has_value());
    CHECK(shape->functions
          == std::vector<FrameShape> {
              FrameShape {}, FrameShape {}, FrameShape { .maxOperands = 3, .maxLabels = 0 } });
}

TEST_CASE("Validator.frameShapeOfDeepNesting")
{
    // 100'000 nested blocks: the depth is recorded without recursing.
    constexpr auto Depth = 100'000;
    auto body = std::vector<uint8_t> {};
    body.reserve(std::size_t { 3 } * Depth);
    for ([[maybe_unused]] auto const level: std::views::iota(0, Depth))
        body.insert(body.end(), { 0x02, 0x40 });
    body.insert(body.end(), Depth, 0x0B);
    auto const shape = validated(voidFunctionOf(std::move(body)), FeatureSet::wasm3());
    REQUIRE(shape.has_value());
    CHECK(shape->functions.at(0) == FrameShape { .maxOperands = 0, .maxLabels = Depth });
}

TEST_CASE("Validator.frameShapeCountsRestoredUnknowns")
{
    // unreachable br_if 0 to the function's label of [i32 i32]: br_if pops its operands, Unknowns of
    // the polymorphic stack, and pushes them back (popAndRestore), and those are the body's only
    // operands.
    auto const restored =
        functionOf(FuncType { .params = {}, .results = { I32, I32 } }, { 0x00, 0x0D, 0x00 });
    auto const shape = validated(restored, FeatureSet::wasm3());
    REQUIRE(shape.has_value());
    CHECK(shape->functions.at(0) == FrameShape { .maxOperands = 2, .maxLabels = 0 });
}

TEST_CASE("Validator.frameShapeOfBlockParameters")
{
    // i32.const 1 block (param i32) (result i32) i32.const 2 i32.add end drop: the block's parameter
    // stays an operand inside it, beneath the constant.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .type(FuncType { .params = { I32 }, .results = { I32 } })
        .function(0, {}, { 0x41, 0x01, 0x02, 0x01, 0x41, 0x02, 0x6A, 0x0B, 0x1A });
    auto const shape = validated(builder, FeatureSet::wasm3());
    REQUIRE(shape.has_value());
    CHECK(shape->functions.at(0) == FrameShape { .maxOperands = 2, .maxLabels = 1 });
}

TEST_CASE("Validator.frameShapeOfIfElse")
{
    // i32.const 0 if (result i32) i32.const 1 else i32.const 3 i32.const 4 i32.const 5 i32.add i32.add
    // end drop: the else branch, which starts again at the if's height, is the deeper one.
    auto const ifElse = voidFunctionOf({ 0x41,
                                         0x00,
                                         0x04,
                                         0x7F,
                                         0x41,
                                         0x01,
                                         0x05,
                                         0x41,
                                         0x03,
                                         0x41,
                                         0x04,
                                         0x41,
                                         0x05,
                                         0x6A,
                                         0x6A,
                                         0x0B,
                                         0x1A });
    auto const shape = validated(ifElse, FeatureSet::wasm3());
    REQUIRE(shape.has_value());
    CHECK(shape->functions.at(0) == FrameShape { .maxOperands = 3, .maxLabels = 1 });
}

TEST_CASE("Validator.frameShapeOfLoops")
{
    // loop block loop i32.const 1 drop end end end: a loop is a label like a block.
    auto const loops =
        voidFunctionOf({ 0x03, 0x40, 0x02, 0x40, 0x03, 0x40, 0x41, 0x01, 0x1A, 0x0B, 0x0B, 0x0B });
    auto const shape = validated(loops, FeatureSet::wasm3());
    REQUIRE(shape.has_value());
    CHECK(shape->functions.at(0) == FrameShape { .maxOperands = 1, .maxLabels = 3 });
}

TEST_CASE("Validator.frameShapeOfTryTable")
{
    // block try_table (catch_all 0) i32.const 1 i32.const 2 drop drop end end: a try_table is a label.
    auto const tryTable = voidFunctionOf(
        { 0x02, 0x40, 0x1F, 0x40, 0x01, 0x02, 0x00, 0x41, 0x01, 0x41, 0x02, 0x1A, 0x1A, 0x0B, 0x0B });
    auto const shape = validated(tryTable, FeatureSet::wasm3());
    REQUIRE(shape.has_value());
    CHECK(shape->functions.at(0) == FrameShape { .maxOperands = 2, .maxLabels = 2 });
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
    CHECK(errorOf(builder) == ValidationErrorCode::ImmutableGlobal);

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
    // A tag names a function type without results: its params are the exceptions' payload.
    auto const features = FeatureSet::wasm3();
    auto const message = [&](ModuleBuilder const& builder) {
        auto const result = validated(builder, features);
        REQUIRE_FALSE(result.has_value());
        return specMessage(result.error());
    };
    auto valid = ModuleBuilder {};
    valid.type(FuncType { .params = { I32 }, .results = {} })
        .importTag("m", "t", 0)
        .tag(0)
        .exportEntity("imported", ExternKind::Tag, 0)
        .exportEntity("defined", ExternKind::Tag, 1);
    CHECK(validated(valid, features).has_value());

    auto unknownType = ModuleBuilder {};
    unknownType.type(FuncType {}).tag(7);
    CHECK(message(unknownType) == "unknown type 7");
    auto unknownImportType = ModuleBuilder {};
    unknownImportType.importTag("m", "t", 0);
    CHECK(message(unknownImportType) == "unknown type 0");

    auto unknownExport = ModuleBuilder {};
    unknownExport.type(FuncType {}).tag(0).exportEntity("t", ExternKind::Tag, 1);
    CHECK(message(unknownExport) == "unknown tag 1");
}

TEST_CASE("Validator.nonEmptyTagResult")
{
    auto const features = FeatureSet::wasm3();
    auto const returning = FuncType { .params = {}, .results = { I32 } };
    auto defined = ModuleBuilder {};
    defined.type(returning).tag(0);
    auto const definedResult = validated(defined, features);
    REQUIRE_FALSE(definedResult.has_value());
    CHECK(definedResult.error() == ValidationError { ValidationErrorCode::NonEmptyTagResultType });
    CHECK(specMessage(definedResult.error()) == "non-empty tag result type");
    auto imported = ModuleBuilder {};
    imported.type(returning).importTag("m", "t", 0);
    CHECK(errorOf(imported, features) == ValidationErrorCode::NonEmptyTagResultType);
}

namespace
{

/// A module of exception handling: type 0 [i32] -> [], the type of tag 0; type 1 [] -> [], of the one
/// function, whose body is @p body; then @p types.
ModuleBuilder throwingModule(std::vector<uint8_t> body, std::initializer_list<FuncType> types = {})
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = {} }).type(FuncType {});
    for (auto const& type: types)
        builder.type(type);
    builder.tag(0).function(1, {}, std::move(body));
    return builder;
}

/// The message validating @p builder's module, which must be invalid, reports under 3.0.
std::string invalidMessage(ModuleBuilder const& builder)
{
    auto const result = validated(builder, FeatureSet::wasm3());
    REQUIRE_FALSE(result.has_value());
    return specMessage(result.error());
}

} // namespace

TEST_CASE("Validator.throwMismatchDetail")
{
    // throw pops its tag's params at once, as valid.ml does, and names them and what the block has.
    CHECK(invalidMessage(throwingModule({ 0x08, 0x00 }))
          == "type mismatch: instruction requires [i32] but stack has []");
    CHECK(invalidMessage(throwingModule({ 0x42, 0x00, 0x08, 0x00 }))
          == "type mismatch: instruction requires [i32] but stack has [i64]");
    auto const result = validated(throwingModule({ 0x08, 0x00 }), FeatureSet::wasm3());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().mismatch.has_value());
    CHECK(result.error().mismatch->required == std::vector { I32 });
    CHECK(result.error().mismatch->actual.empty());
    // Only the block's operands are the stack's: one beneath the block is not.
    CHECK(invalidMessage(throwingModule({ 0x41, 0x00, 0x02, 0x40, 0x08, 0x00, 0x0B, 0x1A }))
          == "type mismatch: instruction requires [i32] but stack has []");
    // An unreachable block's stack supplies operands of unknown type beneath its own, spelled bot.
    auto twoParams = ModuleBuilder {};
    twoParams.type(FuncType { .params = { I32, I32 }, .results = {} })
        .type(FuncType {})
        .tag(0)
        .function(1, {}, { 0x00, 0x42, 0x00, 0x08, 0x00 });
    CHECK(invalidMessage(twoParams)
          == "type mismatch: instruction requires [i32 i32] but stack has [bot i64]");
    // A reference is spelled as the text format writes it.
    CHECK(invalidMessage(throwingModule({ 0xD0, 0x69, 0x08, 0x00 }))
          == "type mismatch: instruction requires [i32] but stack has [(ref null exn)]");
    // Enough operands of the right types pass, and the stack is polymorphic afterwards.
    CHECK(validated(throwingModule({ 0x41, 0x07, 0x08, 0x00, 0x6A, 0x1A }), FeatureSet::wasm3()).has_value());
    CHECK(validated(throwingModule({ 0x00, 0x08, 0x00 }), FeatureSet::wasm3()).has_value());
    // An operand of unknown type on the stack, unreachable select's, matches the i32 too.
    CHECK(validated(throwingModule({ 0x00, 0x1B, 0x08, 0x00 }), FeatureSet::wasm3()).has_value());
    // The error records the operands, and compares by them.
    auto const missing = validated(throwingModule({ 0x42, 0x00, 0x08, 0x00 }), FeatureSet::wasm3());
    REQUIRE_FALSE(missing.has_value());
    CHECK(
        missing.error()
        == ValidationError { ValidationErrorCode::TypeMismatch,
                             0,
                             1,
                             std::nullopt,
                             isoline::validate::OperandMismatch { .required = { I32 }, .actual = { I64 } } });
    CHECK(
        missing.error()
        != ValidationError { ValidationErrorCode::TypeMismatch,
                             0,
                             1,
                             std::nullopt,
                             isoline::validate::OperandMismatch { .required = { I32 }, .actual = { I32 } } });
    CHECK(invalidMessage(throwingModule({ 0x08, 0x01 })) == "unknown tag 1");
}

TEST_CASE("Validator.typeText")
{
    // valid.ml's string_of_valtype, as type mismatches spell their operands.
    using isoline::binary::AbstractHeapType;
    using isoline::binary::Nullable;
    using isoline::validate::resultTypeText;
    using isoline::validate::typeText;
    CHECK(typeText(I64) == "i64");
    CHECK(typeText(isoline::binary::V128) == "v128");
    CHECK(typeText(std::nullopt) == "bot");
    CHECK(typeText(FuncRef) == "(ref null func)");
    CHECK(typeText(isoline::binary::NonNullFuncRef) == "(ref func)");
    CHECK(typeText(isoline::binary::concreteRef(3, Nullable::No)) == "(ref 3)");
    CHECK(typeText(isoline::binary::concreteRef(3, Nullable::Yes)) == "(ref null 3)");
    CHECK(typeText(isoline::binary::ValueType { .kind = isoline::binary::ValueKind::Ref,
                                                .nullable = Nullable::No,
                                                .heap = { .kind = AbstractHeapType::Bot } })
          == "(ref something)");
    CHECK(resultTypeText(std::vector { I32, isoline::binary::ExnRef }) == "[i32 (ref null exn)]");
    CHECK(resultTypeText(std::vector<isoline::binary::ValueType> {}) == "[]");
}

TEST_CASE("Validator.throwRef")
{
    CHECK(validated(throwingModule({ 0xD0, 0x69, 0x0A }), FeatureSet::wasm3()).has_value());
    CHECK(validated(throwingModule({ 0xD0, 0x74, 0x0A }), FeatureSet::wasm3()).has_value()); // nullexnref
    CHECK(validated(throwingModule({ 0x00, 0x0A }), FeatureSet::wasm3()).has_value());
    CHECK(errorOf(throwingModule({ 0xD0, 0x70, 0x0A }), FeatureSet::wasm3())
          == ValidationErrorCode::TypeMismatch);
    CHECK(errorOf(throwingModule({ 0x0A }), FeatureSet::wasm3()) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.tryTableClauses")
{
    auto const features = FeatureSet::wasm3();
    auto const valid = [&](std::vector<uint8_t> body, std::initializer_list<FuncType> types = {}) {
        return validated(throwingModule(std::move(body), types), features).has_value();
    };
    auto const error = [&](std::vector<uint8_t> body, std::initializer_list<FuncType> types = {}) {
        return errorOf(throwingModule(std::move(body), types), features);
    };
    auto const exnRef = isoline::binary::ExnRef;
    // block (result i32) try_table (catch 0 0) i32.const 1 throw 0 end i32.const 0 end drop: catch hands
    // the tag's params, [i32], to the block, which carries [i32].
    CHECK(valid({ 0x02,
                  0x7F,
                  0x1F,
                  0x40,
                  0x01,
                  0x00,
                  0x00,
                  0x00,
                  0x41,
                  0x01,
                  0x08,
                  0x00,
                  0x0B,
                  0x41,
                  0x00,
                  0x0B,
                  0x1A }));
    // catch_ref hands [i32 (ref exn)]: to a block of [i32 exnref] (type 2), not to one of [i32].
    CHECK(valid({ 0x02, 0x02, 0x1F, 0x40, 0x01, 0x01, 0x00, 0x00, 0x0B, 0x00, 0x0B, 0x1A, 0x1A },
                { FuncType { .params = {}, .results = { I32, exnRef } } }));
    CHECK(error({ 0x02, 0x7F, 0x1F, 0x40, 0x01, 0x01, 0x00, 0x00, 0x0B, 0x00, 0x0B, 0x1A })
          == ValidationErrorCode::TypeMismatch);
    // catch_all hands nothing; catch_all_ref (ref exn), which an exnref label takes.
    CHECK(valid({ 0x1F, 0x40, 0x01, 0x02, 0x00, 0x0B }));
    CHECK(error({ 0x02, 0x7F, 0x1F, 0x40, 0x01, 0x02, 0x00, 0x0B, 0x00, 0x0B, 0x1A })
          == ValidationErrorCode::TypeMismatch);
    CHECK(valid({ 0x02, 0x69, 0x1F, 0x40, 0x01, 0x03, 0x00, 0x0B, 0xD0, 0x69, 0x0B, 0x1A }));
    CHECK(error({ 0x1F, 0x40, 0x01, 0x03, 0x00, 0x0B }) == ValidationErrorCode::TypeMismatch);
    // A clause's label counts from outside the try_table: label 0 is the function's here, which has no
    // results, though the try_table's own label would take an i32.
    CHECK(valid({ 0x1F, 0x7F, 0x01, 0x02, 0x00, 0x41, 0x00, 0x0B, 0x1A }));
    CHECK(invalidMessage(throwingModule({ 0x1F, 0x40, 0x01, 0x02, 0x01, 0x0B })) == "unknown label 1");
    CHECK(invalidMessage(throwingModule({ 0x1F, 0x40, 0x01, 0x00, 0x05, 0x00, 0x0B })) == "unknown tag 5");
    // A try_table takes its params as a block does, and is branched to as one.
    CHECK(valid({ 0x41, 0x01, 0x1F, 0x02, 0x00, 0x0C, 0x00, 0x0B, 0x1A },
                { FuncType { .params = { I32 }, .results = { I32 } } }));
    CHECK(error({ 0x1F, 0x02, 0x00, 0x0B, 0x1A }, { FuncType { .params = { I32 }, .results = { I32 } } })
          == ValidationErrorCode::TypeMismatch);
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

TEST_CASE("Validator.invalidPageSize")
{
    auto const all = FeatureSet::all();
    auto const memoryOf = [](uint8_t pageSizeLog2,
                             Limits const& limits = Limits { .min = 1, .max = std::nullopt }) {
        auto builder = ModuleBuilder {};
        builder.memory(MemoryType { .limits = limits, .pageSizeLog2 = pageSizeLog2 });
        return builder;
    };
    // Only 1-byte and 64 KiB pages are valid; spec custom-page-sizes-invalid.wast tries 2 to 2^17.
    CHECK(errorOf(memoryOf(0), all) == std::nullopt);
    CHECK(errorOf(memoryOf(16), all) == std::nullopt);
    for (auto const exponent: { 1, 12, 15, 17, 63 })
    {
        INFO("exponent " << exponent);
        CHECK(errorOf(memoryOf(static_cast<uint8_t>(exponent)), all)
              == ValidationErrorCode::InvalidCustomPageSize);
    }
    auto const result = validated(memoryOf(12), all);
    REQUIRE_FALSE(result.has_value());
    CHECK(specMessage(result.error()) == "invalid custom page size");

    // 1-byte pages: as many as the address type's largest address (spec memory_max*.wast).
    auto const i32Pages = [](uint64_t min) {
        return Limits { .min = min, .max = std::nullopt };
    };
    auto const i64Pages = [](uint64_t min) {
        return Limits { .min = min, .max = std::nullopt, .addressType = isoline::binary::AddressType::I64 };
    };
    CHECK(errorOf(memoryOf(0, i32Pages(0xFFFF'FFFF)), all) == std::nullopt);
    CHECK(errorOf(memoryOf(0, i32Pages(uint64_t { 1 } << 32)), all)
          == ValidationErrorCode::MemorySizeMustBeAtMost);
    CHECK(errorOf(memoryOf(0, i64Pages(~uint64_t { 0 })), all) == std::nullopt);
    CHECK(errorOf(memoryOf(16, i64Pages(uint64_t { 1 } << 48)), all) == std::nullopt);
    CHECK(errorOf(memoryOf(16, i64Pages((uint64_t { 1 } << 48) + 1)), all)
          == ValidationErrorCode::MemorySizeMustBeAtMost);
    // The message names the bound of the memory's own page size.
    auto const tooLarge = validated(memoryOf(0, i32Pages(uint64_t { 1 } << 32)), all);
    REQUIRE_FALSE(tooLarge.has_value());
    CHECK(specMessage(tooLarge.error()) == "memory size must be at most 4294967295 pages for i32");
    auto const tooLarge64k = validated(memoryOf(16, i32Pages(65'537)), all);
    REQUIRE_FALSE(tooLarge64k.has_value());
    CHECK(specMessage(tooLarge64k.error()) == "memory size must be at most 2^16 pages (4 GiB) for i32");
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
    // The decoder reads up to DecodeLimits::maxMemories; the validator allows one before multi-memory.
    auto memories = ModuleBuilder {};
    memories.memory(atLeast(1)).memory(atLeast(1));
    CHECK(errorOf(memories) == ValidationErrorCode::MultipleMemories);
}

TEST_CASE("Validator.memoryIndexImmediates")
{
    auto const twoMemories = [](std::vector<uint8_t> body) {
        auto builder = voidFunctionOf(std::move(body));
        builder.memory(atLeast(1)).memory(atLeast(1));
        return builder;
    };
    auto const wasm3 = FeatureSet::wasm3();
    // i32.load from memory 1 is valid; from memory 2 it names no memory.
    CHECK(errorOf(twoMemories({ 0x41, 0x00, 0x28, 0x42, 0x01, 0x00, 0x1A }), wasm3) == std::nullopt);
    CHECK(errorOf(twoMemories({ 0x41, 0x00, 0x28, 0x42, 0x02, 0x00, 0x1A }), wasm3)
          == ValidationErrorCode::UnknownMemory);
    // The alignment field's six bits reach 63; validation bounds them by the natural alignment.
    CHECK(errorOf(twoMemories({ 0x41, 0x00, 0x28, 0x43, 0x01, 0x00, 0x1A }), wasm3)
          == ValidationErrorCode::AlignmentMustNotBeLarger);
    CHECK(errorOf(twoMemories({ 0x41, 0x00, 0x28, 0x3F, 0x00, 0x1A }), wasm3)
          == ValidationErrorCode::AlignmentMustNotBeLarger);
    // memory.size, memory.grow, memory.fill and memory.copy (either side) name their memories by
    // index; memory 1 exists, memory 2 does not.
    struct Row
    {
        std::vector<uint8_t> body;
        std::optional<ValidationErrorCode> expected;
    };
    auto const rows = std::to_array<Row>({
        { .body = { 0x3F, 0x01, 0x1A }, .expected = std::nullopt },
        { .body = { 0x3F, 0x02, 0x1A }, .expected = ValidationErrorCode::UnknownMemory },
        { .body = { 0x41, 0x00, 0x40, 0x01, 0x1A }, .expected = std::nullopt },
        { .body = { 0x41, 0x00, 0x40, 0x02, 0x1A }, .expected = ValidationErrorCode::UnknownMemory },
        { .body = { 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0B, 0x01 }, .expected = std::nullopt },
        { .body = { 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0B, 0x02 },
          .expected = ValidationErrorCode::UnknownMemory },
        { .body = { 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0A, 0x00, 0x01 }, .expected = std::nullopt },
        { .body = { 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0A, 0x01, 0x02 }, // source
          .expected = ValidationErrorCode::UnknownMemory },
        { .body = { 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0A, 0x02, 0x01 }, // destination
          .expected = ValidationErrorCode::UnknownMemory },
        // i32.store to memory 1, then to memory 2.
        { .body = { 0x41, 0x00, 0x41, 0x00, 0x36, 0x42, 0x01, 0x00 }, .expected = std::nullopt },
        { .body = { 0x41, 0x00, 0x41, 0x00, 0x36, 0x42, 0x02, 0x00 },
          .expected = ValidationErrorCode::UnknownMemory },
    });
    auto index = 0;
    for (auto const& row: rows)
    {
        INFO("row " << index++);
        CHECK(errorOf(twoMemories(row.body), wasm3) == row.expected);
    }

    // v128.load8_lane lane 0 from memory 1, then from memory 2.
    auto const laneLoad = [](uint8_t memory) {
        auto body = std::vector<uint8_t> { 0x41, 0x00, 0xFD, 0x0C };
        body.insert(body.end(), 16, 0x00);
        body.insert(body.end(), { 0xFD, 0x54, 0x40, memory, 0x00, 0x00, 0x1A });
        return body;
    };
    CHECK(errorOf(twoMemories(laneLoad(1)), wasm3) == std::nullopt);
    CHECK(errorOf(twoMemories(laneLoad(2)), wasm3) == ValidationErrorCode::UnknownMemory);

    // memory.init of data segment 0 into memory 1, then into memory 2.
    auto const memoryInit = [&](uint8_t memory) {
        auto builder = twoMemories({ 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x08, 0x00, memory });
        builder.dataCount(1).data({ 0x01, 0x00 }); // one passive, empty segment
        return builder;
    };
    CHECK(errorOf(memoryInit(1), wasm3) == std::nullopt);
    CHECK(errorOf(memoryInit(2), wasm3) == ValidationErrorCode::UnknownMemory);
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
    // WebAssembly 3.0's wording, which the 3.0 suite asserts.
    CHECK(specMessage(ValidationErrorCode::ImmutableGlobal) == "immutable global");
    CHECK(specMessage(ValidationErrorCode::UninitializedLocal) == "uninitialized local");
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
    // A row without a detail spells its text alone, whatever subject an error carries; so does a type
    // mismatch that records no operands.
    CHECK(
        specMessage(ValidationError { ValidationErrorCode::InvalidResultArity, NoFunction, NoInstruction, 3 })
        == "invalid result arity");
    CHECK(specMessage(ValidationError { ValidationErrorCode::TypeMismatch, NoFunction, NoInstruction, 3 })
          == "type mismatch");
}

namespace
{

/// (ref $index): a non-null reference to the module's type @p index.
isoline::binary::ValueType refTo(uint32_t index)
{
    return isoline::binary::concreteRef(index, isoline::binary::Nullable::No);
}

/// (ref null $index): a nullable reference to the module's type @p index.
isoline::binary::ValueType refNullTo(uint32_t index)
{
    return isoline::binary::concreteRef(index, isoline::binary::Nullable::Yes);
}

/// (ref func): a non-null reference to any function.
constexpr auto NonNullFunc = isoline::binary::ValueType {
    .kind = isoline::binary::ValueKind::Ref,
    .nullable = isoline::binary::Nullable::No,
    .heap = { .kind = isoline::binary::AbstractHeapType::Func },
};

} // namespace

TEST_CASE("Validator.selfReferenceIsUnknownType")
{
    auto const wasm3 = FeatureSet::wasm3();
    // Without rec groups a type may only name the types before it.
    auto self = ModuleBuilder {};
    self.type(FuncType { .params = { refNullTo(0) }, .results = {} });
    auto const result = validated(self, wasm3);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == ValidationError { ValidationErrorCode::UnknownType, NoFunction, NoInstruction, 0 });
    auto forward = ModuleBuilder {};
    forward.type(FuncType { .params = {}, .results = { refTo(1) } }).type(FuncType {});
    CHECK(errorOf(forward, wasm3) == ValidationErrorCode::UnknownType);
    auto backward = ModuleBuilder {};
    backward.type(FuncType {}).type(FuncType { .params = {}, .results = { refTo(0) } });
    CHECK(errorOf(backward, wasm3) == std::nullopt);
}

TEST_CASE("Validator.unknownTypeInValueTypes")
{
    auto const wasm3 = FeatureSet::wasm3();
    auto const subjectOf = [&](ModuleBuilder const& builder) {
        auto const result = validated(builder, wasm3);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ValidationErrorCode::UnknownType);
        return result.error().subject;
    };
    // A local, a block type, a typed select, ref.null.
    CHECK(subjectOf(functionOf(FuncType {}, {}, { refNullTo(3) })) == 3U);
    CHECK(subjectOf(voidFunctionOf({ 0x02, 0x63, 0x04, 0x00, 0x0B, 0x1A })) == 4U);
    CHECK(subjectOf(voidFunctionOf({ 0x00, 0x1C, 0x01, 0x63, 0x05, 0x1A })) == 5U);
    CHECK(subjectOf(voidFunctionOf({ 0xD0, 0x06, 0x1A })) == 6U);
    // A global, an imported global, a table, an imported table, an element segment.
    auto global = ModuleBuilder {};
    global.global(GlobalType { .type = refNullTo(7), .mutability = Mutability::Const }, { 0xD0, 0x07 });
    CHECK(subjectOf(global) == 7U);
    auto importedGlobal = ModuleBuilder {};
    importedGlobal.importGlobal(
        "m", "g", GlobalType { .type = refNullTo(8), .mutability = Mutability::Const });
    CHECK(subjectOf(importedGlobal) == 8U);
    auto table = ModuleBuilder {};
    table.table(TableType { .element = refNullTo(9), .limits = atLeast(1) });
    CHECK(subjectOf(table) == 9U);
    auto importedTable = ModuleBuilder {};
    importedTable.importTable("m", "t", TableType { .element = refNullTo(10), .limits = atLeast(1) });
    CHECK(subjectOf(importedTable) == 10U);
    auto segment = ModuleBuilder {};
    segment.element({ 0x05, 0x63, 0x0B, 0x00 }); // passive, (ref null 11), no items
    CHECK(subjectOf(segment) == 11U);
}

TEST_CASE("Validator.uninitializedLocal")
{
    auto const wasm3 = FeatureSet::wasm3();
    // local 0 is (ref func), which has no default; function 0 is exported, so ref.func may name it.
    auto const withLocal = [&](std::vector<uint8_t> body) {
        auto builder = functionOf(FuncType {}, std::move(body), { NonNullFunc });
        builder.exportFunction("f", 0);
        return errorOf(builder, wasm3);
    };
    auto const set = std::vector<uint8_t> { 0xD2, 0x00, 0x21, 0x00 }; // ref.func 0, local.set 0
    auto const get = std::vector<uint8_t> { 0x20, 0x00, 0x1A };       // local.get 0, drop
    auto const join = [](std::initializer_list<std::vector<uint8_t>> parts) {
        auto bytes = std::vector<uint8_t> {};
        for (auto const& part: parts)
            bytes.insert(bytes.end(), part.begin(), part.end());
        return bytes;
    };

    auto const unset = validated(functionOf(FuncType {}, get, { NonNullFunc }), wasm3);
    REQUIRE_FALSE(unset.has_value());
    CHECK(unset.error() == ValidationError { ValidationErrorCode::UninitializedLocal, 0, 0 });
    CHECK(specMessage(unset.error()) == "uninitialized local");
    CHECK(withLocal(join({ set, get })) == std::nullopt);
    // local.tee sets it too.
    CHECK(withLocal(join({ { 0xD2, 0x00, 0x22, 0x00, 0x1A }, get })) == std::nullopt);
    // A set in a block lasts to the block's end, and no further.
    CHECK(withLocal(join({ { 0x02, 0x40 }, set, get, { 0x0B } })) == std::nullopt);
    CHECK(withLocal(join({ { 0x02, 0x40 }, set, { 0x0B }, get })) == ValidationErrorCode::UninitializedLocal);
    CHECK(withLocal(join({ { 0x03, 0x40 }, set, { 0x0B }, get })) == ValidationErrorCode::UninitializedLocal);
    // A set before a block holds within it, and within the blocks nested in it.
    CHECK(withLocal(join({ set, { 0x02, 0x40, 0x02, 0x40 }, get, { 0x0B, 0x0B }, get })) == std::nullopt);
    // An if's then branch does not set it for the else branch, nor after the if.
    auto const inThen = join({ { 0x41, 0x01, 0x04, 0x40 }, set, { 0x05 }, get, { 0x0B } });
    CHECK(withLocal(inThen) == ValidationErrorCode::UninitializedLocal);
    CHECK(withLocal(join({ { 0x41, 0x01, 0x04, 0x40 }, set, { 0x05 }, set, { 0x0B }, get }))
          == ValidationErrorCode::UninitializedLocal);
    // Unreachable code is checked all the same.
    CHECK(withLocal(join({ { 0x00 }, get })) == ValidationErrorCode::UninitializedLocal);
    // Parameters and nullable locals are always set.
    CHECK(errorOf(functionOf(FuncType { .params = { NonNullFunc }, .results = {} }, get), wasm3)
          == std::nullopt);
    CHECK(errorOf(functionOf(FuncType {}, get, { refNullTo(0) }), wasm3) == std::nullopt);
}

TEST_CASE("Validator.uninitializedLocalAmongManyLocals")
{
    // 50 000 locals without a default cost one run, and setting the last leaves the others unset.
    auto const wasm3 = FeatureSet::wasm3();
    auto const last = encodeU32(49'999);
    auto body = std::vector<uint8_t> { 0xD2, 0x00, 0x21 };
    body.insert(body.end(), last.begin(), last.end());
    body.push_back(0x20);
    body.insert(body.end(), last.begin(), last.end());
    body.push_back(0x1A);
    auto valid = functionOf(FuncType {}, body, std::vector(50'000, NonNullFunc));
    valid.exportFunction("f", 0);
    CHECK(errorOf(valid, wasm3) == std::nullopt);
    body.insert(body.end(), { 0x20, 0x00, 0x1A });
    auto invalid = functionOf(FuncType {}, body, std::vector(50'000, NonNullFunc));
    invalid.exportFunction("f", 0);
    CHECK(errorOf(invalid, wasm3) == ValidationErrorCode::UninitializedLocal);
    // The state is per function: a later function starts with its locals unset again.
    auto twice = ModuleBuilder {};
    twice.type(FuncType {})
        .function(0, { NonNullFunc }, { 0xD2, 0x00, 0x21, 0x00, 0x20, 0x00, 0x1A })
        .function(0, { NonNullFunc }, { 0x20, 0x00, 0x1A })
        .exportFunction("f", 0);
    auto const result = validated(twice, wasm3);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ValidationError { ValidationErrorCode::UninitializedLocal, 1, 0 });
}

TEST_CASE("Validator.refFuncIsTyped")
{
    auto const wasm3 = FeatureSet::wasm3();
    // Types 0 and 1 are equal; type 2 takes an i32. ref.func 0 is a (ref 0), so a (ref 1) too.
    auto const returning = [&](isoline::binary::ValueType result) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType {})
            .type(FuncType {})
            .type(FuncType { .params = { I32 }, .results = {} })
            .type(FuncType { .params = {}, .results = { result } })
            .function(0, {}, {})
            .function(3, {}, { 0xD2, 0x00 })
            .exportFunction("f", 0);
        return errorOf(builder, wasm3);
    };
    CHECK(returning(refTo(0)) == std::nullopt);
    CHECK(returning(refTo(1)) == std::nullopt);
    CHECK(returning(refNullTo(1)) == std::nullopt);
    CHECK(returning(FuncRef) == std::nullopt);
    CHECK(returning(NonNullFunc) == std::nullopt);
    CHECK(returning(refTo(2)) == ValidationErrorCode::TypeMismatch);
    CHECK(returning(isoline::binary::ExternRef) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.subtypingAtEveryPop")
{
    auto const wasm3 = FeatureSet::wasm3();
    auto const passes = [&](isoline::binary::ValueType from, isoline::binary::ValueType to) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType {})
            .type(FuncType { .params = { from }, .results = { to } })
            .function(1, {}, { 0x20, 0x00 });
        return errorOf(builder, wasm3);
    };
    CHECK(passes(refNullTo(0), FuncRef) == std::nullopt);
    CHECK(passes(refTo(0), refNullTo(0)) == std::nullopt);
    CHECK(passes(FuncRef, refNullTo(0)) == ValidationErrorCode::TypeMismatch);
    CHECK(passes(refNullTo(0), refTo(0)) == ValidationErrorCode::TypeMismatch);

    // A call's arguments, a global.set, a branch's operands, an if without else.
    auto calls = ModuleBuilder {};
    calls.type(FuncType {})
        .type(FuncType { .params = { FuncRef }, .results = {} })
        .type(FuncType { .params = { refTo(0) }, .results = {} })
        .function(1, {}, {})
        .function(2, {}, { 0x20, 0x00, 0x10, 0x00 }) // call 0 with a (ref 0) for a funcref
        .global(GlobalType { .type = FuncRef, .mutability = Mutability::Var }, { 0xD0, 0x70 })
        .function(2, {}, { 0x20, 0x00, 0x24, 0x00 }); // global.set 0
    CHECK(errorOf(calls, wasm3) == std::nullopt);
    auto branch = ModuleBuilder {};
    branch.type(FuncType {})
        .type(FuncType { .params = { refTo(0) }, .results = { FuncRef } })
        .type(FuncType { .params = { refTo(0) }, .results = { refNullTo(0) } })
        // block (result funcref) local.get 0 br 0 end
        .function(1, {}, { 0x02, 0x70, 0x20, 0x00, 0x0C, 0x00, 0x0B })
        // local.get 0 i32.const 1 if (type 2) end: the absent else passes (ref 0) on as (ref null 0)
        .function(1, {}, { 0x20, 0x00, 0x41, 0x01, 0x04, 0x02, 0x0B });
    CHECK(errorOf(branch, wasm3) == std::nullopt);
}

TEST_CASE("Validator.tablesOfTypedReferences")
{
    auto const wasm3 = FeatureSet::wasm3();
    auto const tableWith = [&](isoline::binary::ValueType element, std::optional<std::vector<uint8_t>> init) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType {}).function(0, {}, {}).exportFunction("f", 0);
        if (init)
            builder.table(TableType { .element = element, .limits = atLeast(1) }, *init);
        else
            builder.table(TableType { .element = element, .limits = atLeast(1) });
        return errorOf(builder, wasm3);
    };
    // A table of non-null references needs an initializer, whose value its slots start with.
    CHECK(tableWith(NonNullFunc, std::nullopt) == ValidationErrorCode::TypeMismatch);
    CHECK(tableWith(refTo(0), std::nullopt) == ValidationErrorCode::TypeMismatch);
    CHECK(tableWith(NonNullFunc, std::vector<uint8_t> { 0xD2, 0x00 }) == std::nullopt);
    CHECK(tableWith(refTo(0), std::vector<uint8_t> { 0xD2, 0x00 }) == std::nullopt);
    CHECK(tableWith(refNullTo(0), std::nullopt) == std::nullopt);
    CHECK(tableWith(refTo(0), std::vector<uint8_t> { 0xD0, 0x00 }) == ValidationErrorCode::TypeMismatch);
    CHECK(tableWith(FuncRef, std::vector<uint8_t> { 0xD0, 0x6F }) == ValidationErrorCode::TypeMismatch);
    CHECK(tableWith(FuncRef, std::vector<uint8_t> { 0x41, 0x00 }) == ValidationErrorCode::TypeMismatch);
    CHECK(tableWith(FuncRef, std::vector<uint8_t> { 0x01 })
          == ValidationErrorCode::ConstantExpressionRequired);

    // call_indirect takes any table of function references; an element segment of (ref func) items
    // fills a funcref table.
    auto typed = ModuleBuilder {};
    typed.type(FuncType {})
        .table(TableType { .element = refNullTo(0), .limits = atLeast(1) })
        .table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .function(0, {}, { 0x41, 0x00, 0x11, 0x00, 0x00 })
        .element({ 0x06, 0x01, 0x41, 0x00, 0x0B, 0x64, 0x70, 0x01, 0xD2, 0x00, 0x0B }); // table 1
    CHECK(errorOf(typed, wasm3) == std::nullopt);
    // table.copy from a (ref null 0) table to a funcref one, not back.
    auto copy = ModuleBuilder {};
    copy.type(FuncType {})
        .table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .table(TableType { .element = refNullTo(0), .limits = atLeast(1) })
        .function(0, {}, { 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0E, 0x00, 0x01 });
    CHECK(errorOf(copy, wasm3) == std::nullopt);
    auto copyBack = ModuleBuilder {};
    copyBack.type(FuncType {})
        .table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .table(TableType { .element = refNullTo(0), .limits = atLeast(1) })
        .function(0, {}, { 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x0E, 0x01, 0x00 });
    CHECK(errorOf(copyBack, wasm3) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.functionIndicesAreNonNullFuncRefs")
{
    auto const wasm3 = FeatureSet::wasm3();
    // spec elem.wast: a segment of function indices fills a table of (ref func), as table.init from
    // it does; a ref.func in a table initializer declares its function for the bodies.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .table(TableType { .element = NonNullFunc, .limits = atLeast(1) }, { 0xD2, 0x00 })
        .element({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00 }) // active, table 0, func 0
        .element({ 0x01, 0x00, 0x01, 0x00 })             // passive, elemkind func, func 0
        .function(0, {}, { 0x41, 0x00, 0x41, 0x00, 0x41, 0x01, 0xFC, 0x0C, 0x01, 0x00 }) // table.init 1 0
        .function(0, {}, { 0xD2, 0x00, 0x1A });                                          // ref.func 0
    CHECK(errorOf(builder, wasm3) == std::nullopt);
    // An expression segment of flag 4 is a funcref one, which no (ref func) table takes.
    auto nullable = ModuleBuilder {};
    nullable.type(FuncType {})
        .table(TableType { .element = NonNullFunc, .limits = atLeast(1) }, { 0xD2, 0x00 })
        .element({ 0x04, 0x41, 0x00, 0x0B, 0x01, 0xD2, 0x00, 0x0B })
        .function(0, {}, {});
    CHECK(errorOf(nullable, wasm3) == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.tablesBeforeGlobals")
{
    // As 3.0's check_module: a module with an invalid table and an invalid global reports the table.
    auto builder = ModuleBuilder {};
    builder.table(TableType { .element = NonNullFunc, .limits = atLeast(1) })
        .global(GlobalType { .type = I32, .mutability = Mutability::Const }, { 0x01 });
    CHECK(errorOf(builder, FeatureSet::wasm3()) == ValidationErrorCode::TypeMismatch);
    auto globalOnly = ModuleBuilder {};
    globalOnly.global(GlobalType { .type = I32, .mutability = Mutability::Const }, { 0x01 });
    CHECK(errorOf(globalOnly, FeatureSet::wasm3()) == ValidationErrorCode::ConstantExpressionRequired);
}

namespace
{

/// A module whose type 0 is [] -> [i32], type 1 [i32] -> [] and type 2 @p type, and whose one
/// function, of type 2, has @p locals and the body @p body, validated under WebAssembly 3.0.
std::optional<ValidationErrorCode> typedReferenceCheck(FuncType type,
                                                       std::vector<uint8_t> body,
                                                       std::vector<isoline::binary::ValueType> locals = {})
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { I32 } })
        .type(FuncType { .params = { I32 }, .results = {} })
        .type(std::move(type))
        .function(2, std::move(locals), std::move(body));
    return errorOf(builder, FeatureSet::wasm3());
}

} // namespace

TEST_CASE("Validator.brOnNonNullNeedsRefLabel")
{
    // br_on_non_null 0 in a block whose label carries @p label, on a (ref null 0) parameter.
    auto const branching = [](uint8_t label) {
        return typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = {} },
                                   { 0x02, label, 0x20, 0x00, 0xD6, 0x00, 0x00, 0x0B, 0x1A });
    };
    CHECK(branching(0x7F) == ValidationErrorCode::TypeMismatch); // i32
    CHECK(branching(0x6F) == ValidationErrorCode::TypeMismatch); // externref
    CHECK(branching(0x70) == std::nullopt);                      // funcref, a supertype of (ref 0)
    // A label with no types has no last one to carry the reference.
    CHECK(typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = {} },
                              { 0x02, 0x40, 0x20, 0x00, 0xD6, 0x00, 0x00, 0x0B })
          == ValidationErrorCode::TypeMismatch);
    // (ref 0) exactly: block (result (ref 0)) local.get 0 br_on_non_null 0 unreachable end drop.
    CHECK(typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = {} },
                              { 0x02, 0x64, 0x00, 0x20, 0x00, 0xD6, 0x00, 0x00, 0x0B, 0x1A })
          == std::nullopt);
    // Unreachable code still needs a reference label: the bottom operand is a (ref bot).
    CHECK(typedReferenceCheck(FuncType {}, { 0x02, 0x7F, 0x00, 0xD6, 0x00, 0x00, 0x0B, 0x1A })
          == ValidationErrorCode::TypeMismatch);
    CHECK(typedReferenceCheck(FuncType {}, { 0x02, 0x6F, 0x00, 0xD6, 0x00, 0x00, 0x0B, 0x1A })
          == std::nullopt);
    // It falls through with the label's other types, the reference consumed.
    CHECK(typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = {} },
                              { 0x02, 0x70, 0x20, 0x00, 0xD6, 0x00, 0x1A, 0x00, 0x0B, 0x1A })
          == ValidationErrorCode::TypeMismatch);
    CHECK(typedReferenceCheck(FuncType { .params = { I32 }, .results = {} }, { 0x20, 0x00, 0xD6, 0x00 })
          == ValidationErrorCode::TypeMismatch); // the label is the body's, which carries nothing
    CHECK(typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = {} },
                              { 0x20, 0x00, 0xD6, 0x01 })
          == ValidationErrorCode::UnknownLabel);
}

TEST_CASE("Validator.callRef")
{
    // call_ref 0 calls a [] -> [i32] through a (ref null 0).
    auto const calling = [](isoline::binary::ValueType operand, std::vector<uint8_t> call = { 0x14, 0x00 }) {
        auto body = std::vector<uint8_t> { 0x20, 0x00 };
        body.insert(body.end(), call.begin(), call.end());
        return typedReferenceCheck(FuncType { .params = { operand }, .results = { I32 } }, std::move(body));
    };
    CHECK(calling(refNullTo(0)) == std::nullopt);
    CHECK(calling(refTo(0)) == std::nullopt);
    CHECK(calling(refNullTo(1)) == ValidationErrorCode::TypeMismatch); // type 1 takes a parameter
    CHECK(calling(FuncRef) == ValidationErrorCode::TypeMismatch);      // not known to be a (ref null 0)
    CHECK(calling(I32) == ValidationErrorCode::TypeMismatch);
    CHECK(calling(refNullTo(0), { 0x14, 0x03 }) == ValidationErrorCode::UnknownType);
    // Its parameters below the reference, its results pushed.
    CHECK(typedReferenceCheck(FuncType { .params = { I32, refNullTo(1) }, .results = { I32 } },
                              { 0x20, 0x00, 0x20, 0x01, 0x14, 0x01, 0x1A, 0x41, 0x00 })
          == ValidationErrorCode::TypeMismatch); // type 1 returns nothing to drop
    CHECK(typedReferenceCheck(FuncType { .params = { I32, refNullTo(1) }, .results = {} },
                              { 0x20, 0x00, 0x20, 0x01, 0x14, 0x01 })
          == std::nullopt);
    CHECK(typedReferenceCheck(FuncType { .params = { I32, refNullTo(1) }, .results = {} },
                              { 0x20, 0x01, 0x14, 0x01 })
          == ValidationErrorCode::TypeMismatch); // no i32 argument
    // In unreachable code the operands are bottom, but the results are still typed.
    CHECK(calling(I32, { 0x00, 0x14, 0x00 }) == std::nullopt);
    CHECK(typedReferenceCheck(FuncType { .params = {}, .results = { I64 } }, { 0x00, 0x14, 0x00 })
          == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.refAsNonNull")
{
    auto const converting = [](isoline::binary::ValueType operand, isoline::binary::ValueType result) {
        return typedReferenceCheck(FuncType { .params = { operand }, .results = { result } },
                                   { 0x20, 0x00, 0xD4 });
    };
    CHECK(converting(refNullTo(0), refTo(0)) == std::nullopt);
    CHECK(converting(FuncRef, NonNullFunc) == std::nullopt);
    CHECK(converting(refNullTo(0), NonNullFunc) == std::nullopt);
    CHECK(converting(refNullTo(0), I32) == ValidationErrorCode::TypeMismatch);
    CHECK(converting(I32, I32) == ValidationErrorCode::TypeMismatch);
    CHECK(typedReferenceCheck(FuncType {}, { 0xD4, 0x1A })
          == ValidationErrorCode::TypeMismatch); // nothing to pop
    // On the polymorphic stack it yields a (ref bot): any reference, but no number.
    CHECK(typedReferenceCheck(FuncType { .params = {}, .results = { refTo(0) } }, { 0x00, 0xD4 })
          == std::nullopt);
    CHECK(typedReferenceCheck(FuncType { .params = {}, .results = { isoline::binary::ExternRef } },
                              { 0x00, 0xD4 })
          == std::nullopt);
    CHECK(typedReferenceCheck(FuncType {}, { 0x00, 0xD4, 0x8B, 0x1A }) == ValidationErrorCode::TypeMismatch);
    CHECK(typedReferenceCheck(FuncType {}, { 0x00, 0xD4, 0x1B, 0x1A }) == ValidationErrorCode::TypeMismatch);
    // A non-defaultable local set from it is initialized.
    CHECK(typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = { I32 } },
                              { 0x20, 0x00, 0xD4, 0x21, 0x01, 0x20, 0x01, 0x14, 0x00 },
                              { refTo(0) })
          == std::nullopt);
}

TEST_CASE("Validator.brOnNull")
{
    // block (result i32) i32.const 1 local.get 0 br_on_null 0 <then> end
    auto const branching = [](isoline::binary::ValueType operand, std::vector<uint8_t> then) {
        auto body = std::vector<uint8_t> { 0x02, 0x7F, 0x41, 0x01, 0x20, 0x00, 0xD5, 0x00 };
        body.insert(body.end(), then.begin(), then.end());
        body.push_back(0x0B);
        return typedReferenceCheck(FuncType { .params = { operand }, .results = { I32 } }, std::move(body));
    };
    // The reference falls through non-null, above the label's operands.
    CHECK(branching(refNullTo(0), { 0x14, 0x00, 0x6A }) == std::nullopt);
    CHECK(branching(FuncRef, { 0x1A }) == std::nullopt);
    CHECK(branching(isoline::binary::ExternRef, { 0x1A }) == std::nullopt);
    CHECK(branching(refNullTo(0), {}) == ValidationErrorCode::TypeMismatch);
    CHECK(branching(I32, { 0x1A }) == ValidationErrorCode::TypeMismatch);
    // The branch carries the label's types.
    CHECK(typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = { I32 } },
                              { 0x02, 0x7F, 0x20, 0x00, 0xD5, 0x00, 0x14, 0x00, 0x0B })
          == ValidationErrorCode::TypeMismatch);
    // Its non-null result is a (ref 0), which a non-defaultable local takes.
    CHECK(typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = {} },
                              { 0x20, 0x00, 0xD5, 0x00, 0x21, 0x01 },
                              { refTo(0) })
          == std::nullopt);
    // In unreachable code: a (ref bot) falls through, which is no number.
    CHECK(typedReferenceCheck(FuncType {}, { 0x00, 0xD5, 0x00, 0x1A }) == std::nullopt);
    CHECK(typedReferenceCheck(FuncType {}, { 0x00, 0xD5, 0x00, 0x8B, 0x1A })
          == ValidationErrorCode::TypeMismatch);
    CHECK(typedReferenceCheck(FuncType {}, { 0x00, 0xD5, 0x01 }) == ValidationErrorCode::UnknownLabel);
}

TEST_CASE("Validator.branchOnNullErrorOrder")
{
    // The first error is the reference interpreter's. br_on_null peeks: an empty stack is a (ref bot),
    // so an unknown label fails first, and only a top that is no reference fails before the label.
    CHECK(typedReferenceCheck(FuncType {}, { 0xD5, 0x05 }) == ValidationErrorCode::UnknownLabel);
    CHECK(typedReferenceCheck(FuncType {}, { 0x41, 0x00, 0xD5, 0x05 }) == ValidationErrorCode::TypeMismatch);
    CHECK(typedReferenceCheck(FuncType {}, { 0xD5, 0x00 }) == ValidationErrorCode::TypeMismatch);
    // br_on_non_null looks its label up before it touches the stack.
    CHECK(typedReferenceCheck(FuncType {}, { 0x41, 0x00, 0xD6, 0x05 }) == ValidationErrorCode::UnknownLabel);
    CHECK(typedReferenceCheck(FuncType {}, { 0xD6, 0x05 }) == ValidationErrorCode::UnknownLabel);
    // ref.as_non_null on an empty stack fails at its pop.
    CHECK(typedReferenceCheck(FuncType {}, { 0xD4, 0x1A }) == ValidationErrorCode::TypeMismatch);
    // An Unknown operand on the stack, an untyped select's in unreachable code, is a (ref bot) too.
    CHECK(typedReferenceCheck(FuncType {}, { 0x00, 0x1B, 0xD4, 0x1A }) == std::nullopt);
    CHECK(typedReferenceCheck(FuncType {}, { 0x00, 0x1B, 0xD4, 0x8B, 0x1A })
          == ValidationErrorCode::TypeMismatch);
    CHECK(typedReferenceCheck(FuncType {}, { 0x00, 0x1B, 0xD5, 0x00, 0x8B, 0x1A })
          == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Validator.returnCallResultMismatch")
{
    using isoline::binary::ValueType;
    // Function 0, of results @p caller, tail-calls function 1, of params @p params and results @p callee.
    auto const tailCalling = [](std::vector<ValueType> callee,
                                std::vector<ValueType> caller = { I32 },
                                std::vector<uint8_t> body = { 0x12, 0x01 },
                                std::vector<ValueType> params = {}) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = {}, .results = std::move(caller) })
            .type(FuncType { .params = std::move(params), .results = std::move(callee) })
            .function(0, {}, std::move(body))
            .function(1, {}, { 0x00 });
        return errorOf(builder, FeatureSet::wasm3());
    };
    CHECK(tailCalling({ I32 }) == std::nullopt);
    CHECK(tailCalling({ I64 }) == ValidationErrorCode::TypeMismatch);
    CHECK(tailCalling({}) == ValidationErrorCode::TypeMismatch);
    CHECK(tailCalling({ I32, I32 }) == ValidationErrorCode::TypeMismatch);
    CHECK(tailCalling({}, {}) == std::nullopt);
    // The callee's results need only be subtypes of the caller's.
    CHECK(tailCalling({ NonNullFunc }, { FuncRef }) == std::nullopt);
    CHECK(tailCalling({ FuncRef }, { NonNullFunc }) == ValidationErrorCode::TypeMismatch);
    // The arguments are popped, and the stack is polymorphic afterwards: anything may follow.
    CHECK(tailCalling({ I32 }, { I32 }, { 0x41, 0x05, 0x12, 0x01 }, { I32 }) == std::nullopt);
    CHECK(tailCalling({ I32 }, { I32 }, { 0x12, 0x01 }, { I32 }) == ValidationErrorCode::TypeMismatch);
    CHECK(tailCalling({ I32 }, { I32 }, { 0x12, 0x01, 0x1A, 0x42, 0x00, 0x1A }) == std::nullopt);
    CHECK(tailCalling({ I32 }, { I32 }, { 0x12, 0x07 }) == ValidationErrorCode::UnknownFunction);

    // return_call_indirect: as call_indirect, then the same result check.
    auto const indirect = [](std::vector<ValueType> callee, ValueType element = FuncRef) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = {}, .results = { I32 } })
            .type(FuncType { .params = {}, .results = std::move(callee) })
            .table(TableType { .element = element, .limits = atLeast(1) })
            .function(0, {}, { 0x41, 0x00, 0x13, 0x01, 0x00 });
        return errorOf(builder, FeatureSet::wasm3());
    };
    CHECK(indirect({ I32 }) == std::nullopt);
    CHECK(indirect({ I64 }) == ValidationErrorCode::TypeMismatch);
    CHECK(indirect({ I32 }, isoline::binary::ExternRef) == ValidationErrorCode::TypeMismatch);

    // return_call_ref: as call_ref, then the same result check. Type 0 is [] -> [i32].
    CHECK(typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = { I32 } },
                              { 0x20, 0x00, 0x15, 0x00 })
          == std::nullopt);
    CHECK(typedReferenceCheck(FuncType { .params = { refNullTo(0) }, .results = { I64 } },
                              { 0x20, 0x00, 0x15, 0x00 })
          == ValidationErrorCode::TypeMismatch);
    CHECK(typedReferenceCheck(FuncType { .params = { FuncRef }, .results = { I32 } },
                              { 0x20, 0x00, 0x15, 0x00 })
          == ValidationErrorCode::TypeMismatch);
    CHECK(typedReferenceCheck(FuncType { .params = {}, .results = { I32 } }, { 0x15, 0x09 })
          == ValidationErrorCode::UnknownType);
}
