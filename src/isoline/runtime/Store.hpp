// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

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
#include <variant>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/FloatEnvironment.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/runtime/RawValue.hpp>
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
class Store;
class ExceptionRootsRegistration;

/// The exception @p value refers to.
/// @param value The value.
/// @return Its address when it is an exnref to an exception; nullopt for any other value, null included.
[[nodiscard]] inline std::optional<ExnAddr> exceptionOf(Value const& value) noexcept
{
    if (auto const* reference = std::get_if<Ref>(&value); reference && reference->kind == RefKind::Exn)
        return reference->address;
    return std::nullopt;
}

/// What holds exnrefs the store cannot see: an executor's operand and locals stacks. A sweep of the
/// exception arena (Store::addException()) scans every registered one's values
/// (ExceptionRootsRegistration) and reclaims no exception they refer to.
///
/// The roots are a contract every engine reproduces, since what a sweep keeps decides whether a later
/// throw traps, and how many items it visits decides its step charge (PollSite::Sweep): every operand
/// and every local of every active frame, of every invocation in progress. None is pruned as dead,
/// however an engine knows it to be.
class IExceptionRoots
{
  public:
    virtual ~IExceptionRoots() = default;

    /// Appends the ranges of values the holder keeps: every operand and local of its active frames.
    /// They must stay unchanged while the sweep runs, which no guest code does.
    /// @param ranges Receives the ranges.
    virtual void appendExceptionRoots(std::vector<std::span<Value const>>& ranges) const = 0;
};

/// Registers an IExceptionRoots with a store for as long as it lives, so that a sweep keeps what the
/// holder refers to. An intrusive list node: registering allocates nothing.
class ExceptionRootsRegistration
{
  public:
    /// Registers @p roots with @p store.
    /// @param store The store; must outlive the registration.
    /// @param roots The holder; must outlive the registration.
    ExceptionRootsRegistration(Store& store, IExceptionRoots const& roots) noexcept;

    /// Unregisters it.
    ~ExceptionRootsRegistration();

    ExceptionRootsRegistration(ExceptionRootsRegistration const&) = delete;
    ExceptionRootsRegistration(ExceptionRootsRegistration&&) = delete;
    ExceptionRootsRegistration& operator=(ExceptionRootsRegistration const&) = delete;
    ExceptionRootsRegistration& operator=(ExceptionRootsRegistration&&) = delete;

  private:
    friend class Store;

    Store* _store;                 ///< The store it is registered with.
    IExceptionRoots const* _roots; ///< The holder.
    ExceptionRootsRegistration* _previous =
        nullptr;                       ///< The registration before it in the store's list; non-owning.
    ExceptionRootsRegistration* _next; ///< The registration after it; non-owning.
};

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

    /// Its storage, read-only: its tail and guard past the end (keepsItsTail()).
    /// @return The storage.
    [[nodiscard]] ILinearMemory const& storage() const noexcept { return *_memory; }

  private:
    friend class Store;

    /// Grows the storage to @p newBytes, zeroed, asking it for the tail the new size needs
    /// (ILinearMemory::grow(), GrowRequest): only the store, which charges the growth against its budget,
    /// grows a memory. A storage that cannot keep that tail refuses with Guard; one that grew without it
    /// anyway (keepsItsTail()) lied, breaking ILinearMemory's contract, which is checked: a refusal could not
    /// undo the grow it already made.
    /// @param newBytes The new size in bytes.
    /// @return Nothing, or the storage's refusal.
    [[nodiscard]] std::expected<void, Refusal> grow(uint64_t newBytes) noexcept
    {
        return _memory->grow(GrowRequest { .newBytes = newBytes }).transform([this] {
            Require(keepsItsTail(*_memory));
        });
    }

    std::unique_ptr<ILinearMemory> _memory;
};

/// A global instance: its type and its raw cell, the value's two words (RawValue), which every engine
/// reads and writes in the same encoding, generated code in place.
struct GlobalInstance
{
    binary::GlobalType type; ///< Its type.
    RawValue cell;           ///< Its current value, encoded (encodeValue()).

    /// Its current value, decoded as its type.
    /// @return The value.
    [[nodiscard]] Value value() const noexcept { return decodeValue(cell, type.type); }

    /// Makes @p value its current value. Precondition (checked): @p value is of its type's kind, as
    /// validation guarantees global.set's operand and the embedder's checked writes are.
    /// @param value The value.
    void assign(Value const& value) noexcept
    {
        Require(kindOf(value) == type.type.kind);
        cell = encodeValue(value);
    }
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
    /// The validated module it instantiates, whose frame shapes give its functions' stack costs; null
    /// only for a module instance the embedder assembled, which has no code (Store::addModuleInstance()
    /// checks it).
    std::shared_ptr<validate::ValidatedModule const> validated;
    std::shared_ptr<binary::Module const> module; ///< validated's module, sharing its ownership.

