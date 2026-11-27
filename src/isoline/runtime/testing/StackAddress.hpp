// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// Where the host stack is, for tests that measure what a nesting takes of it. The platform's half is
/// CMake-selected by the compiler's frontend (StackAddressGnu.cpp, StackAddressMsvc.cpp). Tests only.
namespace isoline::runtime::testing
{

/// An address in the frame of this out-of-line function, on the real host stack: not a local's address,
/// which a sanitizer may move to a stack of its own (ASan's fake stack, for use-after-return detection).
/// Two calls at different depths differ by the stack between them.
/// @return The address.
[[nodiscard]] std::uintptr_t stackAddress() noexcept;

} // namespace isoline::runtime::testing
