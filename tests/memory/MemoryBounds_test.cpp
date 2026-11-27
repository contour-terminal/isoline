// SPDX-License-Identifier: Apache-2.0

// A guest must never make the host run out of memory (spec §7). What a module costs before it runs is
// bounded by its size: loading it (decoding and validating) holds at most BytesPerModuleByte bytes per byte
// of the module, and so does each instantiation of it in the store, besides the memories and tables the
// allocator approves. Each test below is a module shaped to amplify one entity the decoder or the store
// copies, at a size where a cost per item that grows with another item's size (a name, a type) is orders of
// magnitude over the bound, and a constant cost per item is well under it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

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
#include <utility>
#include <variant>
#include <vector>

#include "AllocationCounter.hpp"
#include "AmplifyingModules.hpp"
#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Extern.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/testing/EngineRows.hpp>
#include <isoline/api/testing/RuntimeAccess.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/validate/TypeMatching.hpp>
#include <isoline/validate/ValidatedModule.hpp>
#include <isoline/validate/ValidationError.hpp>
#include <isoline/validate/testing/BranchBodies.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::Nullable;
using isoline::binary::ValueType;
using isoline::binary::testing::ModuleBuilder;
using isoline::testing::AllocationLimit;
using isoline::testing::AllocationScope;
using isoline::testing::compactImportGroup;
using isoline::testing::deepTypeChain;
using isoline::testing::lyingNameCount;
using isoline::testing::manyCatchClauses;
using isoline::testing::manyFunctionImports;
using isoline::testing::manyFunctions;
using isoline::testing::manyNames;
using isoline::testing::manyV128Literals;
using isoline::testing::multiValueCalls;
using isoline::testing::RefusalException;
using isoline::testing::sparseLongNames;
using isoline::testing::wideTags;
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

