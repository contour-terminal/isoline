// SPDX-License-Identifier: Apache-2.0
//
// Contour's WebAssembly backend contract (vtextension/wasm/testing/BackendContract.hpp), one case per
// engine-level case, named after it and expressed against Isoline's public api. Contour-only rules
// stay in Contour's adapter and have no case here: the WASI shim, MaxTables, offering only all-i32
// exports, runtime generations and thread guards.
#include <core/Assert.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/Contract.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/runtime/testing/ProvidedMemory.hpp>

using isoline::api::Abrupt;
using isoline::api::CallError;
using isoline::api::CallFailure;
using isoline::api::DecodeError;
using isoline::api::Engine;
using isoline::api::EngineConfig;
using isoline::api::ExportAccessError;
using isoline::api::ExternKind;
using isoline::api::ForeignHandle;
using isoline::api::FuncType;
using isoline::api::GrowthRecord;
using isoline::api::HostContext;
using isoline::api::I32;
using isoline::api::Imports;
using isoline::api::Instance;
using isoline::api::InstantiateError;
using isoline::api::Limit;
using isoline::api::LimitExceeded;
using isoline::api::Linkage;
using isoline::api::LinkError;
using isoline::api::LinkErrorCode;
using isoline::api::makeHostFunction;
using isoline::api::MemoryAccessError;
using isoline::api::Module;
using isoline::api::Refusal;
using isoline::api::Store;
using isoline::api::StoreDeleted;
using isoline::api::Trap;
using isoline::api::TrapCode;
using isoline::api::Value;
using isoline::api::testing::contract::addModule;
using isoline::api::testing::contract::atLeast;
using isoline::api::testing::contract::bytesOf;
using isoline::api::testing::contract::callI32;
using isoline::api::testing::contract::configOf;
using isoline::api::testing::contract::ContractLimits;
using isoline::api::testing::contract::describedModule;
using isoline::api::testing::contract::embedderFailure;
using isoline::api::testing::contract::functionOf;
using isoline::api::testing::contract::GateOutcome;
using isoline::api::testing::contract::Handover;
using isoline::api::testing::contract::HandoverInterrupt;
using isoline::api::testing::contract::HostAnswer;
using isoline::api::testing::contract::HostCall;
using isoline::api::testing::contract::hostImports;
using isoline::api::testing::contract::HostLog;
using isoline::api::testing::contract::HostModule;
using isoline::api::testing::contract::i32Add;
using isoline::api::testing::contract::instanceModule;
using isoline::api::testing::contract::JoiningThread;
using isoline::api::testing::contract::memoryOf;
using isoline::api::testing::contract::onPreparedThread;
using isoline::api::testing::contract::PageBytes;
using isoline::api::testing::contract::Runtime;
using isoline::api::testing::contract::SpinBackstopSteps;
using isoline::api::testing::contract::StartGate;
using isoline::api::testing::contract::TypeI32;
using isoline::api::testing::contract::wordAt;
using isoline::api::testing::contract::Worker;
using isoline::binary::TableType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::call;
using isoline::binary::testing::code;
using isoline::binary::testing::drop;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::memoryGrow;
using isoline::binary::testing::ModuleBuilder;
using isoline::binary::testing::tableGrow;
using isoline::binary::testing::unreachable;
using isoline::runtime::testing::GrowDecision;
using isoline::runtime::testing::ProvidedMemoryAllocator;

namespace
{

/// The error an outcome that must have failed failed with.
template <typename T, typename E>
E errorOf(std::expected<T, E> const& outcome)
{
    REQUIRE_FALSE(outcome.has_value());
    return outcome.error();
}

/// The index of @p module's import @p moduleName : @p name, which must be there.
std::size_t importIndexOf(Module const& module, std::string_view moduleName, std::string_view name)
{
    auto const imports = module.imports();
    auto const entry = std::ranges::find_if(
        imports, [&](auto const& import) { return import.module == moduleName && import.name == name; });
    REQUIRE(entry != imports.end());
    return static_cast<std::size_t>(entry - imports.begin());
}

/// Whether @p imports satisfy @p module's import @p moduleName : @p name.
Linkage linkageOf(Imports const& imports,
                  Module const& module,
                  std::string_view moduleName,
                  std::string_view name)
{
    return imports.resolve(module).at(importIndexOf(module, moduleName, name));
}

/// The function type @p module's import @p moduleName : @p name requires, which must be a function.
FuncType importedSignature(Module const& module, std::string_view moduleName, std::string_view name)
{
    auto const& type = module.imports()[importIndexOf(module, moduleName, name)].type;
    REQUIRE(std::holds_alternative<isoline::binary::FuncType const*>(type));
    return *std::get<isoline::binary::FuncType const*>(type);
}

/// The type of @p module's export @p name, which must be there.
isoline::api::ExternType exportTypeOf(Module const& module, std::string_view name)
{
    auto const exports = module.exports();
    auto const entry = std::ranges::find(exports, name, &isoline::api::ExportDescription::name);
    REQUIRE(entry != exports.end());
    return entry->type;
}

/// A call of `ping` with @p args on this thread, as a HostLog records it.
HostCall pingOnThisThread(std::vector<uint32_t> args)
{
    return HostCall { .function = "ping",
                      .arguments = std::move(args),
                      .thread = std::this_thread::get_id() };
}

/// Imports what no runtime provides: (module (import "nowhere" "f" (func))).
ModuleBuilder unknownImportModule()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("nowhere", "f", 0);
    return builder;
}

/// Imports `ping` with one parameter too many: (module (import "contract:test/host@0" "ping" (func (param
/// i32 i32)))).
ModuleBuilder wrongSignatureModule()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32, TypeI32 }, .results = {} })
        .importFunction(HostModule, "ping", 0);
    return builder;
}

/// Passes its two parameters on to `pair`, in order.
///
///     (module
///       (import "contract:test/host@0" "pair" (func $pair (param i32 i32)))
///       (func (export "pair") (param i32 i32) (call $pair (local.get 0) (local.get 1))))
ModuleBuilder pairModule()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32, TypeI32 }, .results = {} })
        .importFunction(HostModule, "pair", 0)
        .function(0, {}, code({ localGet(0), localGet(1), call(0) }))
        .exportFunction("pair", 1);
    return builder;
}

/// What the nested-call cases run: `fail` calls `ping(1)`; `absorb` calls `ping(0)`, then returns 7;
/// `absorb-then-trap` calls `ping(0)`, then traps on its own; `grow-absorb-then-trap` grows its memory by
/// 4 pages, ignoring a refusal, then does what `absorb-then-trap` does.
///
///     (module
///       (import "contract:test/host@0" "ping" (func $ping (param i32)))
///       (memory (export "memory") 1)
///       (func (export "fail") (call $ping (i32.const 1)))
///       (func (export "absorb") (result i32) (call $ping (i32.const 0)) (i32.const 7))
///       (func (export "absorb-then-trap") (call $ping (i32.const 0)) unreachable)
///       (func (export "grow-absorb-then-trap")
///         (drop (memory.grow (i32.const 4))) (call $ping (i32.const 0)) unreachable))
ModuleBuilder nestedCallModule()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = {} })
        .type(FuncType {})
        .type(FuncType { .params = {}, .results = { TypeI32 } })
        .importFunction(HostModule, "ping", 0)
        .memory(atLeast(1))
        .function(1, {}, code({ i32Const(1), call(0) }))
        .function(2, {}, code({ i32Const(0), call(0), i32Const(7) }))
        .function(1, {}, code({ i32Const(0), call(0), unreachable() }))
        .function(1, {}, code({ i32Const(4), memoryGrow(), drop(), i32Const(0), call(0), unreachable() }))
        .exportEntity("memory", ExternKind::Memory, 0)
        .exportFunction("fail", 1)
        .exportFunction("absorb", 2)
        .exportFunction("absorb-then-trap", 3)
        .exportFunction("grow-absorb-then-trap", 4);
    return builder;
}

