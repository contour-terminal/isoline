// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace isoline::fastinterpreter
{

/// An integer linear memory holds: any integral type but bool. Floats are carried as their bits, so
/// they are accessed as the unsigned integer of their width.
template <typename T>
concept MemoryInteger = std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>;

/// A byte order a host can have: the order of a memory access's template, never a mixed one.
template <std::endian Order>
concept HostByteOrder = Order == std::endian::little || Order == std::endian::big;

/// The integer whose little-endian bytes start at @p address: how the fast interpreter reads linear
/// memory, which is little-endian on every host.
///
/// The bytes are copied as the host stores a T, then swapped where the host is big-endian. The host's
/// order is a parameter, not a fixed fact, so that tests run both branches on every host: with the
/// order that is not the host's, the result is what a host of that order would read.
/// @tparam T The integer to read.
/// @tparam HostOrder The host's byte order; native unless a test simulates the other.
/// @param address The first byte; any alignment.
/// @return The integer.
template <MemoryInteger T, std::endian HostOrder = std::endian::native>
    requires HostByteOrder<HostOrder>
[[nodiscard]] T loadLittleEndian(std::byte const* address) noexcept
{
    auto value = T {};
    std::memcpy(&value, address, sizeof(T));
    if constexpr (HostOrder == std::endian::big)
        value = std::byteswap(value);
    return value;
}

/// Writes @p value as little-endian bytes starting at @p address: how the fast interpreter writes
/// linear memory.
///
/// The inverse of loadLittleEndian(): swapped where the host is big-endian, then copied. T is never
/// deduced from @p value, so a promoted argument (`u16 + 1`, an int) converts to T and writes exactly
/// sizeof(T) bytes; every call names T.
/// @tparam T The integer to write.
/// @tparam HostOrder The host's byte order; native unless a test simulates the other.
/// @param address The first byte; any alignment.
/// @param value The integer.
template <MemoryInteger T, std::endian HostOrder = std::endian::native>
    requires HostByteOrder<HostOrder>
void storeLittleEndian(std::byte* address, std::type_identity_t<T> value) noexcept
{
    if constexpr (HostOrder == std::endian::big)
        value = std::byteswap(value);
    std::memcpy(address, &value, sizeof(T));
}

} // namespace isoline::fastinterpreter
