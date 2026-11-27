#pragma once

namespace isoline::runtime
{
namespace
{
    struct Handle
    {
        int value;
    };
} // namespace
} // namespace isoline::runtime

namespace isoline::api
{
// Re-exports a type of an anonymous namespace: it has internal linkage, so no interface unit can export it.
using isoline::runtime::Handle;
} // namespace isoline::api
