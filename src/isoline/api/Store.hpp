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

/// The identity of a store: what its handles (Instance, Function, Memory) carry, and what an
/// Imports definition of one of its entities keeps, so that a handle of another store is told apart.
///
/// It is opaque. Each one has a serial number no other identity of the process has had, which the
/// handles copy: a handle outliving its store is never taken for one of a later store, even one whose
/// identity the allocator places at the same address. An Imports definition keeps the identity
/// alive.
class StoreId: public std::enable_shared_from_this<StoreId>
{
  public:
    /// A new identity, of the next serial number of the process.
    StoreId() noexcept;

    /// Its serial number, unique within the process.
    /// @return The serial number.
    [[nodiscard]] uint64_t serial() const noexcept { return _serial; }

  private:
    uint64_t _serial; ///< Unique within the process; never reused.
};

/// A function of a store: a handle valid as long as the store, which only the store makes
/// (Instance::function(), Store::define()), so that it always names a function the store holds.
class Function
{
  public:
    /// The identity of the store that holds it.
    /// @return The identity.
    [[nodiscard]] StoreId const& store() const noexcept { return *_store; }

    /// Its address in that store, which HostContext::invoke() takes.
    /// @return The address.
    [[nodiscard]] FuncAddr address() const noexcept { return _address; }

    /// Its type, in its store's form.
    /// @return The type, valid as long as the store.
    [[nodiscard]] FuncType const& type() const noexcept { return *_type; }

    /// Whether it is a function of the store @p store names. Unlike store(), it may be asked of a
    /// handle whose store is gone: it compares serial numbers, never reading the handle's store.
    /// @param store A live store's identity.
    /// @return Whether that store made it.
    [[nodiscard]] bool isOf(StoreId const& store) const noexcept { return _storeSerial == store.serial(); }

  private:
    friend class Store;
    friend class Instance;

    /// The function at @p address of the store @p store names, of @p type.
    Function(StoreId const& store, FuncAddr address, FuncType const& type) noexcept:
        _store { &store }, _storeSerial { store.serial() }, _address { address }, _type { &type }
    {
    }

    StoreId const* _store; ///< The identity of the store that holds it; never null.
    uint64_t _storeSerial; ///< That identity's serial number, which isOf() compares.
    FuncAddr _address;     ///< Its address in that store.
    FuncType const* _type; ///< Its type, held by that store; never null.
};

/// A linear memory of a store: a handle valid as long as the store, which only the store makes
/// (Instance::memory()), so that it always names a memory the store holds.
class Memory
{
  public:
    /// The identity of the store that holds it.
    /// @return The identity.
    [[nodiscard]] StoreId const& store() const noexcept { return *_store; }

    /// Its address in that store.
    /// @return The address.
    [[nodiscard]] MemAddr address() const noexcept { return _address; }

    /// Whether it is a memory of the store @p store names, as Function::isOf() asks it.
    /// @param store A live store's identity.
    /// @return Whether that store holds it.
    [[nodiscard]] bool isOf(StoreId const& store) const noexcept { return _storeSerial == store.serial(); }

  private:
    friend class Instance;

    /// The memory at @p address of the store @p store names.
    Memory(StoreId const& store, MemAddr address) noexcept:
        _store { &store }, _storeSerial { store.serial() }, _address { address }
    {
    }

    StoreId const* _store; ///< The identity of the store that holds it; never null.
    uint64_t _storeSerial; ///< That identity's serial number, which isOf() compares.
    MemAddr _address;      ///< Its address in that store.
};

/// The current size of a linear memory (Instance::memorySize()).
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

/// An import bound to an Instance of another store (Imports::define()): entities do not cross stores.
struct ForeignImport
{
    uint32_t index = 0; ///< The import's index in the module's import section.

    /// Equal when they name the same import.
    bool operator==(ForeignImport const&) const = default;
};

/// Why Store::instantiate() failed: the module is of another engine; an import is bound to another
/// store's entity; an import did not link (no definition, or one of another kind or type); the
/// instance would exceed one of the store's limits; a memory or table could not be allocated; the host
/// ran out of memory before anything was added to the store; or initializing it (a segment, the start
/// function) failed as a call does.
using InstantiateError = std::variant<EngineMismatch,
                                      ForeignImport,
                                      LinkError,
                                      LimitExceeded,
                                      AllocationFailure,
                                      HostMemoryExhausted,
                                      CallError>;

/// Why Store::define() could not add a host function: its type is new and over the store's type
/// budget, or the host ran out of memory before anything was added to the store.
using DefineError = std::variant<LimitExceeded, HostMemoryExhausted>;

/// Why Instance::memoryRange() could not view a range.
enum class MemoryAccessError : uint8_t
{
    OutOfBounds, ///< A byte of the range lies outside the memory.
};

