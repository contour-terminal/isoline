// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/EngineIdentity.hpp>
#include <isoline/api/Extern.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Types.hpp>

namespace isoline::api
{

class Instance;

namespace testing
{
    struct RuntimeAccess;
} // namespace testing

/// The current size of a linear memory (Store::memorySize(), Instance::memorySize()).
struct MemorySize
{
    uint64_t pages = 0; ///< In pages of the memory's own page size.
    uint64_t bytes = 0; ///< In bytes: pages times the page size.

    /// Equal when both sizes are.
    bool operator==(MemorySize const&) const = default;
};

/// A module loaded by an engine of another identity than the store's (Module::engine()): its code was
/// decoded and validated for another engine, mode or feature set.
struct EngineMismatch
{
    /// All such refusals are equal.
    bool operator==(EngineMismatch const&) const = default;
};

/// An import bound to an entity of another store, by an Imports definition (Store::instantiate()) or a
/// positional extern (Store::instantiateByPosition()): entities do not cross stores.
struct ForeignImport
{
    uint32_t index = 0; ///< The import's index in the module's import section.

    /// Equal when they name the same import.
    bool operator==(ForeignImport const&) const = default;
};

/// Positional externs (Store::instantiateByPosition()) not one per import of the module: nothing was linked.
struct ImportCountMismatch
{
    uint32_t expected = 0; ///< The module's imports.
    uint32_t given = 0;    ///< The externs given.

    /// Equal when both counts are.
    bool operator==(ImportCountMismatch const&) const = default;
};

/// Why Store::instantiate() or instantiateByPosition() failed: the module is of another engine; positional
/// externs are not one per
/// import; an import is bound to another store's entity; an import did not link (no definition, or one of
/// another kind or type); the instance would exceed one of the store's limits; a memory or table could not be
/// allocated; the host ran out of memory before anything was added to the store; or initializing it (a
/// segment, the start function) failed as a call does.
using InstantiateError = std::variant<EngineMismatch,
                                      ImportCountMismatch,
                                      ForeignImport,
                                      LinkError,
                                      LimitExceeded,
                                      AllocationFailure,
                                      HostMemoryExhausted,
                                      CallError>;

/// What is wrong with a type the host gave the store to define an entity of (InvalidType).
enum class InvalidTypeReason : uint8_t
{
    ConcreteTypeIndex, ///< It names a concrete type index, which only a module's own types may name.
    PageSize,          ///< A memory's page size is not one custom-page-sizes admits (1 byte, 64 KiB).
    BottomHeap,        ///< It names the bot heap: the validator's own type, which no module may declare.
};

/// A type no entity the host defines may have (Store::define(), defineMemory(), defineTable(),
/// defineGlobal()): nothing was added to the store.
struct InvalidType
{
    InvalidTypeReason reason; ///< What is wrong with it.

