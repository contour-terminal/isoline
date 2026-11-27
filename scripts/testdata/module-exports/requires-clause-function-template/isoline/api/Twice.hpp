#pragma once

namespace isoline::api
{
template <typename T>
    requires(sizeof(T) < 8)
T twice(T value)
{
    return value + value;
}
} // namespace isoline::api
