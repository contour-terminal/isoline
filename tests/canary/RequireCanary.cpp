// SPDX-License-Identifier: Apache-2.0
//
// The api preconditions, each checked as a process of its own: a check that fails ends the process, so no
// Catch2 case can observe it in-process. The case to run is the first argument, one row of CanaryCases
// each. CTest judges a case by the check's own text on stderr (tests/CMakeLists.txt), which only that
// check prints; the line main() prints when the check let the call through fails it. Linked with
// ThrowingHandler.cpp, a contract's violation handler throws: main() catches what reaches it, which only
// a contract in a function that may throw lets through.
#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include "CheckPositions.hpp"
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/EngineRows.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/platform/InstructionCache.hpp>
#include <isoline/platform/JitMemory.hpp>
#include <isoline/platform/Membarrier.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/PlatformTable.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <isoline/platform/VirtualMemory.hpp>
#include <isoline/platform/testing/JitWriteScopeKeys.hpp>
#include <isoline/platform/testing/RecordingVirtualMemory.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/MemoryLayout.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/testing/ProvidedMemory.hpp>

namespace
{

/// Ends the process normally on SIGABRT: CTest fails a test that dies of a signal whatever its output,
/// so the abort a failed check ends with becomes an exit, and the output decides. std::_Exit() flushes no
/// stream: what a case prints before its check went out unbuffered, or was flushed.
extern "C" void exitOnAbort(int /*signal*/)
{
    std::_Exit(EXIT_FAILURE);
}

using namespace isoline::api;
using isoline::api::testing::engineRows;
using isoline::api::testing::contract::configOf;
using isoline::api::testing::contract::ContractLimits;
using isoline::platform::CodeMemoryKind;
using isoline::platform::DecommitKind;
using isoline::platform::IVirtualMemory;
using isoline::platform::JitMapping;
using isoline::platform::OperatingSystem;
using isoline::platform::Protection;
using isoline::platform::SystemJitMemory;
using isoline::platform::SystemVirtualMemory;
using isoline::platform::ThisPlatform;
using isoline::platform::VirtualMemoryError;
using isoline::platform::VirtualMemoryFailure;
using isoline::platform::testing::CallRefusal;
using isoline::platform::testing::RecordingVirtualMemory;

/// Relaxed SIMD without SIMD: a feature set not closed over its prerequisites, which Engine's
/// constructor refuses (Engine.cpp).
void engineOfOpenFeatureSet()
{
    auto const open = FeatureSet::wasm2().without(Feature::Simd).with(Feature::RelaxedSimd);
    [[maybe_unused]] auto const engine = Engine { EngineConfig { .features = open } };
}

/// The handles of a store that is gone: one of every kind, taken from an instance of a module that
/// exports one of each, before its store was destroyed.
struct DeletedStoreHandles
{
    Instance instance; ///< The instance.
    Function function; ///< Its function "f".
    Memory memory;     ///< Its memory "m".
    Global global;     ///< Its global "g".
    Table table;       ///< Its table "t".
    Tag tag;           ///< Its tag "e".
};

/// Ends the case as a failure when what @p step sets up did not happen: a case that cannot reach its
/// check prints no check text, so CTest fails it, and dereferences nothing it did not get.
/// @param done Whether the step succeeded.
/// @param step What it set up.
void setUp(bool done, std::string_view step)
{
    if (done)
        return;
    std::println("require-canary: set-up failed: {}", step);
    std::fflush(stdout);
    std::_Exit(EXIT_FAILURE);
}

/// One handle of every kind, of a store destroyed before this returns.
/// @return The handles.
[[nodiscard]] DeletedStoreHandles deletedStoreHandles()
{
    using namespace isoline::binary::testing;
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .table(TableType {
            .element = isoline::binary::FuncRef,
            .limits = { .min = 1, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 } })
        .memory(Limits { .min = 1, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 })
        .tag(0)
        .global(GlobalType { .type = isoline::binary::I32, .mutability = isoline::binary::Mutability::Var },
                i32Const(5))
        .function(0, {}, {})
        .exportFunction("f", 0)
        .exportEntity("m", ExternKind::Memory, 0)
        .exportEntity("g", ExternKind::Global, 0)
        .exportEntity("t", ExternKind::Table, 0)
        .exportEntity("e", ExternKind::Tag, 0);
    auto const engine = Engine { configOf(ContractLimits {}, engineRows().front()) };
    auto const module = engine.load(builder.build());
    setUp(module.has_value(), "the module loads");
    auto allocator = HeapMemoryAllocator {};
    auto store = std::make_unique<Store>(engine, allocator);
    auto const instance = store->instantiate(*module, Imports {});
    setUp(instance.has_value(), "the module instantiates");
    auto const function = instance->function("f");
    auto const memory = instance->memory("m");
    auto const global = instance->global("g");
    auto const table = instance->table("t");
    auto const tag = instance->tag("e");
    setUp(function && memory && global && table && tag, "the instance exports one entity of each kind");
    auto handles = DeletedStoreHandles { .instance = *instance,
                                         .function = *function,
                                         .memory = *memory,
                                         .global = *global,
                                         .table = *table,
                                         .tag = *tag };
    store.reset();
    return handles;
}

/// Reports that a call on a handle of a deleted store went through.
/// @param what What was called.
/// @param serial What the call read, so that it is not optimized away.
void wentThrough(std::string_view what, uint64_t serial)
{
    std::println("{} of a deleted store returned ({})", what, serial);
}

void functionStoreAfterDelete()
{
    wentThrough("Function::store()", deletedStoreHandles().function.store().serial());
}

void functionTypeAfterDelete()
{
    wentThrough("Function::type()", deletedStoreHandles().function.type().params.size());
}

void memoryStoreAfterDelete()
{
    wentThrough("Memory::store()", deletedStoreHandles().memory.store().serial());
}

void globalStoreAfterDelete()
{
    wentThrough("Global::store()", deletedStoreHandles().global.store().serial());
}

void tableStoreAfterDelete()
{
    wentThrough("Table::store()", deletedStoreHandles().table.store().serial());
}

void tagStoreAfterDelete()
{
    wentThrough("Tag::store()", deletedStoreHandles().tag.store().serial());
}

void tagTypeAfterDelete()
{
    wentThrough("Tag::type()", deletedStoreHandles().tag.type().params.size());
}

void importsInstanceAfterDelete()
{
    auto imports = Imports {};
    imports.define("m", "f", deletedStoreHandles().instance, "f");
    wentThrough("Imports::define(instance)", 0);
}

void importsFunctionAfterDelete()
{
    auto imports = Imports {};
    imports.define("m", "f", deletedStoreHandles().function);
    wentThrough("Imports::define(function)", 0);
}

void importsMemoryAfterDelete()
{
    auto imports = Imports {};
    imports.define("m", "m", deletedStoreHandles().memory);
    wentThrough("Imports::define(memory)", 0);
}

/// A host function that does nothing, which the host-function type cases define.
[[nodiscard]] std::shared_ptr<IHostFunction> doingNothing()
{
    return makeHostFunction([](HostContext&, std::span<Value const>, std::span<Value>) noexcept {
        return std::expected<void, Abrupt> {};
    });
}

/// A nullable reference to a heap of @p kind, of type index 0 where it names one.
[[nodiscard]] ValueType referenceTo(isoline::binary::AbstractHeapType kind)
{
    return ValueType { .kind = ValueKind::Ref,
                       .nullable = isoline::binary::Nullable::Yes,
                       .heap = { .kind = kind, .typeIndex = 0 } };
}

void importsHostTypeConcrete()
{
    auto imports = Imports {};
    auto const concrete = referenceTo(isoline::binary::AbstractHeapType::Concrete);
    imports.define("m", "f", FuncType { .params = { concrete }, .results = {} }, doingNothing());
    wentThrough("Imports::define() of a concrete type index", 0);
}

void importsHostTypeBottom()
{
    auto imports = Imports {};
    auto const bottom = referenceTo(isoline::binary::AbstractHeapType::Bot);
    imports.define("m", "f", FuncType { .params = {}, .results = { bottom } }, doingNothing());
    wentThrough("Imports::define() of the bot heap", 0);
}

/// The handles a store made without an instance (Store::defineMemory(), defineTable(), defineGlobal()),
/// of a store that is gone.
struct DeletedHostHandles
{
    Memory memory; ///< A memory of one page.
    Table table;   ///< A funcref table of one element.
    Global global; ///< A mutable i32 global.
};

/// One host-made handle of every kind defineMemory(), defineTable() and defineGlobal() make, of a store
/// destroyed before this returns.
/// @return The handles.
[[nodiscard]] DeletedHostHandles deletedHostHandles()
{
    auto const engine = Engine { configOf(ContractLimits {}, engineRows().front()) };
    auto allocator = HeapMemoryAllocator {};
    auto store = std::make_unique<Store>(engine, allocator);
    auto const unbounded =
        Limits { .min = 1, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 };
    auto const memory = store->defineMemory(MemoryType { .limits = unbounded });
    auto const table =
        store->defineTable(TableType { .element = isoline::binary::FuncRef, .limits = unbounded },
                           Ref { .kind = RefKind::Null, .heap = {}, .address = 0 });
    auto const global = store->defineGlobal(
        GlobalType { .type = isoline::binary::I32, .mutability = isoline::binary::Mutability::Var },
        I32 { 1 });
    setUp(memory && table && global, "the store defines one entity of each kind");
    auto handles = DeletedHostHandles { .memory = *memory, .table = *table, .global = *global };
    store.reset();
    return handles;
}

void hostMemoryStoreAfterDelete()
{
    wentThrough("a defined Memory::store()", deletedHostHandles().memory.store().serial());
}

void hostTableStoreAfterDelete()
{
    wentThrough("a defined Table::store()", deletedHostHandles().table.store().serial());
}

void hostGlobalStoreAfterDelete()
{
    wentThrough("a defined Global::store()", deletedHostHandles().global.store().serial());
}

/// binary::normalize() of a hand-built module whose custom section's anchor is past LastSectionAnchor
/// (binary/Encoder.cpp): no decoder or text reader makes one, and the section table is not read past its end.
void normalizeAnchorOutOfRange()
{
    auto module = isoline::binary::Module {};
    module.customs.push_back(
        isoline::binary::CustomSection { .name = "c",
                                         .bytes = {},
                                         .after = static_cast<isoline::binary::SectionAnchor>(
                                             std::to_underlying(isoline::binary::LastSectionAnchor) + 1) });
    auto const normalized = isoline::binary::normalize(std::move(module));
    std::println("binary::normalize() of an anchor past the last returned ({} customs)",
                 normalized.customs.size());
}

/// Store::define() of no host function (api/Store.cpp).
void storeDefineNullFunction()
{
    auto const engine = Engine { configOf(ContractLimits {}, engineRows().front()) };
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const defined = store.define(FuncType {}, std::shared_ptr<IHostFunction> {});
    std::println("Store::define() of no host function returned ({})", defined.has_value());
}

/// A thread preparation token of no resource (api/Engine.cpp).
void threadPreparationOfNothing()
{
    auto const token = ThreadPreparation { std::unique_ptr<IThreadResource> {} };
    std::println("ThreadPreparation of no resource was made (empty: {})", token.empty());
}

/// The check positions (CheckPositions.hpp), each given one round, so that its check fails.
void positionPreconditionFirst()
{
    isoline::canary::preconditionFirst(1);
    wentThrough("ISOLINE_PRECONDITION as a function's first statement", 0);
}

void positionPreconditionInBracelessIf()
{
    isoline::canary::preconditionInBracelessIf(1);
    wentThrough("ISOLINE_PRECONDITION as a braceless if's body", 0);
}

void positionInvariantInBracelessElse()
{
    isoline::canary::invariantInBracelessElse(1);
    wentThrough("ISOLINE_INVARIANT as a braceless else's body", 0);
}

void positionInvariantInForBody()
{
    isoline::canary::invariantInForBody(1);
    wentThrough("ISOLINE_INVARIANT first in a for body", 0);
}

void positionInvariantInWhileBody()
{
    isoline::canary::invariantInWhileBody(1);
    wentThrough("ISOLINE_INVARIANT first in a while body", 0);
}

void positionInvariantInDoBody()
{
    isoline::canary::invariantInDoBody(1);
    wentThrough("ISOLINE_INVARIANT first in a do body", 0);
}

void positionInvariantInRangeForBody()
{
    isoline::canary::invariantInRangeForBody(1);
    wentThrough("ISOLINE_INVARIANT first in a range-based for body", 0);
}

void positionInvariantInNoexceptLambdaInTry()
{
    isoline::canary::invariantInNoexceptLambdaInTry(1);
    wentThrough("ISOLINE_INVARIANT in a noexcept lambda inside a try block", 0);
}

/// How a virtual-memory call a precondition should have stopped ended.
enum class CallResult : uint8_t
{
    Failed,    ///< It returned a failure.
    Succeeded, ///< It returned success.
};

/// The result of @p outcome, as the canary reports it.
/// @param outcome Whether the call returned a value.
/// @return Its CallResult.
template <typename Expected>
[[nodiscard]] CallResult resultOf(Expected const& outcome)
{
    return outcome.has_value() ? CallResult::Succeeded : CallResult::Failed;
}

/// Reports a virtual-memory call a precondition should have stopped.
/// @param what The call.
/// @param result How it ended.
void virtualMemoryWentThrough(std::string_view what, CallResult result)
{
    std::println("{} returned ({})", what, result == CallResult::Succeeded ? "success" : "a failure");
}

/// One allocation granule of @p memory, reserved: the set-up of the protect() and release() cases.
/// @param memory The memory.
/// @return The reservation.
[[nodiscard]] isoline::platform::Region reservedGranule(SystemVirtualMemory& memory)
{
    auto const region = memory.reserve(memory.allocationGranularity());
    setUp(region.has_value(), "reserve one allocation granule");
    return *region;
}

void virtualMemoryReserveNothing()
{
    auto memory = SystemVirtualMemory {};
    virtualMemoryWentThrough("reserve(0)", resultOf(memory.reserve(0)));
}

void virtualMemoryReservePartGranule()
{
    auto memory = SystemVirtualMemory {};
    virtualMemoryWentThrough("reserve(granularity + 1)",
                             resultOf(memory.reserve(memory.allocationGranularity() + 1)));
}

void virtualMemoryProtectNothing()
{
    auto memory = SystemVirtualMemory {};
    auto const empty = isoline::platform::Region { .base = reservedGranule(memory).base, .bytes = 0 };
    virtualMemoryWentThrough("protect() of no bytes", resultOf(memory.protect(empty, Protection::ReadWrite)));
}

void virtualMemoryProtectPartPage()
{
    auto memory = SystemVirtualMemory {};
    auto const part =
        isoline::platform::Region { .base = reservedGranule(memory).base, .bytes = memory.pageSize() / 2 };
    virtualMemoryWentThrough("protect() of half a page",
                             resultOf(memory.protect(part, Protection::ReadWrite)));
}

void virtualMemoryReleaseUnaligned()
{
    auto memory = SystemVirtualMemory {};
    auto const shifted =
        isoline::platform::Region { .base = reservedGranule(memory).base + 1, .bytes = memory.pageSize() };
    virtualMemoryWentThrough("release() of an unaligned region", resultOf(memory.release(shifted)));
}

void virtualMemoryReleasePart()
{
    // The system checks a whole reservation where it can tell (Windows); the recording double does on
    // every platform, so the case runs through whichever checks it here.
    auto system = SystemVirtualMemory {};
    auto recording = RecordingVirtualMemory { system, std::nullopt };
    auto& memory = ThisPlatform.system == OperatingSystem::Windows ? static_cast<IVirtualMemory&>(system)
                                                                   : static_cast<IVirtualMemory&>(recording);
    auto const reservation = memory.reserve(memory.allocationGranularity() * 2);
    setUp(reservation.has_value(), "reserve two allocation granules");
    auto const half =
        isoline::platform::Region { .base = reservation->base, .bytes = memory.allocationGranularity() };
    virtualMemoryWentThrough("release() of half a reservation", resultOf(memory.release(half)));
}

void virtualMemoryRefusedProtectNothing()
{
    // A refused call still checks its preconditions: a test double never accepts what the system would not.
    auto system = SystemVirtualMemory {};
    auto const refusal = VirtualMemoryFailure { .error = VirtualMemoryError::ProtectRefused };
    auto memory = RecordingVirtualMemory { system, CallRefusal { .call = 1, .failure = refusal } };
    auto const empty = isoline::platform::Region { .base = reservedGranule(system).base, .bytes = 0 };
    virtualMemoryWentThrough("a refused protect() of no bytes",
                             resultOf(memory.protect(empty, Protection::ReadWrite)));
}

void virtualMemoryForeignDecommit()
{
    // A decommit mechanism of another system's rows.
    // Windows' on POSIX, a POSIX one on Windows: never one that decommits here.
    auto const foreign = ThisPlatform.system == OperatingSystem::Windows ? DecommitKind::MapFixedAnonymous
                                                                         : DecommitKind::VirtualFreeDecommit;
    auto memory = SystemVirtualMemory { foreign };
    virtualMemoryWentThrough("a memory of a foreign decommit row",
                             resultOf(memory.reserve(memory.allocationGranularity())));
}

void protectionRowOutOfRange()
{
    auto const& row = isoline::platform::protectionRow(static_cast<Protection>(200));
    virtualMemoryWentThrough("protectionRow(200)",
                             row.read == isoline::platform::PageAccess::Allowed ? CallResult::Succeeded
                                                                                : CallResult::Failed);
}

void platformRowOutOfRange()
{
    auto const& row = isoline::platform::platformRow(OperatingSystem::Linux,
                                                     static_cast<isoline::platform::Architecture>(200));
    virtualMemoryWentThrough("platformRow(Linux, 200)",
                             row.codeMemory == isoline::platform::CodeMemoryKind::Unavailable
                                 ? CallResult::Failed
                                 : CallResult::Succeeded);
}

void jitMappingWritableExecutableWithoutToggle()
{
    // The system's mapJit() wherever the row has no per-thread toggle; on the row that has one (macOS
    // arm64), the same check of macOS x86-64's kind, which it would refuse.
    if (!isoline::platform::togglesJitPerThread(ThisPlatform.codeMemory))
    {
        auto memory = SystemJitMemory {};
        virtualMemoryWentThrough(
            "mapJit(ReadWriteExecute) without the toggle",
            resultOf(memory.mapJit(SystemVirtualMemory {}.pageSize(), JitMapping::ReadWriteExecute)));
        return;
    }
    isoline::platform::detail::requireJitMapping(CodeMemoryKind::MapJitWithProtection,
                                                 JitMapping::ReadWriteExecute);
    virtualMemoryWentThrough("requireJitMapping(MapJitWithProtection, ReadWriteExecute)",
                             CallResult::Succeeded);
}

void jitAccessWithoutToggle()
{
    if (!isoline::platform::togglesJitPerThread(ThisPlatform.codeMemory))
    {
        auto memory = SystemJitMemory {};
        memory.setThreadAccess(isoline::platform::JitAccess::Write);
        virtualMemoryWentThrough("setThreadAccess(Write) without the toggle", CallResult::Succeeded);
        return;
    }
    isoline::platform::detail::requireJitThreadToggle(CodeMemoryKind::MapJitWithProtection);
    virtualMemoryWentThrough("requireJitThreadToggle(MapJitWithProtection)", CallResult::Succeeded);
}

void jitReleaseWithoutMapJit()
{
    // The system's release() wherever there is no MAP_JIT, of a region it cannot have mapped; on the
    // macOS rows, the same check of a row without MAP_JIT.
    if (!isoline::platform::mapsJit(ThisPlatform.codeMemory))
    {
        auto virtualMemory = SystemVirtualMemory {};
        auto memory = SystemJitMemory {};
        virtualMemoryWentThrough("release() without MAP_JIT",
                                 resultOf(memory.release(reservedGranule(virtualMemory))));
        return;
    }
    isoline::platform::detail::requireJitMapping(CodeMemoryKind::ReserveWriteThenExecute,
                                                 JitMapping::ReadWrite);
    virtualMemoryWentThrough("requireJitMapping(ReserveWriteThenExecute, ReadWrite)", CallResult::Succeeded);
}

/// JIT memory that maps nothing and toggles no thread: what the scope case needs, without MAP_JIT.
class InertJitMemory final: public isoline::platform::IJitMemory
{
  public:
    [[nodiscard]] std::expected<isoline::platform::Region, VirtualMemoryFailure> mapJit(
        uint64_t /*bytes*/, JitMapping /*mapping*/) noexcept override
    {
        return std::unexpected { VirtualMemoryFailure { .error = VirtualMemoryError::ReserveRefused } };
    }

