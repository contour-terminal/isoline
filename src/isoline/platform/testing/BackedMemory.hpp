// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include <isoline/platform/VirtualMemory.hpp>

namespace isoline::platform::testing
{

/// How many bytes of @p region are backed by memory now: resident on POSIX (mincore), committed on
/// Windows (VirtualQuery). Reserved pages never touched count none, on every platform.
/// @param region Pages of a reservation.
/// @param pageSize The page size @p region is made of.
/// @return The backed bytes.
[[nodiscard]] uint64_t backedBytes(Region region, uint64_t pageSize);

} // namespace isoline::platform::testing
