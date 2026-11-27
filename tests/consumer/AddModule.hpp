// SPDX-License-Identifier: Apache-2.0
#pragma once

// No Isoline header, and nothing but <array> and <cstddef>: a consumer that only imports Isoline
// includes this too.
#include <array>
#include <cstddef>

namespace isoline::consumer
{

namespace detail
{

    /// The bytes @p octets, each as a std::byte, so that a module reads as the hex dump it is.
    /// @param octets The bytes.
    /// @return The same bytes, in order.
    template <std::size_t Size>
    consteval std::array<std::byte, Size> bytesOf(std::array<unsigned char, Size> const& octets)
    {
        auto bytes = std::array<std::byte, Size> {};
        auto next = bytes.begin();
        for (auto const octet: octets)
            *next++ = static_cast<std::byte>(octet);
        return bytes;
    }

} // namespace detail

/// The add module, assembled by hand, for every consumer test of the package:
///
///     (module
///       (func (export "add") (param i32 i32) (result i32)
///         local.get 0
///         local.get 1
///         i32.add))
///
/// Its 41 bytes, in order:
/// - 8: the magic number `00 61 73 6D` and version 1, `01 00 00 00`.
/// - 9: the type section (1), 7 bytes: one type, `60 02 7F 7F 01 7F`, (func (param i32 i32) (result i32)).
/// - 4: the function section (3), 2 bytes: one function, of type 0.
/// - 9: the export section (7), 7 bytes: one export, `03 'a' 'd' 'd'`, of function 0 (`00 00`).
/// - 11: the code section (10), 9 bytes: one body of 7 bytes, no locals, then local.get 0 (`20 00`),
///   local.get 1 (`20 01`), i32.add (`6A`) and end (`0B`).
inline constexpr auto AddModule = detail::bytesOf(std::to_array<unsigned char>(
    { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60, 0x02, 0x7F,
      0x7F, 0x01, 0x7F, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64,
      0x00, 0x00, 0x0A, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A, 0x0B }));

/// A module that imports `env.double: (i32) -> i32` and exports that import as `double`, for the tests
/// that instantiate a host function (makeHostFunction()):
///
///     (module
///       (import "env" "double" (func (param i32) (result i32)))
///       (export "double" (func 0)))
///
/// Its 44 bytes, in order:
/// - 8: the magic number and version 1.
/// - 8: the type section (1), 6 bytes: one type, `60 01 7F 01 7F`, (func (param i32) (result i32)).
/// - 16: the import section (2), 14 bytes: one import, `03 'env' 06 'double'`, a function (`00`) of type 0.
/// - 12: the export section (7), 10 bytes: one export, `06 'double'`, of function 0 (`00 00`).
inline constexpr auto DoubleImportModule = detail::bytesOf(std::to_array<unsigned char>(
    { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60, 0x01, 0x7F, 0x01,
      0x7F, 0x02, 0x0E, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x06, 0x64, 0x6F, 0x75, 0x62, 0x6C, 0x65,
      0x00, 0x00, 0x07, 0x0A, 0x01, 0x06, 0x64, 0x6F, 0x75, 0x62, 0x6C, 0x65, 0x00, 0x00 }));

} // namespace isoline::consumer