    /// Equal when both name the same reason.
    bool operator==(InvalidType const&) const = default;
};

/// Why the store could not add an entity outside any instantiation (Store::define(), defineMemory(),
/// defineTable(), defineGlobal()): it would exceed one of the store's limits; the host ran out of memory
/// before anything was added to the store; a memory or table could not be allocated (its size past its
/// type's reach, or the allocator's refusal); its initial value is not of its type; or its type is one
/// the host may not define.
using DefineError =
    std::variant<LimitExceeded, HostMemoryExhausted, AllocationFailure, ValueMismatch, InvalidType>;

/// Why a memory could not be reached (Store::memoryRange(), Store::memorySize(),
/// Instance::memoryRange(), Instance::memorySize()).
enum class MemoryAccessError : uint8_t
{
    ForeignHandle, ///< The memory is another store's (or a store's that is gone).
    OutOfBounds,   ///< A byte of the range lies outside the memory.
    StoreDeleted,  ///< The instance's store is gone: Instance's accessors.
};

/// Why the store could not read or write a global (Store::globalValue(), Store::setGlobal()).
enum class GlobalAccessError : uint8_t
{
    ForeignHandle, ///< The global is another store's (or a store's that is gone).
    Immutable,     ///< Only a mutable global is set: the global is const.
    ValueMismatch, ///< The value is not of the global's type.
};

/// Why the store could not read or write a table (Store::tableSize(), Store::tableGet(),
/// Store::tableSet()).
enum class TableAccessError : uint8_t
{
    ForeignHandle, ///< The table is another store's (or a store's that is gone).
    OutOfBounds,   ///< The index is not below the table's current size.
    ValueMismatch, ///< The reference is not of the table's element type.
};

/// Why Instance::exportAt() could not name an export.
enum class ExportAccessError : uint8_t
{
    OutOfRange,   ///< The index is not below the number of the instance's exports.
    StoreDeleted, ///< The instance's store is gone.
};

/// A store: where instances live, with their memories, tables, globals and the host functions bound
/// to them, under the limits of the engine it was made for.
///
/// It copies the engine's configuration, so the Engine may go before it.
///
/// Threads: the store and every handle of it (its instances, and every Function, Memory, Global, Table
/// and Tag) are one unit, used by one thread at a time; the unit may move between threads between calls.
/// Destroying the store on one thread while another thread holds or uses one of its handles is a
/// contract violation: the alive-token a handle checks guards against a store destroyed before the use,
/// on the same thread, and synchronizes nothing. Its interrupter (interrupter()) is the one thing another
/// thread may use while it runs. A host function implementation shared by several stores (one Imports used by
/// stores on several threads) is called on each of their threads at once, so it must be thread-safe.
///
/// Nothing but the constructor throws. Where the host runs out of memory before anything is added to the
/// store, the call is refused (HostMemoryExhausted): instantiate() while it prepares, define() while it
/// reserves, and defineMemory(), defineTable() and defineGlobal() wholly, as each adds its entity in one
/// step that leaves the store unchanged when it fails. Once the store is being changed (an instance
/// allocated, a host function added, a call running) running out of memory terminates the process, as
/// it does in runtime::invokeChecked(): the store is not exception-safe midway through a change.
///
/// A store is neither copied nor moved: its instances point at it.
class Store
{
  public:
    /// An empty store for modules of @p engine.
    /// @param engine The engine: its configuration (limits, identity, guard size) is copied, so it may
    ///               be destroyed before the store.
    /// @param allocator Allocates every linear memory of the store; must outlive it.
    /// @param observer Observes every poll of the store's executions; none when null. Must outlive it.
    /// @throws std::bad_alloc When the host has no memory for the empty store.
    Store(Engine const& engine, IMemoryAllocator& allocator, IPollObserver* observer = nullptr);

    Store(Store const&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store const&) = delete;
    Store& operator=(Store&&) = delete;
    ~Store();

    /// The store's interrupter: interrupt() it from any thread to stop the call running in the store,
    /// and every later call and instantiation, for good. It exists before any instantiation, so a start
    /// function is interruptible too, and a watchdog may keep it past the store.
    ///
    /// Interrupter::interrupt() is async-signal-safe; copying the shared pointer is not. To interrupt
    /// from a signal handler, keep a raw Interrupter* (interrupter().get()) where the handler finds it,
    /// and the shared pointer alive for as long as the handler may run.
    /// @return The interrupter, shared.
    [[nodiscard]] std::shared_ptr<Interrupter> const& interrupter() const noexcept;

    /// How much of @p limit the store holds now: what an embedder reads to meter its guests or to
    /// share one budget between stores. A count or size reads what the store's entities take
    /// together; StackValues reads the frames in progress and NestedInvocations the invocations in
    /// progress, 0 between calls; Steps reads the steps of the latest outermost call or
    /// instantiation, the calls nested in it included, and keeps that after it ended; CallDepth reads 0,
    /// as the executor counts it while it runs.
    /// @param limit The limit.
    /// @return What the store holds of it, in the unit its LimitTable row names; at most its limit.
    [[nodiscard]] uint64_t used(Limit limit) const noexcept { return _runtime.used(limit); }

    /// The identity its handles carry.
    /// @return It, valid as long as the store.
    [[nodiscard]] StoreId const& id() const noexcept { return *_id; }

