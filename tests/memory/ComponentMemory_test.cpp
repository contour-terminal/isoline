// SPDX-License-Identifier: Apache-2.0

// A component must never make the host run out of memory (spec §7). The component decoder's share of
// that: a vector count reserves nothing, so a count that lies about what follows fails having
// allocated nothing beyond the decoder's copy of its input; and every item it decodes is charged
// against maxDecodedItems before it is read, so input made of the smallest items, which the pool holds
// at many times their size, holds a bounded number of them: definitions, the elements of their vectors,
// type declarations and the type scopes they open alike. The validator's share: types nest only
// through indices, so a hostile type graph is refused at its depth or effective size where it is
// interned, holding no more than the types read so far.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <vector>

#include "AllocationCounter.hpp"
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/ComponentFixtures.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/ComponentPlan.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/ValidatedComponent.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/component/validate/testing/CoreModuleValidator.hpp>

using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::binary::ComponentPool;
using isoline::component::binary::decodeComponent;
using isoline::component::binary::DecodeError;
using isoline::component::binary::DecodeErrorCode;
using isoline::component::binary::exceededLimit;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::FunctionTypeDefinition;
using isoline::component::binary::LabeledType;
using isoline::component::binary::ListType;
using isoline::component::binary::OwnType;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::RecordType;
using isoline::component::binary::Sort;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::testing::appendBytes;
using isoline::component::binary::testing::appendLeb128;
using isoline::component::binary::testing::Bytes;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::componentOf;
using isoline::component::binary::testing::componentPreamble;
using isoline::component::binary::testing::section;
using isoline::component::binary::testing::TypeScopeBuilder;
using isoline::component::validate::validateComponent;
using isoline::component::validate::ValidationError;
using isoline::component::validate::ValidationWork;
using isoline::component::validate::testing::CoreModuleValidator;
using isoline::testing::AllocationLimit;
using isoline::testing::AllocationScope;