/// A table of 40 elements without a maximum, more than half the contract's table cap: an engine that
/// capped such a table at twice its initial size would let it grow past the cap. `grow` returns what
/// table.grow does: the old size, or -1.
///
///     (module
///       (table 40 funcref)
///       (func (export "grow") (param i32) (result i32) (table.grow 0 (ref.null func) (local.get 0)))
///       (func (export "size") (result i32) (table.size 0)))
ModuleBuilder tableModule()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = { TypeI32 } })
        .type(FuncType { .params = {}, .results = { TypeI32 } })
        .table(TableType { .element = isoline::binary::FuncRef, .limits = atLeast(40) })
        .function(0, {}, code({ { 0xD0, 0x70 }, localGet(0), tableGrow(0) })) // ref.null func
        .function(1, {}, Bytes { 0xFC, 0x10, 0x00 })                          // table.size 0
        .exportFunction("grow", 0)
        .exportFunction("size", 1);
    return builder;
}

/// A table one element larger than the contract's table cap: (module (table 65 funcref)).
ModuleBuilder oversizedTableModule()
{
    auto builder = ModuleBuilder {};
    builder.table(TableType { .element = isoline::binary::FuncRef,
                              .limits = atLeast(ContractLimits {}.tableElements + 1) });
    return builder;
}

/// A memory of one page that no export shows: (module (memory 1)).
ModuleBuilder unexportedMemoryModule()
{
    auto builder = ModuleBuilder {};
    builder.memory(atLeast(1));
    return builder;
}

/// One table of two elements without a maximum, then a byte the table section's size covers but no
/// table does: 04 05 | 01 | 70 00 02 | ff.
constexpr auto TrailingBytesTableModule = std::to_array<uint8_t>(
    { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, 0x04, 0x05, 0x01, 0x70, 0x00, 0x02, 0xFF });

/// One export of a module whose export section is written by hand.
struct RawExport
{
    std::string_view name; ///< Its name, shorter than 128 bytes.
    uint32_t function;     ///< The index of the function it exports.
};

/// The u32 LEB128 at @p position of @p bytes, which is moved past it.
uint32_t readU32(std::span<std::byte const> bytes, std::size_t& position)
{
    auto value = uint32_t { 0 };
    auto shift = 0U;
    auto byte = std::byte { 0x80 };
    while ((byte & std::byte { 0x80 }) != std::byte { 0 })
    {
        REQUIRE(position < bytes.size());
        byte = bytes[position++];
        value |= (std::to_integer<uint32_t>(byte) & 0x7FU) << shift;
        shift += 7;
    }
    return value;
}

/// @p module with its export section replaced by function exports @p exports, each name's length
/// encoded in five LEB128 bytes, as a linker that leaves room to patch it writes it.
std::vector<std::byte> withPaddedExportNames(std::vector<std::byte> const& module,
                                             std::span<RawExport const> exports)
{
    constexpr auto ExportSectionId = std::byte { 7 };
    constexpr auto PreambleBytes = std::size_t { 8 };
    auto payload = encodeU32(static_cast<uint32_t>(exports.size()));
    for (auto const& entry: exports)
    {
        auto const length = static_cast<uint8_t>(entry.name.size());
        payload.insert(payload.end(), { static_cast<uint8_t>(0x80U | length), 0x80, 0x80, 0x80, 0x00 });
        payload.insert(payload.end(), entry.name.begin(), entry.name.end());
        payload.push_back(0x00); // a function
        auto const index = encodeU32(entry.function);
        payload.insert(payload.end(), index.begin(), index.end());
    }
    auto out = std::vector<std::byte>(module.begin(), module.begin() + PreambleBytes);
    auto position = PreambleBytes;
    while (position < module.size())
    {
        auto const id = module[position++];
        auto const size = readU32(module, position);
        REQUIRE(size <= module.size() - position);
        out.push_back(id);
        if (id == ExportSectionId)
        {
            auto const sizeBytes = encodeU32(static_cast<uint32_t>(payload.size()));
            std::ranges::transform(
                sizeBytes, std::back_inserter(out), [](uint8_t byte) { return std::byte { byte }; });
            std::ranges::transform(
                payload, std::back_inserter(out), [](uint8_t byte) { return std::byte { byte }; });
        }
        else
        {
            auto const sizeBytes = encodeU32(size);
            std::ranges::transform(
                sizeBytes, std::back_inserter(out), [](uint8_t byte) { return std::byte { byte }; });
            auto const body = std::span { module }.subspan(position, size);
            out.insert(out.end(), body.begin(), body.end());
        }
        position += size;
    }
    return out;
}

/// The reactor's exports, by function index.
constexpr auto ReactorExports = std::to_array<RawExport>({
    { .name = "_initialize", .function = 0 },
    { .name = "initializations", .function = 1 },
    { .name = "started", .function = 2 },
    { .name = "load", .function = 3 },
});

/// A WASI reactor, as wasi-libc's crt1-reactor.o makes one: `_initialize` traps when run a second time,
/// `initializations` answers how often it ran, `load` traps unless it ran. Its start section counts too:
/// `started` answers how often the start function ran.
///
///     (module
///       (global $initializations (mut i32) (i32.const 0))
///       (global $started (mut i32) (i32.const 0))
///       (func (export "_initialize")
///         (if (global.get $initializations) (then unreachable))
///         (global.set $initializations (i32.add (global.get $initializations) (i32.const 1))))
///       (func (export "initializations") (result i32) (global.get $initializations))
///       (func (export "started") (result i32) (global.get $started))
///       (func (export "load") (param i32 i32) (result i32)
///         (if (i32.eqz (global.get $initializations)) (then unreachable))
///         (i32.const 0))
///       (func $start (global.set $started (i32.add (global.get $started) (i32.const 1))))
///       (start $start))
ModuleBuilder reactorModule()
{
    using isoline::binary::GlobalType;
    using isoline::binary::Mutability;
    auto const counter = GlobalType { .type = TypeI32, .mutability = Mutability::Var };
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .type(FuncType { .params = {}, .results = { TypeI32 } })
        .type(FuncType { .params = { TypeI32, TypeI32 }, .results = { TypeI32 } })
        .global(counter, i32Const(0))
        .global(counter, i32Const(0))
        .function(0,
                  {},
                  code({ { 0x23, 0x00, 0x04, 0x40 }, // global.get 0; if
                         unreachable(),
                         { 0x0B, 0x23, 0x00 }, // end; global.get 0
                         i32Const(1),
                         i32Add(),
                         { 0x24, 0x00 } })) // global.set 0
        .function(1, {}, Bytes { 0x23, 0x00 })
        .function(1, {}, Bytes { 0x23, 0x01 })
        .function(2,
                  {},
                  code({ { 0x23, 0x00, 0x45, 0x04, 0x40 }, // global.get 0; i32.eqz; if
                         unreachable(),
                         { 0x0B }, // end
                         i32Const(0) }))
        .function(0, {}, code({ { 0x23, 0x01 }, i32Const(1), i32Add(), { 0x24, 0x01 } }))
        .start(4);
    for (auto const& entry: ReactorExports)
        builder.exportFunction(std::string { entry.name }, entry.function);
    return builder;
}

/// A host for nestedCallModule(): `ping(0)` calls the instance's `fail` back, through Instance::call as
/// Contour does, and absorbs its failure, as a host function may when it can do without what it asked
/// the guest for; `ping` with any other argument fails.
class AbsorbingHost
{
  public:
    AbsorbingHost(): _state { std::make_shared<State>() } {}