/// Appends @p bytes to @p out.
void append(std::vector<uint8_t>& out, std::vector<uint8_t> const& bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
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

TEST_CASE("MemoryBounds.positionalInstantiateRefusesHostMemoryBeforeTheStoreChanges", "[host-memory]")
{
    // Positional externs name entities of the store already: their copy is the one allocation before the
    // store changes. Refused, the instantiation is HostMemoryExhausted, and the store is as it was.
    using isoline::api::HostMemoryExhausted;
    using isoline::api::InstantiateError;
    auto const constI32 =
        isoline::binary::GlobalType { .type = I32, .mutability = isoline::binary::Mutability::Const };
    auto builder = ModuleBuilder {};
    builder.importGlobal("env", "g", constI32);
    auto const row = GENERATE(from_range(isoline::api::testing::engineRows()));
    auto const engine =
        isoline::api::Engine { isoline::api::testing::contract::configOf(isoline::api::StoreLimits {}, row) };
    auto allocator = isoline::runtime::HeapMemoryAllocator {};
    auto store = isoline::api::Store { engine, allocator };
    auto const module = engine.load(builder.build());
    REQUIRE(module.has_value());
    auto const global = store.defineGlobal(constI32, isoline::runtime::I32 { 1 });
    REQUIRE(global.has_value());
    auto const externs = std::array<isoline::api::Extern, 1> { *global };
    auto& runtime = isoline::api::testing::runtimeOf(store);
    for (auto const refusal: { RefusalException::BadAlloc, RefusalException::LengthError })
    {
        auto const result = [&] {
            auto const limit = AllocationLimit { 0, refusal };
            return store.instantiateByPosition(*module, externs);
        }();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == InstantiateError { HostMemoryExhausted {} });
        CHECK(runtime.moduleInstanceCount() == 0);
    }
    CHECK(store.instantiateByPosition(*module, externs).has_value());
    CHECK(runtime.moduleInstanceCount() == 1);
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

namespace
{

/// The bytes of one page of the default page size.
constexpr auto PageBytes = uint64_t { 65'536 };

/// An allocator that hands out memories of one page made beforehand, so that handing one out allocates
/// nothing: what a store does after it is the only allocation a define is left to refuse.
class PremadeMemoryAllocator final: public isoline::runtime::IMemoryAllocator
{
  public:
    /// Makes @p count memories of one page.
    explicit PremadeMemoryAllocator(std::size_t count)
    {
        auto heap = isoline::runtime::HeapMemoryAllocator {};
        auto const request = isoline::runtime::MemoryRequest {
            .type = {}, .initialBytes = PageBytes, .maximumBytes = PageBytes, .requiredGuardBytes = 0
        };
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        {
            auto memory = heap.allocate(request);
            REQUIRE(memory.has_value());
            _memories.push_back(std::move(*memory));
        }
    }

    /// The next memory made beforehand; Embedder once none is left, or for a size other than one page.
    [[nodiscard]] std::expected<std::unique_ptr<isoline::runtime::ILinearMemory>, isoline::runtime::Refusal>
    allocate(isoline::runtime::MemoryRequest const& request) noexcept override
    {
        if (_memories.empty() || request.initialBytes != PageBytes)
            return std::unexpected { isoline::runtime::Refusal::Embedder };
        auto memory = std::move(_memories.back());
        _memories.pop_back();
        return memory;
    }

  private:
    std::vector<std::unique_ptr<isoline::runtime::ILinearMemory>> _memories; ///< What is left to hand out.
};

/// The most defines a host-memory case tries: more than a store's container records per allocation, on
/// every standard library, so that one of them must allocate.
constexpr auto DefineAttempts = std::size_t { 256 };

/// Defines entities with @p define under an allocation budget of @p budget bytes each, until one is
/// refused, which must be as HostMemoryExhausted.
/// @return How many were defined before the refusal.
template <typename Define>
std::size_t defineUntilRefused(Define const& define, std::size_t budget, RefusalException refusal)
{
    for (auto const attempt: std::views::iota(std::size_t { 0 }, DefineAttempts))
    {
        auto const defined = [&] {
            auto const limit = AllocationLimit { budget, refusal };
            return define();
        }();
        if (!defined)
        {
            CHECK(defined.error() == isoline::api::DefineError { isoline::api::HostMemoryExhausted {} });
            return attempt;
        }
    }
    FAIL("no define was refused");
    return DefineAttempts;
}

} // namespace

TEST_CASE("MemoryBounds.defineRefusesHostMemoryLeavingTheStoreUnchanged", "[host-memory]")
{
    // The host runs out of memory where a define records its entity in the store, after the entity's own
    // storage is made: each define is refused as HostMemoryExhausted, and the store is as it was, nothing
    // recorded and nothing charged, so the next define takes the address the refused one would have.
    using isoline::api::MemoryType;
    auto const row = GENERATE(from_range(isoline::api::testing::engineRows()));
    for (auto const refusal: { RefusalException::BadAlloc, RefusalException::LengthError })
    {
        CAPTURE(refusal);
        auto const engine = isoline::api::Engine { isoline::api::testing::contract::configOf(
            isoline::api::StoreLimits {}, row) };
        auto allocator = PremadeMemoryAllocator { DefineAttempts + 1 };
        auto store = isoline::api::Store { engine, allocator };
        auto& runtime = isoline::api::testing::runtimeOf(store);
        auto const onePage = isoline::binary::Limits { .min = 1, .max = std::nullopt, .addressType = {} };

        auto const defineMemory = [&] {
            return store.defineMemory(MemoryType { .limits = onePage });
        };
        auto const memories = defineUntilRefused(defineMemory, 0, refusal);
        CHECK(runtime.memoryCount() == memories);
        CHECK(store.used(isoline::api::Limit::MemoryBytes) == memories * PageBytes);
        auto const memory = defineMemory();
        REQUIRE(memory.has_value());
        CHECK(memory->address() == memories);

        // A table of one element: its elements are allocated within the budget, its record past it.
        auto const funcRef = isoline::binary::FuncRef;
        auto const null =
            isoline::runtime::Ref { .kind = isoline::runtime::RefKind::Null, .heap = {}, .address = 0 };
        auto const defineTable = [&] {
            return store.defineTable(isoline::binary::TableType { .element = funcRef, .limits = onePage },
                                     null);
        };
        auto const tables = defineUntilRefused(defineTable, sizeof(isoline::runtime::Ref), refusal);
        CHECK(runtime.tableCount() == tables);
        CHECK(store.used(isoline::api::Limit::TableElements) == tables);
        auto const table = defineTable();
        REQUIRE(table.has_value());
        CHECK(table->address() == tables);

        auto const defineGlobal = [&] {
            return store.defineGlobal(
                isoline::binary::GlobalType { .type = I32, .mutability = isoline::binary::Mutability::Const },
                isoline::runtime::I32 { 1 });
        };
        auto const globals = defineUntilRefused(defineGlobal, 0, refusal);
        auto const global = defineGlobal();
        REQUIRE(global.has_value());
        CHECK(global->address() == globals);
    }
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

namespace
{

/// The bytes decoding a name section, and writing it again, may hold per byte of its payload.
///
/// Measured under Clang/libstdc++ with ASan, the densest payload is manyNames: about 23 bytes per byte (a
/// two-byte entry becomes a 40-byte NameAssoc or a 32-byte IndirectNameMap, and a vector holds up to
/// twice what it uses). Long names cost about one byte per byte. The bound leaves room for larger
/// standard library types, and stays below what reserving a claimed count would hold (40 bytes per
/// byte of lyingNameCount) and far below storage addressed by index (gigabytes for sparseLongNames).
constexpr auto BytesPerNameSectionByte = std::size_t { 64 };

/// Decodes the name section of @p bytes, and writes it again, and checks that neither held more than
/// BytesPerNameSectionByte per byte of its payload.
/// @return The names.
isoline::binary::NameSection checkNameCodecLinear(std::vector<std::byte> const& bytes)
{
    auto const module = isoline::binary::decodeModule(bytes, FeatureSet::wasm3());
    REQUIRE(module.has_value());
    REQUIRE(module->customs.size() == 1);
    auto const& payload = module->customs[0].bytes;
    auto const budget = (BytesPerNameSectionByte * payload.size()) + FixedBytes;

    // The budget is enforced, not only measured: a codec that would hold more is refused the
    // allocation that crosses it (bad_alloc, which fails the test) before the host commits it.
    auto names = std::expected<isoline::binary::NameSection, isoline::binary::DecodeError> {};
    auto reencoded = std::vector<std::byte> {};
    auto codecBytes = std::size_t { 0 };
    {
        auto const scope = AllocationScope { budget };
        names = isoline::binary::decodeNameSection(payload);
        if (names)
            reencoded = isoline::binary::encodeNameSection(*names);
        codecBytes = scope.peakBytes();
    }
    CAPTURE(payload.size(), budget, codecBytes);
    REQUIRE(names.has_value());
    CHECK(codecBytes <= budget);
    CHECK(reencoded == payload);
    return std::move(*names);
}

} // namespace

TEST_CASE("MemoryBounds.nameSectionIsLinear")
{
    // 20 000 empty function names and as many empty local maps, two to four bytes each.
    auto const names = checkNameCodecLinear(manyNames(20'000));
    CHECK(names.direct[static_cast<std::size_t>(isoline::binary::NameKind::Function)].size() == 20'000);
    CHECK(names.indirect[static_cast<std::size_t>(isoline::binary::NameKind::Local)].size() == 20'000);
}

TEST_CASE("MemoryBounds.sparseNamesAreNotAddressedByIndex")
{
    // 2000 functions and locals named by 256-byte names at indices up to 2^32 - 1: storage addressed
    // by index would ask for about 160 GiB at once, and a copy per name of what came before for
    // megabytes per name; the budgeted scope refuses either at the first allocation past 64 MiB.
    auto const names = checkNameCodecLinear(sparseLongNames(2000, 256));
    auto const& functions = names.direct[static_cast<std::size_t>(isoline::binary::NameKind::Function)];
    REQUIRE(functions.size() == 2000);
    CHECK(functions.back().index > 4'290'000'000U);
}

TEST_CASE("MemoryBounds.nameCountsAreNotTrusted")
{
    // A count of 1 MiB entries in 1 MiB of bytes that fail at the first index: nothing is reserved for
    // the entries the count claims.
    auto const bytes = lyingNameCount(1024 * 1024);
    auto const module = isoline::binary::decodeModule(bytes, FeatureSet::wasm3());
    REQUIRE(module.has_value());
    auto const& payload = module->customs[0].bytes;
    auto names = std::expected<isoline::binary::NameSection, isoline::binary::DecodeError> {};
    auto codecBytes = std::size_t { 0 };
    {
        auto const scope = AllocationScope { FixedBytes };
        names = isoline::binary::decodeNameSection(payload);
        codecBytes = scope.peakBytes();
    }
    REQUIRE(!names.has_value());
    CHECK(names.error().code == isoline::binary::DecodeErrorCode::IntegerTooLarge);
    CAPTURE(codecBytes);
    CHECK(codecBytes <= FixedBytes);

    // A count above the bytes the subsection has left is refused before anything is read.
    // Function names: two bytes, a count of three, and one byte.
    auto const overclaimed = std::vector<std::byte> {
        std::byte { 0x01 }, std::byte { 0x02 }, std::byte { 0x03 }, std::byte { 0x00 }
    };
    auto const refused = isoline::binary::decodeNameSection(overclaimed);
    REQUIRE(!refused.has_value());
    CHECK(refused.error().code == isoline::binary::DecodeErrorCode::LengthOutOfBounds);
}

namespace
{

/// A chain of @p length types: @p bottom, then each (func (param (ref null $previous))).
std::vector<FuncType> typeChain(std::size_t length, FuncType bottom)
{
    auto types = std::vector<FuncType> {};
    types.reserve(length);
    types.push_back(std::move(bottom));
    for ([[maybe_unused]] auto const level: std::views::iota(std::size_t { 1 }, length))
        types.push_back(FuncType { .params = { isoline::binary::concreteRef(
                                       static_cast<uint32_t>(types.size() - 1), Nullable::Yes) },
                                   .results = {} });
    return types;
}

/// The bytes of a module whose type section is @p types.
std::size_t moduleBytesOf(std::vector<FuncType> const& types)
{
    auto builder = ModuleBuilder {};
    for (auto const& type: types)
        builder.type(type);
    return builder.build().size();
}

} // namespace

TEST_CASE("TypeMatching.diagonalChainsLinearMemory")
{
    // The top of one chain queried against every level of another, which differs at its bottom: each
    // query a new pair, the shape that drove a per-pair memo to n^2 / 2 pairs. Building the two
    // sections' joint space and answering every query holds what loading the two modules may
    // (BytesPerModuleByte), and grows with n alone: the peak at 2n within 2.2 x the peak at n.
    auto const peakOf = [](std::size_t length) {
        auto const sub = typeChain(length, FuncType {});
        auto const super = typeChain(length, FuncType { .params = { I32 }, .results = {} });
        auto const budget = FixedBytes + (BytesPerModuleByte * (moduleBytesOf(sub) + moduleBytesOf(super)));
        auto const scope = AllocationScope {};
        {
            auto const limit = AllocationLimit { budget };
            auto const matcher = isoline::validate::TypeMatcher { sub, super };
            for (auto const level: std::views::iota(std::size_t { 0 }, length))
                REQUIRE_FALSE(
                    matcher.matches(static_cast<uint32_t>(length - 1), static_cast<uint32_t>(level)));
        }
        return scope.peakBytes();
    };
    auto const single = peakOf(10'000);
    auto const twice = peakOf(20'000);
    CHECK(twice * 10 <= single * 22);
}
