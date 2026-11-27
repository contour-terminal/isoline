// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>
#include <isoline/runtime/testing/MovingMemory.hpp>
#include <isoline/runtime/testing/ProvidedMemory.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::binary::AddressType;
using isoline::binary::ExternKind;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::TableType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::call;
using isoline::binary::testing::code;
using isoline::binary::testing::drop;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::memoryGrow;
using isoline::binary::testing::ModuleBuilder;
using isoline::binary::testing::tableGrow;
using isoline::binary::testing::unreachable;
using isoline::runtime::Abrupt;
using isoline::runtime::DispatchRows;
using isoline::runtime::ExternValue;
using isoline::runtime::FuncAddr;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::HostContext;
using isoline::runtime::IMemoryAllocator;
using isoline::runtime::MemAddr;
using isoline::runtime::MemoryView;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Refusal;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::TableAddr;
using isoline::runtime::TableView;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::GrowDecision;
using isoline::runtime::testing::MovingMemoryAllocator;
using isoline::runtime::testing::ProvidedMemoryAllocator;

namespace
{

/// The default page size, 64 KiB.
constexpr auto PageSize = isoline::binary::pageSize(MemoryType {});

/// The null funcref, which a new table's slots hold.
constexpr auto NullFunction = Ref { .kind = RefKind::Null, .heap = FuncRef.heap, .address = 0 };

/// A memory of 1 to 8 pages, i32-addressed.
constexpr auto OneToEightPages =
    MemoryType { .limits = Limits { .min = 1, .max = 8, .addressType = AddressType::I32 } };

/// A funcref table of 1 to 1'000 slots, i32-indexed.
constexpr auto OneToAThousandSlots =
    TableType { .element = FuncRef,
                .limits = Limits { .min = 1, .max = 1'000, .addressType = AddressType::I32 } };

/// Whether @p view is what @p bytes are now: their data and their size.
[[nodiscard]] bool showsBytes(MemoryView const& view, std::span<std::byte const> bytes) noexcept
{
    return view.base == bytes.data() && view.bytes == bytes.size();
}

/// Whether @p view is what @p elements are now: their data and their size.
[[nodiscard]] bool showsElements(TableView const& view, std::span<Ref const> elements) noexcept
{
    return view.data == elements.data() && view.size == elements.size();
}

/// Whether memory @p address of @p store shows its bytes.
[[nodiscard]] bool memoryViewIsCurrent(Store const& store, MemAddr address) noexcept
{
    auto const& memory = store.memory(address);
    return showsBytes(memory.view(), memory.bytes());
}

/// Whether table @p address of @p store shows its slots.
[[nodiscard]] bool tableViewIsCurrent(Store const& store, TableAddr address) noexcept
{
    auto const& table = store.table(address);
    return showsElements(table.view(), table.elements());
}

/// The memory generation as generated code reads it: through its address (Store::memoryGenerationAddress()).
[[nodiscard]] uint64_t generationOf(Store const& store) noexcept
{
    return *store.memoryGenerationAddress();
}

/// Where a store's memories come from.
enum class Storage : uint8_t
{
    Heap,     ///< The store's heap memory.
    Moving,   ///< An embedder's memory that moves on every grow (MovingMemory).
    Provided, ///< An embedder's memory that never moves and grants up to 2 pages (ProvidedMemory).
};

/// Every Storage, with its name.
[[nodiscard]] std::vector<std::pair<Storage, std::string_view>> storages()
{
    return { { Storage::Heap, "heap" }, { Storage::Moving, "moving" }, { Storage::Provided, "provided" } };
}

/// One allocator of each Storage, of which a store takes one.
struct Allocators
{
    HeapMemoryAllocator heap;
    MovingMemoryAllocator moving;
    ProvidedMemoryAllocator provided { 8 * PageSize, 0, [](uint64_t bytes) {
                                          return bytes <= 2 * PageSize ? GrowDecision::Grant
                                                                       : GrowDecision::Refuse;
                                      } };

    /// The allocator of @p storage.
    [[nodiscard]] IMemoryAllocator& of(Storage storage) noexcept
    {
        switch (storage)
        {
            case Storage::Heap: return heap;
            case Storage::Moving: return moving;
            case Storage::Provided: return provided;
        }
        std::abort();
    }
};

/// The decoded, validated module of @p builder, which must be valid.
[[nodiscard]] std::shared_ptr<isoline::validate::ValidatedModule const> validated(
    ModuleBuilder const& builder)
{
    auto module = isoline::validate::ValidatedModule::load(
        builder.build(), isoline::FeatureSet::all(), isoline::binary::DecodeLimits {});
    REQUIRE(module.has_value());
    return std::move(*module);
}

} // namespace

TEST_CASE("Store.viewsFollowEveryGrowPath")
{
    // Whichever storage a memory has, its view is its bytes after every grow the store is asked for: one
    // granted, one by nothing, one the store refuses, one the embedder refuses. The view's address stays,
    // and the generation, read through its address, moves exactly with a granted grow.
    auto const [storage, storageName] = GENERATE(from_range(storages()));
    INFO(storageName);
    auto allocators = Allocators {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocators.of(storage) } };
    auto const memory = store.addMemory(OneToEightPages);
    REQUIRE(memory.has_value());
    auto const* const view = &store.memory(*memory).view();
    CHECK(memoryViewIsCurrent(store, *memory));
    CHECK(view->bytes == PageSize);
    auto const firstBase = view->base;

    CHECK(store.growMemory(*memory, 1) == 1U);
    CHECK(&store.memory(*memory).view() == view);
    CHECK(memoryViewIsCurrent(store, *memory));
    CHECK(view->bytes == 2 * PageSize);
    CHECK(generationOf(store) == 1);
    if (storage == Storage::Moving)
        CHECK(view->base != firstBase);
    if (storage == Storage::Provided)
        CHECK(view->base == firstBase);

    auto const grown = *view;
    CHECK(store.growMemory(*memory, 0) == 2U);
    CHECK(store.growMemory(*memory, 7) == std::unexpected { Refusal::TypeMaximum });
    if (storage == Storage::Provided)
        CHECK(store.growMemory(*memory, 1) == std::unexpected { Refusal::Embedder });
    CHECK(view->base == grown.base);
    CHECK(view->bytes == grown.bytes);
    CHECK(memoryViewIsCurrent(store, *memory));
    CHECK(generationOf(store) == 1);
}

TEST_CASE("Store.tableViewsFollowEveryGrowPath")
{
    // A table's view is its slots after every grow: one that moves them, one by nothing, and one refused by
    // its maximum and one by the store's limit, which leave them where they were.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxTableElements = 600 }, StoreServices { .allocator = allocator } };
    auto const table = store.addTable(OneToAThousandSlots, NullFunction);
    REQUIRE(table.has_value());
    auto const* const view = &store.table(*table).view();
    CHECK(tableViewIsCurrent(store, *table));
    CHECK(view->size == 1);

    // Far past the vector's capacity, so that the slots move.
    CHECK(store.growTable(*table, 499, NullFunction) == 1U);
    CHECK(&store.table(*table).view() == view);
    CHECK(tableViewIsCurrent(store, *table));
    CHECK(view->size == 500);

    auto const grown = *view;
    CHECK(store.growTable(*table, 0, NullFunction) == 500U);
    CHECK(store.growTable(*table, 501, NullFunction) == std::unexpected { Refusal::TypeMaximum });
    CHECK(store.growTable(*table, 101, NullFunction) == std::unexpected { Refusal::Limit });
    CHECK(view->data == grown.data);
    CHECK(view->size == grown.size);
    CHECK(tableViewIsCurrent(store, *table));
    // A table grows no memory.
    CHECK(generationOf(store) == 0);
}

TEST_CASE("Store.viewsStayWhereTheyAre")
{
    // Executors point at the views (compiler::InstanceContext): adding memories and tables, growing them and
    // dropping later ones (a failed instantiation's, releaseTrailing()) moves none of the views kept.
    auto allocator = MovingMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto memories = std::vector<std::pair<MemAddr, MemoryView const*>> {};
    auto tables = std::vector<std::pair<TableAddr, TableView const*>> {};
    constexpr auto Kept = 3U;
    constexpr auto Count = 40U;
    for ([[maybe_unused]] auto const index: std::views::iota(0U, Count))
    {
        auto const memory = store.addMemory(OneToEightPages);
        REQUIRE(memory.has_value());
        memories.emplace_back(*memory, &store.memory(*memory).view());
        auto const table = store.addTable(OneToAThousandSlots, NullFunction);
        REQUIRE(table.has_value());
        tables.emplace_back(*table, &store.table(*table).view());
    }
    for (auto const& [memory, view]: memories)
        REQUIRE(store.growMemory(memory, 1).has_value());
    for (auto const& [table, view]: tables)
        REQUIRE(store.growTable(table, 10, NullFunction).has_value());
    store.releaseTrailing(tables[Kept].first, memories[Kept].first);
    REQUIRE(store.memoryCount() == Kept);
    REQUIRE(store.tableCount() == Kept);
    for (auto const& [memory, view]: memories | std::views::take(Kept))
    {
        CHECK(&store.memory(memory).view() == view);
        CHECK(memoryViewIsCurrent(store, memory));
        CHECK(view->bytes == 2 * PageSize);
    }
    for (auto const& [table, view]: tables | std::views::take(Kept))
    {
        CHECK(&store.table(table).view() == view);
        CHECK(tableViewIsCurrent(store, table));
        CHECK(view->size == 11);
    }
}

namespace
{

/// What one step of Store.viewsChangeOnlyWithTheGeneration does.
enum class StepAction : uint8_t
{
    Grow, ///< Grows memory `memory` by `pages`.
    Add,  ///< Adds a memory.
    Drop, ///< Drops every memory from `memory` on (releaseTrailing()).
};

/// One step, and what it does to the generation.
struct Step
{
    StepAction action;
    MemAddr memory;
    uint64_t pages;
    uint64_t generation; ///< The generation after it.
};

} // namespace

TEST_CASE("Store.viewsChangeOnlyWithTheGeneration")
{
    // P4's contract (Store::memoryGeneration()), as a reader of views relies on it: while the generation is
    // unchanged, every memory's view is too; when a grow moves it by one, exactly the grown memory's view
    // changed. On memories that move with every grow, so that a stale view is a different one.
    auto const steps = std::to_array<Step>({
        { .action = StepAction::Add, .memory = 0, .pages = 0, .generation = 0 },
        { .action = StepAction::Add, .memory = 1, .pages = 0, .generation = 0 },
        { .action = StepAction::Grow, .memory = 0, .pages = 1, .generation = 1 },
        { .action = StepAction::Grow, .memory = 1, .pages = 0, .generation = 1 },
        { .action = StepAction::Grow, .memory = 1, .pages = 8, .generation = 1 },
        { .action = StepAction::Add, .memory = 2, .pages = 0, .generation = 1 },
        { .action = StepAction::Grow, .memory = 2, .pages = 2, .generation = 2 },
        { .action = StepAction::Grow, .memory = 1, .pages = 3, .generation = 3 },
        { .action = StepAction::Drop, .memory = 2, .pages = 0, .generation = 3 },
        { .action = StepAction::Grow, .memory = 0, .pages = 1, .generation = 4 },
    });
    auto allocator = MovingMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto seen = std::vector<MemoryView> {};
    for (auto const& step: steps)
    {
        INFO("step on memory " << step.memory << ", generation " << step.generation);
        auto const before = generationOf(store);
        auto grown = std::optional<MemAddr> {};
        switch (step.action)
        {
            case StepAction::Grow:
                std::ignore = store.growMemory(step.memory, step.pages);
                grown = step.memory;
                break;
            case StepAction::Add: REQUIRE(store.addMemory(OneToEightPages) == step.memory); break;
            case StepAction::Drop:
                store.releaseTrailing(static_cast<TableAddr>(store.tableCount()), step.memory);
                seen.resize(step.memory);
                break;
        }
        CHECK(generationOf(store) == step.generation);
        for (auto const address: std::views::iota(MemAddr { 0 }, static_cast<MemAddr>(store.memoryCount())))
        {
            auto const& view = store.memory(address).view();
            CHECK(memoryViewIsCurrent(store, address));
            if (address >= seen.size())
                continue;
            auto const changed = view.base != seen[address].base || view.bytes != seen[address].bytes;
            CHECK(changed == (generationOf(store) != before && grown == address));
        }
        seen.clear();
        for (auto const address: std::views::iota(MemAddr { 0 }, static_cast<MemAddr>(store.memoryCount())))
            seen.push_back(store.memory(address).view());
    }
}

namespace
{

/// Where a function's memory and table grow while it runs.
enum class GrowPath : uint8_t
{
    InFunction,        ///< memory.grow and table.grow in the function itself.
    InCallee,          ///< In a Wasm function it calls.
    InHost,            ///< A host function it calls grows both through the store.
    InHostThenTrap,    ///< A host function grows both, then traps.
    InReentry,         ///< A host function invokes a Wasm function that grows both.
    InReentryThenTrap, ///< A host function invokes a Wasm function that grows both, then traps.
    InOtherInstance,   ///< Another instance, whose memory and table it imports, grows them.
    RefusedInFunction, ///< The function asks for more than the maximum of each: both grows fail.
};

/// How an invocation along a GrowPath ends.
enum class Ending : uint8_t
{
    Returns, ///< It returns.
    Traps,   ///< It traps, with `trap`.
};

/// One row of Views.followEveryExecutionPath.
struct PathRow
{
    GrowPath path;
    std::string_view name;
    Ending ending;
    TrapCode trap;       ///< The trap it ends with, for Ending::Traps.
    uint64_t pages;      ///< Memory 0's pages after it.
    uint64_t slots;      ///< Table 0's slots after it.
    uint64_t generation; ///< The generation after it.
};

/// What a granted grow adds to the memory, in pages, and to the table, in slots (past the vector's capacity).
constexpr auto GrantedPages = 1;
constexpr auto GrantedSlots = 200;

/// Every GrowPath, with its outcome.
[[nodiscard]] std::vector<PathRow> pathRows()
{
    auto const grown = [](GrowPath path, std::string_view name, Ending ending, TrapCode trap) {
        return PathRow { .path = path,
                         .name = name,
                         .ending = ending,
                         .trap = trap,
                         .pages = 1 + GrantedPages,
                         .slots = 1 + GrantedSlots,
                         .generation = 1 };
    };
    return {
        grown(GrowPath::InFunction, "in the function", Ending::Returns, TrapCode::Unreachable),
        grown(GrowPath::InCallee, "in a callee", Ending::Returns, TrapCode::Unreachable),
        grown(GrowPath::InHost, "in a host function", Ending::Returns, TrapCode::Unreachable),
        grown(GrowPath::InHostThenTrap, "in a host function that traps", Ending::Traps, TrapCode::HostTrap),
        grown(GrowPath::InReentry, "in a host function's re-entry", Ending::Returns, TrapCode::Unreachable),
        grown(GrowPath::InReentryThenTrap,
              "in a host function's re-entry that traps",
              Ending::Traps,
              TrapCode::Unreachable),
        grown(GrowPath::InOtherInstance, "in another instance", Ending::Returns, TrapCode::Unreachable),
        PathRow { .path = GrowPath::RefusedInFunction,
                  .name = "refused in the function",
                  .ending = Ending::Returns,
                  .trap = TrapCode::Unreachable,
                  .pages = 1,
                  .slots = 1,
                  .generation = 0 },
    };
}

/// A function body that grows memory 0 by @p pages and table 0 by @p slots, dropping both results.
[[nodiscard]] Bytes growBoth(int32_t pages, int32_t slots)
{
    return code(
        { i32Const(pages), memoryGrow(), drop(), { 0xD0, 0x70 }, i32Const(slots), tableGrow(0), drop() });
}

/// The storage another instance defines and exports, for GrowPath::InOtherInstance: memory 0, table 0, and
/// function 0, () -> (), which grows both.
[[nodiscard]] ModuleBuilder storageModule()
{
    auto module = ModuleBuilder {};
    module.type(FuncType {});
    module.memory(OneToEightPages);
    module.table(OneToAThousandSlots);
    module.function(0, {}, growBoth(GrantedPages, GrantedSlots));
    module.exportEntity("memory", ExternKind::Memory, 0);
    module.exportEntity("table", ExternKind::Table, 0);
    module.exportFunction("grow", 0);
    return module;
}

/// The module that runs along @p path. Function 0 is the import env.host, () -> (); function 1, () -> (),
/// grows memory 0 and table 0 along @p path; function 2 grows both; function 3 grows both and traps. Memory 0
/// and table 0 are imported from the storage module for GrowPath::InOtherInstance, whose import 0 is its
/// grow, and defined otherwise.
[[nodiscard]] ModuleBuilder pathModule(GrowPath path)
{
    auto module = ModuleBuilder {};
    module.type(FuncType {});
    auto const imported = path == GrowPath::InOtherInstance;
    module.importFunction(imported ? "storage" : "env", imported ? "grow" : "host", 0);
    if (imported)
    {
        module.importMemory("storage", "memory", OneToEightPages);
        module.importTable("storage", "table", OneToAThousandSlots);
    }
    else
    {
        module.memory(OneToEightPages);
        module.table(OneToAThousandSlots);
    }
    auto run = Bytes {};
    switch (path)
    {
        case GrowPath::InFunction: run = growBoth(GrantedPages, GrantedSlots); break;
        case GrowPath::RefusedInFunction: run = growBoth(8, 1'000); break;
        case GrowPath::InCallee: run = call(2); break;
        case GrowPath::InHost:
        case GrowPath::InHostThenTrap:
        case GrowPath::InReentry:
        case GrowPath::InReentryThenTrap:
        case GrowPath::InOtherInstance: run = call(0); break;
    }
    module.function(0, {}, run);
    module.function(0, {}, growBoth(GrantedPages, GrantedSlots));
    module.function(0, {}, code({ growBoth(GrantedPages, GrantedSlots), unreachable() }));
    return module;
}

/// The trap @p outcome ended with; nullopt when it did not trap.
[[nodiscard]] std::optional<TrapCode> trapOf(std::expected<void, Abrupt> const& outcome)
{
    if (outcome.has_value())
        return std::nullopt;
    auto const* trap = std::get_if<Trap>(&outcome.error());
    if (trap == nullptr)
        return std::nullopt;
    return trap->code;
}

/// What a host function saw of the views, after it grew.
enum class HostSaw : uint8_t
{
    Nothing,      ///< It did not run.
    StaleViews,   ///< A view that was not current.
    CurrentViews, ///< Both views current.
};

} // namespace

TEST_CASE("Views.followEveryExecutionPath")
{
    // Whichever way a running function's memory and table grow (in itself, a callee, a host function, a
    // nested invocation, another instance that shares them; trapping after or not; refused), the views are
    // current afterwards, at the addresses they had, and the generation moved by exactly the granted grow.
    // A host function that grew sees both current before it returns: no view waits for the guest to resume.
    auto const row = GENERATE(from_range(pathRows()));
    auto const storage = GENERATE(Storage::Heap, Storage::Moving);
    INFO(row.name << (storage == Storage::Moving ? ", on a memory that moves" : ", on the heap"));
    auto allocators = Allocators {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocators.of(storage) } };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};