    /// The definition of `ping`, bound to this host.
    [[nodiscard]] Imports imports() const
    {
        auto imports = Imports {};
        imports.define(
            std::string { HostModule },
            "ping",
            FuncType { .params = { TypeI32 }, .results = {} },
            makeHostFunction([state = _state](HostContext&,
                                              std::span<Value const> args,
                                              std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                if (std::get<I32>(args[0]).bits != 0)
                    return std::unexpected { Abrupt { embedderFailure() } };
                auto const fail = state->instance->function("fail");
                state->nested =
                    state->instance->call(*fail, {}, {}).transform_error([](CallError const& error) {
                        return error.failure();
                    });
                return {};
            }));
        return imports;
    }

    /// Names the instance `ping(0)` calls back into: an attach-style call after construction, as the
    /// instance is created with this host's imports.
    void attach(Instance const& instance) noexcept { _state->instance = instance; }

    /// What the latest call back into the instance returned; nothing before the first.
    [[nodiscard]] std::optional<std::expected<void, CallFailure>> const& nested() const noexcept
    {
        return _state->nested;
    }

  private:
    struct State
    {
        std::optional<Instance> instance;
        std::optional<std::expected<void, CallFailure>> nested;
    };

    std::shared_ptr<State> _state;
};

/// What AbsorbingHost records of a nested call that failed in the host function.
std::optional<std::expected<void, CallFailure>> nestedHostFailure()
{
    return std::expected<void, CallFailure> { std::unexpected { CallFailure::HostFailure } };
}

/// The trap a failed call ended with, which must be a trap.
TrapCode trapOf(CallError const& error)
{
    REQUIRE(std::holds_alternative<Trap>(error.detail));
    return std::get<Trap>(error.detail).code;
}

/// The contract's limits, with stores that may hold @p memoryBytes of memory.
ContractLimits withMemoryBytes(uint64_t memoryBytes)
{
    return ContractLimits { .memoryBytes = memoryBytes, .tableElements = ContractLimits {}.tableElements };
}

/// Which of two stores alive at once goes first.
enum class FirstToGo : uint8_t
{
    Older, ///< The one created first.
    Newer, ///< The one created second.
};

/// Two stores share one Imports, as Contour's runtimes share their host functions, both bind them, and
/// @p firstToGo goes: what the other instantiated keeps calling them, and it keeps linking them.
void survivorKeepsHostImports(FirstToGo firstToGo)
{
    auto const engine = Engine { configOf({}) };
    auto allocator = isoline::api::HeapMemoryAllocator {};
    auto const log = std::make_shared<HostLog>();
    auto const imports = hostImports(log);
    auto older = std::make_unique<Store>(engine, allocator);
    auto newer = std::make_unique<Store>(engine, allocator);
    auto& gone = firstToGo == FirstToGo::Older ? older : newer;
    auto const& survivor = firstToGo == FirstToGo::Older ? newer : older;

    auto const module = engine.load(instanceModule().build());
    REQUIRE(module.has_value());
    auto const instance = survivor->instantiate(*module, imports);
    REQUIRE(instance.has_value());
    CHECK(&functionOf(*instance, "add").store() == &survivor->id());
    REQUIRE(gone->instantiate(*module, imports).has_value());
    gone.reset();

    CHECK(callI32(*instance, "ping-then-store").has_value());
    CHECK(log->calls() == std::vector { pingOnThisThread({ 42 }) });
    auto const later = engine.load(instanceModule().build());
    REQUIRE(later.has_value());
    CHECK(imports.resolve(*later) == std::vector { Linkage::Satisfied });
    auto const laterInstance = survivor->instantiate(*later, imports);
    REQUIRE(laterInstance.has_value());
    CHECK(callI32(*laterInstance, "ping-then-add", std::array { 2U, 3U }) == std::vector { 5U });
}

} // namespace

TEST_CASE("Contract.loadRejectsNonModule", "[contract]")
{
    auto const engine = Engine { configOf({}) };
    auto const bytes = std::array { std::byte { 1 }, std::byte { 2 } };
    auto const loaded = engine.load(bytes);
    CHECK(std::holds_alternative<DecodeError>(errorOf(loaded)));
}

TEST_CASE("Contract.describesImportsAndExports", "[contract]")
{
    auto runtime = Runtime {};
    auto const module = runtime.load(describedModule());
    CHECK(module.imports().size() == 2);
    CHECK(importedSignature(module, HostModule, "ping") == FuncType { .params = { TypeI32 }, .results = {} });
    CHECK(linkageOf(runtime.imports(), module, HostModule, "ping") == Linkage::Satisfied);
    CHECK(importedSignature(module, HostModule, "pair")
          == FuncType { .params = { TypeI32, TypeI32 }, .results = {} });
    CHECK(linkageOf(runtime.imports(), module, HostModule, "pair") == Linkage::Satisfied);

    CHECK(module.exports().size() == 2);
    auto const memory = exportTypeOf(module, "memory");
    REQUIRE(std::holds_alternative<isoline::binary::MemoryType const*>(memory));
    CHECK(std::get<isoline::binary::MemoryType const*>(memory)->limits.min == 1);
    auto const add = exportTypeOf(module, "add");
    REQUIRE(std::holds_alternative<isoline::binary::FuncType const*>(add));
    CHECK(*std::get<isoline::binary::FuncType const*>(add)
          == FuncType { .params = { TypeI32, TypeI32 }, .results = { TypeI32 } });
}

TEST_CASE("Contract.unknownImportIsUnsatisfied", "[contract]")
{
    auto runtime = Runtime {};
    auto const module = runtime.load(unknownImportModule());
    CHECK(linkageOf(runtime.imports(), module, "nowhere", "f") == Linkage::Unsatisfied);
    // Instantiation agrees with resolve().
    auto const error = errorOf(runtime.store().instantiate(module, runtime.imports()));
    CHECK(error == InstantiateError { LinkError { LinkErrorCode::UnknownImport, 0 } });
}

