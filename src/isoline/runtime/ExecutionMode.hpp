// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace isoline::runtime
{

/// How an engine computes what the WebAssembly specification leaves nondeterministic.
///
/// It is fixed when a module's code is made, and is part of what that code depends on.
enum class ExecutionMode : uint8_t
{
    /// Whatever is fastest within the specification.
    Production,
    /// The same result on every engine and every host: canonical NaNs, the deterministic
    /// relaxed-SIMD profile, and the step limit counted alike everywhere.
    Deterministic,
};

} // namespace isoline::runtime
