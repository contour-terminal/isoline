// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the store caps of Contour's WebAssembly backend contract, which the contract cases
// (testing/Contract.hpp) and the engine rows' configurations (testing/EngineRows.hpp) share.
#pragma once

#include <cstdint>

namespace isoline::api::testing::contract
{

/// The caps a contract store keeps to: Contour's RuntimeLimits, as StoreLimits express them.
struct ContractLimits
{
    uint64_t memoryBytes = uint64_t { 4 } * 1024 * 1024; ///< All memories of the store together.
    uint64_t tableElements = 64; ///< All tables of the store together, below any engine default.
};

} // namespace isoline::api::testing::contract
