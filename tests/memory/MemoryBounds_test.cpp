// SPDX-License-Identifier: Apache-2.0

// A guest must never make the host run out of memory (spec §7). What a module costs before it runs is
// bounded by its size: decoding it holds at most BytesPerModuleByte bytes per byte of the module, and
// so does each instantiation of it in the store, besides the memories and tables the allocator
// approves. Each test below is a module shaped to amplify one entity the decoder or the store copies,
// at a size where a cost per item that grows with another item's size (a name, a type) is orders of
// magnitude over the bound, and a constant cost per item is well under it.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include "AllocationCounter.hpp"
#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/validate/Validator.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::Nullable;
using isoline::binary::ValueType;
using isoline::binary::testing::encodeName;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::ModuleBuilder;
using isoline::testing::AllocationScope;

namespace
{

/// The bytes decoding, or one instantiation, may hold per byte of the module.
///
/// Decoding turns each entity into fixed-size structs, and a vector holds up to twice what it uses,
/// three times while it grows. Measured under Clang/libstdc++ with ASan, the densest module below is
/// manyFunctions: about 113 bytes decoded per module byte (a function of four input bytes becomes a
/// Function and its code, about 450 bytes). Then a compact import group (77: one byte per Import),
/// deep type chains (21), catch clauses (10), v128 literals (7) and wide tags in the store (5). The
/// bound leaves room above them for standard libraries whose containers are larger (MSVC's debug
/// proxies), and stays far below a per-item copy of another item's bytes: a 16 KiB module name per
/// compact import item costs 5500 bytes per module byte, a 1000-param type per tag 5300.
constexpr auto BytesPerModuleByte = std::size_t { 512 };

/// What decoding, or instantiating, may hold whatever the module: the empty module's cost.
constexpr auto FixedBytes = std::size_t { 64 } * 1024;

/// Whether a measured module is instantiated too, or only decoded and validated.
enum class Instantiation : uint8_t
{
    Skip, ///< Decode and validate only: the module imports what the test does not supply.
    Run,  ///< Instantiate it in a fresh store as well, and bound what the store gained.
};

/// Decodes, validates and (when asked) instantiates @p bytes, and checks that neither decoding nor
/// instantiating held more than the bound for a module of their size.
void checkLinear(std::vector<std::byte> const& bytes, FeatureSet features, Instantiation instantiation)
{
    auto const budget = (BytesPerModuleByte * bytes.size()) + FixedBytes;

    auto const decodeScope = AllocationScope {};
    auto decoded = isoline::binary::decodeModule(bytes, features);
    auto const decodeBytes = decodeScope.peakBytes();
    REQUIRE(decoded.has_value());
    CAPTURE(bytes.size(), budget, decodeBytes);
    CHECK(decodeBytes <= budget);
    REQUIRE(isoline::validate::validateModule(*decoded, features).has_value());
    if (instantiation == Instantiation::Skip)
        return;

    auto const module = std::make_shared<isoline::binary::Module const>(std::move(*decoded));
    auto const allocator = isoline::runtime::UnlimitedAllocator {};
    auto store = isoline::runtime::Store { allocator };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    auto const storeScope = AllocationScope {};
    auto const instance = isoline::runtime::instantiate(store, interpreter, module, {});
    auto const storeBytes = storeScope.peakBytes();
    REQUIRE(instance.has_value());
    CAPTURE(storeBytes);
    CHECK(storeBytes <= budget);
}

/// Appends @p bytes to @p out.
void append(std::vector<uint8_t>& out, std::vector<uint8_t> const& bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// A module whose import section is one compact group (0x7E) of @p items global imports, all from
/// one module named by @p nameBytes bytes, each item named by the empty name: one input byte each.
std::vector<std::byte> compactImportGroup(std::size_t nameBytes, uint32_t items)
{
    auto payload = encodeU32(1);
    append(payload, encodeName(std::string(nameBytes, 'm')));
    append(payload, encodeName(""));
    append(payload, { 0x7E, 0x03, 0x7F, 0x00 }); // shared type: global i32 (immutable)
    append(payload, encodeU32(items));
    payload.resize(payload.size() + items, 0x00); // every item's name: empty
    auto builder = ModuleBuilder {};
    builder.rawSectionBeforeCode(2, std::move(payload));
    return builder.build();
}

/// A module of @p tags tags, all of one type of @p params i32 params: two input bytes per tag.
std::vector<std::byte> wideTags(std::size_t params, uint32_t tags)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = std::vector<ValueType>(params, I32), .results = {} });
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, tags))
        builder.tag(0);
    return builder.build();
}

/// A module of @p count functions of type [] -> [] with empty bodies.
std::vector<std::byte> manyFunctions(uint32_t count)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {});
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        builder.function(0, {}, {});
    return builder.build();
}

/// A module of one function that pushes and drops @p count v128 constants.
std::vector<std::byte> manyV128Literals(uint32_t count)
{
    auto body = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        append(body, { 0xFD, 0x0C }); // v128.const
        body.resize(body.size() + 16, 0x00);
        body.push_back(0x1A); // drop
    }
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, std::move(body));
    return builder.build();
}

/// A module of one function whose try_table has @p count catch clauses, each of tag 0 to label 0.
std::vector<std::byte> manyCatchClauses(uint32_t count)
{
    auto body = std::vector<uint8_t> { 0x1F, 0x40 }; // try_table, empty block type
    append(body, encodeU32(count));
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        append(body, { 0x00, 0x00, 0x00 }); // catch tag 0, label 0
    body.push_back(0x0B);                   // end
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).tag(0).function(0, {}, std::move(body));
    return builder.build();
}

/// A module of @p depth types, each taking a nullable reference to the one before it.
std::vector<std::byte> deepTypeChain(uint32_t depth)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {});
    for (auto const index: std::views::iota(uint32_t { 1 }, depth))
        builder.type(
            FuncType { .params = { isoline::binary::concreteRef(index - 1, Nullable::Yes) }, .results = {} });
    return builder.build();
}

} // namespace

TEST_CASE("MemoryBounds.compactImportGroupSharesItsModuleName")
{
    // A 16 KiB module name for 8192 one-byte items: copied per item, it would be 128 MiB.
    checkLinear(compactImportGroup(std::size_t { 16 } * 1024, 8192),
                FeatureSet::wasm3().with(Feature::CompactImports),
                Instantiation::Skip);
}

TEST_CASE("MemoryBounds.tagsShareTheirType")
{
    // 4000 two-byte tags of one 1000-param type: copied per tag, the type would be about 48 MB.
    checkLinear(wideTags(1000, 4000), FeatureSet::wasm3(), Instantiation::Run);
}

TEST_CASE("MemoryBounds.manyFunctions")
{
    checkLinear(manyFunctions(20'000), FeatureSet::wasm3(), Instantiation::Run);
}

TEST_CASE("MemoryBounds.manyV128Literals")
{
    checkLinear(manyV128Literals(4000), FeatureSet::wasm3(), Instantiation::Run);
}

TEST_CASE("MemoryBounds.manyCatchClauses")
{
    checkLinear(manyCatchClauses(10'000), FeatureSet::wasm3(), Instantiation::Run);
}

TEST_CASE("MemoryBounds.deepTypeChain")
{
    checkLinear(deepTypeChain(5000), FeatureSet::wasm3(), Instantiation::Run);
}
