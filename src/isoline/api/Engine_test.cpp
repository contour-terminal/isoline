// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/ExecutionEngines.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/testing/Contract.hpp>
#include <isoline/api/testing/RuntimeAccess.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>
#include <isoline/validate/ValidationError.hpp>

using isoline::api::CompileLimitExceeded;
using isoline::api::Dispatch;
using isoline::api::Engine;
using isoline::api::EngineConfig;
using isoline::api::EngineIdentity;
using isoline::api::ExecutionEngine;
using isoline::api::ExecutionMode;
using isoline::api::Feature;
using isoline::api::FeatureSet;
using isoline::api::LoadError;
using isoline::api::ModuleTooLarge;
using isoline::api::ThreadPreparation;
using isoline::binary::DecodeError;
using isoline::binary::DecodeErrorCode;
using isoline::binary::FuncType;
using isoline::binary::testing::ModuleBuilder;
using isoline::validate::ValidationError;
using isoline::validate::ValidationErrorCode;

namespace
{

/// A module with nothing in it: the magic number and the version, 8 bytes.
std::vector<std::byte> emptyModuleBytes()
{
    return ModuleBuilder {}.build();
}

/// The empty module followed by one byte, which is not a section: 9 bytes that do not decode.
std::vector<std::byte> nineBytes()
{
    auto bytes = emptyModuleBytes();
    bytes.push_back(std::byte { 0x00 });
    return bytes;
}

/// A module whose magic is wrong.
std::vector<std::byte> badMagicBytes()
{
    auto bytes = emptyModuleBytes();
    bytes.front() = std::byte { 0x01 };
    return bytes;
}

/// A module of one function [] -> [i32] whose body adds on an empty stack.
std::vector<std::byte> addOnEmptyStackBytes()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { isoline::binary::I32 } }).function(0, {}, { 0x6A });
    return builder.build();
}

/// A module of one function [] -> [] whose body pushes a v128 constant and drops it.
std::vector<std::byte> v128ConstBytes()
{
    auto body = std::vector<uint8_t> { 0xFD, 0x0C }; // v128.const
    body.resize(body.size() + 16, 0x00);
    body.push_back(0x1A); // drop
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, std::move(body));
    return builder.build();
}

/// A module of one function [] -> [] whose body pushes an i32 constant and drops it @p count times: twice
/// as many instructions.
std::vector<std::byte> pushAndDropBytes(uint32_t count)
{
    auto body = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        body.push_back(0x41); // i32.const 0
        body.push_back(0x00);
        body.push_back(0x1A); // drop
    }
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, std::move(body));
    return builder.build();
}

/// A module of one function [] -> [] whose body pushes @p count i32 constants and drops them all.
std::vector<std::byte> tallStackBytes(uint32_t count)
{
    auto body = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        // Pushed one by one: GCC 14's release build reports inserting a list as a stringop-overflow.
        body.push_back(0x41); // i32.const 0
        body.push_back(0x00);
    }
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        body.push_back(0x1A); // drop
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, std::move(body));
    return builder.build();
}

} // namespace

TEST_CASE("Engine.refusesAModuleAboveTheSizeLimit")
{
    auto const engine = Engine { EngineConfig { .load = { .maxModuleBytes = 8 } } };
    CHECK(engine.load(emptyModuleBytes()).has_value()); // the empty module is 8 bytes

    auto const refused = engine.load(nineBytes());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error()
          == LoadError { ModuleTooLarge { .bytes = 9, .limit = 8 } }); // refused before decoding

    // The same bytes under a limit they fit are decoded, and fail there.
    auto const decoded = Engine { EngineConfig { .load = { .maxModuleBytes = 9 } } }.load(nineBytes());
    REQUIRE_FALSE(decoded.has_value());
    CHECK(std::holds_alternative<DecodeError>(decoded.error()));
}

TEST_CASE("Engine.decodeAndValidationErrorsAreDistinct")
{
    auto const engine = Engine { EngineConfig {} };

    auto const malformed = engine.load(badMagicBytes());
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error() == LoadError { DecodeError { DecodeErrorCode::MagicHeaderNotDetected, 0 } });

    auto const invalid = engine.load(addOnEmptyStackBytes());
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(std::holds_alternative<ValidationError>(invalid.error()));
    CHECK(std::get<ValidationError>(invalid.error()).code == ValidationErrorCode::TypeMismatch);
}