    /// The code its executor runs, made of validated by that executor's engine, sharing its ownership;
    /// null for the reference engine, which runs the validated module itself, and for a module
    /// instance the embedder assembled.
    std::shared_ptr<ModuleCode const> code;

    std::vector<TypeId> types;       ///< Its type section's types, by type index.
    std::vector<FuncAddr> functions; ///< The function index space: imports first.
    std::vector<TableAddr> tables;   ///< The table index space: imports first.
    std::vector<MemAddr> memories;   ///< The memory index space: imports first.
    std::vector<GlobalAddr> globals; ///< The global index space: imports first.
    std::vector<TagAddr> tags;       ///< The tag index space: imports first.

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

    /// How the invocations' floating-point scopes read and set the rounding mode: the C library's,
    /// unless a test stands in another (FloatEnvironmentScope). Referenced, so it must outlive the store.
    IRoundingMode* roundingMode = &libraryRoundingMode();
};

/// The store: every function, table, memory, global and module instance, addressed by index.
///
/// Instances are never removed, and adding one never moves the others, so a reference to an
/// instance stays valid as long as the store. There are two exceptions: releaseTrailing(), which drops
/// the tables and memories of an instantiation that failed before anything referred to them; and the
/// exceptions, which a sweep reclaims once nothing reaches them (addException()), their slots staying
/// where they are.
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
    ///
    /// Throws std::bad_alloc only when recording the table runs out of memory, and then leaves the store
    /// unchanged: nothing recorded, nothing charged.
    /// @param type Its type.
    /// @param init What every slot starts with.
    /// @return Its address; LimitExceeded when the store holds StoreLimits::maxTables tables already or
    ///         its slots exceed the elements left; otherwise an AllocationFailure when its size is
    ///         past its type's reach (TypeMaximum) or allocation fails (HostMemory).
    [[nodiscard]] std::expected<TableAddr, AllocationError> addTable(binary::TableType type, Ref init);

    /// Adds a zeroed memory of type.limits.min pages, allocated by the store's IMemoryAllocator, and
    /// charges its bytes against StoreLimits::maxMemoryBytes.
    ///
    /// Throws std::bad_alloc only when recording the memory runs out of memory, and then leaves the store
    /// unchanged: nothing recorded, nothing charged, the memory freed.
    ///
    /// Precondition (checked): type.pageSizeLog2 is below binary::PageSizeLog2Limit, so that the page
    /// size is a u64, as the decoder guarantees for every module's memory.
    /// @param type Its type.
    /// @return Its address; LimitExceeded when the store holds StoreLimits::maxMemories memories
    ///         already or its bytes exceed those left; otherwise an AllocationFailure: TypeMaximum when
    ///         its size is past its type's reach, the allocator's refusal (Embedder, HostMemory), or
    ///         Guard when the memory's guard is smaller than StoreServices::requiredGuardBytes, or its tail
    ///         and guard together than its MemoryRequest::requiredTailBytes.
    [[nodiscard]] std::expected<MemAddr, AllocationError> addMemory(binary::MemoryType type);

    /// Adds a global. Precondition (checked): @p value is of type.type's kind (GlobalInstance::assign()).
    ///
    /// Throws std::bad_alloc only when recording the global runs out of memory, and then leaves the store
    /// unchanged.
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
    ///
    /// When the arena is full (either cap below) and no host function is running
    /// (hostCallsInProgress() is 0), it first sweeps: it marks every exception reachable from the
    /// roots, through payloads, and reclaims the rest, whose addresses later allocations reuse, the
    /// lowest first. The roots are the tables', globals' and element segments' references, the
    /// registered IExceptionRoots (the executors' stacks), @p payload, and every escaped exception
    /// (markEscaped()). An address the embedder keeps past the next allocation must be escaped. No
    /// sweep runs under a host function, whose locals are no roots: there the caps hold as they are.
    ///
    /// A sweep polls at PollSite::Sweep before every SweepPollItems items it visits, each poll charging
    /// a step, so it is metered and interruptible like guest code. A poll that traps abandons the
    /// sweep before it reclaims anything: the store is as it was, the charged steps aside.
    /// @param tag The tag it is thrown with; must exist (checked).
    /// @param payload The values it carries, of the tag's params' types (checkValues(), checked).
    /// @return Its address; ExceptionLimitExceeded when the store holds StoreLimits::maxExceptions
    ///         live exceptions, or their payloads would exceed StoreLimits::maxExceptionValues, even
    ///         after a sweep; or Interrupted or StepLimitExceeded when a poll ended the sweep.
    [[nodiscard]] std::expected<ExnAddr, Trap> addException(TagAddr tag, std::vector<Value> payload);

    /// Escapes the exception @p value refers to, when it is an exnref of a live exception: no sweep
    /// reclaims it, nor the exceptions its payload refers to, for the rest of the store's life. What
    /// reaches the embedder or a host past its frame is escaped, so its address can never dangle or
    /// name another exception later. Any other value is left alone.
    /// @param value The value.
    void markEscaped(Value const& value) noexcept;

    /// How many exceptions the store holds: allocated and not reclaimed by a sweep.
    /// @return Their count, which StoreLimits::maxExceptions caps.
    [[nodiscard]] std::size_t liveExceptions() const noexcept;

    /// Adds a module instance.
    ///
    /// Precondition (checked): the store holds fewer than StoreLimits::maxInstances instances, as
    /// instantiation checks (checkInstanceDemand()); and the instance has a validated module, or a
    /// module of no code, or none.
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
    /// guests share. StackValues reads the static costs of the frames in progress (chargeFrame()),
    /// NestedInvocations the invocations in progress (InvocationScope), and Steps the steps the
    /// latest outermost invocation took, which it keeps after the invocation ends. CallDepth is the
    /// executor's to count while it runs, so it reads 0.
    /// @param limit The limit.
    /// @return What the store holds of it, in the unit LimitTable names; at most its StoreLimits value.
    [[nodiscard]] uint64_t used(Limit limit) const noexcept;

    /// Charges a frame's static stack cost (StoreLimits::maxStackValues): what an executor does at
    /// every callee entry. The charged sum is the store's, so it spans the invocations host
    /// functions nest: re-entry cannot multiply the cap.
    /// @param cost The frame's cost in stack values.
    /// @return Nothing, the cost charged; or Trap{CallStackExhausted}, nothing charged, when it
    ///         would exceed maxStackValues.
    [[nodiscard]] std::expected<void, Trap> chargeFrame(uint64_t cost) noexcept
    {
        if (cost > _limits.maxStackValues - _chargedStackValues)
            return trap(TrapCode::CallStackExhausted);
        _chargedStackValues += cost;
        return {};
    }

    /// Gives a frame's charge back (chargeFrame()): what an executor does at every frame's end,
    /// however it ends. Precondition (checked): @p cost is at most what is charged.
    /// @param cost The cost chargeFrame() charged for the frame.
    void releaseFrame(uint64_t cost) noexcept
    {
        Require(cost <= _chargedStackValues);
        _chargedStackValues -= cost;
    }

    /// The static stack costs of the frames in progress, over every invocation (chargeFrame()).
    /// @return Their sum; 0 when no frame is in progress.
    [[nodiscard]] uint64_t chargedStackValues() const noexcept { return _chargedStackValues; }

    /// How many invocations are in progress (InvocationScope): the outermost and those nested in it.
    /// @return Their count; 0 when none is.
    [[nodiscard]] uint32_t invocationDepth() const noexcept { return _invocationDepth; }

    /// Whether one more invocation may begin (InvocationScope): fewer than
    /// StoreLimits::maxNestedInvocations are in progress.
    /// @return Nothing when it may; Trap{CallStackExhausted} when it may not.
    [[nodiscard]] std::expected<void, Trap> checkInvocationNesting() const noexcept;

    /// How many host functions are running on the store (HostCallScope), nested ones included.
    /// @return Their count; 0 when no host function is running.
    [[nodiscard]] uint32_t hostCallsInProgress() const noexcept;

    /// The store's interrupt, created with it. Shared, so that a watchdog on another thread may keep
    /// it past the store's lifetime.
    /// @return The interrupter; never null.
    [[nodiscard]] std::shared_ptr<Interrupter> const& interrupter() const noexcept { return _interrupter; }

    /// Polls: what an executor calls at every PollSite. It notifies the poll observer, checks the
    /// interrupt, then charges what the site's PollSiteTable row charges against
    /// StoreLimits::maxSteps: at one poll, Interrupted beats StepLimitExceeded.
    /// @param site Where execution polls.
    /// @return Nothing to go on; Trap{Interrupted} once the store was interrupted; or
    ///         Trap{StepLimitExceeded}, no step charged, when the outermost invocation took
    ///         maxSteps steps already.
    [[nodiscard]] std::expected<void, Trap> poll(PollSite site) noexcept
    {
        if (_pollObserver != nullptr)
            _pollObserver->onPoll(site);
        if (_interrupt->state() == InterruptState::Interrupted)
            return trap(TrapCode::Interrupted);
        if (PollSiteTable[static_cast<std::size_t>(site)].charge == StepCharge::None)
            return {};
        if (_steps >= _limits.maxSteps)
            return trap(TrapCode::StepLimitExceeded);
        ++_steps;
        return {};
    }

    /// How many module instances the store holds.
    /// @return Their count.
    [[nodiscard]] std::size_t moduleInstanceCount() const noexcept;

    /// How many functions the store holds: every FuncAddr below it exists.
    /// @return Their count.
    [[nodiscard]] std::size_t functionCount() const noexcept;

    /// Whether @p address names an exception this store holds, which an exnref may refer to: one it
    /// allocated (addException()) and no sweep reclaimed. A host's address of an exception it never
    /// allocated is not, nor is one a sweep reclaimed until an allocation reuses it.
    /// @param address The exception's address.
    /// @return Whether it is below the arena's count and not on its free list.
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

    /// The exception at @p address. Precondition (checked): it is live (isLiveException()).
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

    /// The generation of the store's memories: what an executor that caches a memory's bytes (its base
    /// and size) compares, after code it does not control ran, to learn whether it must read them again.
    ///
    /// The contract, which every executor may rely on:
    /// - It starts at 0 and only ever grows, by one per change, so two reads that compare equal saw the
    ///   same memories; it is 64 bits wide and never wraps in a store's lifetime.
    /// - Every growMemory() that grows a memory by at least one page increments it, whichever memory of
    ///   the store grew and whoever asked (memory.grow, a host function, the embedder through the api),
    ///   and whether or not the storage moved (an embedder-provided memory that moves included): a grow is
    ///   the only way a memory's bytes() change their data or size (ILinearMemory::grow()).
    /// - A refused grow, and a grow by zero pages, leave it unchanged, as they leave every memory.
    /// - Adding a memory (instantiation) and dropping one (releaseTrailing()) leave it unchanged: they
    ///   change no memory that executing code can have read.
    /// So, while it is unchanged, MemoryInstance::bytes() of every memory of the store returns the data
    /// and size it returned when the generation was last read. Like the rest of the store, it is read and
    /// changed by the one thread that uses the store at a time.
    /// @return The generation.
    [[nodiscard]] uint64_t memoryGeneration() const noexcept;

    /// Where memoryGeneration() is kept: for generated code (P5), which loads the generation itself rather
    /// than calling memoryGeneration(). The address is stable for the store's lifetime (a store is neither
    /// copied nor moved), the value is the one memoryGeneration() returns and follows its contract, and it is
    /// written only by growMemory(), on the thread that uses the store.
    /// @return The counter's address.
    [[nodiscard]] uint64_t const* memoryGenerationAddress() const noexcept;

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
    friend class InvocationScope;
    friend class ExceptionRootsRegistration;
    friend class PollGate;

    /// The steps the latest outermost invocation took so far: what PollGate::reload() counts down from.
    /// @return Its steps, at most StoreLimits::maxSteps.
    [[nodiscard]] uint64_t stepsTaken() const noexcept { return _steps; }

    /// Records the steps the outermost invocation took: what PollGate::commit() writes back.
    /// @param steps Its steps, at most StoreLimits::maxSteps.
    void setStepsTaken(uint64_t steps) noexcept { _steps = steps; }

    /// What an exception arena slot holds.
    enum class ExceptionSlot : uint8_t
    {
        Held,    ///< A live exception a sweep reclaims when nothing reaches it.
        Escaped, ///< A live exception that reached the embedder or a host: never reclaimed.
        Free,    ///< Reclaimed, on the free list until an allocation reuses it.
    };

    /// An exception arena slot: the exception and its state, in one element, so that adding one is a
    /// single allocation that either happens or does not.
    struct ExceptionEntry
    {
        ExceptionInstance instance; ///< The exception; empty when Free.
        ExceptionSlot slot = ExceptionSlot::Held;
    };

    /// Whether one more exception of @p payloadSize values fits within both caps.
    [[nodiscard]] bool exceptionFits(std::size_t payloadSize) const noexcept;

    /// Marks every exception reachable from the roots, @p pending included, and reclaims the rest,
    /// polling at PollSite::Sweep before every SweepPollItems items it visits.
    /// @param pending The payload being added, not yet in the arena.
    /// @return Nothing once swept; the trap of the poll that abandoned it, nothing reclaimed.
    [[nodiscard]] std::expected<void, Trap> sweepExceptions(std::span<Value const> pending);

    IMemoryAllocator* _allocator;
    uint64_t _requiredGuardBytes;
    IPollObserver* _pollObserver;
    IRoundingMode* _roundingMode; ///< How the floating-point scopes switch the rounding mode; never null.
    std::shared_ptr<Interrupter> _interrupter;
    /// _interrupter's, raw: what every poll loads through, sparing the shared_ptr's indirection.
    Interrupter const* _interrupt;
    StoreLimits _limits;
    GrowthRecord _growthRecord;       ///< The current refusal scope's first refused grow per kind.
    uint64_t _exceptionValues = 0;    ///< The payload values of every exception held.
    uint64_t _memoryBytes = 0;        ///< The bytes of every memory, charged against maxMemoryBytes.
    uint64_t _memoryGeneration = 0;   ///< Every successful growth of a memory (memoryGeneration()).
    uint64_t _tableElements = 0;      ///< The slots of every table, charged against maxTableElements.
    uint64_t _typeUnits = 0;          ///< The type units of every interned type, charged against maxTypes.
    uint32_t _hostCalls = 0;          ///< The host functions running, nested ones included.
    uint32_t _invocationDepth = 0;    ///< The invocations in progress, nested ones included.
    uint64_t _steps = 0;              ///< The steps the latest outermost invocation took.
    uint64_t _chargedStackValues = 0; ///< The static costs of the frames in progress.
    std::deque<FunctionInstance> _functions;
    std::deque<TableInstance> _tables;
    std::deque<MemoryInstance> _memories;
    std::deque<GlobalInstance> _globals;
    std::deque<TagInstance> _tags;
    std::deque<ExceptionEntry> _exceptions; ///< The exception arena, by ExnAddr.
    std::vector<ExnAddr> _freeExceptions;   ///< The Free slots, highest first: the lowest on top.
    /// A sweep's ranges of executor values (IExceptionRoots), kept to reuse its capacity.
    std::vector<std::span<Value const>> _exceptionRootRanges;
    ExceptionRootsRegistration* _exceptionRoots = nullptr; ///< The registered roots' list; non-owning.
    std::vector<uint8_t> _exceptionMarks;    ///< A sweep's mark per slot, kept to reuse its capacity.
    std::vector<ExnAddr> _exceptionWorklist; ///< A sweep's mark stack, kept to reuse its capacity.
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

