// SPDX-License-Identifier: Apache-2.0
#include <isoline/api/Extern.hpp>

#include <memory>
#include <utility>

#include <isoline/api/Store.hpp>
#include <isoline/meta/Check.hpp>

namespace isoline::api
{

detail::StoreLink::StoreLink(Store const& store, std::weak_ptr<void> alive) noexcept:
    _store { &store }, _storeSerial { store.id().serial() }, _alive { std::move(alive) }
{
}

Store const& detail::StoreLink::owner() const noexcept
{
    ISOLINE_PRECONDITION(isAlive());
    return *_store;
}

StoreId const& detail::StoreLink::store() const noexcept
{
    return owner().id();
}

FuncType const& Function::type() const noexcept
{
    ISOLINE_PRECONDITION(_link.isAlive());
    return *_type;
}

FuncType const& Tag::type() const noexcept
{
    ISOLINE_PRECONDITION(_link.isAlive());
    return *_type;
}

} // namespace isoline::api