TEST_CASE("Engine.tellsAnImplementationLimitFromAnInvalidModule")
{
    using isoline::api::isImplementationLimit;
    auto const tooLarge = Engine { EngineConfig { .load = { .maxModuleBytes = 8 } } }.load(nineBytes());
    REQUIRE_FALSE(tooLarge.has_value());
    CHECK(isImplementationLimit(tooLarge.error()));

    auto const malformed = Engine { EngineConfig {} }.load(badMagicBytes());
    REQUIRE_FALSE(malformed.has_value());
    CHECK_FALSE(isImplementationLimit(malformed.error()));

    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("m", "f", 0);
    auto const pastImports =
        Engine { EngineConfig { .load = { .decode = { .maxImports = 0 } } } }.load(builder.build());
    REQUIRE_FALSE(pastImports.has_value());
    REQUIRE(std::holds_alternative<DecodeError>(pastImports.error()));
    CHECK(isImplementationLimit(pastImports.error()));

    auto const invalid = Engine { EngineConfig {} }.load(addOnEmptyStackBytes());
    REQUIRE_FALSE(invalid.has_value());
    CHECK_FALSE(isImplementationLimit(invalid.error()));

    // Three operands at once, past a cap of max(maxParams, maxResults) = 1 with no allowance per
    // instruction: valid, but refused by the validator's own limit.
    auto const tall = Engine {
        EngineConfig {
            .load = { .decode = { .maxParams = 1, .maxResults = 1, .maxOperandsPerInstruction = 0 } } }
    }.load(tallStackBytes(3));
    REQUIRE_FALSE(tall.has_value());
    REQUIRE(std::holds_alternative<ValidationError>(tall.error()));
    CHECK(isImplementationLimit(tall.error()));
    CHECK(Engine { EngineConfig {} }.load(tallStackBytes(3)).has_value());
}

TEST_CASE("Engine.honoursItsFeatures")
{
    CHECK(Engine { EngineConfig {} }.load(v128ConstBytes()).has_value());

    auto const withoutSimd =
        Engine { EngineConfig { .features = FeatureSet::wasm2().without(Feature::Simd) } };
    auto const refused = withoutSimd.load(v128ConstBytes());
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(std::holds_alternative<DecodeError>(refused.error()));
    CHECK(std::get<DecodeError>(refused.error()).code == DecodeErrorCode::IllegalOpcode);
    CHECK(isoline::api::requiredFeature(refused.error()) == Feature::Simd);
    CHECK_FALSE(isoline::api::isImplementationLimit(refused.error()));

    // The engine passes its decode limits on, too.
    auto const narrow = Engine { EngineConfig { .load = { .decode = { .maxFunctionBodySize = 4 } } } };
    CHECK_FALSE(narrow.load(v128ConstBytes()).has_value());
}

TEST_CASE("Engine.requiresAClosedFeatureSet")
{
    // Relaxed SIMD without SIMD is not closed; withPrerequisites() is how an embedder closes it.
    auto const open = FeatureSet::wasm2().without(Feature::Simd).with(Feature::RelaxedSimd);
    REQUIRE_FALSE(open.checkPrerequisites().has_value());

    auto const engine = Engine { EngineConfig { .features = open.withPrerequisites() } };
    CHECK(engine.config().features.checkPrerequisites().has_value());
    CHECK(engine.load(v128ConstBytes()).has_value());
}

TEST_CASE("Engine.fastIsTheDefault")
{
    CHECK(EngineConfig {}.engine == ExecutionEngine::Fast);
    CHECK(EngineConfig {}.dispatch == isoline::api::bestDispatch());
}

