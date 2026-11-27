// SPDX-License-Identifier: Apache-2.0

// A guest must never make the host run out of memory (spec §7). What a module costs before it runs is
// bounded by its size: loading it (decoding and validating) holds at most BytesPerModuleByte bytes per byte
// of the module, and so does each instantiation of it in the store, besides the memories and tables the
// allocator approves. Each test below is a module shaped to amplify one entity the decoder or the store
// copies, at a size where a cost per item that grows with another item's size (a name, a type) is orders of
// magnitude over the bound, and a constant cost per item is well under it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "AllocationCounter.hpp"
#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/testing/RuntimeAccess.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/validate/ValidatedModule.hpp>
#include <isoline/validate/ValidationError.hpp>
#include <isoline/validate/testing/BranchBodies.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::Nullable;
using isoline::binary::ValueType;
using isoline::binary::testing::encodeName;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::ModuleBuilder;
using isoline::testing::AllocationLimit;
using isoline::testing::AllocationScope;
using isoline::testing::RefusalException;
using isoline::validate::testing::brIfsToWideLabel;
using isoline::validate::testing::wideBrTables;

namespace
{

/// The bytes loading, or one instantiation, may hold per byte of the module.
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

/// What loading, or instantiating, may hold whatever the module: the empty module's cost.
constexpr auto FixedBytes = std::size_t { 64 } * 1024;

/// Whether a measured module is instantiated too, or only decoded and validated.
enum class Instantiation : uint8_t
{
    Skip, ///< Decode and validate only: the module imports what the test does not supply.
    Run,  ///< Instantiate it in a fresh store as well, and bound what the store gained.
};

/// What loading or instantiating @p bytes may hold.
std::size_t budgetOf(std::vector<std::byte> const& bytes)
{
    return (BytesPerModuleByte * bytes.size()) + FixedBytes;
}

/// Decodes and validates @p bytes, and checks that loading held no more than the bound for a module of
/// their size, whether it succeeded or not.
/// @return What loading returned.
auto loadWithinBound(std::vector<std::byte> const& bytes, FeatureSet features)
{
    auto const budget = budgetOf(bytes);
    auto const loadScope = AllocationScope {};
    auto loaded = isoline::validate::ValidatedModule::load(bytes, features, isoline::binary::DecodeLimits {});
    auto const loadBytes = loadScope.peakBytes();
    CAPTURE(bytes.size(), budget, loadBytes);
    CHECK(loadBytes <= budget);
    return loaded;
}

/// Decodes, validates and (when asked) instantiates @p bytes, and checks that neither loading nor
/// instantiating held more than the bound for a module of their size.
void checkLinear(std::vector<std::byte> const& bytes, FeatureSet features, Instantiation instantiation)
{
    auto const budget = budgetOf(bytes);
    auto loaded = loadWithinBound(bytes, features);
    REQUIRE(loaded.has_value());
    if (instantiation == Instantiation::Skip)
        return;

    auto const module = std::move(*loaded);
    auto allocator = isoline::runtime::HeapMemoryAllocator {};
    auto store = isoline::runtime::Store { isoline::runtime::StoreLimits {},
                                           isoline::runtime::StoreServices { .allocator = allocator } };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    auto const storeScope = AllocationScope {};
    auto const instance = isoline::runtime::instantiate(store, interpreter, module, {});
    auto const storeBytes = storeScope.peakBytes();
    REQUIRE(instance.has_value());
    CAPTURE(budget, storeBytes);
    CHECK(storeBytes <= budget);
}

/// Loads @p bytes through an api::Engine of @p features, which describes the module's imports and
/// exports as well, and checks that loading and describing held no more than the bound for a module of
/// their size.
void checkEngineLoadLinear(std::vector<std::byte> const& bytes, FeatureSet features)
{
    auto const budget = budgetOf(bytes);
    auto const engine = isoline::api::Engine { isoline::api::EngineConfig { .features = features } };
    auto const scope = AllocationScope {};
    auto const module = engine.load(bytes);
    auto const loadBytes = scope.peakBytes();
    REQUIRE(module.has_value());
    CAPTURE(bytes.size(), budget, loadBytes);
    CHECK(loadBytes <= budget);
}

/// A module of @p count function imports, all of one type of @p params i32 params, each from the
/// module "" named "": four input bytes per import.
std::vector<std::byte> manyFunctionImports(std::size_t params, uint32_t count)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = std::vector<ValueType>(params, I32), .results = {} });
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        builder.importFunction("", "", 0);
    return builder.build();
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