    /// Instantiates @p module, binding its imports to @p imports' definitions by name.
    ///
    /// A host function definition is added to the store the first time an instantiation binds it, and
    /// reused by every later one: one function per definition's implementation and type. Every import is
    /// looked up and every host function checked against its import before any is added, so a missing,
    /// foreign or mistyped definition costs the store nothing; but host functions added by an
    /// instantiation that fails later (a limit, a trap, the host out of memory) stay in the store, as
    /// define()d ones do, and the next instantiation binding them reuses them.
    /// @param module A module of an engine of this store's identity.
    /// @param imports The definitions its imports are bound to.
    /// @return The instance (see Instance for a store that is gone); or EngineMismatch for a module of
    ///         another engine, ForeignImport for an import bound to another store's instance, LinkError
    ///         (UnknownImport for an import with no definition, IncompatibleImportType for one of another
    ///         kind or type), LimitExceeded, AllocationFailure, HostMemoryExhausted when the host ran
    ///         out of memory before the instance was begun (runtime::instantiate()'s first step), or a
    ///         CallError when initializing it trapped, threw, failed in a host function or was
    ///         interrupted. A CallError's refusedGrowth is instantiation's own, its start function's first
    ///         refused grows, never a nested invocation's.
    [[nodiscard]] std::expected<Instance, InstantiateError> instantiate(Module const& module,
                                                                        Imports const& imports) noexcept;

    /// Instantiates @p module, binding its imports to @p imports by position: one extern per import, in
    /// import order, as wasm.h's wasm_instance_new() binds them. Names play no part, so two imports of one
    /// name bind two externs.
    ///
    /// Every extern is checked as an Imports definition of a store's entity is: its store first, then,
    /// while linking, its kind and type against its import's.
    /// @param module A module of an engine of this store's identity.
    /// @param imports One extern of this store per import of @p module, in import order.
    /// @return The instance; or EngineMismatch for a module of another engine, ImportCountMismatch when
    ///         @p imports is not one extern per import, ForeignImport (its position as index) for an extern
    ///         of another store, a deleted one included, and every other error the named form reports
    ///         once its imports are found.
    [[nodiscard]] std::expected<Instance, InstantiateError> instantiateByPosition(
        Module const& module, std::span<Extern const> imports) noexcept;

    /// Calls @p function, with no instance: a host function define() added, or a function an instance of
    /// the store exports. It is the one checked entry into the guest (runtime::invokeChecked()), which
    /// Instance::call() and a host function's HostContext::invoke() share.
    ///
    /// Exnrefs among the results are escaped, valid for the store's life. noexcept: the call changes the
    /// store, so running out of memory midway terminates.
    /// @param function A function of this store.
    /// @param args Its arguments, of its params' types.
    /// @param results Receives its results; one slot per result.
    /// @return Nothing, or how it failed: ForeignHandle, without entering the guest, for a function of
    ///         another store (a deleted one included); ArgumentMismatch, without entering the guest, when
    ///         @p args are not of its params' types or @p results has the wrong size; or how the call
    ///         ended (CallError).
    [[nodiscard]] std::expected<void, CallError> call(Function const& function,
                                                      std::span<Value const> args,
                                                      std::span<Value> results) noexcept;

    /// Adds a host function to the store, outside any instantiation: to define it as an import
    /// (Imports::define()), or to hand it to the guest in a table. The same implementation and type,
    /// defined again or bound by an instantiation, is the same function.
    /// @param type Its type.
    /// @param function Its implementation (checked: not null).
    /// @return The function; InvalidType (ConcreteTypeIndex, BottomHeap) when a param or result names a
    ///         concrete type index or the bot heap; LimitExceeded of Types when its type is new and over the
    ///         store's budget; HostMemoryExhausted when the host ran out of memory before it was added.
    [[nodiscard]] std::expected<Function, DefineError> define(
        FuncType const& type, std::shared_ptr<IHostFunction> const& function) noexcept;

    /// Adds a zeroed memory of type.limits.min pages to the store, outside any instantiation: to bind it
    /// to an import, or to share it between instances. It is charged and refused as a module's own
    /// memory is: against StoreLimits::maxMemories and maxMemoryBytes, allocated by the store's
    /// IMemoryAllocator, every size checked before a byte is allocated.
    /// @param type Its type.
    /// @return The memory; InvalidType (PageSize) for a page size custom-page-sizes does not admit;
    ///         LimitExceeded of Memories or MemoryBytes; AllocationFailure of Memory with TypeMaximum for a
    ///         minimum above its maximum or past its type's reach, or with the allocator's refusal
    ///         (Embedder, HostMemory, Guard); HostMemoryExhausted when the host ran out of memory
    ///         recording it, the store unchanged.
    [[nodiscard]] std::expected<Memory, DefineError> defineMemory(MemoryType const& type) noexcept;