TEST_CASE("Engine.refusesOverTheCompileBudget")
{
    // 1'000 instructions lower into more than 128 bytes: the fast engine refuses the module at its one
    // function, an implementation limit; the reference engine lowers nothing, and loads it.
    auto const bytes = pushAndDropBytes(500);
    auto const fast = Engine { EngineConfig { .load = { .maxCompileBytes = 128 } } }.load(bytes);
    REQUIRE_FALSE(fast.has_value());
    REQUIRE(std::holds_alternative<CompileLimitExceeded>(fast.error()));
    auto const refused = std::get<CompileLimitExceeded>(fast.error());
    CHECK(refused.codeIndex == 0);
    CHECK(refused.limit == 128);
    CHECK(refused.kind == isoline::api::CompileLimit::Budget);
    CHECK(refused.requested > 0);
    CHECK(fast.error()
          == LoadError {
              CompileLimitExceeded { .codeIndex = 0, .requested = refused.requested, .limit = 128 } });
    CHECK(refused != CompileLimitExceeded { .codeIndex = 1, .requested = refused.requested, .limit = 128 });
    CHECK(isoline::api::isImplementationLimit(fast.error()));
    CHECK_FALSE(isoline::api::requiredFeature(fast.error()).has_value());
    auto const reference = Engine {
        EngineConfig { .engine = ExecutionEngine::Reference, .load = { .maxCompileBytes = 128 } }
    }.load(bytes);
    REQUIRE(reference.has_value());
    CHECK(reference->code() == nullptr);
    // The default budget lowers it.
    auto const lowered = Engine { EngineConfig {} }.load(bytes);
    REQUIRE(lowered.has_value());
    CHECK(lowered->code() != nullptr);
}

namespace
{

/// Refuses every allocation, as a host out of memory does.
class ExhaustedMemory final: public std::pmr::memory_resource
{
    void* do_allocate(std::size_t /*bytes*/, std::size_t /*alignment*/) override { throw std::bad_alloc {}; }
    void do_deallocate(void* /*pointer*/, std::size_t /*bytes*/, std::size_t /*alignment*/) override {}
    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
    {
        return this == &other;
    }
};

/// Passes every allocation on to the global heap, and counts those not yet returned in a counter it does not
/// own, which outlives it: nothing but its owners keeps its storage, so destroying it frees it.
class LiveMemory final: public std::pmr::memory_resource
{
  public:
    /// A resource that counts into @p live.
    /// @param live The counter; must outlive the resource.
    explicit LiveMemory(int64_t& live) noexcept: _live { &live } {}

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        ++*_live;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
    {
        --*_live;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
    {
        return this == &other;
    }

    int64_t* _live;
};

} // namespace

TEST_CASE("Engine.lowersIntoItsLoweringMemory")
{
    // The fast engine lowers into the memory its configuration names: one with nothing to give refuses the
    // module as the host out of memory, which no limit lifts. The reference engine lowers nothing.
    auto const bytes = pushAndDropBytes(1);
    auto const exhausted = std::make_shared<ExhaustedMemory>();
    auto const fast = Engine { EngineConfig { .loweringMemory = exhausted } }.load(bytes);
    REQUIRE_FALSE(fast.has_value());
    CHECK(fast.error() == LoadError { isoline::api::HostMemoryExhausted {} });
    CHECK_FALSE(isoline::api::isImplementationLimit(fast.error()));
    CHECK(Engine { EngineConfig { .engine = ExecutionEngine::Reference, .loweringMemory = exhausted } }
              .load(bytes)
              .has_value());
}

TEST_CASE("Engine.loweringMemoryLivesAsLongAsItsCode")
{
    // A store's instance keeps the code it runs, and the code the memory it was lowered into: the engine, the
    // module and the caller's handle on the memory go first, and destroying the store last returns every
    // allocation to the memory before it is freed. Nothing else keeps the memory's storage, so a record
    // returned to it after it is gone is a use after free that AddressSanitizer reports.
    auto live = int64_t { 0 };
    auto memory = std::make_shared<LiveMemory>(live);
    auto allocator = isoline::api::HeapMemoryAllocator {};
    auto store = std::optional<isoline::api::Store> {};
    {
        auto const engine = Engine { EngineConfig { .loweringMemory = memory } };
        auto const module = engine.load(pushAndDropBytes(4));
        REQUIRE(module.has_value());
        store.emplace(engine, allocator);
        REQUIRE(store->instantiate(*module, isoline::api::Imports {}).has_value());
    }
    memory.reset();
    CHECK(live > 0);
    store.reset();
    CHECK(live == 0);
}

TEST_CASE("Engine.lowersOnceForManyStores")
{
    // One module, instantiated by 8 stores on 8 threads: every instance runs the one lowered module the
    // engine made when it loaded it.
    namespace contract = isoline::api::testing::contract;
    constexpr auto Threads = 8U;
    auto const engine = Engine { EngineConfig {} };
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { isoline::binary::I32 } })
        .function(0, {}, { 0x41, 0x2A }) // i32.const 42
        .exportFunction("answer", 0);
    auto const module = engine.load(builder.build());
    REQUIRE(module.has_value());
    REQUIRE(module->code() != nullptr);
    /// What one thread saw: whether its instance runs the module's code, and what it answered.
    struct Seen
    {
        bool sharesTheCode = false;
        int32_t answer = 0;
    };
    auto const instantiate = [&] {
        auto const preparation = engine.prepareThread();
        auto allocator = isoline::api::HeapMemoryAllocator {};
        auto store = isoline::api::Store { engine, allocator };
        auto const instance = store.instantiate(*module, isoline::api::Imports {});
        if (!preparation || !instance)
            return Seen {};
        auto& runtime = isoline::api::testing::runtimeOf(store);
        auto const& code =
            runtime.moduleInstance(isoline::api::testing::RuntimeAccess::addressOf(*instance)).code;
        auto results = std::array<isoline::api::Value, 1> {};
        auto const answer = instance->function("answer");
        if (!answer || !instance->call(*answer, {}, results))
            return Seen {};
        return Seen { .sharesTheCode = code.get() == module->code().get(),
                      .answer = static_cast<int32_t>(std::get<isoline::api::I32>(results[0]).bits) };
    };
    auto workers = std::vector<std::unique_ptr<contract::Worker<Seen>>> {};
    for ([[maybe_unused]] auto const thread: std::views::iota(0U, Threads))
        workers.push_back(std::make_unique<contract::Worker<Seen>>(instantiate));
    for (auto const& worker: workers)
    {
        auto const seen = worker->result("an instantiating thread");
        CHECK(seen.sharesTheCode);
        CHECK(seen.answer == 42);
    }
}

