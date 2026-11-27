// SPDX-License-Identifier: Apache-2.0
#include <isoline/api/Extern.hpp>

#include <core/Assert.hpp>

namespace isoline::api
{

StoreId const& detail::StoreLink::store() const noexcept
{
    Require(isAlive());
    return *_store;
}

FuncType const& Function::type() const noexcept
{
    Require(_link.isAlive());
    return *_type;
}

FuncType const& Tag::type() const noexcept
{
    Require(_link.isAlive());
    return *_type;
}

} // namespace isoline::api
