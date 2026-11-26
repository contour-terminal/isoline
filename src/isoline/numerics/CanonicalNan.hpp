// SPDX-License-Identifier: Apache-2.0
#pragma once

// The canonical NaNs, on their own so that a consumer that only names them (the reference
// interpreter's Numerics.hpp, tests) includes no kernel.

#include <cstdint>

namespace isoline::numerics
{

/// The canonical NaN of f32 (spec 4.3.3): quiet, with no other payload bit; positive.
inline constexpr auto CanonicalNanF32 = uint32_t { 0x7FC0'0000 };
/// The canonical NaN of f64: quiet, with no other payload bit; positive.
inline constexpr auto CanonicalNanF64 = uint64_t { 0x7FF8'0000'0000'0000 };

} // namespace isoline::numerics