    /// Adds a table of type.limits.min elements, each holding @p init, to the store, outside any
    /// instantiation. It is charged and refused as a module's own table is: against
    /// StoreLimits::maxTables and maxTableElements, every size checked before an element is allocated.
    /// @param type Its type, in store form.
    /// @param init What every element starts with, of the element type.
    /// @return The table; InvalidType (ConcreteTypeIndex, BottomHeap) when its element type names a
    ///         concrete type index or the bot heap; ValueMismatch for an @p init not of the element type (a
    ///         reference of another heap, or naming what the store does not hold, included), before anything
    ///         is charged; LimitExceeded of Tables or TableElements; AllocationFailure of Table with
    ///         TypeMaximum for a minimum above its maximum or past its type's reach, or HostMemory for its
    ///         elements; HostMemoryExhausted when the host ran out of memory recording it, the store
    ///         unchanged.
    [[nodiscard]] std::expected<Table, DefineError> defineTable(TableType const& type, Ref init) noexcept;

    /// Adds a global of @p value to the store, outside any instantiation. A global charges no store
    /// limit, as a module's own does not: module globals are bounded by the decoder's limits only, and an
    /// embedder bounds the globals it defines itself.
    /// @param type Its type, in store form.
    /// @param value Its initial value, of type.type.
    /// @return The global; InvalidType (ConcreteTypeIndex, BottomHeap) when its type names a concrete type
    ///         index or the bot heap; ValueMismatch for a @p value not of its type (a reference of another
    ///         heap, or naming what the store does not hold, included); HostMemoryExhausted when the host ran
    ///         out of memory recording it, the store unchanged.
    [[nodiscard]] std::expected<Global, DefineError> defineGlobal(GlobalType const& type,
                                                                  Value value) noexcept;

    /// The current size of @p memory, which a call into the store may grow.
    /// @param memory A memory of this store: an instance's, or one defineMemory() added.
    /// @return Its size in pages and in bytes; ForeignHandle for a memory of another store.
    [[nodiscard]] std::expected<MemorySize, MemoryAccessError> memorySize(
        Memory const& memory) const noexcept;

    /// A view of @p length bytes of @p memory from @p offset, valid until the next call into the store or
    /// grow of the memory, either of which may move it.
    /// @param memory A memory of this store: an instance's, or one defineMemory() added.
    /// @param offset The first byte's offset.
    /// @param length The number of bytes; 0 views nothing, at any offset.
    /// @return The view; ForeignHandle for a memory of another store, OutOfBounds when a byte of it lies
    ///         outside the memory.
    [[nodiscard]] std::expected<std::span<std::byte>, MemoryAccessError> memoryRange(
        Memory const& memory, uint64_t offset, uint64_t length) noexcept;

    /// Grows @p memory by @p deltaPages zeroed pages, as memory.grow does: through the guest's own path,
    /// charged against StoreLimits::maxMemoryBytes alike, so that its limits and refusals are the guest's.
    /// @param memory A memory of this store: an instance's, or one defineMemory() added.
    /// @param deltaPages The number of pages to add.
    /// @return The old size in pages; or, the memory unchanged: ForeignHandle for a memory of another
    ///         store, or the Refusal memory.grow would have met (TypeMaximum, Limit, Embedder,
    ///         HostMemory), every size checked before a byte is allocated. A refusal while a call into the
    ///         store runs (from a host function) is that call's: it is recorded in the call's
    ///         refusedGrowth.memory, as a refused memory.grow of the guest's is.
    [[nodiscard]] std::expected<uint64_t, MemoryGrowthError> growMemory(Memory const& memory,
                                                                        uint64_t deltaPages) noexcept;

