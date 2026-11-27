#pragma once

namespace isoline::api
{
struct Foo;

template <typename T>
struct Box
{
    T value;
};

template <typename T>
T identity(T value)
{
    return value;
}

template <typename T>
inline constexpr bool isBox = false;

template <typename T>
using BoxOf = Box<T>;

using Pages = int;
typedef unsigned Count;

template <typename T>
concept Boxed = isBox<T>;

static_assert(sizeof(int) >= 2);
} // namespace isoline::api