/// A module of a function $f of type [] -> [i32 x 1000], and a function [] -> [] whose body is
/// @p calls calls of $f: two input bytes per call, each pushing a thousand operands.
std::vector<std::byte> multiValueCalls(uint32_t calls)
{
    auto body = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, calls))
        append(body, { 0x10, 0x00 }); // call $f
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = std::vector<ValueType>(1000, I32) })
        .type(FuncType {})
        .function(0, {}, { 0x00 })
        .function(1, {}, std::move(body));
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

TEST_CASE("MemoryBounds.multiValueCallsAreRefusedEarly")
{
    // 4000 calls of a function of a thousand results: validated to the end, the operand stack would
    // hold four million operands, about 64 MB. The validator refuses the body as soon as its stack
    // outgrows maxOperandsPerInstruction per instruction.
    auto const loaded = loadWithinBound(multiValueCalls(4000), FeatureSet::wasm3());
    REQUIRE_FALSE(loaded.has_value());
    auto const* const error = std::get_if<isoline::validate::ValidationError>(&loaded.error());
    REQUIRE(error != nullptr);
    CHECK(error->code == isoline::validate::ValidationErrorCode::OperandStackLimitExceeded);
}

// The [branch-cost] cases bound validation time, not memory: each is a body of branches whose labels carry
// many operands, which a validator that pops and pushes a label's operands per branch, or checks each
// br_table label on its own, types in some 10^9 type checks (about 18 s on clang-release, far longer under
// a sanitizer or an emulator). Checked in place they take a fraction of a second. CMake runs them as a
// test of their own, under a timeout; Validator_test's branchCostIsLinear counts the type checks of the
// same shapes, the deterministic guard.

TEST_CASE("MemoryBounds.brIfsToALabelOfOneOperand", "[branch-cost]")
{
    checkLinear(brIfsToWideLabel(1, 1'000'000), FeatureSet::wasm3(), Instantiation::Skip);
}

TEST_CASE("MemoryBounds.brIfsToAWideLabel", "[branch-cost]")
{
    // A million br_ifs to a label of a thousand operands: 10^9 type checks, popped and pushed per branch.
    checkLinear(brIfsToWideLabel(1000, 1'000'000), FeatureSet::wasm3(), Instantiation::Skip);
}

TEST_CASE("MemoryBounds.wideBrTables", "[branch-cost]")
{
    // 50 br_tables of 65'520 labels, cycling through a thousand blocks of a thousand operands each: 3.3 *
    // 10^9 type checks, label by label.
    checkLinear(wideBrTables(1000, 1000, 65'520, 50), FeatureSet::wasm3(), Instantiation::Skip);
}

TEST_CASE("MemoryBounds.loadDescribesWithoutCopying")
{
    // An engine's module describes every import and export by pointing into the decoded module. A 64 KiB
    // module name for 8192 one-byte items, copied per description, would be 512 MiB.
    checkEngineLoadLinear(compactImportGroup(std::size_t { 64 } * 1024, 8192),
                          FeatureSet::wasm3().with(Feature::CompactImports));
    // 100'000 imports of one 1000-param type: a copy of the type per description would be about 1.6 GB.
    checkEngineLoadLinear(manyFunctionImports(1000, 100'000), FeatureSet::wasm3());
}

TEST_CASE("MemoryBounds.loadReportsHostMemoryExhaustion", "[host-memory]")
{
    // Every budget from nothing up to what loading needs: each load either succeeds or reports the host
    // out of memory, and none throws through Engine::load or ends the process in a noexcept frame.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = {} });
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 200 }))
        builder.importFunction("host", "f", 0);
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 200 }))
        builder.function(0, {}, { 0x20, 0x00, 0x1A }); // local.get 0; drop
    builder.exportFunction("first", 200);
    auto const bytes = builder.build();
    auto const engine = isoline::api::Engine { isoline::api::EngineConfig {} };

    auto refusals = 0;
    auto loadedWithin = std::optional<std::size_t> {};
    // Budgets 1/64 apart (32 bytes at least), so every allocation of the load is refused at some step.
    for (auto budget = std::size_t { 0 }; budget <= std::size_t { 64 } << 20;
         budget += std::max(std::size_t { 32 }, budget / 64))
    {
        auto const result = [&] {
            auto const limit = AllocationLimit { budget };
            return engine.load(bytes);
        }();
        if (result.has_value())
        {
            loadedWithin = budget;
            break;
        }
        CAPTURE(budget);
        CHECK(std::holds_alternative<isoline::api::HostMemoryExhausted>(result.error()));
        ++refusals;
    }
    CAPTURE(refusals);
    CHECK(refusals > 0);
    CHECK(loadedWithin.has_value());
}

