// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::runtime
{

/// The address of a function instance in its store.
using FuncAddr = uint32_t;
/// The address of a table instance in its store.
using TableAddr = uint32_t;
/// The address of a memory instance in its store.
using MemAddr = uint32_t;
/// The address of a global instance in its store.
using GlobalAddr = uint32_t;
/// A function type's identity in its store: equal types, of whichever modules, have one TypeId.
using TypeId = uint32_t;

class IHostFunction; // HostFunction.hpp

/// A function instance: a module's function, or a host function.
struct FunctionInstance
{
    TypeId typeId; ///< Its type's identity in the store (Store::type), which call_indirect compares.
    uint32_t moduleInstance; ///< A module function: the instance that defines it.
    uint32_t codeIndex;      ///< A module function: its index in that module's code section.
    /// A host function: its implementation, shared with whoever else holds it; null for a module
    /// function.
    std::shared_ptr<IHostFunction> host;

    /// Whether this is a host function.
    /// @return Whether host is set.
    [[nodiscard]] bool isHost() const noexcept { return host != nullptr; }
};

/// A table instance.
struct TableInstance
{
    binary::TableType type;    ///< Its type as allocated; its current size is elements.size().
    std::vector<Ref> elements; ///< Its slots.
};

/// A memory instance: its type, and its storage from the store's IMemoryAllocator.
///
/// The storage is private, so that a const instance reads its bytes only as const, and only the store,
/// which charges growth against its budget, grows it.
class MemoryInstance
{
  public:
    /// @param type Its type as allocated.
    /// @param memory Its storage (checked: not null).
    MemoryInstance(binary::MemoryType type, std::unique_ptr<ILinearMemory> memory);

    binary::MemoryType type; ///< Its type as allocated; its size is bytes().size() / pageSize(type).

    /// Its contents, a whole number of pages; re-fetched after every grow and host call.
    /// @return The storage's bytes.
    [[nodiscard]] std::span<std::byte> bytes() noexcept { return _memory->bytes(); }

    /// @copydoc bytes()
    [[nodiscard]] std::span<std::byte const> bytes() const noexcept
    {
        return std::as_const(*_memory).bytes();
    }

  private:
    friend class Store;

    /// Grows the storage to @p newBytes, zeroed (ILinearMemory::grow()): only the store, which
    /// charges the growth against its budget, grows a memory.
    /// @param newBytes The new size in bytes.
    /// @return Nothing, or the storage's refusal.
    [[nodiscard]] std::expected<void, Refusal> grow(uint64_t newBytes) noexcept
    {
        return _memory->grow(newBytes);
    }

    std::unique_ptr<ILinearMemory> _memory;
};

/// A global instance.
struct GlobalInstance
{
    binary::GlobalType type; ///< Its type.
    Value value;             ///< Its current value.
};

/// A tag instance: what marks an exception, and the types of the payload it carries.
///
/// It names its type by TypeId only, as a FunctionInstance does: Store::type() reads the type, whose
/// params are the payload's types. A copy per tag would make a module of many tags of one wide type
/// cost the store memory of the tag count times the type's size.
struct TagInstance
{
    TypeId typeId = 0; ///< Its type's identity in the store, which an import of it must name.
};

/// An exception instance: what throw allocates, and an exnref refers to.
struct ExceptionInstance
{
    TagAddr tag = 0;            ///< The tag it was thrown with; handlers catch by it.
    std::vector<Value> payload; ///< The tag's params' values, in order.
};

/// An external value: an entity of the store an import binds to or an export names.
struct ExternValue
{
    binary::ExternKind kind; ///< The kind of entity.
    uint32_t address;        ///< Its address in the store, in the address space of kind.

    bool operator==(ExternValue const&) const = default;
};

/// A module instance: a module's entities, as store addresses in its index spaces' order.
struct ModuleInstance
{
    /// The validated module it instantiates; null for a module instance the embedder assembled.
    std::shared_ptr<validate::ValidatedModule const> validated;
    std::shared_ptr<binary::Module const> module; ///< validated's module, sharing its ownership.
    std::vector<TypeId> types;                    ///< Its type section's types, by type index.
    std::vector<FuncAddr> functions;              ///< The function index space: imports first.
    std::vector<TableAddr> tables;                ///< The table index space: imports first.
    std::vector<MemAddr> memories;                ///< The memory index space: imports first.
    std::vector<GlobalAddr> globals;              ///< The global index space: imports first.
    std::vector<TagAddr> tags;                    ///< The tag index space: imports first.

    /// The element segments' references, by segment index; nullopt once dropped.
    std::vector<std::optional<std::vector<Ref>>> elements;

    /// The data segments' bytes, viewing module, by segment index; nullopt once dropped.
    std::vector<std::optional<std::span<std::byte const>>> data;

    /// Its exports, in the module's order.
    std::vector<std::pair<std::string, ExternValue>> exports;
};

/// The services a store reaches the host through, injected at construction.
struct StoreServices
{
    /// Allocates every linear memory of the store; referenced, so it must outlive the store.
    IMemoryAllocator& allocator;

    /// The guard region every memory must declare after its bytes (ILinearMemory::guardBytes()): a
    /// memory with a smaller one is refused with Guard. 0 requires none, as HeapMemoryAllocator has.
    uint64_t requiredGuardBytes = 0;

    /// Observes every poll (Store::poll()); none when null. Referenced, so it must outlive the store.
    IPollObserver* pollObserver = nullptr;
};

/// The store: every function, table, memory, global and module instance, addressed by index.
///
/// Instances are never removed, and adding one never moves the others, so a reference to an
/// instance stays valid as long as the store. The one exception is releaseTrailing(), which drops the
/// tables and memories of an instantiation that failed before anything referred to them.
///
/// Every resource a guest can claim is capped by the store's StoreLimits, fixed at construction: the
/// counts of memories, tables, instances and tags, the bytes of all memories and the elements of all
/// tables together, and the type units of the interned types. The store keeps running totals, which
/// allocation and growth charge.
class Store
{
  public:
    /// Constructs an empty store.
    /// @param limits What execution and the store's entities may claim.
    /// @param services What the store allocates memories through and who observes its polls;
    ///                 referenced, so they must outlive the store.
    explicit Store(StoreLimits limits, StoreServices services);

    /// A store's instances are addressed by index and its types handed out by reference (type()),
    /// so it stays where it was constructed: it is neither copied nor moved.
    Store(Store const&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store const&) = delete;
    Store& operator=(Store&&) = delete;
    ~Store() = default;

    /// Interns a function type: the TypeId of the type equal to it, added if there is none. A type
    /// added gets the next TypeId, typeCount() before the call, so that instantiation can tell the
    /// TypeIds a module's types will get before it interns them, and charges its typeUnits() against
    /// StoreLimits::maxTypes.
    ///
    /// Precondition (checked): @p type is in store form, every concrete type index in it a TypeId of
    /// this store, as interning a module's types in type order makes them; and a new type fits the
    /// type budget, as instantiation checks (checkInstanceDemand()) and addHostFunction() does.
    /// @param type The type, in store form.
    /// @return Its TypeId: equal for equal types.
    [[nodiscard]] TypeId internType(binary::FuncType type);

    /// Looks a function type up without interning it.
    /// @param type The type, in store form.
    /// @return Its TypeId, or nullopt when no equal type is interned.
    [[nodiscard]] std::optional<TypeId> findType(binary::FuncType const& type) const;

    /// How many types are interned: the TypeId the next new type gets.
    /// @return The number of interned types.
    [[nodiscard]] TypeId typeCount() const noexcept;

    /// What interning @p type anew charges against StoreLimits::maxTypes.
    /// @param type The type.
    /// @return 1 + its params + its results.
    [[nodiscard]] static uint64_t typeUnits(binary::FuncType const& type) noexcept;

    /// The type with TypeId @p id. Precondition (checked): it exists.
    /// @param id The type's identity.
    /// @return The type, in store form; the reference stays valid as long as the store.
    [[nodiscard]] binary::FuncType const& type(TypeId id) const;

    /// Adds a host function, interning its type (internType()).
    ///
    /// The type budget is shared with the guests' modules, which may have used most of it, so a new
    /// type past what is left of it is refused rather than a failed precondition.
    /// @param type Its type, in store form (checked).
    /// @param function Its implementation; must not be null (checked).
    /// @return Its address; LimitExceeded of Types when its type is new and its type units exceed
    ///         those left.
    [[nodiscard]] std::expected<FuncAddr, LimitExceeded> addHostFunction(
        binary::FuncType type, std::shared_ptr<IHostFunction> function);

    /// Adds a module function.
    /// @param typeId Its type, interned already.
    /// @param moduleInstance The instance that defines it.
    /// @param codeIndex Its index in that module's code section.
    /// @return Its address.
    [[nodiscard]] FuncAddr addModuleFunction(TypeId typeId, uint32_t moduleInstance, uint32_t codeIndex);

    /// Adds a table of type.limits.min slots, each holding @p init, and charges them against
    /// StoreLimits::maxTableElements.
    /// @param type Its type.
    /// @param init What every slot starts with.
    /// @return Its address; LimitExceeded when the store holds StoreLimits::maxTables tables already or
    ///         its slots exceed the elements left; otherwise an AllocationFailure when its size is
    ///         past its type's reach (TypeMaximum) or allocation fails (HostMemory).
    [[nodiscard]] std::expected<TableAddr, AllocationError> addTable(binary::TableType type, Ref init);

    /// Adds a zeroed memory of type.limits.min pages, allocated by the store's IMemoryAllocator, and
    /// charges its bytes against StoreLimits::maxMemoryBytes.
    ///
    /// Precondition (checked): type.pageSizeLog2 is below binary::PageSizeLog2Limit, so that the page
    /// size is a u64, as the decoder guarantees for every module's memory.
    /// @param type Its type.
    /// @return Its address; LimitExceeded when the store holds StoreLimits::maxMemories memories
    ///         already or its bytes exceed those left; otherwise an AllocationFailure: TypeMaximum when
    ///         its size is past its type's reach, the allocator's refusal (Embedder, HostMemory), or
    ///         Guard when the memory's guard is smaller than StoreServices::requiredGuardBytes.
    [[nodiscard]] std::expected<MemAddr, AllocationError> addMemory(binary::MemoryType type);

    /// Adds a global.
    /// @param type Its type.
    /// @param value Its initial value, of type.type.
    /// @return Its address.
    [[nodiscard]] GlobalAddr addGlobal(binary::GlobalType type, Value value);

    /// Adds a tag.
    /// @param typeId Its type, interned already (checked).
    /// @return Its address: a new tag, distinct from every other, whatever its type; LimitExceeded of
    ///         Tags when the store holds StoreLimits::maxTags tags already, which guests' modules
    ///         share with the embedder.
    [[nodiscard]] std::expected<TagAddr, LimitExceeded> addTag(TypeId typeId);

    /// Allocates an exception, as throw does, or as a host function may to throw one.
    /// @param tag The tag it is thrown with; must exist (checked).
    /// @param payload The values it carries, of the tag's params' types (checkValues(), checked).
    /// @return Its address, or ExceptionLimitExceeded when the store holds StoreLimits::maxExceptions
    ///         exceptions already, or their payloads would exceed StoreLimits::maxExceptionValues.
    [[nodiscard]] std::expected<ExnAddr, Trap> addException(TagAddr tag, std::vector<Value> payload);

    /// Adds a module instance.
    ///
    /// Precondition (checked): the store holds fewer than StoreLimits::maxInstances instances, as
    /// instantiation checks (checkInstanceDemand()).
    /// @param instance The instance; the module and its entities' addresses, as far as allocated.
    /// @return Its address.
    [[nodiscard]] uint32_t addModuleInstance(ModuleInstance instance);

    /// Checks that one more module instance, and what it will allocate, fits every limit the store
    /// caps: the instance count, the memory, table and tag counts, the memories' bytes, the tables'
    /// elements and the type units, each against what is left of it. Charges nothing: allocating
    /// (addMemory(), addTable(), internType(), addTag(), addModuleInstance()) does.
    /// @param demand What the instance will claim beyond itself.
    /// @return Nothing when it fits; otherwise the first limit, in Limit order, it would exceed.
    [[nodiscard]] std::expected<void, LimitExceeded> checkInstanceDemand(InstanceDemand const& demand) const;

    /// The limits the store was constructed with.
    /// @return Its StoreLimits.
    [[nodiscard]] StoreLimits const& limits() const noexcept;

    /// How much of @p limit the store holds: what an embedder reads before adding to a budget the
    /// guests share. The limits of one invocation (CallDepth, StackValues, NestedInvocations) are the
    /// executor's to count while it runs; the store holds none of them, so they read 0.
    /// @param limit The limit.
    /// @return What the store holds of it, in the unit LimitTable names; at most its StoreLimits value.
    [[nodiscard]] uint64_t used(Limit limit) const noexcept;

    /// How many host functions are running on the store (HostCallScope), nested ones included.
    /// @return Their count; 0 when no host function is running.
    [[nodiscard]] uint32_t hostCallsInProgress() const noexcept;

    /// The store's interrupt, created with it. Shared, so that a watchdog on another thread may keep
    /// it past the store's lifetime.
    /// @return The interrupter; never null.
    [[nodiscard]] std::shared_ptr<Interrupter> const& interrupter() const noexcept { return _interrupter; }

    /// Polls: what an executor calls at every PollSite. It notifies the poll observer, then checks
    /// the interrupt.
    /// @param site Where execution polls.
    /// @return Nothing to go on; or Trap{Interrupted} once the store was interrupted.
    [[nodiscard]] std::expected<void, Trap> poll(PollSite site) noexcept
    {
        if (_pollObserver != nullptr)
            _pollObserver->onPoll(site);
        if (_interrupter->state() == InterruptState::Interrupted)
            return trap(TrapCode::Interrupted);
        return {};
    }

    /// How many module instances the store holds.
    /// @return Their count.
    [[nodiscard]] std::size_t moduleInstanceCount() const noexcept;

    /// How many functions the store holds: every FuncAddr below it exists.
    /// @return Their count.
    [[nodiscard]] std::size_t functionCount() const noexcept;

    /// Whether @p address names an exception this store holds, which an exnref may refer to: one it
    /// allocated (addException()). A host's address of an exception it never allocated is not.
    /// @param address The exception's address.
    /// @return Whether it is below the arena's count.
    [[nodiscard]] bool isLiveException(ExnAddr address) const noexcept;

    /// How many memories the store holds.
    /// @return Their count.
    [[nodiscard]] std::size_t memoryCount() const noexcept;

    /// How many tables the store holds.
    /// @return Their count.
    [[nodiscard]] std::size_t tableCount() const noexcept;

    /// How many tags the store holds: every TagAddr below it exists.
    /// @return Their count.
    [[nodiscard]] std::size_t tagCount() const noexcept;

    /// Drops the tables from @p firstTable and the memories from @p firstMemory on, destroying their
    /// storage and returning their elements and bytes to the budgets: what instantiation does when it
    /// fails to allocate before it registers the instance, so that a retry gets its quota back.
    ///
    /// Precondition (checked): @p firstTable is at most tableCount() and @p firstMemory at most
    /// memoryCount(); nothing refers to the entities dropped.
    /// @param firstTable The first table to drop: tableCount() before the instantiation allocated.
    /// @param firstMemory The first memory to drop: memoryCount() before the instantiation allocated.
    void releaseTrailing(TableAddr firstTable, MemAddr firstMemory) noexcept;

    /// The current refusal scope's first refused grow of each kind (growMemory(), growTable()): what
    /// the embedder reads to learn why a memory.grow or table.grow returned -1. A RefusalScope starts
    /// an empty record and restores the outer one when it ends.
    /// @return The record.
    [[nodiscard]] GrowthRecord const& growthRecord() const noexcept;

    /// The function at @p address. Precondition (checked): the address exists.
    /// @param address The function's address.
    /// @return The function instance.
    [[nodiscard]] FunctionInstance const& function(FuncAddr address) const;
    /// @copydoc function(FuncAddr) const
    [[nodiscard]] FunctionInstance& function(FuncAddr address);

    /// The type of the function at @p address. Precondition (checked): the address exists.
    /// @param address The function's address.
    /// @return Its type, in store form.
    [[nodiscard]] binary::FuncType const& functionType(FuncAddr address) const;

    /// The table at @p address. Precondition (checked): the address exists.
    /// @param address The table's address.
    /// @return The table instance.
    [[nodiscard]] TableInstance const& table(TableAddr address) const;
    /// @copydoc table(TableAddr) const
    [[nodiscard]] TableInstance& table(TableAddr address);

    /// The memory at @p address. Precondition (checked): the address exists.
    /// @param address The memory's address.
    /// @return The memory instance.
    [[nodiscard]] MemoryInstance const& memory(MemAddr address) const;
    /// @copydoc memory(MemAddr) const
    [[nodiscard]] MemoryInstance& memory(MemAddr address);

    /// The global at @p address. Precondition (checked): the address exists.
    /// @param address The global's address.
    /// @return The global instance.
    [[nodiscard]] GlobalInstance const& global(GlobalAddr address) const;
    /// @copydoc global(GlobalAddr) const
    [[nodiscard]] GlobalInstance& global(GlobalAddr address);

    /// The tag at @p address. Precondition (checked): the address exists.
    /// @param address The tag's address.
    /// @return The tag instance.
    [[nodiscard]] TagInstance const& tag(TagAddr address) const;

    /// The exception at @p address. Precondition (checked): the address exists.
    /// @param address The exception's address.
    /// @return The exception instance.
    [[nodiscard]] ExceptionInstance const& exception(ExnAddr address) const;

    /// The module instance at @p address. Precondition (checked): the address exists.
    /// @param address The module instance's address.
    /// @return The module instance.
    [[nodiscard]] ModuleInstance const& moduleInstance(uint32_t address) const;
    /// @copydoc moduleInstance(uint32_t) const
    [[nodiscard]] ModuleInstance& moduleInstance(uint32_t address);

    /// Grows a memory by @p deltaPages zeroed pages, as memory.grow does, and charges them against
    /// StoreLimits::maxMemoryBytes.
    /// @param address The memory's address.
    /// @param deltaPages The number of pages to add.
    /// @return The old size in pages; or, the size unchanged, why it was refused: TypeMaximum past the
    ///         memory's maximum or its address type's reach, Limit past the store's bytes left, or the
    ///         memory's own refusal (Embedder, HostMemory). The first refusal is recorded in
    ///         growthRecord().memory.
    [[nodiscard]] std::expected<uint64_t, Refusal> growMemory(MemAddr address, uint64_t deltaPages);

    /// Grows a table by @p delta slots holding @p init, as table.grow does, and charges them against
    /// StoreLimits::maxTableElements.
    /// @param address The table's address.
    /// @param delta The number of slots to add.
    /// @param init What the new slots hold.
    /// @return The old size; or, the size unchanged, why it was refused: TypeMaximum, Limit or
    ///         HostMemory, as growMemory()'s. The first refusal is recorded in growthRecord().table.
    [[nodiscard]] std::expected<uint64_t, Refusal> growTable(TableAddr address, uint64_t delta, Ref init);

  private:
    friend class RefusalScope;
    friend class HostCallScope;

    IMemoryAllocator* _allocator;
    uint64_t _requiredGuardBytes;
    IPollObserver* _pollObserver;
    std::shared_ptr<Interrupter> _interrupter;
    StoreLimits _limits;
    GrowthRecord _growthRecord;    ///< The current refusal scope's first refused grow per kind.
    uint64_t _exceptionValues = 0; ///< The payload values of every exception held.
    uint64_t _memoryBytes = 0;     ///< The bytes of every memory, charged against maxMemoryBytes.
    uint64_t _tableElements = 0;   ///< The slots of every table, charged against maxTableElements.
    uint64_t _typeUnits = 0;       ///< The type units of every interned type, charged against maxTypes.
    uint32_t _hostCalls = 0;       ///< The host functions running, nested ones included.
    std::deque<FunctionInstance> _functions;
    std::deque<TableInstance> _tables;
    std::deque<MemoryInstance> _memories;
    std::deque<GlobalInstance> _globals;
    std::deque<TagInstance> _tags;
    std::deque<ExceptionInstance> _exceptions;
    std::deque<ModuleInstance> _moduleInstances;

    /// Every interned type and its TypeId. An ordered map, not a hash table: types are the guest's
    /// to choose, and a lookup costs O(log n) comparisons whatever they are.
    std::map<binary::FuncType, TypeId> _typeIds;
    /// The interned types by TypeId, pointing at _typeIds' keys, which never move.
    std::vector<binary::FuncType const*> _types;
};

/// Scopes the store's growth record to one invocation: it saves the record, starts an empty one, and
/// restores the saved one when it ends, so that a nested call's refused grows never leak into its
/// caller's record and the caller's first refusal survives the nested call.
class RefusalScope
{
  public:
    /// Saves @p store's record and starts an empty one.
    /// @param store The store; must outlive the scope.
    explicit RefusalScope(Store& store) noexcept;

    /// Restores the record saved at construction.
    ~RefusalScope();

    RefusalScope(RefusalScope const&) = delete;
    RefusalScope(RefusalScope&&) = delete;
    RefusalScope& operator=(RefusalScope const&) = delete;
    RefusalScope& operator=(RefusalScope&&) = delete;

    /// This scope's record: the first refused grow of each kind since it began, nested scopes' aside.
    /// @return The store's current record.
    [[nodiscard]] GrowthRecord const& record() const noexcept;

  private:
    Store* _store;
    GrowthRecord _outer; ///< The record of the scope this one is nested in.
};

/// Counts a host function call in progress on a store (Store::hostCallsInProgress()) for as long as it
/// lives: what an executor opens around every host function it runs.
class HostCallScope
{
  public:
    /// Counts one more host call on @p store.
    /// @param store The store; must outlive the scope.
    explicit HostCallScope(Store& store) noexcept;

    /// Gives the count back.
    ~HostCallScope();

    HostCallScope(HostCallScope const&) = delete;
    HostCallScope(HostCallScope&&) = delete;
    HostCallScope& operator=(HostCallScope const&) = delete;
    HostCallScope& operator=(HostCallScope&&) = delete;

  private:
    Store* _store;
};

} // namespace isoline::runtime