    /// The current value of @p global, which any instance of the store may have changed.
    ///
    /// An exnref value is escaped (runtime::Store::markEscaped()): the embedder holds it now, so it stays
    /// valid for the store's life.
    /// @param global A global of any instance of this store.
    /// @return Its value; ForeignHandle for a global of another store.
    [[nodiscard]] std::expected<Value, GlobalAccessError> globalValue(Global const& global) noexcept;

    /// Assigns @p value to @p global, as global.set does.
    /// @param global A global of any instance of this store.
    /// @param value Its new value, of its type.
    /// @return Nothing, or, the global unchanged: ForeignHandle for a global of another store, Immutable
    ///         for a const one, ValueMismatch for a value not of its type (a reference of another heap,
    ///         or naming what the store does not hold, included).
    [[nodiscard]] std::expected<void, GlobalAccessError> setGlobal(Global const& global,
                                                                   Value value) noexcept;

    /// The type of @p global, in its store's form (a concrete heap names a TypeId of the store).
    /// @param global A global of any instance of this store.
    /// @return Its type; ForeignHandle for a global of another store.
    [[nodiscard]] std::expected<GlobalType, GlobalAccessError> globalType(
        Global const& global) const noexcept;

    /// The current size of @p table, in elements, which a call into the store may grow.
    /// @param table A table of any instance of this store.
    /// @return Its size; ForeignHandle for a table of another store.
    [[nodiscard]] std::expected<uint64_t, TableAccessError> tableSize(Table const& table) const noexcept;

    /// The element at @p index of @p table, as table.get reads it.
    ///
    /// An exnref element is escaped, as globalValue()'s is.
    /// @param table A table of any instance of this store.
    /// @param index The element's index.
    /// @return The element; ForeignHandle for a table of another store, OutOfBounds for an index not
    ///         below its size.
    [[nodiscard]] std::expected<Ref, TableAccessError> tableGet(Table const& table, uint64_t index) noexcept;

    /// Assigns @p value to the element at @p index of @p table, as table.set does.
    /// @param table A table of any instance of this store.
    /// @param index The element's index.
    /// @param value The element's new value, of the table's element type.
    /// @return Nothing, or, the table unchanged: ForeignHandle for a table of another store, OutOfBounds
    ///         for an index not below its size, ValueMismatch for a reference not of its element type.
    [[nodiscard]] std::expected<void, TableAccessError> tableSet(Table const& table,
                                                                 uint64_t index,
                                                                 Ref value) noexcept;

    /// Grows @p table by @p delta elements holding @p init, as table.grow does: through the guest's
    /// own path, charged against StoreLimits::maxTableElements alike, so that its limits and refusals
    /// are the guest's.
    /// @param table A table of any instance of this store.
    /// @param delta The number of elements to add.
    /// @param init What the new elements hold, of the table's element type.
    /// @return The old size; or, the table unchanged: ForeignHandle for a table of another store,
    ///         InitValueMismatch for an @p init not of its element type, or the Refusal table.grow would
    ///         have met (TypeMaximum, Limit, HostMemory). A refusal while a call into the store runs (from
    ///         a host function) is that call's: it is recorded in the call's refusedGrowth.table, as a
    ///         refused table.grow of the guest's is.
    [[nodiscard]] std::expected<uint64_t, TableGrowthError> growTable(Table const& table,
                                                                      uint64_t delta,
                                                                      Ref init) noexcept;

    /// The type of @p table, in its store's form, its minimum the table's current size.
    /// @param table A table of any instance of this store.
    /// @return Its type; ForeignHandle for a table of another store.
    [[nodiscard]] std::expected<TableType, TableAccessError> tableType(Table const& table) const noexcept;

  private:
    friend class Instance;
    friend class Imports;
    friend struct testing::RuntimeAccess;

    /// The host functions of the store, by implementation and type: empty while an instantiation or
    /// define() has reserved the entry and not yet added the function.
    using HostFunctions = std::map<std::pair<IHostFunction const*, FuncType>, std::optional<FuncAddr>>;

    /// A host function an instantiation or define() binds: its reserved entry and what to add.
    struct PendingHost
    {
        HostFunctions::iterator entry;           ///< Its entry; empty until it is added.
        FuncType type;                           ///< Its type, copied before the store changes.
        std::shared_ptr<IHostFunction> function; ///< Its implementation.
    };

