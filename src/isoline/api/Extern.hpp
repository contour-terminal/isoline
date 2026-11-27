// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

#include <isoline/api/Types.hpp>

/// The handles of a store's entities: what an instance exports (Instance::exportAt()) and what an
/// embedder reads and writes through the store (Store::globalValue(), Store::tableGet(), …).
///
/// Every handle is made by its store alone, so that it always names an entity that store holds. It
/// keeps the store's identity, to be told apart from another store's (isOf()), and a token that
/// expires with the store, so that nothing dereferences a store that is gone: the accessors that
/// return a reference into the store (store(), type()) check it, and abort when the store is gone.
namespace isoline::api
{

class Instance;
class Store;

/// The identity of a store: what its handles (Instance, Function, Memory, Global, Table, Tag) carry,
/// and what an Imports definition of one of its entities keeps, so that a handle of another store is
/// told apart.
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

namespace detail
{
    /// What every handle keeps of the store that made it: its identity, that identity's serial
    /// number, and the store's alive-token, which expires when the store is destroyed.
    class StoreLink
    {
      public:
        /// A link to the store of identity @p store, alive while @p alive is.
        /// @param store The store's identity.
        /// @param alive The store's alive-token.
        StoreLink(StoreId const& store, std::weak_ptr<void> alive) noexcept:
            _store { &store }, _storeSerial { store.serial() }, _alive { std::move(alive) }
        {
        }

        /// Whether the store is still alive: whether its alive-token has not expired.
        /// @return Whether the store may be dereferenced.
        [[nodiscard]] bool isAlive() const noexcept { return !_alive.expired(); }

        /// The identity of the store. Precondition (checked, aborting): the store is alive.
        /// @return The identity.
        [[nodiscard]] StoreId const& store() const noexcept;

        /// Whether the store is the one @p store names: compares serial numbers, never reading the
        /// linked store, so it may be asked when the store is gone.
        /// @param store A live store's identity.
        /// @return Whether they are the same store.
        [[nodiscard]] bool isOf(StoreId const& store) const noexcept
        {
            return _storeSerial == store.serial();
        }

      private:
        StoreId const* _store;      ///< The identity of the store; never null, valid while it is alive.
        uint64_t _storeSerial;      ///< That identity's serial number, which isOf() compares.
        std::weak_ptr<void> _alive; ///< The store's alive-token; expired once the store is destroyed.
    };
} // namespace detail

/// A function of a store: a handle which only the store makes (Instance::function(), Store::define()),
/// so that it always names a function the store holds.
class Function
{
  public:
    /// The identity of the store that holds it. Precondition (checked, aborting): the store is alive.
    /// @return The identity.
    [[nodiscard]] StoreId const& store() const noexcept { return _link.store(); }

    /// Its address in that store, which HostContext::invoke() takes.
    /// @return The address.
    [[nodiscard]] FuncAddr address() const noexcept { return _address; }

    /// Its type, in its store's form. Precondition (checked, aborting): the store is alive.
    /// @return The type, valid as long as the store.
    [[nodiscard]] FuncType const& type() const noexcept;

    /// Whether it is a function of the store @p store names. Unlike store(), it may be asked of a
    /// handle whose store is gone: it compares serial numbers, never reading the handle's store.
    /// @param store A live store's identity.
    /// @return Whether that store made it.
    [[nodiscard]] bool isOf(StoreId const& store) const noexcept { return _link.isOf(store); }

  private:
    friend class Store;
    friend class Instance;

    /// The function at @p address of the store @p link names, of @p type.
    Function(detail::StoreLink link, FuncAddr address, FuncType const& type) noexcept:
        _link { std::move(link) }, _address { address }, _type { &type }
    {
    }

    detail::StoreLink _link; ///< The store that holds it.
    FuncAddr _address;       ///< Its address in that store.
    FuncType const* _type;   ///< Its type, held by that store; never null.
};

/// A linear memory of a store: a handle which only the store makes (Instance::memory()), so that it
/// always names a memory the store holds.
class Memory
{
  public:
    /// The identity of the store that holds it. Precondition (checked, aborting): the store is alive.
    /// @return The identity.
    [[nodiscard]] StoreId const& store() const noexcept { return _link.store(); }

    /// Its address in that store.
    /// @return The address.
    [[nodiscard]] MemAddr address() const noexcept { return _address; }

    /// Whether it is a memory of the store @p store names, as Function::isOf() asks it.
    /// @param store A live store's identity.
    /// @return Whether that store holds it.
    [[nodiscard]] bool isOf(StoreId const& store) const noexcept { return _link.isOf(store); }

  private:
    friend class Store;
    friend class Instance;

    /// The memory at @p address of the store @p link names.
    Memory(detail::StoreLink link, MemAddr address) noexcept: _link { std::move(link) }, _address { address }
    {
    }

