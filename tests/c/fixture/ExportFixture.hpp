// SPDX-License-Identifier: Apache-2.0
#pragma once

// The export-rules fixture (the c-export-rules test): a shared library built under the C API's export
// rules, defining beside its one C function what a C++ library defines and must not export.

#include <isoline/c/export.h>

/// The fixture's C API: exported as every isoline_ function is, through ISOLINE_API.
/// @param value What fixture::leak() adds to.
/// @return fixture::leak(value).
extern "C" ISOLINE_API int isoline_fixture(int value) noexcept;

namespace fixture
{

/// A C++ function of default visibility, as a header outside Isoline's control may declare one: the
/// hidden visibility the library is compiled with does not hide it, the version script must.
/// @param value What leak() adds its count to.
/// @return The sum, through a std::vector, whose instantiations libstdc++ gives default visibility.
[[gnu::visibility("default")]] int leak(int value);

} // namespace fixture