    /// Reserves the entry of host function @p function under @p type: everything binding it allocates,
    /// done before the store changes. May throw std::bad_alloc.
    /// @return What to add, or the entry of the function added already.
    [[nodiscard]] PendingHost reserveHost(FuncType const& type,
                                          std::shared_ptr<IHostFunction> const& function);

    /// Adds @p pending's function, unless its entry holds it already.
    /// @return Its address; LimitExceeded of Types when its type is new and over the store's budget.
    [[nodiscard]] std::expected<FuncAddr, LimitExceeded> commitHost(PendingHost& pending) noexcept;

    /// Drops every reserved entry whose function was not added: what a refused instantiation or
    /// define() leaves behind otherwise.
    void discardReservations() noexcept;

    /// Instantiates @p module with @p externs, once every allocation before the store changes is done.
    /// @param pending The host functions to add first; externs[index] receives each one's address.
    [[nodiscard]] std::expected<Instance, InstantiateError> link(
        Module const& module,
        std::vector<runtime::ExternValue> externs,
        std::vector<std::pair<std::size_t, PendingHost>> pending) noexcept;

    /// What every handle of the store keeps of it: the store and its alive-token.
    [[nodiscard]] detail::StoreLink handleLink() const noexcept
    {
        return detail::StoreLink { *this, _alive };
    }

    /// The store that made @p value. Precondition (checked, aborting): it is alive.
    [[nodiscard]] static Store const& ownerOf(Extern const& value) noexcept;

    /// The kind and address of the entity @p value names, in its store.
    [[nodiscard]] static runtime::ExternValue externValueOf(Extern const& value) noexcept;

    /// The type of the entity @p value names, as the store holds it now: what an Imports definition of
    /// it snapshots. May throw std::bad_alloc.
    [[nodiscard]] Imports::EntityType entityTypeOf(runtime::ExternValue value) const;

    /// Whether the host may define a function of @p type: the check define() reports as InvalidType,
    /// which Imports::define() of a host function requires.
    /// @return Nothing, or InvalidType of the first param or result type it may not define.
    [[nodiscard]] static std::expected<void, DefineError> checkHostFunctionType(
        FuncType const& type) noexcept;

    /// The handle of the entity @p value names.
    [[nodiscard]] Extern externOf(runtime::ExternValue value) const noexcept;

    /// Whether @p value is of @p type in this store, as runtime::checkValues() checks a value crossing
    /// the host boundary.
    /// @return Nothing, or the mismatch.
    [[nodiscard]] std::expected<void, ValueMismatch> checkValue(Value const& value,
                                                                ValueType const& type) const noexcept;

    /// What a call ended with, as the embedder is told: an exception copied out of the store, the
    /// exnrefs in its payload escaped.
    /// @param abrupt How the call ended.
    /// @param refusedGrowth The call's own first refused grows.
    /// @return The error.
    [[nodiscard]] CallError callError(Abrupt const& abrupt, GrowthRecord const& refusedGrowth) noexcept;

    EngineIdentity _identity;           ///< The identity of the engine it was made for.
    std::shared_ptr<StoreId const> _id; ///< Its identity; shared with the Imports that name it.
    runtime::Store _runtime;            ///< The store proper.
    std::unique_ptr<runtime::ICodeExecutor> _executor; ///< Runs its code: one per store.

    HostFunctions _hostFunctions; ///< The host functions added so far, by implementation and type.

    /// The alive-token its handles and instances keep a weak reference to: declared last, so that it
    /// is destroyed first, and anything torn down after it already sees the store as gone.
    std::shared_ptr<void> _alive;
};

/// An instance of a module in a store: a handle, cheap to copy.
///
/// It keeps its store's alive-token: once the store is destroyed, every operation of the instance
/// reports it (StoreDeleted) and reads nothing, and its by-name lookups find nothing. A deleted store
/// names no export.
///
/// Calls into it are re-entrant: a host function may call() back into this or another instance of the
/// store, from within the call that called it.
class Instance
{
  public:
    /// Its exported function @p name.
    /// @param name The export's name.
    /// @return The function; nothing when it exports no function of that name, or its store is gone.
    [[nodiscard]] std::optional<Function> function(std::string_view name) const noexcept;