    void setThreadAccess(isoline::platform::JitAccess /*access*/) noexcept override {}

    [[nodiscard]] std::expected<void, VirtualMemoryFailure> release(
        isoline::platform::Region /*region*/) noexcept override
    {
        return std::unexpected { VirtualMemoryFailure { .error = VirtualMemoryError::ReleaseRefused } };
    }
};

void jitScopeEndedOnAnotherThread()
{
    // Opened here, ended on another thread, as a coroutine resumed elsewhere would: it could restore only
    // that thread's view, and leave this one writable.
    auto memory = InertJitMemory {};
    auto scope = std::make_unique<isoline::platform::JitWriteScope>(
        memory, isoline::platform::testing::JitWriteScopeKeys::key());
    auto ender = std::thread { [&scope] { scope.reset(); } };
    ender.join();
    virtualMemoryWentThrough("a JitWriteScope ended on another thread", CallResult::Succeeded);
}

void codeMemoryRowOutOfRange()
{
    auto const& row = isoline::platform::codeMemoryRow(static_cast<CodeMemoryKind>(200));
    virtualMemoryWentThrough("codeMemoryRow(200)",
                             row.jit == isoline::platform::JitMappingUse::None ? CallResult::Failed
                                                                               : CallResult::Succeeded);
}

void jitMappingOutOfRange()
{
    auto const allowed = isoline::platform::allowsJitMapping(CodeMemoryKind::MapJitWithThreadToggle,
                                                             static_cast<JitMapping>(200));
    virtualMemoryWentThrough("allowsJitMapping(MapJitWithThreadToggle, 200)",
                             allowed ? CallResult::Succeeded : CallResult::Failed);
}

void instructionCacheRowOutOfRange()
{
    auto const& row =
        isoline::platform::instructionCacheRow(static_cast<isoline::platform::InstructionCacheKind>(200));
    virtualMemoryWentThrough("instructionCacheRow(200)",
                             row.invalidation == isoline::platform::InstructionCacheInvalidation::None
                                 ? CallResult::Failed
                                 : CallResult::Succeeded);
}

void instructionCacheWithoutMaintenance()
{
    auto const membarrier = isoline::platform::makeSystemMembarrier();
    virtualMemoryWentThrough("makeInstructionCache() without maintenance",
                             resultOf(isoline::platform::makeInstructionCache(
                                 isoline::platform::InstructionCacheKind::Coherent, nullptr, *membarrier)));
}

/// Reports a memory-layout call a precondition should have stopped.
/// @param what The call.
void memoryLayoutWentThrough(std::string_view what)
{
    std::println("{} returned", what);
}

/// A memory of i32 addresses and 64 KiB pages, of one page.
[[nodiscard]] isoline::binary::MemoryType onePageMemory32()
{
    return isoline::binary::MemoryType {
        .limits = { .min = 1, .max = 1, .addressType = isoline::binary::AddressType::I32 }
    };
}

void memoryLayoutGuardAboveMaximum()
{
    auto const layout = isoline::runtime::MemoryLayout { .reservationBytes = uint64_t { 32 } << 20,
                                                         .guardBytes = (uint64_t { 2 } << 30) + (64 << 10) };
    [[maybe_unused]] auto const strategy = isoline::runtime::boundsStrategyFor(
        layout,
        onePageMemory32(),
        { .offset = 0, .size = 4, .kind = isoline::runtime::AccessKind::Load },
        isoline::runtime::StoreFaultAtomicity::AllOrNothing);
    memoryLayoutWentThrough("boundsStrategyFor() under a guard above 2 GiB");
}

void memoryRequirementsReachOverflows()
{
    auto const layout =
        isoline::runtime::MemoryLayout { .reservationBytes = ~uint64_t { 0 }, .guardBytes = 1 };
    [[maybe_unused]] auto const requirements =
        isoline::runtime::memoryRequirementsFor(layout, onePageMemory32());
    memoryLayoutWentThrough("memoryRequirementsFor() of a reach past 2^64");
}

void layoutOfPageSizeNotAPower()
{
    [[maybe_unused]] auto const layout =
        isoline::runtime::layoutOf(isoline::runtime::LayoutRow::Capped, 0, 3'000);
    memoryLayoutWentThrough("layoutOf() of a 3000-byte page");
}

void layoutOfCapPastTheReach()
{
    [[maybe_unused]] auto const layout =
        isoline::runtime::layoutOf(isoline::runtime::LayoutRow::Capped, ~uint64_t { 0 }, 4'096);
    memoryLayoutWentThrough("layoutOf() of a cap of 2^64 - 1");
}

void layoutOfRowOutOfRange()
{
    [[maybe_unused]] auto const layout =
        isoline::runtime::layoutOf(static_cast<isoline::runtime::LayoutRow>(200), 0, 4'096);
    memoryLayoutWentThrough("layoutOf(200)");
}

void storeFaultAtomicityOutOfRange()
{
    [[maybe_unused]] auto const atomicity =
        isoline::runtime::storeFaultAtomicityOf(static_cast<isoline::platform::Architecture>(200));
    memoryLayoutWentThrough("storeFaultAtomicityOf(200)");
}

/// boundsStrategyFor() of a load of @p size bytes into a one-page memory32 under Contour's layout.
/// @param size The access's size, which its precondition refuses.
void strategyOfAccessSize(uint8_t size)
{
    auto const layout = isoline::runtime::MemoryLayout { .reservationBytes = uint64_t { 32 } << 20,
                                                         .guardBytes = uint64_t { 32 } << 20 };
    [[maybe_unused]] auto const strategy = isoline::runtime::boundsStrategyFor(
        layout,
        onePageMemory32(),
        { .offset = 0, .size = size, .kind = isoline::runtime::AccessKind::Load },
        isoline::runtime::StoreFaultAtomicity::AllOrNothing);
    memoryLayoutWentThrough("boundsStrategyFor() of an access no instruction makes");
}

void memoryAccessSizeNotAPower()
{
    strategyOfAccessSize(3);
}

void memoryAccessTooWide()
{
    strategyOfAccessSize(32);
}

void storeGrowBelowTheGuard()
{
    // Contour's layout needs 32 MiB of guard behind a memory32. A provided mapping of 64 MiB has it at one
    // page, but a grow to 48 MiB leaves only 16 MiB behind the bytes.
    auto const layout = isoline::runtime::MemoryLayout { .reservationBytes = uint64_t { 32 } << 20,
                                                         .guardBytes = uint64_t { 32 } << 20 };
    auto allocator = isoline::runtime::testing::ProvidedMemoryAllocator {
        uint64_t { 64 } << 20,
        isoline::runtime::testing::ProvidedReach { uint64_t { 64 } << 20 },
        [](uint64_t /*newBytes*/) { return isoline::runtime::testing::GrowDecision::Grant; }
    };
    auto store = isoline::runtime::Store { isoline::runtime::StoreLimits {},
                                           isoline::runtime::StoreServices { .allocator = allocator,
                                                                             .guardPageLayout = layout } };
    auto const memory = store.addMemory(isoline::binary::MemoryType {
        .limits = { .min = 1, .max = 1'024, .addressType = isoline::binary::AddressType::I32 } });
    setUp(memory.has_value(), "add a memory of 64 MiB reach");
    [[maybe_unused]] auto const grown = store.growMemory(*memory, 767);
    memoryLayoutWentThrough("growMemory() to 48 MiB of a 64 MiB reach");
}

void storeOfIllFormedLayout()
{
    auto allocator = isoline::runtime::HeapMemoryAllocator {};
    [[maybe_unused]] auto const store = isoline::runtime::Store {
        isoline::runtime::StoreLimits {},
        isoline::runtime::StoreServices {
            .allocator = allocator,
            .guardPageLayout = isoline::runtime::MemoryLayout { .reservationBytes = 0,
                                                                .guardBytes = (uint64_t { 2 } << 30) + 1 } }
    };
    memoryLayoutWentThrough("Store() under a guard above 2 GiB");
}

/// A case: its name on the command line, and what it calls.
struct CanaryCase
{
    std::string_view name; ///< The first argument that runs it.
    void (*run)();         ///< Calls what must fail its check.
};

/// Every case, from RequireCanaryCases.def, which tests/CMakeLists.txt reads for the tests.
constexpr auto CanaryCases = std::to_array<CanaryCase>({
#define ISOLINE_REQUIRE_CANARY_CASE(function, name, spelling, check) { name, function },
#include "RequireCanaryCases.def"
#undef ISOLINE_REQUIRE_CANARY_CASE
});

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGABRT, exitOnAbort);
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    auto const name = arguments.size() > 1 ? std::string_view { arguments[1] } : std::string_view {};
    auto const found = std::ranges::find(CanaryCases, name, &CanaryCase::name);
    if (found == CanaryCases.end())
    {
        std::puts("require-canary: usage: isoline_require_canary <case>");
        return EXIT_FAILURE;
    }
    try
    {
        found->run();
    }
    catch (...)
    {
        std::println(
            "require-canary: {}: the check let the call through (the handler's exception was caught)", name);
        return EXIT_SUCCESS;
    }
    std::println("require-canary: {}: the check let the call through", name);
    return EXIT_SUCCESS;
}
