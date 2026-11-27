// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/FeatureSupport.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::Feature;
using isoline::FeatureCount;
using isoline::FeatureSet;
using isoline::binary::concreteRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Limits;
using isoline::binary::Mutability;
using isoline::binary::Nullable;
using isoline::binary::Opcode;
using isoline::binary::TableType;
using isoline::binary::testing::encodeValueType;
using isoline::binary::testing::ModuleBuilder;
using isoline::compiler::checkModule;
using isoline::compiler::checkOpcode;
using isoline::compiler::checkValueType;
using isoline::compiler::compiledFeatures;
using isoline::compiler::CompileError;
using isoline::compiler::CompileErrorCode;
using isoline::compiler::CompilerFeatureTable;
using isoline::compiler::CompilerSupport;
using isoline::validate::ValidatedModule;

namespace
{

/// The features P5 defers to P5b, as the plan's Decisions list them.
constexpr auto DeferredFeatures = FeatureSet::none()
                                      .with(Feature::Simd)
                                      .with(Feature::RelaxedSimd)
                                      .with(Feature::TailCall)
                                      .with(Feature::TypedFunctionReferences)
                                      .with(Feature::ExceptionHandling)
                                      .with(Feature::WideArithmetic);

/// (ref null 0): a nullable reference to the module's first type.
constexpr auto RefNullType0 = concreteRef(0, Nullable::Yes);

Limits atLeast(uint64_t min)
{
    return Limits { .min = min, .max = std::nullopt };
}

/// @p builder's module, decoded and validated with every feature; fails the test unless it is valid.
std::shared_ptr<ValidatedModule const> validated(ModuleBuilder const& builder)
{
    auto module = ValidatedModule::load(builder.build(), FeatureSet::all(), isoline::binary::DecodeLimits {});
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// The refusal of @p feature's use in @p function.
CompileError unsupported(Feature feature, std::optional<uint32_t> function)
{
    return CompileError { .code = CompileErrorCode::UnsupportedFeature,
                          .feature = feature,
                          .function = function };
}

/// Appends @p tail to @p bytes.
std::vector<uint8_t> concat(std::vector<uint8_t> bytes, std::vector<uint8_t> const& tail)
{
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
}

} // namespace

TEST_CASE("FeatureSupport.tableIsComplete")
{
    REQUIRE(CompilerFeatureTable.size() == FeatureCount);
    auto deferred = FeatureSet::none();
    auto compiled = FeatureSet::none();
    for (auto const index: std::views::iota(std::size_t { 0 }, FeatureCount))
    {
        auto const& row = CompilerFeatureTable[index];
        INFO("row " << index << ": " << isoline::featureRow(row.feature).name);
        CHECK(static_cast<std::size_t>(row.feature) == index);
        if (row.support == CompilerSupport::Deferred)
        {
            deferred = deferred.with(row.feature);
            CHECK(row.plan == "P5b");
        }
        else
        {
            compiled = compiled.with(row.feature);
            CHECK(row.plan == "P5");
        }
    }
    CHECK(deferred == DeferredFeatures);
    CHECK(compiledFeatures() == compiled);
    static_assert(!compiledFeatures().has(Feature::Simd) && compiledFeatures().has(Feature::Memory64));
}

TEST_CASE("FeatureSupport.refusesByUseNotBySet")
{
    CHECK(checkOpcode(Opcode::I32Add, 0).has_value());
    auto const refused = checkOpcode(Opcode::V128Const, 3);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == CompileErrorCode::UnsupportedFeature);
    CHECK(refused.error().feature == Feature::Simd);
    CHECK(refused.error().function == 3);

    // return_call_ref needs tail calls and typed function references: the first deferred, in Feature order.
    auto const tailCallRef = checkOpcode(Opcode::ReturnCallRef, 1);
    REQUIRE_FALSE(tailCallRef.has_value());
    CHECK(tailCallRef.error() == unsupported(Feature::TailCall, 1));

    // Every MVP and compiled-proposal instruction is accepted, and every instruction of a deferred one
    // refused with a feature it needs.
    for (auto const& info: isoline::binary::OpcodeTable)
    {
        INFO(info.text);
        auto const checked = checkOpcode(info.opcode, 0);
        CHECK(checked.has_value() == compiledFeatures().includes(info.required));
        if (!checked)
        {
            REQUIRE(checked.error().feature.has_value());
            CHECK(info.required.has(*checked.error().feature));
        }
    }

