// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <isoline/platform/JitMemory.hpp>

namespace isoline::platform::testing
{

/// The test helpers' maker of JitWriteScope's passkey: a test that writes into MAP_JIT memory itself
/// opens its one scope with it, as the platform code memory's seal does in production.
struct JitWriteScopeKeys
{
    /// A passkey.
    /// @return It.
    [[nodiscard]] static JitWriteScope::Key key() noexcept { return {}; }
};

} // namespace isoline::platform::testing
