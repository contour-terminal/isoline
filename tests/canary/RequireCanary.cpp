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
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string_view>
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