    // A module validated with every feature on, which uses none the compiler defers, compiles: every
    // position the walk checks holds a compiled type.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { isoline::binary::I32 }, .results = { isoline::binary::I32 } })
        .importFunction("m", "f", 0)
        .importTable("m", "t", TableType { .element = isoline::binary::FuncRef, .limits = atLeast(1) })
        .importMemory("m", "memory", atLeast(1))
        .importGlobal("m", "g", GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Const })
        .table(TableType { .element = isoline::binary::FuncRef, .limits = atLeast(1) }, { 0xD0, 0x70 })
        .global(GlobalType { .type = isoline::binary::I64, .mutability = Mutability::Var }, { 0x42, 0x07 })
        .element({ 0x05, 0x70, 0x01, 0xD0, 0x70, 0x0B }) // passive funcref (ref.null func)
        .element({ 0x01, 0x00, 0x01, 0x00 })             // passive elem func 0: function indices
        .data({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0xAA })    // memory 0 at 0: one byte
        .function(0,
                  { isoline::binary::F64, isoline::binary::ExternRef },
                  {
                      0x02, 0x40, 0x0B,                   // block end
                      0x02, 0x7F, 0x41, 0x01, 0x0B, 0x1A, // block (result i32) i32.const 1 end; drop
                      0x41, 0x01, 0x41, 0x02, 0x41, 0x00, 0x1C, 0x01, 0x7F, 0x1A, // select (result i32); drop
                      0xD0, 0x70, 0x1A,                                           // ref.null func; drop
                      0x20, 0x00, 0x41, 0x01, 0x6A, // local.get 0; i32.const 1; i32.add
                  });
    CHECK(checkModule(*validated(builder)).has_value());
}

TEST_CASE("FeatureSupport.refusesAConcreteHeapInAnImportedGlobal")
{
    // (import "m" "g" (global (ref null 0))): no body refers to it, yet a Ref carries no type index.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importGlobal("m", "g", GlobalType { .type = RefNullType0, .mutability = Mutability::Const });
    auto const checked = checkModule(*validated(builder));
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error() == unsupported(Feature::TypedFunctionReferences, std::nullopt));
}

