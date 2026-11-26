// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <type_traits>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/validate/ValidatedModule.hpp>
#include <isoline/validate/ValidationError.hpp>
#include <isoline/validate/Validator.hpp>

using isoline::FeatureSet;
using isoline::binary::DecodeError;
using isoline::binary::DecodeLimits;
using isoline::binary::FuncType;
using isoline::binary::testing::ModuleBuilder;
using isoline::validate::FrameShape;
using isoline::validate::ValidatedModule;
using isoline::validate::ValidationError;
using isoline::validate::ValidationErrorCode;

namespace
{

/// A module with nothing in it.
std::vector<std::byte> emptyModuleBytes()
{
    return ModuleBuilder {}.build();
}

/// A module of one function [] -> [i32] whose body adds on an empty stack.
std::vector<std::byte> addOnEmptyStackBytes()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { isoline::binary::I32 } }).function(0, {}, { 0x6A });
    return builder.build();
}

/// A module whose magic is wrong.
std::vector<std::byte> badMagicBytes()
{
    auto bytes = emptyModuleBytes();
    bytes.front() = std::byte { 0x01 };
    return bytes;
}

} // namespace

TEST_CASE("ValidatedModule.carriesItsFeatures")
{
    auto validated = ValidatedModule::load(emptyModuleBytes(), FeatureSet::wasm3(), DecodeLimits {});
    REQUIRE(validated.has_value());
    CHECK((*validated)->features() == FeatureSet::wasm3());
    CHECK(ValidatedModule::moduleOf(*validated).get() == &(*validated)->module());
    STATIC_CHECK_FALSE(std::is_default_constructible_v<ValidatedModule::Key>); // the passkey is private
}

TEST_CASE("ValidatedModule.moduleOfSharesOwnership")
{
    auto validated = ValidatedModule::load(emptyModuleBytes(), FeatureSet::wasm2(), DecodeLimits {});
    REQUIRE(validated.has_value());
    auto const module = ValidatedModule::moduleOf(*validated);
    CHECK(module.use_count() == validated->use_count());
    CHECK((*validated)->features() == FeatureSet::wasm2());
}

TEST_CASE("ValidatedModule.refusesAnInvalidModule")
{
    auto const validated =
        ValidatedModule::load(addOnEmptyStackBytes(), FeatureSet::wasm3(), DecodeLimits {});
    REQUIRE_FALSE(validated.has_value());
    CHECK(std::get<ValidationError>(validated.error()).code == ValidationErrorCode::TypeMismatch);
    CHECK(std::holds_alternative<DecodeError>(
        ValidatedModule::load(badMagicBytes(), FeatureSet::wasm3(), DecodeLimits {}).error()));
}

TEST_CASE("ValidatedModule.decodesWithItsLimits")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).type(FuncType {});
    auto const refused =
        ValidatedModule::load(builder.build(), FeatureSet::wasm3(), DecodeLimits { .maxTypes = 1 });
    REQUIRE_FALSE(refused.has_value());
    CHECK(std::holds_alternative<DecodeError>(refused.error()));
}

TEST_CASE("ValidatedModule.frameShape")
{
    // (block i32.const 1 drop) and (i32.const 1 i32.const 2 drop drop), by code index.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .function(0, {}, { 0x02, 0x40, 0x41, 0x01, 0x1A, 0x0B })
        .function(0, {}, { 0x41, 0x01, 0x41, 0x02, 0x1A, 0x1A });
    auto const validated = ValidatedModule::load(builder.build(), FeatureSet::wasm3(), DecodeLimits {});
    REQUIRE(validated.has_value());
    CHECK((*validated)->frameShape(0) == FrameShape { .maxOperands = 1, .maxLabels = 1 });
    CHECK((*validated)->frameShape(1) == FrameShape { .maxOperands = 2, .maxLabels = 0 });
}

TEST_CASE("ValidatedModule.frameCost")
{
    // params + locals + the shape's most operands and deepest labels + 1, by code index: function 0 takes
    // 2 params, declares 3 locals and holds (block i32.const 1 drop): 2 + 3 + 1 + 1 + 1 = 8; function 1
    // holds nothing: 0 + 0 + 0 + 0 + 1 = 1.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { isoline::binary::I32, isoline::binary::I64 }, .results = {} })
        .type(FuncType {})
        .function(0,
                  { isoline::binary::I32, isoline::binary::I32, isoline::binary::F64 },
                  { 0x02, 0x40, 0x41, 0x01, 0x1A, 0x0B })
        .function(1, {}, {});
    auto const validated = ValidatedModule::load(builder.build(), FeatureSet::wasm3(), DecodeLimits {});
    REQUIRE(validated.has_value());
    CHECK((*validated)->frameShape(0) == FrameShape { .maxOperands = 1, .maxLabels = 1 });
    CHECK((*validated)->frameCost(0) == 8);
    CHECK((*validated)->frameCost(1) == 1);
}