TEST_CASE("Engine.keepsItsConfiguration")
{
    auto const config = EngineConfig { .features = FeatureSet::wasm2(),
                                       .mode = ExecutionMode::Deterministic,
                                       .load = { .maxModuleBytes = 1024, .decode = { .maxImports = 7 } },
                                       .store = { .maxMemoryBytes = 4096 } };
    auto const engine = Engine { config };
    CHECK(engine.config().identity() == config.identity());
    CHECK(engine.config().load == config.load);
    CHECK(engine.config().load.decode.maxImports == 7);
    CHECK(engine.config().store.maxMemoryBytes == 4096);
}

TEST_CASE("Engine.identityIsWhatCodeDependsOn")
{
    auto const wasm3 = EngineConfig {};
    CHECK(wasm3.identity()
          == EngineIdentity { .engine = isoline::api::ExecutionEngine::Fast,
                              .mode = ExecutionMode::Production,
                              .features = FeatureSet::wasm3(),
                              .dispatch = isoline::api::bestDispatch() });
    CHECK(EngineConfig { .engine = ExecutionEngine::Reference }.identity()
          == EngineIdentity { .engine = isoline::api::ExecutionEngine::Reference,
                              .mode = ExecutionMode::Production,
                              .features = FeatureSet::wasm3(),
                              .dispatch = std::nullopt });
    CHECK(EngineConfig { .features = FeatureSet::wasm2() }.identity() != wasm3.identity());
    CHECK(EngineConfig { .mode = ExecutionMode::Deterministic }.identity() != wasm3.identity());
    // Limits decide whether a module loads, not what its code is.
    CHECK(EngineConfig { .load = { .maxModuleBytes = 1, .decode = { .maxImports = 1 } } }.identity()
          == wasm3.identity());
    CHECK(EngineConfig { .store = { .maxMemoryBytes = 1 } }.identity() == wasm3.identity());
    CHECK(EngineConfig { .load = { .maxCompileBytes = 1 } }.identity() == wasm3.identity());
}

