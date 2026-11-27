#pragma once

// At global scope, outside the root the dump is filtered by: the walk does not see it.
static int probe()
{
    return 1;
}

namespace isoline::api
{
inline constexpr int storeLimit = 16;

// Re-exports an entity of internal linkage: only the precompile can tell.
using ::probe;
} // namespace isoline::api