    auto memory = std::optional<MemAddr> {};
    auto table = std::optional<TableAddr> {};
    auto functions = std::vector<FuncAddr> {};
    auto seen = HostSaw::Nothing;
    auto imports = std::vector<ExternValue> {};
    if (row.path == GrowPath::InOtherInstance)
    {
        auto const other = isoline::runtime::instantiate(store, interpreter, validated(storageModule()), {});
        REQUIRE(other.has_value());
        auto const& instance = store.moduleInstance(*other);
        imports = { ExternValue { .kind = ExternKind::Func, .address = instance.functions.at(0) },
                    ExternValue { .kind = ExternKind::Memory, .address = instance.memories.at(0) },
                    ExternValue { .kind = ExternKind::Table, .address = instance.tables.at(0) } };
    }
    else
    {
        auto const host = store.addHostFunction(
            FuncType {},
            isoline::runtime::makeHostFunction(
                [&, path = row.path](HostContext& context,
                                     std::span<Value const>,
                                     std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                    auto outcome = std::expected<void, Abrupt> {};
                    if (path == GrowPath::InReentry)
                        outcome = context.invoke(functions.at(2), {}, {});
                    else if (path == GrowPath::InReentryThenTrap)
                        outcome = context.invoke(functions.at(3), {}, {});
                    else if (!store.growMemory(*memory, GrantedPages)
                             || !store.growTable(*table, GrantedSlots, NullFunction))
                        outcome = std::unexpected { Abrupt { Trap { TrapCode::HostTrap } } };
                    seen = memoryViewIsCurrent(store, *memory) && tableViewIsCurrent(store, *table)
                               ? HostSaw::CurrentViews
                               : HostSaw::StaleViews;
                    if (outcome && path == GrowPath::InHostThenTrap)
                        outcome = std::unexpected { Abrupt { Trap { TrapCode::HostTrap } } };
                    return outcome;
                }));
        REQUIRE(host.has_value());
        imports = { ExternValue { .kind = ExternKind::Func, .address = *host } };
    }
    auto const address =
        isoline::runtime::instantiate(store, interpreter, validated(pathModule(row.path)), imports);
    REQUIRE(address.has_value());
    auto const& instance = store.moduleInstance(*address);
    functions = instance.functions;
    memory = instance.memories.at(0);
    table = instance.tables.at(0);
    if (row.path == GrowPath::InOtherInstance)
    {
        // One memory and one table, so one view each, whichever instance's code reads them.
        CHECK(memory == imports[1].address);
        CHECK(table == imports[2].address);
    }
    auto const* const memoryView = &store.memory(*memory).view();
    auto const* const tableView = &store.table(*table).view();