/// Scopes one invocation of guest code on a store, for as long as it lives: what every entry into
/// guest code opens, runtime::invokeChecked() and runtime::instantiate() (for its whole run, its start
/// function nested in it). It counts the invocation (Store::invocationDepth()), and the outermost one
/// starts a fresh step budget (StoreLimits::maxSteps) that the invocations nested in it share. It
/// pins the floating-point environment to the IEEE defaults (FloatEnvironmentScope), nested ones
/// too, since a host function may change the environment and then re-enter.
class InvocationScope
{
  public:
    /// Opens an invocation on @p store. Precondition (checked): one more may begin
    /// (Store::checkInvocationNesting()).
    /// @param store The store; must outlive the scope.
    explicit InvocationScope(Store& store) noexcept;

    /// Ends the invocation.
    ~InvocationScope();

    InvocationScope(InvocationScope const&) = delete;
    InvocationScope(InvocationScope&&) = delete;
    InvocationScope& operator=(InvocationScope const&) = delete;
    InvocationScope& operator=(InvocationScope&&) = delete;

  private:
    Store* _store;
    FloatEnvironmentScope _floatEnvironment; ///< The IEEE defaults, for as long as the invocation runs.
};

/// Counts a host function call in progress on a store (Store::hostCallsInProgress()) for as long as it
/// lives: what an executor opens around every host function it runs. When the host function returns,
/// it re-establishes the IEEE floating-point defaults (establishFloatDefaults()), so that a host that
/// changed the environment cannot change the results of the guest code that called it.
class HostCallScope
{
  public:
    /// Counts one more host call on @p store.
    /// @param store The store; must outlive the scope.
    explicit HostCallScope(Store& store) noexcept;

    /// Gives the count back, and re-establishes the floating-point defaults.
    ~HostCallScope();

    HostCallScope(HostCallScope const&) = delete;
    HostCallScope(HostCallScope&&) = delete;
    HostCallScope& operator=(HostCallScope const&) = delete;
    HostCallScope& operator=(HostCallScope&&) = delete;

  private:
    Store* _store;
};

} // namespace isoline::runtime
