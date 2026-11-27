#pragma once

namespace isoline::api
{
template <typename Value>
inline constexpr int sizeOf = static_cast<int>(sizeof(Value));
} // namespace isoline::api