TEST_CASE("MemoryBounds.loadReportsAContainerPastItsSizeAsHostMemoryExhaustion", "[host-memory]")
{
    // A container asked for more than its max_size() throws std::length_error, not std::bad_alloc: it
    // is a memory refusal all the same, reported as one, never a throw out of a noexcept Engine::load.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = {} }).function(0, {}, { 0x20, 0x00, 0x1A });
    auto const bytes = builder.build();
    auto const engine = isoline::api::Engine { isoline::api::EngineConfig {} };
    auto const result = [&] {
        auto const limit = AllocationLimit { 0, RefusalException::LengthError };
        return engine.load(bytes);
    }();
    REQUIRE_FALSE(result.has_value());
    CHECK(std::holds_alternative<isoline::api::HostMemoryExhausted>(result.error()));
}

TEST_CASE("MemoryBounds.instantiateRefusesHostMemoryBeforeTheStoreChanges", "[host-memory]")
{
    // Two host imports, the second of 1'000 params. Every budget below that type's size runs out while
    // the externs, the definitions' lookup and the host functions' entries (which copy their types) are
    // made: before the store changes. Each is refused as HostMemoryExhausted, leaving the store as it was.
    using isoline::api::HostMemoryExhausted;
    using isoline::api::InstantiateError;
    constexpr auto WideParams = std::size_t { 1'000 };
    auto const type = FuncType { .params = { I32 }, .results = {} };
    auto const wide = FuncType { .params = std::vector<ValueType>(WideParams, I32), .results = {} };
    auto builder = ModuleBuilder {};
    builder.type(type).type(wide).importFunction("env", "f", 0).importFunction("env", "g", 1);
    auto const host = isoline::runtime::makeHostFunction([](isoline::runtime::HostContext&,
                                                            std::span<isoline::runtime::Value const>,
                                                            std::span<isoline::runtime::Value>) noexcept {
        return std::expected<void, isoline::runtime::Abrupt> {};
    });
    auto imports = isoline::api::Imports {};
    imports.define("env", "f", type, host).define("env", "g", wide, host);
    auto const engine = isoline::api::Engine { isoline::api::EngineConfig {} };
    auto allocator = isoline::runtime::HeapMemoryAllocator {};
    auto store = isoline::api::Store { engine, allocator };
    auto const module = engine.load(builder.build());
    REQUIRE(module.has_value());
    auto& runtime = isoline::api::testing::runtimeOf(store);

    for (auto budget = std::size_t { 0 }; budget < WideParams * sizeof(ValueType); budget += 16)
    {
        auto const result = [&] {
            auto const limit = AllocationLimit { budget };
            return store.instantiate(*module, imports);
        }();
        CAPTURE(budget);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == InstantiateError { HostMemoryExhausted {} });
        CHECK(runtime.functionCount() == 0);
        CHECK(runtime.moduleInstanceCount() == 0);
    }
    // A container asked past its max_size() is refused alike.
    auto const pastMaxSize = [&] {
        auto const limit = AllocationLimit { 0, RefusalException::LengthError };
        return store.instantiate(*module, imports);
    }();
    REQUIRE_FALSE(pastMaxSize.has_value());
    CHECK(pastMaxSize.error() == InstantiateError { HostMemoryExhausted {} });
    CHECK(runtime.functionCount() == 0);
    for (auto const refusal: { RefusalException::BadAlloc, RefusalException::LengthError })
    {
        auto const defined = [&] {
            auto const limit = AllocationLimit { 0, refusal };
            return store.define(type, host);
        }();
        REQUIRE_FALSE(defined.has_value());
        CHECK(std::holds_alternative<HostMemoryExhausted>(defined.error()));
        CHECK(runtime.functionCount() == 0);
    }
    // With memory, the same store instantiates it: nothing refused earlier is in its way.
    CHECK(store.instantiate(*module, imports).has_value());
    CHECK(runtime.functionCount() == 2);
}

