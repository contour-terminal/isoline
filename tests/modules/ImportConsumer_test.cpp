// SPDX-License-Identifier: Apache-2.0
//
// Isoline through `import isoline;` alone: no Isoline header. Every type a public signature names is
// spelled here by its own name, so a type that is reachable but not nameable through the module fails to
// compile (the plan's Review Focus 3).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <variant>

#include "AddModule.hpp"

import isoline;

namespace
{

using namespace isoline::api;

/// A module that imports `env.double: (i32) -> i32` and exports that import as `double`:
///
///     (module
///       (import "env" "double" (func (param i32) (result i32)))
///       (export "double" (func 0)))
///
/// Its 44 bytes: the magic number and version 1; the type section (1), 6 bytes, `60 01 7F 01 7F`; the
/// import section (2), 14 bytes, `03 'env' 06 'double'`, a function (`00`) of type 0; and the export
/// section (7), 10 bytes, `06 'double'`, function 0.
constexpr auto DoubleImportModule = std::to_array<std::byte>({
    std::byte { 0x00 }, std::byte { 0x61 }, std::byte { 0x73 }, std::byte { 0x6D }, std::byte { 0x01 },
    std::byte { 0x00 }, std::byte { 0x00 }, std::byte { 0x00 }, std::byte { 0x01 }, std::byte { 0x06 },
    std::byte { 0x01 }, std::byte { 0x60 }, std::byte { 0x01 }, std::byte { 0x7F }, std::byte { 0x01 },
    std::byte { 0x7F }, std::byte { 0x02 }, std::byte { 0x0E }, std::byte { 0x01 }, std::byte { 0x03 },
    std::byte { 0x65 }, std::byte { 0x6E }, std::byte { 0x76 }, std::byte { 0x06 }, std::byte { 0x64 },
    std::byte { 0x6F }, std::byte { 0x75 }, std::byte { 0x62 }, std::byte { 0x6C }, std::byte { 0x65 },
    std::byte { 0x00 }, std::byte { 0x00 }, std::byte { 0x07 }, std::byte { 0x0A }, std::byte { 0x01 },
    std::byte { 0x06 }, std::byte { 0x64 }, std::byte { 0x6F }, std::byte { 0x75 }, std::byte { 0x62 },
    std::byte { 0x6C }, std::byte { 0x65 }, std::byte { 0x00 }, std::byte { 0x00 },
});

/// The value an alternative of a visited error variant reports: which alternative it was.
enum class Alternative : std::uint8_t
{
    ModuleTooLarge,
    HostMemoryExhausted,
    DecodeError,
    ValidationError,
    EngineMismatch,
    ForeignImport,
    LinkError,
    LimitExceeded,
    AllocationFailure,
    CallError,
    Trap,
    HostFailure,
    UncaughtException,
    ValueMismatch,
    ImportCountMismatch,
    InvalidType,
    ForeignHandle,
    StoreDeleted,
};

/// A visitor of every alternative of the API's error variants, each spelled by its name.
struct AlternativeOf
{
    Alternative operator()(ModuleTooLarge const&) const noexcept { return Alternative::ModuleTooLarge; }
    Alternative operator()(HostMemoryExhausted const&) const noexcept
    {
        return Alternative::HostMemoryExhausted;
    }
    Alternative operator()(DecodeError const&) const noexcept { return Alternative::DecodeError; }
    Alternative operator()(ValidationError const&) const noexcept { return Alternative::ValidationError; }
    Alternative operator()(EngineMismatch const&) const noexcept { return Alternative::EngineMismatch; }
    Alternative operator()(ForeignImport const&) const noexcept { return Alternative::ForeignImport; }
    Alternative operator()(LinkError const&) const noexcept { return Alternative::LinkError; }
    Alternative operator()(LimitExceeded const&) const noexcept { return Alternative::LimitExceeded; }
    Alternative operator()(AllocationFailure const&) const noexcept { return Alternative::AllocationFailure; }
    Alternative operator()(CallError const&) const noexcept { return Alternative::CallError; }
    Alternative operator()(Trap const&) const noexcept { return Alternative::Trap; }
    Alternative operator()(HostFailure const&) const noexcept { return Alternative::HostFailure; }
    Alternative operator()(UncaughtException const&) const noexcept { return Alternative::UncaughtException; }
    Alternative operator()(ValueMismatch const&) const noexcept { return Alternative::ValueMismatch; }
    Alternative operator()(ImportCountMismatch const&) const noexcept
    {
        return Alternative::ImportCountMismatch;
    }
    Alternative operator()(InvalidType const&) const noexcept { return Alternative::InvalidType; }
    Alternative operator()(ForeignHandle const&) const noexcept { return Alternative::ForeignHandle; }
    Alternative operator()(StoreDeleted const&) const noexcept { return Alternative::StoreDeleted; }
};

} // namespace