TEST_CASE("Engine.prepareThreadAndGuard")
{
    static_assert(std::is_move_constructible_v<ThreadPreparation>);
    static_assert(!std::is_copy_constructible_v<ThreadPreparation>);
    static_assert(!std::is_copy_assignable_v<ThreadPreparation>);

    auto const engine = Engine { EngineConfig {} };
    auto preparation = engine.prepareThread();
    REQUIRE(preparation.has_value());
    CHECK(preparation->empty()); // an interpreter needs nothing of a thread
    CHECK(engine.requiredGuardBytes() == 0);
}

TEST_CASE("Engine.threadPreparationReleasesWhatItHolds")
{
    /// A thread resource that reports its release.
    class Resource final: public isoline::api::IThreadResource
    {
      public:
        explicit Resource(int& releases) noexcept: _releases { &releases } {}
        ~Resource() override { ++*_releases; }
        Resource(Resource const&) = delete;
        Resource& operator=(Resource const&) = delete;
        Resource(Resource&&) = delete;
        Resource& operator=(Resource&&) = delete;

      private:
        int* _releases;
    };

    auto releases = 0;
    {
        auto token = ThreadPreparation { std::make_unique<Resource>(releases) };
        CHECK_FALSE(token.empty());
        auto const moved = std::move(token);
        CHECK_FALSE(moved.empty());
        CHECK(releases == 0);
    }
    CHECK(releases == 1);
}

TEST_CASE("Engine.hostMemoryExhaustionIsNeitherALimitNorAFeature")
{
    using isoline::api::HostMemoryExhausted;
    auto const exhausted = LoadError { HostMemoryExhausted {} };
    CHECK(exhausted == LoadError { HostMemoryExhausted {} });
    CHECK_FALSE(isoline::api::isImplementationLimit(exhausted));
    CHECK_FALSE(isoline::api::requiredFeature(exhausted).has_value());
    CHECK_FALSE(
        isoline::api::requiredFeature(LoadError { ModuleTooLarge { .bytes = 9, .limit = 8 } }).has_value());
}

TEST_CASE("Contract.Engine.fastDispatchMustBeAvailable", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    // One past the last Dispatch: no build compiles it, whether or not a probe row passed for TailCall.
    auto const uncompiled = static_cast<Dispatch>(std::to_underlying(Dispatch::TailCall) + 1);
    std::ignore = Engine { EngineConfig { .engine = ExecutionEngine::Fast, .dispatch = uncompiled } };
    FAIL("SURVIVED: the fast engine accepted a dispatch this build did not compile");
}

TEST_CASE("Contract.Engine.knownEngine", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    // One past the last ExecutionEngine: no row names it.
    auto const unknown = static_cast<ExecutionEngine>(std::to_underlying(ExecutionEngine::Fast) + 1);
    std::ignore = Engine { EngineConfig { .engine = unknown } };
    FAIL("SURVIVED: an engine of no ExecutionEngine enumerator");
}

TEST_CASE("Engine.rowsNameEveryEngine")
{
    namespace engines = isoline::api::engines;
    // One past the last ExecutionEngine: no row names it, and no identity carries a dispatch for it.
    auto const unknown = static_cast<ExecutionEngine>(std::to_underlying(ExecutionEngine::Fast) + 1);
    for (auto const engine: { ExecutionEngine::Reference, ExecutionEngine::Fast })
    {
        REQUIRE(engines::rowOf(engine) != nullptr);
        CHECK(engines::rowOf(engine)->engine == engine);
        CHECK(engines::rowNamed(engines::rowOf(engine)->name) == engines::rowOf(engine));
    }
    CHECK(engines::rowOf(unknown) == nullptr);
    CHECK(engines::rowNamed("jit") == nullptr);
    CHECK_FALSE(EngineConfig { .engine = unknown }.identity().dispatch.has_value());
}

TEST_CASE("Engine.referenceIgnoresTheDispatch")
{
    // The reference engine lowers nothing: a dispatch no build compiles is no part of it.
    auto const uncompiled = static_cast<Dispatch>(std::to_underlying(Dispatch::TailCall) + 1);
    auto const engine =
        Engine { EngineConfig { .engine = ExecutionEngine::Reference, .dispatch = uncompiled } };
    CHECK_FALSE(engine.config().identity().dispatch.has_value());
    CHECK(engine.load(emptyModuleBytes()).has_value());
}