/// A store: where instances live, with their memories, tables, globals and the host functions bound
/// to them, under the limits of the engine it was made for.
///
/// It copies the engine's configuration, so the Engine may go before it.
///
/// Threads: the store and all its instances are one unit, used by one thread at a time; the unit may
/// move between threads between calls. Its interrupter (interrupter()) is the one thing another thread
/// may use while it runs. A host function implementation shared by several stores (one Imports used by
/// stores on several threads) is called on each of their threads at once, so it must be thread-safe.
///
/// Nothing is thrown. Where the host runs out of memory before anything is added to the store, the
/// call is refused (HostMemoryExhausted). Once the store is being changed (an instance allocated, a
/// host function added, a call running) running out of memory terminates the process, as it does in
/// runtime::invokeChecked(): the store is not exception-safe midway through a change.
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
    Store(Engine const& engine, IMemoryAllocator& allocator, IPollObserver* observer = nullptr);

    Store(Store const&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store const&) = delete;
    Store& operator=(Store&&) = delete;
    ~Store();

    /// The store's interrupter: interrupt() it from any thread to stop the call running in the store,
    /// and every later call and instantiation, for good. It exists before any instantiation, so a start
    /// function is interruptible too, and a watchdog may keep it past the store.
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
    /// @return The instance, valid as long as the store; or EngineMismatch for a module of another
    ///         engine, ForeignImport for an import bound to another store's instance, LinkError
    ///         (UnknownImport for an import with no definition, IncompatibleImportType for one of another
    ///         kind or type), LimitExceeded, AllocationFailure, HostMemoryExhausted when the host ran
    ///         out of memory before the instance was begun (runtime::instantiate()'s first step), or a
    ///         CallError when initializing it trapped, threw, failed in a host function or was
    ///         interrupted. A CallError's refusedGrowth is instantiation's own, its start function's first
    ///         refused grows, never a nested invocation's.
    [[nodiscard]] std::expected<Instance, InstantiateError> instantiate(Module const& module,
                                                                        Imports const& imports) noexcept;

    /// Adds a host function to the store, outside any instantiation: to define it as an import
    /// (Imports::define()), or to hand it to the guest in a table. The same implementation and type,
    /// defined again or bound by an instantiation, is the same function.
    /// @param type Its type. It names no concrete type index (checked), as an Imports host definition's.
    /// @param function Its implementation (checked: not null).
    /// @return The function; LimitExceeded of Types when its type is new and over the store's budget;
    ///         HostMemoryExhausted when the host ran out of memory before it was added.
    [[nodiscard]] std::expected<Function, DefineError> define(
        FuncType const& type, std::shared_ptr<IHostFunction> const& function) noexcept;

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
};

/// An instance of a module in a store: a handle, valid as long as the store, and cheap to copy.
///
/// Calls into it are re-entrant: a host function may call() back into this or another instance of the
/// store, from within the call that called it.
class Instance
{
  public:
    /// Its exported function @p name.
    /// @param name The export's name.
    /// @return The function; nothing when it exports no function of that name.
    [[nodiscard]] std::optional<Function> function(std::string_view name) const noexcept;

    /// Its exported memory @p name.
    /// @param name The export's name.
    /// @return The memory; nothing when it exports no memory of that name.
    [[nodiscard]] std::optional<Memory> memory(std::string_view name) const noexcept;

    /// Its exported tag @p name, which an UncaughtException's tag compares with.
    /// @param name The export's name.
    /// @return The tag's address; nothing when it exports no tag of that name.
    [[nodiscard]] std::optional<TagAddr> tag(std::string_view name) const noexcept;

    /// Calls @p function: the one checked entry into the guest (runtime::invokeChecked()), as a host
    /// function's HostContext::invoke() is.
    ///
    /// Exnrefs among the results are escaped, valid for the store's life. noexcept: the call changes
    /// the store, so running out of memory midway terminates (see Store).
    /// @param function A function of this instance's store (checked: calling another store's function
    ///                 is a programming error of the embedder).
    /// @param args Its arguments, of its params' types.
    /// @param results Receives its results; one slot per result.
    /// @return Nothing, or how it failed: ArgumentMismatch, without entering the guest, when @p args
    ///         are not of its params' types or @p results has the wrong size.
    [[nodiscard]] std::expected<void, CallError> call(Function function,
                                                      std::span<Value const> args,
                                                      std::span<Value> results) const noexcept;

    /// A view of @p size bytes of @p memory from @p address, valid until the next call into the store,
    /// which may grow (and so move) the memory.
    /// @param memory A memory of this instance's store (checked).
    /// @param address The first byte's address.
    /// @param size The number of bytes; 0 views nothing, at any address.
    /// @return The view; OutOfBounds when a byte of it lies outside the memory.
    [[nodiscard]] std::expected<std::span<std::byte>, MemoryAccessError> memoryRange(
        Memory memory, uint64_t address, uint64_t size) const noexcept;

    /// The current size of @p memory, which a call into the store may grow.
    /// @param memory A memory of this instance's store (checked).
    /// @return Its size in pages and in bytes.
    [[nodiscard]] MemorySize memorySize(Memory memory) const noexcept;

  private:
    friend class Store;
    friend class Imports;
    friend struct testing::RuntimeAccess;

    /// The instance at @p address of @p store.
    Instance(Store& store, uint32_t address) noexcept;

    /// Its export @p name, when it is of @p kind.
    [[nodiscard]] std::optional<runtime::ExternValue> exported(std::string_view name,
                                                               ExternKind kind) const noexcept;

    Store* _store;     ///< The store it lives in; never null.
    uint32_t _address; ///< Its module instance's address in that store.
};

} // namespace isoline::api