TEST_CASE("Contract.wrongSignatureImportIsUnsatisfied", "[contract]")
{
    auto runtime = Runtime {};
    auto const module = runtime.load(wrongSignatureModule());
    CHECK(importedSignature(module, HostModule, "ping")
          == FuncType { .params = { TypeI32, TypeI32 }, .results = {} });
    CHECK(linkageOf(runtime.imports(), module, HostModule, "ping") == Linkage::Unsatisfied);
    // Refused at instantiation, rather than linked and trapping when called.
    auto const error = errorOf(runtime.store().instantiate(module, runtime.imports()));
    CHECK(error == InstantiateError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
}

TEST_CASE("Contract.moduleCrossesThreads", "[contract][threading]")
{
    auto runtime = Runtime {};
    auto const bytes = describedModule().build();
    auto const loaded = onPreparedThread(runtime.engine(), [&] { return runtime.engine().load(bytes); });
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    auto const& module = **loaded;
    CHECK(module.imports().size() == 2);
    CHECK(exportTypeOf(module, "add").index() == 0); // a function
    auto const instance = runtime.instantiate(module);
    CHECK(callI32(instance, "add", std::array { 2U, 3U }) == std::vector { 5U });
}

TEST_CASE("Contract.instantiateLeavesReactorToCaller", "[contract]")
{
    auto runtime = Runtime {};
    auto const builder = reactorModule();
    auto bytes = builder.build();
    SECTION("a one-byte name length")
    {
    }
    SECTION("a five-byte name length")
    {
        bytes = withPaddedExportNames(bytes, ReactorExports);
    }
    auto loaded = runtime.engine().load(bytes);
    REQUIRE(loaded.has_value());
    auto const& module = *loaded;
    // The module shows the export by its own name.
    CHECK(exportTypeOf(module, "_initialize").index() == 0);
    auto const instance = runtime.instantiate(module);
    // Instantiation ran the start section, and nothing by name.
    CHECK(callI32(instance, "started") == std::vector { 1U });
    CHECK(callI32(instance, "initializations") == std::vector { 0U });
    REQUIRE(callI32(instance, "_initialize").has_value());
    CHECK(callI32(instance, "initializations") == std::vector { 1U });
    CHECK(callI32(instance, "load", std::array { 0U, 0U }) == std::vector { 0U });
    CHECK(callI32(instance, "started") == std::vector { 1U });
}

TEST_CASE("Contract.callsExport", "[contract]")
{
    auto runtime = Runtime {};
    auto const instance = runtime.instantiate(runtime.load(instanceModule()));
    CHECK_FALSE(instance.function("nothing").has_value());
    CHECK_FALSE(instance.function("memory").has_value());
    CHECK(callI32(instance, "add", std::array { 2U, 3U }) == std::vector { 5U });
    CHECK(runtime.log().calls().empty());
}

TEST_CASE("Contract.memoryViewIsBoundsChecked", "[contract]")
{
    auto runtime = Runtime {};
    auto const instance = runtime.instantiate(runtime.load(instanceModule()));
    auto const memory = memoryOf(instance);
    auto const whole = instance.memoryRange(memory, 0, PageBytes);
    REQUIRE(whole.has_value());
    CHECK(whole->size() == PageBytes);
    auto const hello = instance.memoryRange(memory, 16, 5);
    REQUIRE(hello.has_value());
    CHECK(std::ranges::equal(*hello, std::string_view { "hello" }, [](std::byte byte, char letter) {
        return std::to_integer<char>(byte) == letter;
    }));
    CHECK(errorOf(instance.memoryRange(memory, PageBytes - 1, 2)) == MemoryAccessError::OutOfBounds);
    CHECK(errorOf(instance.memoryRange(memory, 0xFFFF'FFF0, 32)) == MemoryAccessError::OutOfBounds);
    CHECK(errorOf(instance.memoryRange(memory, std::numeric_limits<uint64_t>::max(), 2))
          == MemoryAccessError::OutOfBounds);
    auto const empty = instance.memoryRange(memory, 70'000, 0);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("Contract.hostCallbackReceivesArguments", "[contract]")
{
    auto runtime = Runtime {};
    auto const instance = runtime.instantiate(runtime.load(instanceModule()));
    CHECK(callI32(instance, "ping-then-store").has_value());
    CHECK(runtime.log().calls() == std::vector { pingOnThisThread({ 42 }) });
    CHECK(wordAt(instance, 0) == 1);
}

TEST_CASE("Contract.hostCallbackReceivesArgumentsInOrder", "[contract]")
{
    auto runtime = Runtime {};
    auto const module = runtime.load(pairModule());
    CHECK(linkageOf(runtime.imports(), module, HostModule, "pair") == Linkage::Satisfied);
    auto const instance = runtime.instantiate(module);
    CHECK(callI32(instance, "pair", std::array { 7U, 9U }).has_value());
    CHECK(runtime.log().calls()
          == std::vector {
              HostCall { .function = "pair", .arguments = { 7, 9 }, .thread = std::this_thread::get_id() } });
}

TEST_CASE("Contract.hostCallbackErrorFails", "[contract]")
{
    auto runtime = Runtime { configOf({}), nullptr, HostAnswer::Fail };
    auto const instance = runtime.instantiate(runtime.load(instanceModule()));
    auto const error = errorOf(callI32(instance, "ping-then-store"));
    CHECK(error.failure() == CallFailure::HostFailure);
    CHECK(error == CallError { .detail = embedderFailure(), .refusedGrowth = {} });
    CHECK(runtime.log().calls().size() == 1);
    // Nothing the guest would have done after the host call happened.
    CHECK(wordAt(instance, 0) == 0);
}

TEST_CASE("Contract.nestedHostFailureStaysNested", "[contract]")
{
    auto runtime = Runtime {};
    auto host = AbsorbingHost {};
    auto const instance = runtime.store().instantiate(runtime.load(nestedCallModule()), host.imports());
    REQUIRE(instance.has_value());
    host.attach(*instance);

    auto const trapped = errorOf(callI32(*instance, "absorb-then-trap"));
    CHECK(host.nested() == nestedHostFailure());
    CHECK(trapped.failure() == CallFailure::Trap);
    CHECK(trapOf(trapped) == TrapCode::Unreachable);

    CHECK(callI32(*instance, "absorb") == std::vector { 7U });
    CHECK(host.nested() == nestedHostFailure());
}

TEST_CASE("Contract.nestedCallKeepsTheOuterRecord", "[contract]")
{
    auto runtime = Runtime { configOf(withMemoryBytes(2 * PageBytes)) };
    auto host = AbsorbingHost {};
    auto const instance = runtime.store().instantiate(runtime.load(nestedCallModule()), host.imports());
    REQUIRE(instance.has_value());
    host.attach(*instance);
    // A refused growth, then an absorbed nested failure, then the guest's own trap: the call fails as
    // the trap, with its own refused growth, which Isoline always reports.
    auto const error = errorOf(callI32(*instance, "grow-absorb-then-trap"));
    CHECK(host.nested() == nestedHostFailure());
    CHECK(error.failure() == CallFailure::Trap);
    CHECK(error.refusedGrowth == GrowthRecord { .memory = Refusal::Limit, .table = {} });
}

TEST_CASE("Contract.twoRuntimesShareHostImports", "[contract]")
{
    survivorKeepsHostImports(FirstToGo::Older);
}

TEST_CASE("Contract.olderRuntimeOutlivesNewer", "[contract]")
{
    survivorKeepsHostImports(FirstToGo::Newer);
}

TEST_CASE("Contract.memoryCapRefusesGrowth", "[contract]")
{
    SECTION("the store's cap")
    {
        auto runtime = Runtime { configOf(withMemoryBytes(2 * PageBytes)) };
        auto const instance = runtime.instantiate(runtime.load(instanceModule()));
        auto const error = errorOf(callI32(instance, "grow"));
        CHECK(error.failure() == CallFailure::Trap);
        CHECK(error.refusedGrowth == GrowthRecord { .memory = Refusal::Limit, .table = {} });
        CHECK(errorOf(instance.memoryRange(memoryOf(instance), 0, 2 * PageBytes))
              == MemoryAccessError::OutOfBounds);
    }
    SECTION("the embedder's own allocator, as Contour provides its mapping")
    {
        auto runtime =
            Runtime { configOf({}), std::make_unique<ProvidedMemoryAllocator>(2 * PageBytes, 0, [](uint64_t) {
                          return GrowDecision::Grant;
                      }) };
        auto const instance = runtime.instantiate(runtime.load(instanceModule()));
        auto const error = errorOf(callI32(instance, "grow"));
        CHECK(error.failure() == CallFailure::Trap);
        CHECK(error.refusedGrowth == GrowthRecord { .memory = Refusal::Embedder, .table = {} });
        CHECK(errorOf(instance.memoryRange(memoryOf(instance), 0, 2 * PageBytes))
              == MemoryAccessError::OutOfBounds);
    }
}

TEST_CASE("Contract.grownMemoryIsVisible", "[contract]")
{
    constexpr auto Address = uint32_t { 65'540 };
    constexpr auto Word = uint32_t { 0xC0DE };
    auto allocator = std::unique_ptr<isoline::api::IMemoryAllocator> {};
    SECTION("over the heap")
    {
    }
    SECTION("over the embedder's own allocator")
    {
        allocator = std::make_unique<ProvidedMemoryAllocator>(
            2 * PageBytes, 0, [](uint64_t) { return GrowDecision::Grant; });
    }
    auto runtime = Runtime { configOf({}), std::move(allocator) };
    auto const instance = runtime.instantiate(runtime.load(instanceModule()));
    CHECK(errorOf(instance.memoryRange(memoryOf(instance), Address, 4)) == MemoryAccessError::OutOfBounds);
    CHECK(callI32(instance, "grow-by", std::array { 1U }) == std::vector { 1U });
    REQUIRE(callI32(instance, "store", std::array { Address, Word }).has_value());
    CHECK(wordAt(instance, Address) == Word);
}

TEST_CASE("Contract.callAfterTrapRuns", "[contract]")
{
    auto runtime = Runtime { configOf(withMemoryBytes(2 * PageBytes)) };
    auto const instance = runtime.instantiate(runtime.load(instanceModule()));
    CHECK(errorOf(callI32(instance, "grow")).failure() == CallFailure::Trap);
    CHECK(callI32(instance, "add", std::array { 2U, 3U }) == std::vector { 5U });
}

TEST_CASE("Contract.memoryCapRefusesInitialSize", "[contract]")
{
    auto runtime = Runtime { configOf(withMemoryBytes(PageBytes / 2)) };
    auto const error =
        errorOf(runtime.store().instantiate(runtime.load(instanceModule()), runtime.imports()));
    CHECK(error == InstantiateError { LimitExceeded { Limit::MemoryBytes } });
}

TEST_CASE("Contract.memoryCapRefusesUnexportedInitialSize", "[contract]")
{
    auto runtime = Runtime { configOf(withMemoryBytes(PageBytes / 2)) };
    auto const error =
        errorOf(runtime.store().instantiate(runtime.load(unexportedMemoryModule()), runtime.imports()));
    CHECK(error == InstantiateError { LimitExceeded { Limit::MemoryBytes } });
}

TEST_CASE("Contract.tableGrowthIsCapped", "[contract][memory]")
{
    constexpr auto Refused = std::numeric_limits<uint32_t>::max();
    auto const cap = static_cast<uint32_t>(ContractLimits {}.tableElements);
    auto runtime = Runtime {};
    auto const instance = runtime.instantiate(runtime.load(tableModule()));
    CHECK(callI32(instance, "grow", std::array { cap - 40 }) == std::vector { 40U });
    CHECK(callI32(instance, "grow", std::array { 1U }) == std::vector { Refused });
    CHECK(callI32(instance, "grow", std::array { Refused }) == std::vector { Refused });
    CHECK(callI32(instance, "size") == std::vector { cap });
}

TEST_CASE("Contract.tableInitialSizeIsCapped", "[contract][memory]")
{
    auto runtime = Runtime {};
    auto const error =
        errorOf(runtime.store().instantiate(runtime.load(oversizedTableModule()), runtime.imports()));
    CHECK(error == InstantiateError { LimitExceeded { Limit::TableElements } });
}

TEST_CASE("Contract.tableSectionWithTrailingBytesIsRefused", "[contract]")
{
    auto const engine = Engine { configOf({}) };
    auto const loaded = engine.load(bytesOf(TrailingBytesTableModule));
    CHECK(std::holds_alternative<DecodeError>(errorOf(loaded)));
}

TEST_CASE("Contract.unboundedRecursionTraps", "[contract]")
{
    auto runtime = Runtime {};
    auto const instance = runtime.instantiate(runtime.load(instanceModule()));
    auto const error = errorOf(callI32(instance, "recurse"));
    CHECK(error.failure() == CallFailure::Trap);
    CHECK(trapOf(error) == TrapCode::CallStackExhausted);
}

TEST_CASE("Contract.interruptStopsLoop", "[contract][threading]")
{
    using namespace std::chrono_literals;
    SECTION("at a fixed back-edge")
    {
        // Contour's case interrupts 50 ms in; here the spinning thread hands over at a fixed back-edge
        // and waits until another thread interrupted it, so the case does not depend on scheduling.
        auto handover = HandoverInterrupt {};
        auto runtime = Runtime { configOf({}, SpinBackstopSteps), nullptr, HostAnswer::Return, &handover };
        auto const instance = runtime.instantiate(runtime.load(instanceModule()));
        auto const started = std::chrono::steady_clock::now();
        auto handedOver = Handover::TimedOut;
        auto watchdog =
            JoiningThread { [&handover, &handedOver, interrupter = runtime.store().interrupter()] {
                handedOver = handover.interruptAtHandover(*interrupter);
            } };
        auto const called = callI32(instance, "spin");
        auto const elapsed = std::chrono::steady_clock::now() - started;
        handover.finish();
        watchdog.join();
        CHECK(handedOver == Handover::Interrupted);
        CHECK(errorOf(called).failure() == CallFailure::Interrupted);
        CHECK(elapsed < 5s);
    }
    SECTION("free-running")
    {
        // No handover: the interrupt races the spinning call's polls, as it does in production, so that
        // ThreadSanitizer sees the poll's read against the other thread's write. Interrupted whichever
        // comes first: an interrupt before the call is sticky. The backstop is four times the handover's,
        // room for the other thread to be scheduled late.
        auto runtime = Runtime { configOf({}, SpinBackstopSteps * 4) };
        auto const instance = runtime.instantiate(runtime.load(instanceModule()));
        auto watchdog =
            JoiningThread { [interrupter = runtime.store().interrupter()] { interrupter->interrupt(); } };
        auto const called = callI32(instance, "spin");
        watchdog.join();
        CHECK(errorOf(called).failure() == CallFailure::Interrupted);
    }
}

TEST_CASE("Contract.interruptIsFinal", "[contract]")
{
    auto runtime = Runtime {};
    auto const instance = runtime.instantiate(runtime.load(instanceModule()));
    runtime.store().interrupter()->interrupt();
    for (auto const attempt: std::views::iota(0, 2))
    {
        INFO("attempt " << attempt);
        CHECK(errorOf(callI32(instance, "ping-then-add", std::array { 2U, 3U })).failure()
              == CallFailure::Interrupted);
    }
    CHECK(runtime.log().calls().empty());
}

TEST_CASE("Contract.instanceCrossesThreads", "[contract][threading]")
{
    auto runtime = Runtime {};
    auto const bytes = describedModule().build();
    auto const loaded = onPreparedThread(runtime.engine(), [&] { return runtime.engine().load(bytes); });
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    auto const created = onPreparedThread(
        runtime.engine(), [&] { return runtime.store().instantiate(**loaded, runtime.imports()); });
    REQUIRE(created.has_value());
    REQUIRE(created->has_value());
    auto const instance = **created;
    auto const add = functionOf(instance, "add");
    auto const sum =
        onPreparedThread(runtime.engine(), [&] { return callI32(instance, add, std::array { 2U, 3U }); });
    REQUIRE(sum.has_value());
    CHECK(*sum == std::vector { 5U });
}

TEST_CASE("Contract.instantiatesConcurrently", "[contract][threading]")
{
    // One module is instantiated on two threads at once, each into a store of its own, sharing the
    // module and the host functions' Imports: every instance is whole and runs. So too for a module
    // whose import is unsatisfied: each thread meets the outcome one instantiation alone has.
    constexpr auto Rounds = 25U;
    /// What one thread achieved: instantiations that went as the reference did, and instances that added
    /// right.
    struct Tally
    {
        GateOutcome gate = GateOutcome::TimedOut;
        uint32_t asReference = 0;
        uint32_t added = 0;
    };
    auto const engine = Engine { configOf({}) };
    auto const log = std::make_shared<HostLog>();
    auto const imports = hostImports(log);
    auto const onTwoThreads = [&](Module const& module) {
        auto referenceAllocator = isoline::api::HeapMemoryAllocator {};
        auto referenceStore = Store { engine, referenceAllocator };
        auto const reference = referenceStore.instantiate(module, imports);
        auto gate = StartGate { 2 };
        auto const instantiateMany = [&] {
            auto tally = Tally {};
            auto const preparation = engine.prepareThread();
            auto allocator = isoline::api::HeapMemoryAllocator {};
            auto store = Store { engine, allocator };
            tally.gate = gate.arriveAndWait();
            if (!preparation.has_value())
                return tally;
            for (auto const round: std::views::iota(0U, Rounds))
            {
                auto const instance = store.instantiate(module, imports);
                if (instance.has_value() != reference.has_value()
                    || (!instance && instance.error() != reference.error()))
                    continue;
                ++tally.asReference;
                if (!instance)
                    continue;
                auto const add = instance->function("add");
                auto const args = std::array<Value, 2> { I32 { round }, I32 { 3 } };
                auto results = std::array<Value, 1> {};
                if (add && instance->call(*add, args, results) && results[0] == Value { I32 { round + 3 } })
                    ++tally.added;
            }
            return tally;
        };
        auto first = Worker { instantiateMany };
        auto second = Worker { instantiateMany };
        return std::array { first.result("an instantiating thread"),
                            second.result("an instantiating thread") };
    };

    auto const module = engine.load(instanceModule().build());
    REQUIRE(module.has_value());
    for (auto const& tally: onTwoThreads(*module))
    {
        CHECK(tally.gate == GateOutcome::Opened);
        CHECK(tally.asReference == Rounds);
        CHECK(tally.added == Rounds);
    }
    auto const unsatisfied = engine.load(unknownImportModule().build());
    REQUIRE(unsatisfied.has_value());
    for (auto const& tally: onTwoThreads(*unsatisfied))
    {
        CHECK(tally.gate == GateOutcome::Opened);
        CHECK(tally.asReference == Rounds);
    }
}

TEST_CASE("Contract.threadPreparationNests", "[contract][threading]")
{
    // Tokens nest: two of one engine and one of a second on one thread; dropping the inner one of the
    // first leaves the thread prepared for both, and guest calls of both engines' stores still run.
    auto first = Runtime {};
    auto second = Runtime {};
    auto const firstInstance = first.instantiate(first.load(addModule()));
    auto const secondInstance = second.instantiate(second.load(addModule()));
    /// What the thread saw.
    struct Report
    {
        bool tokensGranted = false;
        std::expected<std::vector<uint32_t>, CallError> firstAfterInner = std::vector<uint32_t> {};
        std::expected<std::vector<uint32_t>, CallError> secondAfterInner = std::vector<uint32_t> {};
        std::expected<std::vector<uint32_t>, CallError> firstAfterSecond = std::vector<uint32_t> {};
    };
    auto const firstAdd = functionOf(firstInstance, "add");
    auto const secondAdd = functionOf(secondInstance, "add");
    auto worker = Worker { [&] {
        auto report = Report {};
        auto const outer = first.engine().prepareThread();
        auto inner = std::optional { first.engine().prepareThread() };
        auto other = std::optional { second.engine().prepareThread() };
        report.tokensGranted = outer.has_value() && inner->has_value() && other->has_value();
        inner.reset();
        report.firstAfterInner = callI32(firstInstance, firstAdd, std::array { 2U, 3U });
        report.secondAfterInner = callI32(secondInstance, secondAdd, std::array { 4U, 5U });
        other.reset();
        report.firstAfterSecond = callI32(firstInstance, firstAdd, std::array { 6U, 7U });
        return report;
    } };
    auto const report = worker.result("a thread nesting preparations");
    CHECK(report.tokensGranted);
    CHECK(report.firstAfterInner == std::vector { 5U });
    CHECK(report.secondAfterInner == std::vector { 9U });
    CHECK(report.firstAfterSecond == std::vector { 13U });
}

TEST_CASE("Contract.tableCountIsCapped", "[contract]")
{
    // Contour's MaxTables, expressed as Isoline's decode limit: a module defining more tables than the
    // engine's LoadLimits allow is not loaded, and the refusal is an implementation limit.
    auto config = configOf({});
    config.load.decode.maxTables = 1;
    auto const engine = Engine { config };
    auto builder = ModuleBuilder {};
    builder.table(TableType { .element = isoline::binary::FuncRef, .limits = atLeast(1) })
        .table(TableType { .element = isoline::binary::FuncRef, .limits = atLeast(1) });
    auto const loaded = engine.load(builder.build());
    CHECK(isoline::api::isImplementationLimit(errorOf(loaded)));
    builder = ModuleBuilder {};
    builder.table(TableType { .element = isoline::binary::FuncRef, .limits = atLeast(1) });
    CHECK(engine.load(builder.build()).has_value());
}

namespace
{

/// Whether Contour lets extensions use a feature (its EngineFeatures table).
enum class ExtensionUse : uint8_t
{
    Refused,  ///< Off: every engine refuses a module using it.
    Accepted, ///< On: every engine accepts it.
};

/// Whether Isoline implements a feature.
enum class Implementation : uint8_t
{
    Missing,     ///< Isoline decodes none of it: refused whatever the engine's features.
    Implemented, ///< A FeatureSet switch decides.
};

/// One feature of Contour's table, the module that uses it and nothing else, and how Isoline holds it.
struct FeatureCase
{
    std::string_view name;         ///< The feature, as Contour's table names it.
    ExtensionUse use;              ///< Whether extensions may use it.
    Implementation implementation; ///< Whether Isoline implements it.
    /// Isoline's switch for it; none for an MVP-era feature or a missing one.
    std::optional<isoline::Feature> feature;
    std::vector<std::byte> (*module)(); ///< A module using it.
    /// The feature a refusal names while it is off, when not feature itself: the first one off that the
    /// module reaches, a prerequisite read before the feature's own use.
    std::optional<isoline::Feature> named = std::nullopt;
};

/// A module of one function of @p type with @p body.
std::vector<std::byte> functionModule(FuncType type,
                                      Bytes body,
                                      std::vector<isoline::binary::ValueType> locals = {})
{
    auto builder = ModuleBuilder {};
    builder.type(std::move(type)).function(0, std::move(locals), std::move(body));
    return builder.build();
}

/// A module of only @p sections after the preamble, written by hand: what ModuleBuilder cannot express.
std::vector<std::byte> rawModule(std::initializer_list<uint8_t> sections)
{
    auto bytes = std::vector<uint8_t> { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
    bytes.insert(bytes.end(), sections);
    return bytesOf(bytes);
}

/// The FeatureSet Contour's EngineFeatures maps to, the one P9's adapter configures Isoline with: its On
/// rows, and nothing else (Annotations and compact imports are not in Contour's table, so off).
isoline::FeatureSet extensionFeatures()
{
    using isoline::Feature;
    return isoline::FeatureSet::none()
        .with(Feature::SignExtension)
        .with(Feature::NonTrappingFloatToInt)
        .with(Feature::MultiValue)
        .with(Feature::ReferenceTypes)
        .with(Feature::BulkMemory)
        .with(Feature::ExtendedConst);
}

using isoline::Feature;
using isoline::binary::ValueType;
constexpr auto TypeI64 = isoline::binary::I64;
constexpr auto TypeV128 = isoline::binary::V128;

/// Every row of Contour's EngineFeatures a core module can be refused for, with the module Contour's
/// contract uses for it, rebuilt from its WebAssembly text. The rows without one concern components
/// (component-model*), a behaviour (relaxed-simd-deterministic) or a custom section an engine ignores
/// (branch-hinting).
std::vector<FeatureCase> const& featureCases()
{
    static auto const cases = std::vector<FeatureCase> {
        // (module (memory 1) (func (memory.fill (i32.const 0) (i32.const 0) (i32.const 0))))
        { "bulk-memory",
          ExtensionUse::Accepted,
          Implementation::Implemented,
          Feature::BulkMemory,
          [] {
              auto builder = ModuleBuilder {};
              builder.type(FuncType {})
                  .memory(atLeast(1))
                  .function(
                      0,
                      {},
                      code(
                          { i32Const(0), i32Const(0), i32Const(0), isoline::binary::testing::memoryFill() }));
              return builder.build();
          } },
        // (module (func (result i32) (i32.extend8_s (i32.const 1))))
        { "sign-extension",
          ExtensionUse::Accepted,
          Implementation::Implemented,
          Feature::SignExtension,
          [] {
              return functionModule(FuncType { .params = {}, .results = { TypeI32 } },
                                    code({ i32Const(1), { 0xC0 } }));
          } },
        // (module (func (result i32) (i32.trunc_sat_f32_s (f32.const 1))))
        { "saturating-float-to-int",
          ExtensionUse::Accepted,
          Implementation::Implemented,
          Feature::NonTrappingFloatToInt,
          [] {
              return functionModule(FuncType { .params = {}, .results = { TypeI32 } },
                                    Bytes { 0x43, 0x00, 0x00, 0x80, 0x3F, 0xFC, 0x00 });
          } },
        // (module (global (export "g") (mut i32) (i32.const 0))): part of every standard Isoline knows.
        { "mutable-globals",
          ExtensionUse::Accepted,
          Implementation::Implemented,
          std::nullopt,
          [] {
              auto builder = ModuleBuilder {};
              builder
                  .global(isoline::binary::GlobalType { .type = TypeI32,
                                                        .mutability = isoline::binary::Mutability::Var },
                          i32Const(0))
                  .exportEntity("g", ExternKind::Global, 0);
              return builder.build();
          } },
        // (module (func (result i32 i32) (i32.const 1) (i32.const 2)))
        { "multi-value",
          ExtensionUse::Accepted,
          Implementation::Implemented,
          Feature::MultiValue,
          [] {
              return functionModule(FuncType { .params = {}, .results = { TypeI32, TypeI32 } },
                                    code({ i32Const(1), i32Const(2) }));
          } },
        // (module (func (result funcref) (ref.null func)))
        { "reference-types",
          ExtensionUse::Accepted,
          Implementation::Implemented,
          Feature::ReferenceTypes,
          [] {
              return functionModule(FuncType { .params = {}, .results = { isoline::binary::FuncRef } },
                                    Bytes { 0xD0, 0x70 });
          } },
        // (module (global i32 (i32.add (i32.const 1) (i32.const 2))))
        { "extended-const",
          ExtensionUse::Accepted,
          Implementation::Implemented,
          Feature::ExtendedConst,
          [] {
              auto builder = ModuleBuilder {};
              builder.global(isoline::binary::GlobalType { .type = TypeI32 },
                             code({ i32Const(1), i32Const(2), i32Add() }));
              return builder.build();
          } },
        // (module (type (struct)))
        { "gc",
          ExtensionUse::Refused,
          Implementation::Missing,
          std::nullopt,
          [] { return rawModule({ 0x01, 0x03, 0x01, 0x5F, 0x00 }); } },
        // (module (tag))
        { "exceptions",
          ExtensionUse::Refused,
          Implementation::Implemented,
          Feature::ExceptionHandling,
          [] {
              auto builder = ModuleBuilder {};
              builder.type(FuncType {}).tag(0);
              return builder.build();
          } },
        // (module (memory 1 1 shared))
        { "threads",
          ExtensionUse::Refused,
          Implementation::Missing,
          std::nullopt,
          [] { return rawModule({ 0x05, 0x04, 0x01, 0x03, 0x01, 0x01 }); } },
        // (module (memory i64 1))
        { "memory64",
          ExtensionUse::Refused,
          Implementation::Implemented,
          Feature::Memory64,
          [] {
              auto builder = ModuleBuilder {};
              builder.memory(isoline::binary::Limits {
                  .min = 1, .max = std::nullopt, .addressType = isoline::binary::AddressType::I64 });
              return builder.build();
          } },
        // (module (memory 1) (memory 1))
        { "multi-memory",
          ExtensionUse::Refused,
          Implementation::Implemented,
          Feature::MultiMemory,
          [] {
              auto builder = ModuleBuilder {};
              builder.memory(atLeast(1)).memory(atLeast(1));
              return builder.build();
          } },
        // (module (func (return_call 0)))
        { "tail-call",
          ExtensionUse::Refused,
          Implementation::Implemented,
          Feature::TailCall,
          [] { return functionModule(FuncType {}, Bytes { 0x12, 0x00 }); } },
        // (module (func (drop (v128.const i64x2 0 0))))
        { "simd",
          ExtensionUse::Refused,
          Implementation::Implemented,
          Feature::Simd,
          [] {
              auto body = Bytes { 0xFD, 0x0C };
              body.resize(body.size() + 16);
              body.push_back(0x1A); // drop
              return functionModule(FuncType {}, body);
          } },
        // (module (func (param v128) (result v128) (i32x4.relaxed_trunc_f32x4_s (local.get 0))))
        { "relaxed-simd",
          ExtensionUse::Refused,
          Implementation::Implemented,
          Feature::RelaxedSimd,
          [] {
              return functionModule(FuncType { .params = { TypeV128 }, .results = { TypeV128 } },
                                    code({ localGet(0), { 0xFD, 0x81, 0x02 } })); // 0x101
          },
          // Its v128 param is read before the relaxed instruction, and simd is off too.
          Feature::Simd },
        // (module (type $t (func)) (func (param (ref null $t))))
        { "function-references",
          ExtensionUse::Refused,
          Implementation::Implemented,
          Feature::TypedFunctionReferences,
          [] {
              auto builder = ModuleBuilder {};
              builder.type(FuncType {})
                  .type(FuncType {
                      .params = { ValueType {
                          .kind = isoline::binary::ValueKind::Ref,
                          .nullable = isoline::binary::Nullable::Yes,
                          .heap = { .kind = isoline::binary::AbstractHeapType::Concrete, .typeIndex = 0 } } },
                      .results = {} })
                  .function(1, {}, {});
              return builder.build();
          } },
        // (module (func (param i64 i64 i64 i64) (result i64 i64) (i64.add128 (local.get 0) ... (local.get
        // 3))))
        { "wide-arithmetic",
          ExtensionUse::Refused,
          Implementation::Implemented,
          Feature::WideArithmetic,
          [] {
              return functionModule(
                  FuncType { .params = { TypeI64, TypeI64, TypeI64, TypeI64 },
                             .results = { TypeI64, TypeI64 } },
                  code({ localGet(0), localGet(1), localGet(2), localGet(3), { 0xFC, 0x13 } }));
          } },
        // (module (memory 1 (pagesize 1)))
        { "custom-page-sizes",
          ExtensionUse::Refused,
          Implementation::Implemented,
          Feature::CustomPageSizes,
          [] {
              auto builder = ModuleBuilder {};
              builder.memory(isoline::binary::MemoryType { .limits = atLeast(1), .pageSizeLog2 = 0 });
              return builder.build();
          } },
        // (module (type $f (func)) (type (cont $f)))
        { "stack-switching",
          ExtensionUse::Refused,
          Implementation::Missing,
          std::nullopt,
          [] { return rawModule({ 0x01, 0x06, 0x02, 0x60, 0x00, 0x00, 0x5D, 0x00 }); } },
    };
    return cases;
}

/// Loads each feature case whose use is @p use with the extension features, each in a section named
/// after the feature: accepted when extensions may use it, refused otherwise. A refused feature Isoline
/// implements is refused naming it (requiredFeature()), or the row's named prerequisite that is off too
/// (relaxed-simd's module takes a v128 before it reaches a relaxed instruction), and loads once the
/// engine enables it, so the module is valid otherwise; one Isoline lacks is refused whatever the
/// features.
void checkFeatureCases(ExtensionUse use)
{
    auto const extensions = Engine { EngineConfig { .features = extensionFeatures() } };
    auto const everything = Engine { EngineConfig { .features = isoline::FeatureSet::all() } };
    for (auto const& row: featureCases())
    {
        if (row.use != use)
            continue;
        DYNAMIC_SECTION(row.name)
        {
            auto const bytes = row.module();
            auto const loaded = extensions.load(bytes);
            if (use == ExtensionUse::Accepted)
            {
                CHECK(loaded.has_value());
                continue;
            }
            auto const error = errorOf(loaded);
            CHECK_FALSE(isoline::api::isImplementationLimit(error));
            if (row.implementation == Implementation::Missing)
            {
                CHECK_FALSE(everything.load(bytes).has_value());
                continue;
            }
            REQUIRE(row.feature.has_value());
            auto const named = isoline::api::requiredFeature(error);
            REQUIRE(named.has_value());
            CHECK(*named == row.named.value_or(*row.feature));
            // Refused because it is off: the engine does not enable what the refusal names.
            CHECK_FALSE(extensionFeatures().has(*named));
            CHECK(everything.load(bytes).has_value());
        }
    }
}

} // namespace

TEST_CASE("Contract.refusesDisabledFeatures", "[contract]")
{
    checkFeatureCases(ExtensionUse::Refused);
}

TEST_CASE("Contract.acceptsEnabledFeatures", "[contract]")
{
    CHECK(extensionFeatures().checkPrerequisites().has_value());
    checkFeatureCases(ExtensionUse::Accepted);
}

namespace
{

/// Counts the contract checks (Require(), Guarantee()) that fail while it lives, through
/// core::setFailHandler(): a case asserts that none did. A failed check still ends the process once the
/// handler returned, so the count is also what the report says before it does.
class FailedChecks
{
  public:
    FailedChecks()
    {
        core::setFailHandler(
            [this](std::string_view text, std::string_view message, std::string_view file, int line) {
                ++_count;
                std::cerr << "contract check failed: [" << file << ':' << line << "] " << message << ' '
                          << text << '\n';
            });
    }

    FailedChecks(FailedChecks const&) = delete;
    FailedChecks(FailedChecks&&) = delete;
    FailedChecks& operator=(FailedChecks const&) = delete;
    FailedChecks& operator=(FailedChecks&&) = delete;

    /// Restores the default handler.
    ~FailedChecks() { core::setFailHandler({}); }

    /// The checks that failed so far.
    [[nodiscard]] std::size_t count() const noexcept { return _count; }

  private:
    std::size_t _count = 0; ///< The checks that failed so far.
};

/// An operation given a handle of another store than the one asked (Contract.foreignHandlesAreValues).
enum class ForeignUse : uint8_t
{
    InstanceCall,        ///< Instance::call() of the other store's function.
    InstanceMemoryRange, ///< Instance::memoryRange() of the other store's memory.
    InstanceMemorySize,  ///< Instance::memorySize() of the other store's memory.
    StoreCall,           ///< Store::call() of the other store's function.
};

/// An operation of an instance whose store is gone (Contract.storeDeletedHandlesAreValues).
enum class DeletedUse : uint8_t
{
    Call,        ///< Instance::call().
    MemoryRange, ///< Instance::memoryRange().
    MemorySize,  ///< Instance::memorySize().
    ExportAt,    ///< Instance::exportAt().
    Function,    ///< Instance::function(name).
};

} // namespace

TEST_CASE("Contract.foreignHandlesAreValues", "[contract]")
{
    // A handle of another store is refused with ForeignHandle wherever it is given, reading nothing of
    // either store: never a contract check that ends the process.
    auto const use = GENERATE(ForeignUse::InstanceCall,
                              ForeignUse::InstanceMemoryRange,
                              ForeignUse::InstanceMemorySize,
                              ForeignUse::StoreCall);
    CAPTURE(use);
    auto const checks = FailedChecks {};
    auto runtimeA = Runtime {};
    auto runtimeB = Runtime {};
    auto const instanceA = runtimeA.instantiate(runtimeA.load(instanceModule()));
    auto const instanceB = runtimeB.instantiate(runtimeB.load(instanceModule()));
    auto const function = functionOf(instanceB, "add");
    auto const memory = memoryOf(instanceB);
    auto const args = std::array<Value, 2> { I32 { 2 }, I32 { 3 } };
    auto results = std::array<Value, 1> {};
    auto const foreignCall = CallError { .detail = ForeignHandle {}, .refusedGrowth = {} };
    switch (use)
    {
        case ForeignUse::InstanceCall:
            CHECK(errorOf(instanceA.call(function, args, results)) == foreignCall);
            CHECK(errorOf(instanceA.call(function, args, results)).failure() == CallFailure::ForeignHandle);
            break;
        case ForeignUse::InstanceMemoryRange:
            CHECK(errorOf(instanceA.memoryRange(memory, 0, 1)) == MemoryAccessError::ForeignHandle);
            break;
        case ForeignUse::InstanceMemorySize:
            CHECK(errorOf(instanceA.memorySize(memory)) == MemoryAccessError::ForeignHandle);
            break;
        case ForeignUse::StoreCall:
            CHECK(errorOf(runtimeA.store().call(function, args, results)) == foreignCall);
            break;
    }
    CHECK(checks.count() == 0);
    CHECK(results[0] == Value {});
    // Both stores still work.
    CHECK(callI32(instanceA, "add", std::array { 2U, 3U }) == std::vector { 5U });
    CHECK(callI32(instanceB, "add", std::array { 2U, 3U }) == std::vector { 5U });
}

TEST_CASE("Contract.storeDeletedHandlesAreValues", "[contract]")
{
    // An instance whose store is gone reports it (StoreDeleted), or finds nothing, reading nothing of
    // the store: never a contract check that ends the process.
    auto const use = GENERATE(DeletedUse::Call,
                              DeletedUse::MemoryRange,
                              DeletedUse::MemorySize,
                              DeletedUse::ExportAt,
                              DeletedUse::Function);
    CAPTURE(use);
    auto const checks = FailedChecks {};
    auto runtime = std::make_unique<Runtime>();
    auto const instance = runtime->instantiate(runtime->load(instanceModule()));
    auto const function = functionOf(instance, "add");
    auto const memory = memoryOf(instance);
    runtime.reset();
    auto const args = std::array<Value, 2> { I32 { 2 }, I32 { 3 } };
    auto results = std::array<Value, 1> {};
    switch (use)
    {
        case DeletedUse::Call:
            CHECK(errorOf(instance.call(function, args, results))
                  == CallError { .detail = StoreDeleted {}, .refusedGrowth = {} });
            break;
        case DeletedUse::MemoryRange:
            CHECK(errorOf(instance.memoryRange(memory, 0, 1)) == MemoryAccessError::StoreDeleted);
            break;
        case DeletedUse::MemorySize:
            CHECK(errorOf(instance.memorySize(memory)) == MemoryAccessError::StoreDeleted);
            break;
        case DeletedUse::ExportAt:
            CHECK(errorOf(instance.exportAt(0)) == ExportAccessError::StoreDeleted);
            break;
        case DeletedUse::Function: CHECK_FALSE(instance.function("add").has_value()); break;
    }
    CHECK(checks.count() == 0);
    CHECK(results[0] == Value {});
}
