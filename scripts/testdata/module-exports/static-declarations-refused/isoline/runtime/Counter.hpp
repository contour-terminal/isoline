#pragma once

namespace isoline::runtime
{
static int counter = 0;

template <typename Value>
static Value zero = Value {};

namespace detail
{
    // An internal namespace's own namespaces are looked at too.
    namespace
    {
        int hidden = 0;
    } // namespace
} // namespace detail
} // namespace isoline::runtime
