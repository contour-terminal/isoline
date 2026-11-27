// SPDX-License-Identifier: Apache-2.0
#pragma once

// The header side of a program that both includes and imports Isoline (D7): this header includes Isoline
// headers only, and HeaderSide.cpp, which defines its functions, is an ordinary TU that imports nothing.
// An importer declares the same functions by the names `import isoline;` provides (ImportConsumer_test.cpp)
// and links them: the types are one set of entities on both sides.

#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>

namespace isoline::modules_test
{

/// Instantiates the add module in a store made by the caller.
/// @param store The store to instantiate in.
/// @param module The add module, loaded by the caller (tests/consumer/AddModule.hpp).
/// @return The instance, or the error.
[[nodiscard]] std::expected<isoline::api::Instance, isoline::api::InstantiateError> instantiateAdd(
    isoline::api::Store& store, isoline::api::Module const& module);

/// The address of FeatureTable as the header side sees it.
/// @return Its address.
[[nodiscard]] void const* featureTableAddressFromHeaders() noexcept;

} // namespace isoline::modules_test
