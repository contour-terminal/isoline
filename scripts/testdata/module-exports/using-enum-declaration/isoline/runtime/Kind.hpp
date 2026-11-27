#pragma once

namespace isoline::runtime
{
enum class Kind : unsigned char
{
    Function,
    Memory,
};

namespace detail
{
    // In an internal namespace: a known kind that declares nothing of internal linkage.
    using enum Kind;
} // namespace detail
} // namespace isoline::runtime
