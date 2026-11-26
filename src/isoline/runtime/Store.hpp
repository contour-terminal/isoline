// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
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

/// A host function's implementation: reads its arguments, writes its results, or ends abruptly.
///
/// @p results arrives holding the default value of each result type, and must be left holding a
/// value of each result type's kind: the interpreter checks that as a precondition. A host function
/// that ends with Thrown, an exception of the store it was called from (one a Wasm function it
/// invoked threw, or one it allocated with Store::addException), throws it in its Wasm caller, whose
/// handlers may catch it.
using HostCallback =
    std::function<std::expected<void, Abrupt>(std::span<Value const> args, std::span<Value> results)>;

/// A function instance: a module's function, or a host function.
struct FunctionInstance
{
    TypeId typeId; ///< Its type's identity in the store (Store::type), which call_indirect compares.
    uint32_t moduleInstance; ///< A module function: the instance that defines it.
    uint32_t codeIndex;      ///< A module function: its index in that module's code section.
    HostCallback host;       ///< A host function: its implementation; empty for a module function.

    /// Whether this is a host function.
    /// @return Whether host is set.
    [[nodiscard]] bool isHost() const noexcept { return static_cast<bool>(host); }
};

/// A table instance.
struct TableInstance
{
    binary::TableType type;    ///< Its type as allocated; its current size is elements.size().
    std::vector<Ref> elements; ///< Its slots.
};

/// A memory instance.
struct MemoryInstance
{
    binary::MemoryType type;      ///< Its type as allocated; its size is bytes.size() / pageSize(type).
    std::vector<std::byte> bytes; ///< Its contents, a whole number of pages.
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

/// The limits a store's execution keeps to: what a guest may claim of the host's stack and memory
/// besides its memories and tables.
struct StoreLimits
{
    /// How deep calls may nest before execution traps with CallStackExhausted.
    uint32_t maxCallDepth = 10'000;

    /// How many exceptions the store may hold; a throw past it traps with ExceptionLimitExceeded. The
    /// store never frees one (reclaiming unreferenced ones is P3's), so this bounds what a guest
    /// that throws in a loop may claim.
    uint32_t maxExceptions = 65'536;

    /// How many payload values the exceptions together may hold; a throw past it traps with
    /// ExceptionLimitExceeded.
    uint64_t maxExceptionValues = uint64_t { 1 } << 20;
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

/// Decides how much memory the store may hold: the seam memory and table growth is limited by.
class IMemoryAllocator
{
  public:
    virtual ~IMemoryAllocator() = default;

    /// Whether the store may hold @p bytes for one memory or one table.
    /// @param bytes The size the memory or table would take, in bytes.
    /// @return Whether the store may hold @p bytes for one memory or table.
    [[nodiscard]] virtual bool mayHold(uint64_t bytes) const noexcept = 0;
};

/// The allocator that grants any memory or table of up to 4 GiB: every memory a 32-bit address
/// space can reach, and every table of up to 4 GiB / sizeof(Ref) slots. Tables are charged
/// sizeof(Ref) bytes per slot, so a table of 32-bit addresses near its 2^32 - 1 slot reach is
/// refused.
class UnlimitedAllocator final: public IMemoryAllocator
{
  public:
    /// @param bytes The size asked for.
    /// @return Whether @p bytes is at most 4 GiB.
    [[nodiscard]] bool mayHold(uint64_t bytes) const noexcept override { return bytes <= FourGiB; }

  private:
    static constexpr auto FourGiB = uint64_t { 1 } << 32;
};

/// Why the store could not allocate a memory or a table.
struct AllocationFailure
{
    binary::ExternKind kind; ///< Memory or Table.

    bool operator==(AllocationFailure const&) const = default;
};

/// The store: every function, table, memory, global and module instance, addressed by index.
///
/// Instances are never removed, and adding one never moves the others, so a reference to an
/// instance stays valid as long as the store.
class Store
{
  public:
    /// Constructs an empty store.
    /// @param allocator Decides how large a memory or table may grow; must outlive the store.
    /// @param limits The call depth and the exceptions execution may reach.
    explicit Store(IMemoryAllocator const& allocator, StoreLimits limits = {});

    /// A store's instances are addressed by index and its types handed out by reference (type()),
    /// so it stays where it was constructed: it is neither copied nor moved.
    Store(Store const&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store const&) = delete;
    Store& operator=(Store&&) = delete;
    ~Store() = default;