    detail::StoreLink _link; ///< The store that holds it.
    MemAddr _address;        ///< Its address in that store.
};

/// A global of a store: a handle which only the store makes (Instance::global()), so that it always
/// names a global the store holds. Store::globalValue() and Store::setGlobal() read and write it.
class Global
{
  public:
    /// The identity of the store that holds it. Precondition (checked, aborting): the store is alive.
    /// @return The identity.
    [[nodiscard]] StoreId const& store() const noexcept { return _link.store(); }

    /// Its address in that store.
    /// @return The address.
    [[nodiscard]] GlobalAddr address() const noexcept { return _address; }

    /// Whether it is a global of the store @p store names, as Function::isOf() asks it.
    /// @param store A live store's identity.
    /// @return Whether that store holds it.
    [[nodiscard]] bool isOf(StoreId const& store) const noexcept { return _link.isOf(store); }

  private:
    friend class Store;
    friend class Instance;

    /// The global at @p address of the store @p link names.
    Global(detail::StoreLink link, GlobalAddr address) noexcept:
        _link { std::move(link) }, _address { address }
    {
    }

    detail::StoreLink _link; ///< The store that holds it.
    GlobalAddr _address;     ///< Its address in that store.
};

/// A table of a store: a handle which only the store makes (Instance::table()), so that it always
/// names a table the store holds. Store::tableGet(), Store::tableSet() and Store::growTable() reach it.
class Table
{
  public:
    /// The identity of the store that holds it. Precondition (checked, aborting): the store is alive.
    /// @return The identity.
    [[nodiscard]] StoreId const& store() const noexcept { return _link.store(); }

    /// Its address in that store.
    /// @return The address.
    [[nodiscard]] TableAddr address() const noexcept { return _address; }

    /// Whether it is a table of the store @p store names, as Function::isOf() asks it.
    /// @param store A live store's identity.
    /// @return Whether that store holds it.
    [[nodiscard]] bool isOf(StoreId const& store) const noexcept { return _link.isOf(store); }

  private:
    friend class Store;
    friend class Instance;

    /// The table at @p address of the store @p link names.
    Table(detail::StoreLink link, TableAddr address) noexcept: _link { std::move(link) }, _address { address }
    {
    }

    detail::StoreLink _link; ///< The store that holds it.
    TableAddr _address;      ///< Its address in that store.
};

/// An exception tag of a store: a handle which only the store makes (Instance::tag()), so that it
/// always names a tag the store holds. An UncaughtException's tag compares with its address().
class Tag
{
  public:
    /// The identity of the store that holds it. Precondition (checked, aborting): the store is alive.
    /// @return The identity.
    [[nodiscard]] StoreId const& store() const noexcept { return _link.store(); }

    /// Its address in that store, which UncaughtException::tag compares with.
    /// @return The address.
    [[nodiscard]] TagAddr address() const noexcept { return _address; }

    /// Its type, in its store's form: its params are the payload's types, and it has no results.
    /// Precondition (checked, aborting): the store is alive.
    /// @return The type, valid as long as the store.
    [[nodiscard]] FuncType const& type() const noexcept;

    /// Whether it is a tag of the store @p store names, as Function::isOf() asks it.
    /// @param store A live store's identity.
    /// @return Whether that store holds it.
    [[nodiscard]] bool isOf(StoreId const& store) const noexcept { return _link.isOf(store); }

  private:
    friend class Store;
    friend class Instance;

    /// The tag at @p address of the store @p link names, of @p type.
    Tag(detail::StoreLink link, TagAddr address, FuncType const& type) noexcept:
        _link { std::move(link) }, _address { address }, _type { &type }
    {
    }

    detail::StoreLink _link; ///< The store that holds it.
    TagAddr _address;        ///< Its address in that store.
    FuncType const* _type;   ///< Its type, held by that store; never null.
};

/// Any entity of a store an instance exports or an import binds to, one alternative per ExternKind,
/// in ExternKind's order.
using Extern = std::variant<Function, Table, Memory, Global, Tag>;

/// The alternative of Extern an entity of @p kind is.
template <ExternKind Kind>
using ExternAlternative = std::variant_alternative_t<std::to_underlying(Kind), Extern>;

static_assert(std::is_same_v<ExternAlternative<ExternKind::Func>, Function>, "Extern follows ExternKind");
static_assert(std::is_same_v<ExternAlternative<ExternKind::Table>, Table>, "Extern follows ExternKind");
static_assert(std::is_same_v<ExternAlternative<ExternKind::Memory>, Memory>, "Extern follows ExternKind");
static_assert(std::is_same_v<ExternAlternative<ExternKind::Global>, Global>, "Extern follows ExternKind");
static_assert(std::is_same_v<ExternAlternative<ExternKind::Tag>, Tag>, "Extern follows ExternKind");
static_assert(std::variant_size_v<Extern> == std::to_underlying(ExternKind::Tag) + 1,
              "one Extern alternative per ExternKind");

} // namespace isoline::api