    auto const outcome = isoline::runtime::invokeChecked(store, interpreter, functions.at(1), {}, {}).outcome;
    if (row.ending == Ending::Returns)
        CHECK(outcome.has_value());
    else
        CHECK(trapOf(outcome) == row.trap);
    CHECK(&store.memory(*memory).view() == memoryView);
    CHECK(&store.table(*table).view() == tableView);
    CHECK(memoryViewIsCurrent(store, *memory));
    CHECK(tableViewIsCurrent(store, *table));
    CHECK(memoryView->bytes == row.pages * PageSize);
    CHECK(tableView->size == row.slots);
    CHECK(generationOf(store) == row.generation);
    if (seen != HostSaw::Nothing)
        CHECK(seen == HostSaw::CurrentViews);
    if (storage == Storage::Moving)
        CHECK(allocators.moving.moves() == row.generation);
}

TEST_CASE("Views.dispatchFollowsAnInstantiationInAHostCall")
{
    // A host function that instantiates a module mid-call adds dispatch rows past a chunk, which moves the
    // chunks' array: the store's one DispatchView, at the address it had, shows every row once the host
    // function is back, and before, with each new row's type.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {},
                         StoreServices { .allocator = allocator, .dispatchRows = DispatchRows::Kept } };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    constexpr auto Functions = isoline::runtime::DispatchChunkRows + 10;
    auto many = ModuleBuilder {};
    many.type(FuncType {});
    for ([[maybe_unused]] auto const index: std::views::iota(0U, Functions))
        many.function(0, {}, {});
    auto const manyModule = validated(many);

    auto const rowsAreCurrent = [&store] {
        auto const& view = store.dispatchView();
        if (view.rows != store.functionCount())
            return false;
        for (auto const address: std::views::iota(FuncAddr { 0 }, view.rows))
        {
            auto const& row = view.chunks[address / isoline::runtime::DispatchChunkRows]
                                         [address % isoline::runtime::DispatchChunkRows];
            if (row.typeId != store.function(address).typeId)
                return false;
        }
        return true;
    };
    auto currentInHost = false;
    auto const host = store.addHostFunction(
        FuncType {},
        isoline::runtime::makeHostFunction([&](HostContext&,
                                               std::span<Value const>,
                                               std::span<Value>) noexcept -> std::expected<void, Abrupt> {
            auto const nested = isoline::runtime::instantiate(store, interpreter, manyModule, {});
            currentInHost = nested.has_value() && rowsAreCurrent();
            return {};
        }));
    REQUIRE(host.has_value());
    auto caller = ModuleBuilder {};
    caller.type(FuncType {});
    caller.importFunction("env", "host", 0);
    caller.function(0, {}, call(0));
    auto const address = isoline::runtime::instantiate(
        store,
        interpreter,
        validated(caller),
        std::vector { ExternValue { .kind = ExternKind::Func, .address = *host } });
    REQUIRE(address.has_value());
    auto const* const view = &store.dispatchView();
    auto const chunksBefore = view->chunks;
    REQUIRE(view->rows == 2);

    auto const outcome = isoline::runtime::invokeChecked(
                             store, interpreter, store.moduleInstance(*address).functions.at(1), {}, {})
                             .outcome;
    CHECK(outcome.has_value());
    CHECK(currentInHost);
    CHECK(&store.dispatchView() == view);
    CHECK(view->rows == 2 + Functions);
    CHECK(view->chunks != chunksBefore);
    CHECK(rowsAreCurrent());
}