TEST_CASE("MemoryBounds.instantiateRefusesHostMemoryWhilePreparing", "[host-memory]")
{
    // A module of no imports and 300 distinct types: its first allocations are runtime::instantiate()'s
    // preparation (the TypeIds alone take 1'200 bytes). Every budget below that runs out there, before
    // the store changes: each is refused as HostMemoryExhausted, leaving the store as it was.
    using isoline::api::HostMemoryExhausted;
    using isoline::api::InstantiateError;
    constexpr auto TypeCount = uint32_t { 300 };
    auto builder = ModuleBuilder {};
    for (auto const arity: std::views::iota(uint32_t { 0 }, TypeCount))
        builder.type(FuncType { .params = std::vector<ValueType>(arity, I32), .results = {} });
    auto const engine = isoline::api::Engine { isoline::api::EngineConfig {} };
    auto allocator = isoline::runtime::HeapMemoryAllocator {};
    auto store = isoline::api::Store { engine, allocator };
    auto const module = engine.load(builder.build());
    REQUIRE(module.has_value());
    auto& runtime = isoline::api::testing::runtimeOf(store);
    auto const typesBefore = runtime.typeCount();
    // Made before any budget: an empty std::map allocates its head node in some standard libraries
    // (MSVC's), which is no part of what instantiation holds.
    auto const imports = isoline::api::Imports {};

    for (auto budget = std::size_t { 0 }; budget < TypeCount * sizeof(uint32_t); budget += 8)
    {
        auto const result = [&] {
            auto const limit = AllocationLimit { budget };
            return store.instantiate(*module, imports);
        }();
        CAPTURE(budget);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == InstantiateError { HostMemoryExhausted {} });
        CHECK(runtime.moduleInstanceCount() == 0);
        CHECK(runtime.typeCount() == typesBefore);
    }
    // A container asked past its max_size() there is refused alike.
    auto const pastMaxSize = [&] {
        auto const limit = AllocationLimit { 0, RefusalException::LengthError };
        return store.instantiate(*module, imports);
    }();
    REQUIRE_FALSE(pastMaxSize.has_value());
    CHECK(pastMaxSize.error() == InstantiateError { HostMemoryExhausted {} });
    CHECK(runtime.typeCount() == typesBefore);
    CHECK(store.instantiate(*module, imports).has_value());
    CHECK(runtime.typeCount() == typesBefore + TypeCount);
}

TEST_CASE("MemoryBounds.throwLoopReclaimsItsPayloads")
{
    // 10'000 rounds of throwing a payload of 1000 i32s, caught as an exnref and dropped, in a store of
    // 16 exceptions: the sweeps free the dropped payloads, so the loop holds about 16 of them at once,
    // where keeping every one would be some 300 MB.
    constexpr auto Params = std::size_t { 1000 };
    constexpr auto Rounds = uint32_t { 10'000 };
    constexpr auto MaxExceptions = uint32_t { 16 };
    auto body = std::vector<uint8_t> { 0x03, 0x40, 0x02, 0x69, 0x1F, 0x40, 0x01, 0x03, 0x00 };
    for ([[maybe_unused]] auto const param: std::views::iota(std::size_t { 0 }, Params))
        append(body, { 0x41, 0x00 }); // i32.const 0
    append(body,
           { 0x08,
             0x00,
             0x0B,
             0x00,
             0x0B,
             0x1A, // throw 0; end; unreachable; end; drop
             0x20,
             0x00,
             0x41,
             0x01,
             0x6B,
             0x22,
             0x00, // local 0 -= 1
             0x0D,
             0x00,
             0x0B }); // br_if 0; end
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = std::vector<ValueType>(Params, I32), .results = {} })
        .type(FuncType { .params = { I32 }, .results = {} })
        .tag(0)
        .function(1, {}, std::move(body));
    auto validated = isoline::validate::ValidatedModule::load(
        builder.build(), FeatureSet::wasm3(), isoline::binary::DecodeLimits {});
    REQUIRE(validated.has_value());
    auto allocator = isoline::runtime::HeapMemoryAllocator {};
    auto store = isoline::runtime::Store { isoline::runtime::StoreLimits { .maxExceptions = MaxExceptions },
                                           isoline::runtime::StoreServices { .allocator = allocator } };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    auto const instance = isoline::runtime::instantiate(store, interpreter, std::move(*validated), {});
    REQUIRE(instance.has_value());
    auto const function = store.moduleInstance(*instance).functions.at(0);
    auto const args = std::array<isoline::runtime::Value, 1> { isoline::runtime::I32 { .bits = Rounds } };

    auto const scope = AllocationScope {};
    auto const result = isoline::runtime::invokeChecked(store, interpreter, function, args, {});
    auto const peak = scope.peakBytes();
    REQUIRE(result.outcome.has_value());
    // The arena's payloads, each a vector of Params values that may hold twice what it uses, and the
    // stacks, which hold one payload at a time.
    auto const budget =
        (std::size_t { MaxExceptions + 2 } * 2 * Params * sizeof(isoline::runtime::Value)) + FixedBytes;
    CAPTURE(peak, budget);
    CHECK(peak <= budget);
    CHECK(store.liveExceptions() <= MaxExceptions);
}