    /// Its exported memory @p name.
    /// @param name The export's name.
    /// @return The memory; nothing when it exports no memory of that name, or its store is gone.
    [[nodiscard]] std::optional<Memory> memory(std::string_view name) const noexcept;

    /// Its exported global @p name.
    /// @param name The export's name.
    /// @return The global; nothing when it exports no global of that name, or its store is gone.
    [[nodiscard]] std::optional<Global> global(std::string_view name) const noexcept;

    /// Its exported table @p name.
    /// @param name The export's name.
    /// @return The table; nothing when it exports no table of that name, or its store is gone.
    [[nodiscard]] std::optional<Table> table(std::string_view name) const noexcept;

    /// Its exported tag @p name, whose address() an UncaughtException's tag compares with.
    /// @param name The export's name.
    /// @return The tag; nothing when it exports no tag of that name, or its store is gone.
    [[nodiscard]] std::optional<Tag> tag(std::string_view name) const noexcept;

    /// Its export at @p index, in the module's export order (Module::exports()), whatever its kind.
    /// @param index The export's index.
    /// @return The entity, as the alternative of its kind; OutOfRange for an index not below the number
    ///         of exports, StoreDeleted when its store is gone.
    [[nodiscard]] std::expected<Extern, ExportAccessError> exportAt(uint32_t index) const noexcept;

    /// Calls @p function, as Store::call() does once the instance's store is found alive.
    ///
    /// Exnrefs among the results are escaped, valid for the store's life. noexcept: the call changes
    /// the store, so running out of memory midway terminates (see Store).
    /// @param function A function of this instance's store.
    /// @param args Its arguments, of its params' types.
    /// @param results Receives its results; one slot per result.
    /// @return Nothing, or how it failed: StoreDeleted, reading nothing, when the instance's store is
    ///         gone; ForeignHandle, without entering the guest, for a function of another store;
    ///         ArgumentMismatch, without entering the guest, when @p args are not of its params' types or
    ///         @p results has the wrong size; or how the call ended (CallError).
    [[nodiscard]] std::expected<void, CallError> call(Function const& function,
                                                      std::span<Value const> args,
                                                      std::span<Value> results) const noexcept;

    /// A view of @p size bytes of @p memory from @p address, valid until the memory grows or the next call
    /// into the store, either of which may move it.
    /// @param memory A memory of this instance's store; Store::memoryRange() reaches one without an
    ///               instance.
    /// @param address The first byte's address.
    /// @param size The number of bytes; 0 views nothing, at any address.
    /// @return The view; OutOfBounds when a byte of it lies outside the memory, ForeignHandle for a
    ///         memory of another store, StoreDeleted when the instance's store is gone.
    [[nodiscard]] std::expected<std::span<std::byte>, MemoryAccessError> memoryRange(
        Memory const& memory, uint64_t address, uint64_t size) const noexcept;

    /// The current size of @p memory, which a call into the store may grow.
    /// @param memory A memory of this instance's store; Store::memorySize() reaches one without an
    ///               instance.
    /// @return Its size in pages and in bytes; ForeignHandle for a memory of another store, StoreDeleted
    ///         when the instance's store is gone.
    [[nodiscard]] std::expected<MemorySize, MemoryAccessError> memorySize(
        Memory const& memory) const noexcept;

  private:
    friend class Store;
    friend class Imports;
    friend struct testing::RuntimeAccess;

    /// The instance at @p address of @p store.
    Instance(Store& store, uint32_t address) noexcept;

    /// Whether its store is still alive: whether the store's alive-token has not expired.
    [[nodiscard]] bool isAlive() const noexcept { return !_alive.expired(); }

    /// Its export @p name, when it is of @p kind and its store is alive.
    [[nodiscard]] std::optional<runtime::ExternValue> exported(std::string_view name,
                                                               ExternKind kind) const noexcept;

    Store* _store;              ///< The store it lives in; never null, valid while it is alive.
    uint32_t _address;          ///< Its module instance's address in that store.
    std::weak_ptr<void> _alive; ///< The store's alive-token; expired once the store is destroyed.
};

} // namespace isoline::api
