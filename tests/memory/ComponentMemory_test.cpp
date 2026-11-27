// SPDX-License-Identifier: Apache-2.0

// A component must never make the host run out of memory (spec §7). The component decoder's share of
// that: a vector count reserves nothing, so a count that lies about what follows fails having
// allocated nothing beyond the decoder's copy of its input; and every item it decodes is charged
// against maxDecodedItems before it is read, so input made of the smallest items, which the pool holds
// at many times their size, holds a bounded number of them: definitions, the elements of their vectors,
// type declarations and the type scopes they open alike.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <new>
#include <optional>
#include <ranges>
#include <vector>

#include "AllocationCounter.hpp"
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>

using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::binary::decodeComponent;
using isoline::component::binary::DecodeError;
using isoline::component::binary::DecodeErrorCode;
using isoline::component::binary::exceededLimit;
using isoline::component::binary::testing::appendBytes;
using isoline::component::binary::testing::appendLeb128;
using isoline::component::binary::testing::Bytes;
using isoline::component::binary::testing::componentOf;
using isoline::component::binary::testing::componentPreamble;
using isoline::component::binary::testing::section;
using isoline::testing::AllocationLimit;

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