    /// Interns a function type: the TypeId of the type equal to it, added if there is none. A type
    /// added gets the next TypeId, typeCount() before the call, so that instantiation can tell the
    /// TypeIds a module's types will get before it interns them.
    ///
    /// Precondition (checked): @p type is in store form, every concrete type index in it a TypeId of
    /// this store, as interning a module's types in type order makes them.
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

    /// The type with TypeId @p id. Precondition (checked): it exists.
    /// @param id The type's identity.
    /// @return The type, in store form; the reference stays valid as long as the store.
    [[nodiscard]] binary::FuncType const& type(TypeId id) const;

    /// Adds a host function.
    /// @param type Its type, in store form; interned (internType()).
    /// @param callback Its implementation; must not be empty.
    /// @return Its address.
    [[nodiscard]] FuncAddr addHostFunction(binary::FuncType type, HostCallback callback);

    /// Adds a module function.
    /// @param typeId Its type, interned already.
    /// @param moduleInstance The instance that defines it.
    /// @param codeIndex Its index in that module's code section.
    /// @return Its address.
    [[nodiscard]] FuncAddr addModuleFunction(TypeId typeId, uint32_t moduleInstance, uint32_t codeIndex);

    /// Adds a table of type.limits.min slots, each holding @p init.
    /// @param type Its type.
    /// @param init What every slot starts with.
    /// @return Its address, or the failure when the allocator refuses its size or allocation fails.
    [[nodiscard]] std::expected<TableAddr, AllocationFailure> addTable(binary::TableType type, Ref init);

    /// Adds a zeroed memory of type.limits.min pages.
    ///
    /// Precondition (checked): type.pageSizeLog2 is below binary::PageSizeLog2Limit, so that the page
    /// size is a u64, as the decoder guarantees for every module's memory.
    /// @param type Its type.
    /// @return Its address, or the failure when the allocator refuses its size or allocation fails.
    [[nodiscard]] std::expected<MemAddr, AllocationFailure> addMemory(binary::MemoryType type);

    /// Adds a global.
    /// @param type Its type.
    /// @param value Its initial value, of type.type.
    /// @return Its address.
    [[nodiscard]] GlobalAddr addGlobal(binary::GlobalType type, Value value);

    /// Adds a tag.
    /// @param typeId Its type, interned already (checked).
    /// @return Its address: a new tag, distinct from every other, whatever its type.
    [[nodiscard]] TagAddr addTag(TypeId typeId);

    /// Allocates an exception, as throw does, or as a host function may to throw one.
    /// @param tag The tag it is thrown with; must exist (checked).
    /// @param payload The values it carries: one of each of the tag's params' kinds (checked).
    /// @return Its address, or ExceptionLimitExceeded when the store holds StoreLimits::maxExceptions
    ///         exceptions already, or their payloads would exceed StoreLimits::maxExceptionValues.
    [[nodiscard]] std::expected<ExnAddr, Trap> addException(TagAddr tag, std::vector<Value> payload);

    /// Adds a module instance.
    /// @param instance The instance; the module and its entities' addresses, as far as allocated.
    /// @return Its address.
    [[nodiscard]] uint32_t addModuleInstance(ModuleInstance instance);

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

    /// Grows a memory by @p deltaPages zeroed pages, as memory.grow does.
    /// @param address The memory's address.
    /// @param deltaPages The number of pages to add.
    /// @return The old size in pages; or -1, the size unchanged, when the result would exceed the
    ///         memory's maximum or its address type's reach, the allocator refuses it, or
    ///         allocation fails.
    [[nodiscard]] int64_t growMemory(MemAddr address, uint64_t deltaPages);

    /// Grows a table by @p delta slots holding @p init, as table.grow does.
    /// @param address The table's address.
    /// @param delta The number of slots to add.
    /// @param init What the new slots hold.
    /// @return The old size; or -1, the size unchanged, when the result would exceed the table's
    ///         maximum or its address type's reach, the allocator refuses it, or allocation fails.
    [[nodiscard]] int64_t growTable(TableAddr address, uint64_t delta, Ref init);

    /// How deep calls may nest.
    /// @return The limit the store was constructed with.
    [[nodiscard]] uint32_t maxCallDepth() const noexcept;

  private:
    IMemoryAllocator const* _allocator;
    StoreLimits _limits;
    uint64_t _exceptionValues = 0; ///< The payload values of every exception held.
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

} // namespace isoline::runtime
