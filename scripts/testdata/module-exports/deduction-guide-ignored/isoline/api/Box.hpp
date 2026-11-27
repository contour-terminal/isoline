#pragma once

namespace isoline::api
{
template <typename T>
struct Box
{
    T value;

    Box(T initial): value(initial) {}
};

template <typename T>
Box(T) -> Box<T>;

Box(char const*) -> Box<int>;
} // namespace isoline::api