namespace
{

/// How a LyingMemory breaks ILinearMemory's grow contract.
enum class Lie : uint8_t
{
    MovesWhenRefusing, ///< A refused grow leaves the bytes elsewhere.
    GrowsShort,        ///< A granted grow leaves fewer bytes than asked for.
};

/// An embedder's storage that breaks the grow contract as @p lie says, from an allocator of it.
class LyingAllocator final: public IMemoryAllocator
{
  public:
    explicit LyingAllocator(Lie lie): _lie { lie } {}

    [[nodiscard]] std::expected<std::unique_ptr<isoline::runtime::ILinearMemory>, Refusal> allocate(
        isoline::runtime::MemoryRequest const& request) noexcept override
    {
        return std::make_unique<Memory>(request.initialBytes, _lie);
    }

  private:
    class Memory final: public isoline::runtime::ILinearMemory
    {
      public:
        Memory(uint64_t size, Lie lie): _bytes(static_cast<std::size_t>(size)), _lie { lie } {}

        [[nodiscard]] std::span<std::byte> bytes() noexcept override { return _bytes; }
        [[nodiscard]] std::span<std::byte const> bytes() const noexcept override { return _bytes; }
        [[nodiscard]] uint64_t guardBytes() const noexcept override { return 0; }
        [[nodiscard]] std::expected<void, Refusal> grow(uint64_t newBytes) noexcept override
        {
            switch (_lie)
            {
                case Lie::MovesWhenRefusing:
                    _bytes = std::vector<std::byte>(_bytes.size());
                    return std::unexpected { Refusal::Embedder };
                case Lie::GrowsShort: _bytes.resize(static_cast<std::size_t>(newBytes) - 1); return {};
            }
            std::abort();
        }

      private:
        std::vector<std::byte> _bytes;
        Lie _lie;
    };

    Lie _lie;
};

} // namespace

TEST_CASE("Contract.Store.refusedGrowLeavesTheMemory", "[.contract]")
{
    // A storage that moves while refusing would leave every reader of the unchanged generation on bytes
    // that are gone.
    isoline::runtime::testing::endAtContractViolation();
    auto allocator = LyingAllocator { Lie::MovesWhenRefusing };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const memory = store.addMemory(OneToEightPages);
    REQUIRE(memory.has_value());
    std::ignore = store.growMemory(*memory, 1);
    FAIL("SURVIVED: a refused grow that moved the memory");
}

TEST_CASE("Contract.Store.grantedGrowReachesItsSize", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto allocator = LyingAllocator { Lie::GrowsShort };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const memory = store.addMemory(OneToEightPages);
    REQUIRE(memory.has_value());
    std::ignore = store.growMemory(*memory, 1);
    FAIL("SURVIVED: a granted grow short of its size");
}
