// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include <isoline/api/Store.hpp>
#include <isoline/runtime/Store.hpp>

namespace isoline::api::testing
{

/// Test-only access to what an api::Store keeps from the embedder: the runtime::Store it checks every
/// access to. Not part of the embedding API: raw access bypasses every check the api makes.
struct RuntimeAccess
{
    /// The runtime store @p store wraps.
    /// @param store The store.
    /// @return Its runtime store.
    [[nodiscard]] static runtime::Store& runtimeOf(Store& store) noexcept { return store._runtime; }

    /// The module instance address @p instance is a handle of.
    /// @param instance The instance.
    /// @return Its address in its store.
    [[nodiscard]] static uint32_t addressOf(Instance const& instance) noexcept { return instance._address; }
};

/// The runtime store @p store wraps (RuntimeAccess::runtimeOf()).
/// @param store The store.
/// @return Its runtime store.
[[nodiscard]] inline runtime::Store& runtimeOf(Store& store) noexcept
{
    return RuntimeAccess::runtimeOf(store);
}

} // namespace isoline::api::testing
