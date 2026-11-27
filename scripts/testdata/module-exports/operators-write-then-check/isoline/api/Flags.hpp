#pragma once

#include <cstddef>

namespace isoline::api
{
enum class Flags : unsigned
{
    None = 0,
    Read = 1,
};

constexpr Flags operator|(Flags left, Flags right)
{
    return static_cast<Flags>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}

constexpr Flags& operator|=(Flags& left, Flags right)
{
    return left = left | right;
}

constexpr bool operator!(Flags flags)
{
    return flags == Flags::None;
}

constexpr int operator->*(Flags flags, int shift)
{
    return static_cast<int>(static_cast<unsigned>(flags) << shift);
}

struct Awaiter
{
    int value;
};

Awaiter operator co_await(Flags flags);

constexpr unsigned long long operator""_kib(unsigned long long count)
{
    return count * 1024;
}
} // namespace isoline::api
