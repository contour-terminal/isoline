// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/validate/ValidationError.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::api::Engine;
using isoline::api::EngineConfig;
using isoline::api::EngineIdentity;
using isoline::api::ExecutionMode;
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

/// A module of one function [] -> [] whose body pushes @p count i32 constants and drops them all.
std::vector<std::byte> tallStackBytes(uint32_t count)
{
    auto body = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        body.insert(body.end(), { 0x41, 0x00 }); // i32.const 0
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
    CHECK(std::get<DecodeError>(refused.error()).code == DecodeErrorCode::FeatureNotEnabled);
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
          == EngineIdentity { .engine = isoline::api::ExecutionEngine::Reference,
                              .mode = ExecutionMode::Production,
                              .features = FeatureSet::wasm3() });
    CHECK(EngineConfig { .features = FeatureSet::wasm2() }.identity() != wasm3.identity());
    CHECK(EngineConfig { .mode = ExecutionMode::Deterministic }.identity() != wasm3.identity());
    // Limits decide whether a module loads, not what its code is.
    CHECK(EngineConfig { .load = { .maxModuleBytes = 1, .decode = { .maxImports = 1 } } }.identity()
          == wasm3.identity());
    CHECK(EngineConfig { .store = { .maxMemoryBytes = 1 } }.identity() == wasm3.identity());
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