TEST_CASE("Modules.instantiatesAndCalls")
{
    auto const engine = Engine { EngineConfig {} };
    auto const module = engine.load(isoline::consumer::AddModule);
    REQUIRE(module.has_value());

    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const instance = store.instantiate(*module, Imports {});
    REQUIRE(instance.has_value());
    auto const add = instance->function("add");
    REQUIRE(add.has_value());

    auto const args = std::array<Value, 2> { I32 { 2 }, I32 { 3 } };
    auto results = std::array<Value, 1> {};
    auto const called = instance->call(*add, args, results);
    REQUIRE(called.has_value());
    CHECK(results[0] == Value { I32 { 5 } });
}

TEST_CASE("Modules.hostFunctionTemplate")
{
    // makeHostFunction<F> is instantiated here, in the importer, with a lambda of this TU.
    auto const doubling =
        makeHostFunction([](HostContext&,
                            std::span<Value const> args,
                            std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
            results[0] = I32 { std::get<I32>(args[0]).bits * 2 };
            return {};
        });

    auto const engine = Engine { EngineConfig {} };
    auto const module = engine.load(DoubleImportModule);
    REQUIRE(module.has_value());
    auto imports = Imports {};
    imports.define("env",
                   "double",
                   FuncType { .params = { ValueType { .kind = ValueKind::I32 } },
                              .results = { ValueType { .kind = ValueKind::I32 } } },
                   doubling);

    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const instance = store.instantiate(*module, imports);
    REQUIRE(instance.has_value());
    auto const imported = instance->function("double");
    REQUIRE(imported.has_value());

    auto const args = std::array<Value, 1> { I32 { 21 } };
    auto results = std::array<Value, 1> {};
    auto const called = instance->call(*imported, args, results);
    REQUIRE(called.has_value());
    CHECK(results[0] == Value { I32 { 42 } });
}

TEST_CASE("Modules.constantsEvaluate")
{
    static_assert(isoline::FeatureCount == isoline::FeatureTable.size());
    static_assert(isoline::version().major == isoline::config::versionMajor);
    STATIC_CHECK(isoline::FeatureSet::wasm3().has(isoline::Feature::MultiValue));
}

TEST_CASE("Modules.errorsAreNameable")
{
    auto const engine = Engine { EngineConfig {} };
    auto const truncated = std::span<std::byte const> { isoline::consumer::AddModule }.first(20);
    auto const module = engine.load(truncated);
    REQUIRE_FALSE(module.has_value());
    LoadError const& loadError = module.error();
    CHECK(std::holds_alternative<DecodeError>(loadError));
    CHECK_FALSE(isImplementationLimit(loadError));
    CHECK(std::visit(AlternativeOf {}, loadError) == Alternative::DecodeError);

    auto const instantiateError = InstantiateError { EngineMismatch {} };
    CHECK(std::visit(AlternativeOf {}, instantiateError) == Alternative::EngineMismatch);
    auto const defineError = DefineError { HostMemoryExhausted {} };
    CHECK(std::visit(AlternativeOf {}, defineError) == Alternative::HostMemoryExhausted);
    auto const callError =
        CallError { .detail = CallError::Detail { UncaughtException {} }, .refusedGrowth = {} };
    CHECK(std::visit(AlternativeOf {}, callError.detail) == Alternative::UncaughtException);

    // The members of the configuration, each by the type its declaration names.
    auto const config = EngineConfig {};
    FeatureSet const features = config.features;
    ExecutionEngine const executionEngine = config.engine;
    ExecutionMode const mode = config.mode;
    LoadLimits const loadLimits = config.load;
    DecodeLimits const decodeLimits = loadLimits.decode;
    std::uint64_t const maxModuleBytes = loadLimits.maxModuleBytes;
    StoreLimits const storeLimits = config.store;
    std::uint32_t const maxCallDepth = storeLimits.maxCallDepth;
    std::uint64_t const maxMemoryBytes = storeLimits.maxMemoryBytes;
    EngineIdentity const identity = config.identity();
    CHECK(identity == EngineIdentity { .engine = executionEngine, .mode = mode, .features = features });
    CHECK(maxCallDepth > 0);
    CHECK(maxMemoryBytes > 0);
    CHECK(loadLimits == LoadLimits { .maxModuleBytes = maxModuleBytes, .decode = decodeLimits });
}

TEST_CASE("Modules.defaultedEqualityThroughImport")
{
    auto const identity = EngineConfig {}.identity();
    CHECK(identity == EngineIdentity { identity });
    CHECK_FALSE(identity
                == EngineIdentity { .engine = identity.engine, .mode = identity.mode, .features = {} });

    CHECK(MemorySize { .pages = 1, .bytes = 65'536 } == MemorySize { .pages = 1, .bytes = 65'536 });
    CHECK_FALSE(MemorySize { .pages = 1, .bytes = 65'536 } == MemorySize { .pages = 2, .bytes = 131'072 });
}