namespace
{

/// Whether the budget let every allocation of a decode through.
enum class Budget : uint8_t
{
    Kept,    ///< Every allocation fit.
    Refused, ///< An allocation was refused (std::bad_alloc).
};

/// What decoding under a budget came to.
struct BudgetedDecode
{
    std::optional<DecodeError> error; ///< The decode error; empty when it decoded.
    Budget budget = Budget::Kept;     ///< Whether the budget refused an allocation.
};

/// Decodes @p bytes under a budget of @p budget bytes. Nothing is asserted under the budget, as an
/// assertion allocates.
BudgetedDecode decodeUnderBudget(Bytes const& bytes, std::size_t budget, ComponentLimits const& limits)
{
    auto outcome = BudgetedDecode {};
    try
    {
        auto const limit = AllocationLimit { budget };
        auto const result = decodeComponent(bytes, ComponentFeatureSet::wasi03(), limits);
        if (!result)
            outcome.error = result.error();
    }
    catch (std::bad_alloc const&)
    {
        outcome.budget = Budget::Refused;
    }
    return outcome;
}

/// The code decoding @p bytes fails with, decoded under a budget of 4 KiB; empty when it succeeds.
std::optional<DecodeErrorCode> codeUnderBudget(Bytes const& bytes)
{
    auto const outcome = decodeUnderBudget(bytes, 4096, ComponentLimits {});
    REQUIRE(outcome.budget == Budget::Kept);
    return outcome.error.transform([](DecodeError const& error) { return error.code; });
}

/// @p count copies of @p item.
Bytes repeated(Bytes const& item, uint32_t count)
{
    auto bytes = Bytes {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        appendBytes(bytes, item);
    return bytes;
}

/// A vector of @p count copies of @p item: its count, then the items.
Bytes vectorOf(Bytes const& item, uint32_t count)
{
    auto bytes = Bytes {};
    appendLeb128(bytes, count);
    appendBytes(bytes, repeated(item, count));
    return bytes;
}

} // namespace

TEST_CASE("ComponentMemory.lyingCountsStayInBudget")
{
    // A count of 2^32 - 1 imports with three bytes left.
    auto lying = Bytes {};
    appendBytes(lying, { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x01, 0x61 });
    CHECK(codeUnderBudget(componentOf({ section(10, lying) })) == DecodeErrorCode::UnexpectedEnd);

    // A count equal to the bytes left, of elements that fail at their first byte: a decoder that
    // reserved for the count would hold a thousand definitions.
    auto equal = Bytes {};
    appendLeb128(equal, 1'000);
    for ([[maybe_unused]] auto const index: std::views::iota(0, 1'000))
        equal.push_back(std::byte { 0xFF });
    CHECK(codeUnderBudget(componentOf({ section(10, equal) })) == DecodeErrorCode::InvalidNameKind);
    CHECK(codeUnderBudget(componentOf({ section(5, equal) })) == DecodeErrorCode::InvalidInstanceKind);
}

TEST_CASE("ComponentMemory.smallestItemsHoldAtMostFourTimesTheirInput")
{
    // Each shape is 100'000 of the smallest item of one kind, a few bytes each, which the pool would
    // hold in a definition or vector element many times that size. With maxDecodedItems at 1'000, a
    // decoder that charges every item holds its copy of the input and a thousand items, under 4 x N
    // bytes for an input of N; one that missed a kind would hold 100'000 of them, far over it.
    constexpr auto Items = uint32_t { 100'000 };
    auto limits = ComponentLimits {};
    limits.maxDecodedItems = 1'000;

    auto const custom = section(0, Bytes { std::byte { 0x00 } }); // a custom section named ""
    auto outerAlias = Bytes {};
    appendBytes(outerAlias, { 0x03, 0x02, 0x00, 0x00 }); // (alias outer 0 0 (type))
    auto coreExport = Bytes {};
    appendBytes(coreExport, { 0x00, 0x00, 0x00 }); // (export "" (func 0))
    auto coreArgument = Bytes {};
    appendBytes(coreArgument, { 0x00, 0x12, 0x00 }); // (with "" (instance 0))
    auto inlineExport = Bytes {};
    appendBytes(inlineExport, { 0x00, 0x00, 0x01, 0x00 }); // (export "" (func 0))
    auto instanceArgument = Bytes {};
    appendBytes(instanceArgument, { 0x00, 0x01, 0x00 }); // (with "" (func 0))
    auto import = Bytes {};
    appendBytes(import, { 0x00, 0x00, 0x01, 0x00 }); // (import "" (func (type 0)))
    auto const sibling = section(4, componentPreamble());

    auto coreExports = Bytes {};
    appendBytes(coreExports, { 0x01, 0x01 });
    appendBytes(coreExports, vectorOf(coreExport, Items));
    auto coreArguments = Bytes {};
    appendBytes(coreArguments, { 0x01, 0x00, 0x00 });
    appendBytes(coreArguments, vectorOf(coreArgument, Items));
    auto inlineExports = Bytes {};
    appendBytes(inlineExports, { 0x01, 0x01 });
    appendBytes(inlineExports, vectorOf(inlineExport, Items));
    auto instanceArguments = Bytes {};
    appendBytes(instanceArguments, { 0x01, 0x00, 0x00 });
    appendBytes(instanceArguments, vectorOf(instanceArgument, Items));

    // The type, core type and canonical sections' items: definitions, the elements of their vectors,
    // and the declarations of type scopes, which hold a scope of the pool each when they open one.
    auto typeOf =
        [](std::initializer_list<uint8_t> head, Bytes const& vector, std::initializer_list<uint8_t> tail) {
            auto payload = Bytes {};
            appendBytes(payload, head);
            appendBytes(payload, vector);
            appendBytes(payload, tail);
            return payload;
        };
    auto const bools = vectorOf(Bytes { std::byte { 0x7F } }, Items);
    auto const fields = vectorOf(Bytes { std::byte { 0x00 }, std::byte { 0x7F } }, Items); // "" bool
    auto const cases = vectorOf(Bytes { std::byte { 0x00 }, std::byte { 0x00 }, std::byte { 0x00 } }, Items);
    auto const labels = vectorOf(Bytes { std::byte { 0x00 } }, Items);
    auto const boolDeclarations = vectorOf(Bytes { std::byte { 0x01 }, std::byte { 0x7F } }, Items);
    auto const scopeDeclarations = vectorOf(
        Bytes { std::byte { 0x01 }, std::byte { 0x42 }, std::byte { 0x00 } }, Items); // (type (instance))
    auto coreExportDeclaration = Bytes {};
    appendBytes(coreExportDeclaration, { 0x03, 0x00, 0x00, 0x00 }); // (export "" (func (type 0)))
    auto coreFunctionType = Bytes {};
    appendBytes(coreFunctionType, { 0x60, 0x01, 0x7F, 0x00 });               // (func (param i32))
    auto const backpressure = vectorOf(Bytes { std::byte { 0x24 } }, Items); // (canon backpressure.inc)
    auto const utf8 = vectorOf(Bytes { std::byte { 0x00 } }, Items);         // string-encoding=utf8

    struct Shape
    {
        char const* name;
        Bytes bytes;
    };
    auto const shapes = std::vector<Shape> {
        { .name = "custom sections", .bytes = componentOf({ repeated(custom, Items) }) },
        { .name = "outer aliases", .bytes = componentOf({ section(6, vectorOf(outerAlias, Items)) }) },
        { .name = "core inline exports", .bytes = componentOf({ section(2, coreExports) }) },
        { .name = "core instantiation arguments", .bytes = componentOf({ section(2, coreArguments) }) },
        { .name = "inline exports", .bytes = componentOf({ section(5, inlineExports) }) },
        { .name = "instantiation arguments", .bytes = componentOf({ section(5, instanceArguments) }) },
        { .name = "imports", .bytes = componentOf({ section(10, vectorOf(import, Items)) }) },
        { .name = "sibling components", .bytes = componentOf({ repeated(sibling, Items) }) },
        { .name = "types", .bytes = componentOf({ section(7, bools) }) },
        { .name = "record fields", .bytes = componentOf({ section(7, typeOf({ 0x01, 0x72 }, fields, {})) }) },
        { .name = "variant cases", .bytes = componentOf({ section(7, typeOf({ 0x01, 0x71 }, cases, {})) }) },
        { .name = "tuple elements", .bytes = componentOf({ section(7, typeOf({ 0x01, 0x6F }, bools, {})) }) },
        { .name = "enum labels", .bytes = componentOf({ section(7, typeOf({ 0x01, 0x6D }, labels, {})) }) },
        { .name = "function parameters",
          .bytes = componentOf({ section(7, typeOf({ 0x01, 0x40 }, fields, { 0x01, 0x00 })) }) },
        { .name = "instance type declarations",
          .bytes = componentOf({ section(7, typeOf({ 0x01, 0x42 }, boolDeclarations, {})) }) },
        { .name = "instance type scopes",
          .bytes = componentOf({ section(7, typeOf({ 0x01, 0x42 }, scopeDeclarations, {})) }) },
        { .name = "core types", .bytes = componentOf({ section(3, vectorOf(coreFunctionType, Items)) }) },
        { .name = "module type declarations",
          .bytes = componentOf(
              { section(3, typeOf({ 0x01, 0x50 }, vectorOf(coreExportDeclaration, Items), {})) }) },
        { .name = "canonical definitions", .bytes = componentOf({ section(8, backpressure) }) },
        { .name = "canonical options",
          .bytes = componentOf({ section(8, typeOf({ 0x01, 0x01, 0x00, 0x00 }, utf8, {})) }) },
    };
    for (auto const& shape: shapes)
    {
        INFO(shape.name);
        auto const outcome = decodeUnderBudget(shape.bytes, 4 * shape.bytes.size(), limits);
        CHECK(outcome.budget == Budget::Kept);
        REQUIRE(outcome.error.has_value());
        CHECK(exceededLimit(*outcome.error) == ComponentLimit::DecodedItems);
    }
}

namespace
{

/// A component of @p lists list types, each the list of the one before, over u32.
Bytes listChain(uint32_t lists)
{
    auto builder = ComponentBuilder {};
    auto previous = builder.type(TypeDefinition { .type = PrimitiveValueType::U32 });
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, lists))
        previous =
            builder.type(TypeDefinition { .type = ListType { .element = TypeIndex { .index = previous } } });
    return builder.build();
}

/// What validating under a budget came to.
struct BudgetedValidation
{
    std::optional<ValidationError> error; ///< The validation error; empty when it validated.
    Budget budget = Budget::Kept;         ///< Whether the budget refused an allocation.
    ValidationWork work {};               ///< What the validation cost, when it validated.
};

/// Validates @p pool under a budget of @p budget bytes; nothing is asserted under it.
BudgetedValidation validateUnderBudget(ComponentPool pool, std::size_t budget)
{
    auto outcome = BudgetedValidation {};
    auto coreModules = CoreModuleValidator {};
    try
    {
        auto const limit = AllocationLimit { budget };
        auto result = validateComponent(
            std::move(pool), ComponentFeatureSet::defaults(), ComponentLimits {}, coreModules);
        if (!result)
            outcome.error = std::move(result.error());
        else
            outcome.work = (*result)->work;
    }
    catch (std::bad_alloc const&)
    {
        outcome.budget = Budget::Refused;
    }
    return outcome;
}

} // namespace

namespace
{

/// What decoding and validating a component may hold per byte of it, beyond FixedComponentBytes.
///
/// The pool and the arena turn each definition into structs and vectors several times its encoding,
/// and a vector holds up to twice what it uses, more while it grows (by 2 in libstdc++ and libc++, by
/// 1.5 in the MSVC STL, whose debug build adds a proxy per container). Measured under Clang/libstdc++
/// with ASan, a list chain holds about 46 bytes per byte while decoded and validated and shallow
/// records about 37; under MSVC's debug STL about 62 and 41. The bound leaves room above them for
/// any standard library, and stays far below a hold that grows faster than the input: kept copies of
/// what was read so far, one per definition, pass it within the first few thousand definitions.
constexpr auto BytesPerComponentByte = std::size_t { 512 };

/// What decoding and validating may hold whatever the component: the empty component's cost.
constexpr auto FixedComponentBytes = std::size_t { 64 } * 1024;

/// The budget of a component of @p bytes bytes.
[[nodiscard]] constexpr std::size_t componentBudget(std::size_t bytes) noexcept
{
    return FixedComponentBytes + (BytesPerComponentByte * bytes);
}

} // namespace

TEST_CASE("TypeArena.depthCapLinearMemory")
{
    // A list chain 100'000 deep, decoded and validated: refused at depth 101, holding no more than its
    // budget per byte of the component (BytesPerComponentByte), at n and at 2n.
    for (auto const lists: { uint32_t { 50'000 }, uint32_t { 100'000 } })
    {
        auto const bytes = listChain(lists);
        auto outcome = BudgetedValidation {};
        try
        {
            auto coreModules = CoreModuleValidator {};
            auto const limit = AllocationLimit { componentBudget(bytes.size()) };
            auto pool = decodeComponent(bytes, ComponentFeatureSet::defaults(), ComponentLimits {});
            if (pool)
            {
                auto const validated = validateComponent(
                    std::move(*pool), ComponentFeatureSet::defaults(), ComponentLimits {}, coreModules);
                if (!validated)
                    outcome.error = validated.error();
            }
        }
        catch (std::bad_alloc const&)
        {
            outcome.budget = Budget::Refused;
        }
        INFO(std::format("{} lists, {} bytes", lists, bytes.size()));
        CHECK(outcome.budget == Budget::Kept);
        REQUIRE(outcome.error.has_value());
        CHECK(isoline::component::validate::exceededLimit(*outcome.error) == ComponentLimit::TypeDepth);
        CHECK(outcome.error->position.definition == ComponentLimits {}.maxTypeDepth);
    }
}

TEST_CASE("TypeArena.doublingChainInBudget")
{
    // 1'000 levels of t_i = record { a_i: t_{i-1}, b_i: t_{i-1} }, distinct labels per level: refused
    // at effective size, the validation holding under 4 MiB beyond the pool it is handed.
    auto builder = ComponentBuilder {};
    auto previous = builder.type(TypeDefinition { .type = PrimitiveValueType::U32 });
    for (auto const level: std::views::iota(uint32_t { 1 }, uint32_t { 1'001 }))
    {
        auto const child = TypeIndex { .index = previous };
        previous = builder.type(TypeDefinition {
            .type = RecordType {
                .fields = { LabeledType { .label = std::format("a{}", level), .type = child },
                            LabeledType { .label = std::format("b{}", level), .type = child } } } });
    }
    auto const outcome = validateUnderBudget(builder.pool(), std::size_t { 4 } << 20U);
    CHECK(outcome.budget == Budget::Kept);
    REQUIRE(outcome.error.has_value());
    CHECK(isoline::component::validate::exceededLimit(*outcome.error) == ComponentLimit::TypeSize);
}

TEST_CASE("Validator.acceptedComponentLinearMemory")
{
    // An accepted component of n shallow records, each its own type (distinct labels): validation holds
    // no more than its budget per byte of the component (BytesPerComponentByte), at n and at 2n.
    for (auto const records: { uint32_t { 50'000 }, uint32_t { 100'000 } })
    {
        auto builder = ComponentBuilder {};
        for (auto const index: std::views::iota(uint32_t { 0 }, records))
            std::ignore = builder.type(TypeDefinition {
                .type = RecordType { .fields = { LabeledType { .label = std::format("f{}", index),
                                                               .type = PrimitiveValueType::U32 } } } });
        auto const budget = componentBudget(builder.build().size());
        auto const outcome = validateUnderBudget(builder.pool(), budget);
        INFO(std::format("{} records, budget {} bytes", records, budget));
        CHECK(outcome.budget == Budget::Kept);
        CHECK_FALSE(outcome.error.has_value());
    }
}

TEST_CASE("Validator.freshInstancesShareTheirNames")
{
    // An instance type of 100 exports with 10 KB names, each a function returning an own of the
    // resource it defines, imported 1'000 times: each import mints the resource afresh and rewrites the
    // types naming it, but shares the names. Copying them would take 1'000 x 100 x 10 KB = 1 GB from
    // 1 MB of input; the validation holds under 32 MiB beyond the pool it is handed.
    auto declarations = TypeScopeBuilder {};
    auto const resource = declarations.exportItem(
        "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const own = declarations.type(TypeDefinition { .type = OwnType { .resource = resource } });
    auto const make = declarations.type(TypeDefinition {
        .type = FunctionTypeDefinition { .params = {}, .result = TypeIndex { .index = own } } });
    for (auto const index: std::views::iota(0, 100))
        std::ignore = declarations.exportItem(std::format("{}-{}", std::string(10'000, 'a'), index),
                                              ExternDesc { .sort = Sort::Func, .index = make });
    auto builder = ComponentBuilder {};
    auto const instance = builder.instanceType(std::move(declarations));
    for (auto const index: std::views::iota(0, 1'000))
        std::ignore = builder.import(std::format("i{}", index),
                                     ExternDesc { .sort = Sort::Instance, .index = instance });
    auto const outcome = validateUnderBudget(builder.pool(), std::size_t { 32 } << 20U);
    CHECK(outcome.budget == Budget::Kept);
    CHECK_FALSE(outcome.error.has_value());
}

TEST_CASE("Validator.manyAliasesLinear")
{
    // 100'000 outer aliases of one type into a nested component, each walked for resources and charged
    // to the component's budget of maxTypeSize: validation holds no more than its budget per byte of
    // the component, at n and at 2n. The memory bound is the budget, not a ratio of the two peaks: the
    // index space is one vector, whose capacity at 2n is up to 3 x its capacity at n with the MSVC
    // STL's growth of 1.5 (measured 2.24 x there), so a 2.2 x ratio would assert the STL, not the
    // validator. Time is the work counted, which no library changes: twice the aliases, twice the work.
    auto const validateWith = [](uint32_t aliases) {
        auto builder = ComponentBuilder {};
        auto const tuple = builder.type(
            TypeDefinition { .type = isoline::component::binary::TupleType {
                                 .elements = { PrimitiveValueType::U32, PrimitiveValueType::U32 } } });
        auto nested = ComponentBuilder {};
        for ([[maybe_unused]] auto const alias: std::views::iota(uint32_t { 0 }, aliases))
            std::ignore = nested.aliasOuter(Sort::Type, 1, tuple);
        std::ignore = builder.nested(std::move(nested));
        auto const budget = componentBudget(builder.build().size());
        auto const outcome = validateUnderBudget(builder.pool(), budget);
        INFO(std::format("{} aliases, budget {} bytes", aliases, budget));
        CHECK(outcome.budget == Budget::Kept);
        CHECK_FALSE(outcome.error.has_value());
        return outcome.work.resourceWalkSteps + outcome.work.subtypingSteps;
    };
    auto const single = validateWith(50'000);
    auto const twice = validateWith(100'000);
    INFO(std::format("work {} at n, {} at 2n", single, twice));
    CHECK(single >= 50'000);
    CHECK(twice * 10 <= single * 21);
}

TEST_CASE("Validator.manyExportAliasesLinear")
{
    // An instance of 50'000 exports, each aliased once: the instance's exports are indexed by name
    // once, so each alias costs O(log n) and the validation holds within its budget at n and 2n (a
    // budget, not a ratio, for the reason Validator.manyAliasesLinear gives).
    auto const validateWith = [](uint32_t exports) {
        auto declarations = TypeScopeBuilder {};
        auto const function = declarations.type(
            TypeDefinition { .type = FunctionTypeDefinition { .params = {}, .result = std::nullopt } });
        for (auto const index: std::views::iota(uint32_t { 0 }, exports))
            std::ignore = declarations.exportItem(std::format("f{}", index),
                                                  ExternDesc { .sort = Sort::Func, .index = function });
        auto builder = ComponentBuilder {};
        auto const instance = builder.instanceType(std::move(declarations));
        std::ignore = builder.import("i", ExternDesc { .sort = Sort::Instance, .index = instance });
        for (auto const index: std::views::iota(uint32_t { 0 }, exports))
            std::ignore = builder.aliasExport(0, std::format("f{}", index), Sort::Func);
        auto const budget = componentBudget(builder.build().size());
        auto const outcome = validateUnderBudget(builder.pool(), budget);
        INFO(std::format("{} exports, budget {} bytes", exports, budget));
        CHECK(outcome.budget == Budget::Kept);
        CHECK_FALSE(outcome.error.has_value());
        return outcome.work.exportLookupSteps;
    };
    // An index per instance: n entries indexed once and n lookups, at n as at 2n.
    auto const single = validateWith(25'000);
    auto const twice = validateWith(50'000);
    INFO(std::format("export lookup work {} at n, {} at 2n", single, twice));
    CHECK(single == 50'000);
    CHECK(twice * 10 <= single * 21);
}

TEST_CASE("Validator.sharedClosedTypeWalkedOnce")
{
    // A closed component type (it binds the resource its functions name) of 2'000 functions, aliased
    // into 1'000 nested components (maxComponents): each alias must check it names no outer resource,
    // which wasm-tools walks every time (1'000 x 2'000 types). Found closed once, it is skipped after:
    // the validation stays within its budget, and its walk grows by a step per component: at n and 2n
    // components, the walk differs by n steps' worth, not by n x 2'000.
    auto const validateWith = [](uint32_t components) {
        auto declarations = TypeScopeBuilder {};
        auto const resource = declarations.import(
            "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
        auto const own = declarations.type(TypeDefinition { .type = OwnType { .resource = resource } });
        for (auto const index: std::views::iota(0, 2'000))
        {
            auto const function = declarations.type(TypeDefinition {
                .type =
                    FunctionTypeDefinition { .params = { LabeledType { .label = std::format("p{}", index),
                                                                       .type = TypeIndex { .index = own } } },
                                             .result = std::nullopt } });
            std::ignore = declarations.import(std::format("f{}", index),
                                              ExternDesc { .sort = Sort::Func, .index = function });
        }
        auto builder = ComponentBuilder {};
        auto const closed = builder.componentType(std::move(declarations));
        for ([[maybe_unused]] auto const component: std::views::iota(uint32_t { 0 }, components))
        {
            auto nested = ComponentBuilder {};
            std::ignore = nested.aliasOuter(Sort::Type, 1, closed);
            std::ignore = builder.nested(std::move(nested));
        }
        auto const outcome = validateUnderBudget(builder.pool(), componentBudget(builder.build().size()));
        CHECK(outcome.budget == Budget::Kept);
        if (outcome.error)
            FAIL(isoline::component::validate::specMessage(*outcome.error));
        return outcome.work.resourceWalkSteps;
    };
    auto const single = validateWith(500);
    auto const twice = validateWith(1'000);
    INFO(std::format("resource walk {} at n, {} at 2n", single, twice));
    // The type walked once (its 2'000 functions, a parameter each, and their resource) at either size.
    CHECK(single >= 2'000);
    CHECK(twice - single <= uint64_t { 500 } * 4);
}

// Component values (P11 Task 9): every byte a Values holds is charged to its store's LiveBytesBudget
// before the host allocates it, so a guest's deep or wide values are refused by the budget, by name,
// and never reach the host's allocator beyond it; a count that lies is refused before anything grows;
// and whatever path a build leaves by, its charge is released.

namespace
{

using isoline::component::canonical::LiveBytesBudget;
using isoline::component::canonical::ValueBuilder;
using isoline::component::canonical::ValueErrorCode;
using isoline::component::types::DefinedType;
using isoline::component::types::TypeArena;
using isoline::component::types::TypeId;
using isoline::component::types::ValueKind;

/// What a builder holds besides its charged storage: its frame stack (as deep as the type), its root
/// types, an error's path and detail, and a debug standard library's container proxies.
constexpr auto FixedBuilderBytes = std::size_t { 64 } * 1024;

/// The value types the cases build.
struct ValueTypes
{
    TypeArena arena { ComponentLimits {} };
    TypeId string = arena.primitive(ValueKind::String);
    TypeId bytes =
        intern(DefinedType { .kind = ValueKind::List, .children = { arena.primitive(ValueKind::U8) } });
    TypeId strings = intern(DefinedType { .kind = ValueKind::List, .children = { string } });
    TypeId nested = intern(DefinedType { .kind = ValueKind::List, .children = { strings } });
    TypeId map = intern(DefinedType { .kind = ValueKind::Map, .children = { string, string } });
    TypeId records = intern(DefinedType {
        .kind = ValueKind::List,
        .children = { intern(DefinedType { .kind = ValueKind::Record,
                                           .children = { string, arena.primitive(ValueKind::U32) },
                                           .labels = { "s", "n" } }) } });

    [[nodiscard]] TypeId intern(DefinedType type)
    {
        auto const id = arena.intern(std::move(type));
        REQUIRE(id.has_value());
        return *id;
    }
};

/// What a build under a host allocation limit came to.
struct BudgetedBuild
{
    std::optional<ValueErrorCode> error; ///< The builder's error; empty when it built.
    Budget budget = Budget::Kept;        ///< Whether the host limit refused an allocation.
};

} // namespace

TEST_CASE("Values.lyingCountsRefusedBeforeAllocation")
{
    // A count of 2^32 - 1 for a list of bytes (4 GiB of pool), of strings (2^32 nodes), of lists of
    // strings and for a map: each is refused by the store's budget at begin(), while the host lets
    // the builder allocate no more than its fixed bytes.
    auto const types = ValueTypes {};
    for (auto const type: { types.bytes, types.strings, types.nested, types.map })
    {
        auto const budget = std::make_shared<LiveBytesBudget>(ComponentLimits {}.maxLiveValueBytes);
        auto outcome = BudgetedBuild {};
        try
        {
            auto const limit = AllocationLimit { FixedBuilderBytes };
            auto builder = ValueBuilder { types.arena, std::span { &type, 1 }, budget };
            builder.begin(std::numeric_limits<uint32_t>::max());
            if (builder.error())
                outcome.error = builder.error()->code;
        }
        catch (std::bad_alloc const&)
        {
            outcome.budget = Budget::Refused;
        }
        INFO(std::format("type {}", type.value));
        CHECK(outcome.budget == Budget::Kept);
        CHECK(outcome.error == ValueErrorCode::Budget);
        CHECK(budget->live() == 0);
    }
}

TEST_CASE("Values.hostMemoryFollowsTheBudget")
{
    // A list of 10'000 lists of 100 strings of 1 KiB (a GiB in all) against a budget of 4 MiB: the
    // budget refuses a growth by name, the host never holds more than the budget and the builder's
    // fixed bytes, and the refused build's death leaves the budget empty.
    constexpr auto Limit = uint64_t { 4 } << 20U;
    auto const types = ValueTypes {};
    auto const text = std::string(1024, 'v');
    auto const budget = std::make_shared<LiveBytesBudget>(Limit);
    auto outcome = BudgetedBuild {};
    auto peak = std::size_t { 0 };
    {
        auto const scope = AllocationScope {};
        try
        {
            auto const limit = AllocationLimit { Limit + FixedBuilderBytes };
            auto builder = ValueBuilder { types.arena, std::span { &types.nested, 1 }, budget };
            builder.begin(10'000);
            for ([[maybe_unused]] auto const list: std::views::iota(0, 10'000))
            {
                builder.begin(100);
                for ([[maybe_unused]] auto const index: std::views::iota(0, 100))
                    builder.string(text);
                builder.end();
                if (builder.error())
                    break;
            }
            if (builder.error())
                outcome.error = builder.error()->code;
        }
        catch (std::bad_alloc const&)
        {
            outcome.budget = Budget::Refused;
        }
        peak = scope.peakBytes();
    }
    INFO(std::format("host peak {} bytes for a budget of {}", peak, Limit));
    CHECK(outcome.budget == Budget::Kept);
    CHECK(outcome.error == ValueErrorCode::Budget);
    CHECK(peak <= Limit + FixedBuilderBytes);
    CHECK(budget->live() == 0);
}

TEST_CASE("Values.unwindReleasesTheCharge", "[host-memory]")
{
    // The host refuses an allocation the budget allowed (its limit is far below the budget's): the
    // std::bad_alloc leaves the builder's step, the builder is destroyed while it unwinds, and the
    // budget is left as it was before the build.
    auto const types = ValueTypes {};
    auto const text = std::string(100, 'u');
    auto const budget = std::make_shared<LiveBytesBudget>(uint64_t { 64 } << 20U);
    auto outcome = BudgetedBuild {};
    try
    {
        auto const limit = AllocationLimit { std::size_t { 1 } << 20U };
        auto builder = ValueBuilder { types.arena, std::span { &types.strings, 1 }, budget };
        builder.begin(100'000);
        for ([[maybe_unused]] auto const index: std::views::iota(0, 100'000))
            builder.string(text);
        if (builder.error())
            outcome.error = builder.error()->code;
    }
    catch (std::bad_alloc const&)
    {
        outcome.budget = Budget::Refused;
    }
    CHECK(outcome.budget == Budget::Refused);
    CHECK_FALSE(outcome.error.has_value());
    CHECK(budget->live() == 0);
}

TEST_CASE("Values.buildLinearMemory")
{
    // A list of n records of a 16-byte string and a u32, built and held: the host's peak at 2n is
    // at most 2.2 times its peak at n (each storage grows by doubling, so its peak doubles too).
    auto const types = ValueTypes {};
    auto const text = std::string(16, 'r');
    auto const peakFor = [&](uint32_t records) {
        auto const budget = std::make_shared<LiveBytesBudget>(ComponentLimits {}.maxLiveValueBytes);
        auto const scope = AllocationScope {};
        auto builder = ValueBuilder { types.arena, std::span { &types.records, 1 }, budget };
        builder.begin(records);
        for (auto const index: std::views::iota(uint32_t { 0 }, records))
            builder.begin(2).string(text).scalar(index).end();
        builder.end();
        auto const values = builder.finish();
        REQUIRE(values.has_value());
        CHECK(values->hostBytes() == budget->live());
        return scope.peakBytes();
    };
    auto const single = peakFor(50'000);
    auto const twice = peakFor(100'000);
    INFO(std::format("peak {} bytes at n, {} at 2n", single, twice));
    CHECK(twice * 10 <= single * 22);
}

// The plans' share: one plan per component definition, each definition a step or none, so planning holds
// memory linear in the component however often it instantiates its children.

using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreSort;
using isoline::component::binary::testing::doublingComponent;
using isoline::component::runtime::buildPlans;

namespace
{

/// The most buildPlans() holds at once for @p builder's component, validated beforehand.
std::size_t planningPeak(ComponentBuilder const& builder)
{
    auto coreModules = CoreModuleValidator {};
    auto const validated =
        validateComponent(builder.pool(), ComponentFeatureSet::wasi03(), ComponentLimits {}, coreModules);
    REQUIRE(validated.has_value());
    auto const scope = AllocationScope {};
    auto const plans = buildPlans(**validated);
    CHECK(plans.plans.size() == (*validated)->pool.components.size());
    return scope.peakBytes();
}

} // namespace

TEST_CASE("ComponentPlan.linearInInputMemory")
{
    // n levels each instantiating the previous twice, which inlining would make 2^(n - 1) instances of:
    // planning 2n levels holds at most 2.2 times what n levels hold.
    auto const single = planningPeak(doublingComponent(45));
    auto const twice = planningPeak(doublingComponent(90));
    INFO(std::format("peak {} bytes at n, {} at 2n", single, twice));
    CHECK(twice * 10 <= single * 22);
}

TEST_CASE("ComponentPlan.longNamesCopiedOnce")
{
    // A core instance of n inline exports with 1'000-byte names, each aliased once: a plan copies each
    // name once per definition naming it, so 2n exports hold at most 2.2 times what n hold.
    auto const peakFor = [](uint32_t exports) {
        auto builder = ComponentBuilder {};
        auto const type = builder.type(TypeDefinition { .type = FunctionTypeDefinition {} });
        auto const function = builder.import("f", ExternDesc { .sort = Sort::Func, .index = type });
        auto const lowered = builder.canonLower(function, {});
        auto entries = std::vector<CoreInlineExport> {};
        for (auto const index: std::views::iota(uint32_t { 0 }, exports))
            entries.push_back(CoreInlineExport { .name = std::format("{}{}", std::string(1'000, 'n'), index),
                                                 .sort = CoreSort::Func,
                                                 .index = lowered });
        auto const instance = builder.coreInlineExports(entries);
        for (auto const& entry: entries)
            std::ignore = builder.aliasCoreExport(instance, entry.name, CoreSort::Func);
        return planningPeak(builder);
    };
    auto const single = peakFor(2'000);
    auto const twice = peakFor(4'000);
    INFO(std::format("peak {} bytes at n, {} at 2n", single, twice));
    CHECK(single >= std::size_t { 2'000 } * 2 * 1'000);
    CHECK(twice * 10 <= single * 22);
}
