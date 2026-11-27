#pragma once

namespace isoline::api
{
template <typename T>
    requires(sizeof(T) < 8)
struct Small
{
    T value;
};
} // namespace isoline::api
