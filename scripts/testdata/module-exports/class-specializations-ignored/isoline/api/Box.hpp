#pragma once

namespace isoline::api
{
template <typename T>
struct Box
{
};

template <>
struct Box<int>
{
};

template <typename T>
struct Box<T*>
{
};
} // namespace isoline::api
