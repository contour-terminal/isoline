#pragma once

namespace isoline::runtime
{
// An internal namespace is looked at too: each translation unit would have its own pageLimit.
constexpr int pageLimit = 16;

namespace detail
{
    int const tableLimit = 8;
} // namespace detail
} // namespace isoline::runtime