TEST_CASE("FeatureSupport.checksEveryTypePosition")
{
    // One row per type position: each module uses a deferred feature there and nowhere else, so a
    // walk that skipped the position would let it through.
    struct Row
    {
        std::string_view position;                 ///< Where the type is.
        std::function<void(ModuleBuilder&)> build; ///< Adds it to a module with type 0 = [] -> [].
        CompileError expected;                     ///< checkModule()'s refusal.
    };
    auto const rows = std::vector<Row> {
        { .position = "a function type, an import's",
          .build =
              [](ModuleBuilder& builder) {
                  builder.type(FuncType { .params = { isoline::binary::V128 }, .results = {} })
                      .importFunction("m", "f", 1);
              },
          .expected = unsupported(Feature::Simd, std::nullopt) },
        { .position = "a function type's result",
          .build =
              [](ModuleBuilder& builder) {
                  builder.type(FuncType { .params = {}, .results = { isoline::binary::V128 } });
              },
          .expected = unsupported(Feature::Simd, std::nullopt) },
        { .position = "an imported table's element",
          .build =
              [](ModuleBuilder& builder) {
                  builder.importTable("m", "t", TableType { .element = RefNullType0, .limits = atLeast(1) });
              },
          .expected = unsupported(Feature::TypedFunctionReferences, std::nullopt) },
        { .position = "an imported tag",
          .build = [](ModuleBuilder& builder) { builder.importTag("m", "e", 0); },
          .expected = unsupported(Feature::ExceptionHandling, std::nullopt) },
        { .position = "a table's element",
          .build =
              [](ModuleBuilder& builder) {
                  builder.table(TableType { .element = RefNullType0, .limits = atLeast(1) });
              },
          .expected = unsupported(Feature::TypedFunctionReferences, std::nullopt) },
        { .position = "a table's initializer",
          .build =
              [](ModuleBuilder& builder) {
                  // (table 1 funcref (ref.null 0)): (ref null 0) is a subtype of funcref.
                  builder.table(TableType { .element = isoline::binary::FuncRef, .limits = atLeast(1) },
                                { 0xD0, 0x00 });
              },
          .expected = unsupported(Feature::TypedFunctionReferences, std::nullopt) },
        { .position = "a tag",
          .build = [](ModuleBuilder& builder) { builder.tag(0); },
          .expected = unsupported(Feature::ExceptionHandling, std::nullopt) },
        { .position = "a global's type",
          .build =
              [](ModuleBuilder& builder) {
                  builder.global(GlobalType { .type = RefNullType0, .mutability = Mutability::Const },
                                 { 0xD0, 0x00 });
              },
          .expected = unsupported(Feature::TypedFunctionReferences, std::nullopt) },
        { .position = "a global's initializer",
          .build =
              [](ModuleBuilder& builder) {
                  builder.global(
                      GlobalType { .type = isoline::binary::FuncRef, .mutability = Mutability::Const },
                      { 0xD0, 0x00 });
              },
          .expected = unsupported(Feature::TypedFunctionReferences, std::nullopt) },
        { .position = "an element segment's type",
          .build =
              [](ModuleBuilder& builder) {
                  // A passive segment of (ref null 0) with no items.
                  builder.element(concat(concat({ 0x05 }, encodeValueType(RefNullType0)), { 0x00 }));
              },
          .expected = unsupported(Feature::TypedFunctionReferences, std::nullopt) },
        { .position = "an element segment's item",
          .build =
              [](ModuleBuilder& builder) {
                  // A passive funcref segment of one item, (ref.null 0).
                  builder.element(concat(concat({ 0x05 }, encodeValueType(isoline::binary::FuncRef)),
                                         { 0x01, 0xD0, 0x00, 0x0B }));
              },
          .expected = unsupported(Feature::TypedFunctionReferences, std::nullopt) },
        { .position = "a local",
          .build = [](ModuleBuilder& builder) { builder.function(0, { isoline::binary::V128 }, {}); },
          .expected = unsupported(Feature::Simd, 0) },
        { .position = "a block type",
          .build =
              [](ModuleBuilder& builder) {
                  // block (result v128) unreachable end; drop
                  builder.function(0, {}, { 0x02, 0x7B, 0x00, 0x0B, 0x1A });
              },
          .expected = unsupported(Feature::Simd, 0) },
        { .position = "a typed select, in dead code",
          .build =
              [](ModuleBuilder& builder) {
                  // unreachable; select (result v128); drop
                  builder.function(0, {}, { 0x00, 0x1C, 0x01, 0x7B, 0x1A });
              },
          .expected = unsupported(Feature::Simd, 0) },
        { .position = "a body's opcode",
          .build =
              [](ModuleBuilder& builder) {
                  // return_call 0
                  builder.function(0, {}, { 0x12, 0x00 });
              },
          .expected = unsupported(Feature::TailCall, 0) },
        { .position = "ref.null's heap, in the second body",
          .build =
              [](ModuleBuilder& builder) {
                  // ref.null 0; drop
                  builder.function(0, {}, {}).function(0, {}, { 0xD0, 0x00, 0x1A });
              },
          .expected = unsupported(Feature::TypedFunctionReferences, 1) },
    };
    for (auto const& row: rows)
    {
        INFO(row.position);
        auto builder = ModuleBuilder {};
        builder.type(FuncType {});
        row.build(builder);
        auto const checked = checkModule(*validated(builder));
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error() == row.expected);
    }
}

TEST_CASE("FeatureSupport.checkValueType")
{
    CHECK(checkValueType(isoline::binary::I64, std::nullopt).has_value());
    CHECK(checkValueType(isoline::binary::FuncRef, std::nullopt).has_value());
    CHECK(checkValueType(isoline::binary::ExternRef, 2).has_value());
    CHECK(checkValueType(isoline::binary::NonNullFuncRef, std::nullopt).has_value());
    CHECK(checkValueType(isoline::binary::V128, 2) == std::unexpected { unsupported(Feature::Simd, 2) });
    CHECK(checkValueType(concreteRef(5, Nullable::No), std::nullopt)
          == std::unexpected { unsupported(Feature::TypedFunctionReferences, std::nullopt) });
    CHECK(checkValueType(isoline::binary::ExnRef, std::nullopt)
          == std::unexpected { unsupported(Feature::ExceptionHandling, std::nullopt) });
    CHECK(checkValueType(isoline::binary::NullExnRef, 0)
          == std::unexpected { unsupported(Feature::ExceptionHandling, 0) });
}
