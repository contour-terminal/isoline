#pragma once

namespace isoline::api
{
template <typename T>
inline constexpr int width = 1;

template <>
inline constexpr int width<int> = 4;

template <typename T>
inline constexpr int width<T*> = 8;
} // namespace isoline::api
