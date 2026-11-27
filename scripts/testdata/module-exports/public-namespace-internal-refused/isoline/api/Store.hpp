#pragma once

namespace isoline::api
{
struct Store
{
};

namespace detail
{
    int helper();
} // namespace detail
} // namespace isoline::api

namespace isoline::runtime::api
{
int instanceCount();
} // namespace isoline::runtime::api
